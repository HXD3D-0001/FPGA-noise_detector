"""In-process transport for the AX7021 maintenance UART (115200, 8N1)."""
import json
import re
import threading
import time

STATUS = re.compile(r'LFN ST state=(\d+) start=\d+ stop=\d+ cap=(\d+) cfg=(\d+) fail=(-?\d+) raw=\d+ source=(ADC|TEST|NONE) ch=(\d+)')
STORAGE = re.compile(r'LFN SDSTAT started=([01]) active=([01]) closed=([01]) failed=([01]) session=(\d+) ')
HEARTBEAT = re.compile(r'LFN hb=\d+ app=(\d+) rt=(\d+) fail=(-?\d+)')
PORT = re.compile(r'COM[1-9][0-9]*', re.ASCII)
SUMMARY = re.compile(r'SR[0-9A-Fa-f]{6}/(?:E[0-9A-Fa-f]{6}/)?[AB]SUM\.LFR')
NO_SPECTRUM = {'status':'NO_SPECTRUM', 'data_source':'LIVE_SNAPSHOT', 'live':None}


def open_port(name):
    try:
        import serial
    except ImportError as error:
        raise OSError('缺少 pyserial 串口运行库') from error
    port = serial.Serial(name, baudrate=115200, timeout=.15, write_timeout=1)
    try:
        port.reset_input_buffer()
    except OSError:
        port.close()
        raise
    return port


def list_ports():
    try:
        from serial.tools.list_ports import comports
    except ImportError as error:
        raise OSError('缺少 pyserial 串口运行库') from error
    return sorted({p.device.upper() for p in comports() if PORT.fullmatch(p.device.upper())},
                  key=lambda name:int(name[3:]))


def encode(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(',', ':')).encode()


def token(value):
    return isinstance(value, str) and re.fullmatch(r'[1-9][0-9]{0,19}', value) and int(value) < 2**64


class BoardReplyError(Exception):
    """The firmware explicitly rejected the command."""


class SerialBoard:
    def __init__(self, port):
        if not isinstance(port, str) or not PORT.fullmatch(port.upper()):
            raise ValueError('串口名称无效')
        self.port = port.upper()
        self.lock = threading.Lock()
        self.wire = None
        self.closed = False
        self.heartbeat = None
        self.observation_seq = 0
        self.markers = {}
        self.last_report = None

    def _disconnect(self):
        if self.wire is not None:
            self.wire.close()
            self.wire = None
        self.heartbeat = None

    def close(self):
        with self.lock:
            self.closed = True
            self._disconnect()

    def _command(self, command, pattern, timeout=2):
        self.wire.write((command + '\n').encode())
        deadline = time.monotonic() + timeout
        lines = []
        pending = bytearray()
        while time.monotonic() < deadline:
            pending.extend(self.wire.read_until(b'\n', 2048))
            if len(pending) > 2048:
                raise OSError('串口应答行过长')
            if not pending.endswith(b'\n'):
                continue
            line = pending.decode(errors='replace').rstrip('\r\n')
            pending.clear()
            hb = HEARTBEAT.match(line)
            if hb:
                self.heartbeat = (time.monotonic(), int(hb[1]), int(hb[2]), int(hb[3]))
            if line.startswith('LFN ERR'):
                raise BoardReplyError(line)
            if line:
                lines.append(line)
            if re.fullmatch(pattern, line):
                return lines
        raise TimeoutError(f'串口未及时回复 {command.split()[0]}，请检查板卡供电和串口')

    def _capture(self):
        st = STATUS.fullmatch(self._command('STATUS', STATUS)[-1])
        sd_lines = '\n'.join(self._command('SDSTAT', r'LFN SDEND files=4'))
        sd = STORAGE.search(sd_lines)
        if not sd or int(st[1]) > 5:
            raise OSError('板端采集或 SD 状态字段不完整')
        storage = dict(available=sd[1]=='1', active=sd[2]=='1', closed=sd[3]=='1',
                       failed=sd[4]=='1', session=sd[5])
        marks = re.search(r'LFN SDMARK queued=(\d+) synced=(\d+) failed=(\d+) pending=(\d+)', sd_lines)
        if marks:
            storage['markers'] = dict(zip(('queued','synced','failed','pending'),map(int,marks.groups())))
        epoch = re.search(r'LFN SDROT rotations=\d+ epoch=(\d+)', sd_lines)
        if epoch:
            storage['epoch'] = int(epoch[1])
        phase = ('idle','starting','running','stopping','stopped','error')[int(st[1])]
        if phase == 'stopped' and storage['active']:
            phase = 'stopping'
        if storage['failed'] or int(st[4]) or phase == 'idle' and storage['active']:
            phase = 'error'
        return dict(phase=phase, session=sd[5], capture_id=st[2], config_id=int(st[3]),
                    failure=int(st[4]), source=st[5] if st[5]!='NONE' else None, channel=int(st[6]),
                    may_be_running=storage['active'] or phase in ('starting','running','stopping'), storage=storage)

    def _report(self, command, capture, *, current=True):
        kind = command.split()[0]
        header = self._command(command, rf'LFN OK {kind} bytes=[0-9]+(?: source=SD)?', timeout=10)[-1]
        length = int(re.search(r'bytes=(\d+)',header)[1])
        if not 1 <= length <= 256 * 1024:
            raise OSError('板端报告长度无效')
        raw = bytearray()
        deadline = time.monotonic() + 2 + length / 9000
        while len(raw) < length and time.monotonic() < deadline:
            raw.extend(self.wire.read(length-len(raw)))
        if len(raw) != length:
            raise OSError('串口报告传输不完整')
        live = command == 'REPORT LIVE'
        try:
            report = json.loads(raw)
            bands = report['live' if live else 'stored']['bands']
            if report['kind'] != 'noise_report' or not isinstance(bands,list) or len(bands) != 10:
                raise ValueError()
        except (KeyError, TypeError, ValueError) as error:
            raise OSError('板端报告格式不正确') from error
        if current and str(report.get('session_id')) != capture['session']:
            raise OSError('报告会话与板卡当前会话不匹配')
        if live:
            if (str(report.get('capture_id')) != capture['capture_id'] or report.get('config_id') != capture['config_id']
                    or report.get('source_mode') != capture['source']):
                raise OSError('频谱与当前采集身份不匹配')
            self.observation_seq += 1
            report.update(data_source='LIVE_SNAPSHOT',host_observation_seq=str(self.observation_seq))
            return encode(report)
        return bytes(raw)

    def _request(self, method, path, data):
        if method == 'GET':
            if path == '/api/capture':
                return 200, self._capture()
            if path == '/api/status':
                c = self._capture()
                hb = self.heartbeat
                if hb and time.monotonic()-hb[0] > 3:
                    hb = None
                return 200, {'have_snapshot':bool(hb), 'capture':{'state':hb[1] if hb else None,
                    'runtime_state':hb[2] if hb else None, 'failure':c['failure'] or (hb[3] if hb else 0),
                    'source_mode':c['source']}, 'storage':c['storage']}
            if path == '/api/spectrum':
                c = self._capture()
                if c['phase'] == 'running':
                    try:
                        return 200, self._report('REPORT LIVE', c)
                    except BoardReplyError as error:
                        if str(error) != 'LFN ERR REPORT':
                            raise
                return 200, NO_SPECTRUM
            if path.startswith('/api/marker/') and path[12:] in self.markers:
                return self.markers[path[12:]]
            if self.last_report and path == '/api/report/'+self.last_report[0]:
                return 200, self.last_report[1]
        if method != 'POST':
            return 404, {'error':'NOT_FOUND'}
        if path.startswith('/api/capture/start/') and token(path.rsplit('/',1)[1]):
            session = path.rsplit('/',1)[1]
            c = self._capture()
            if c['session'] == session:
                return 200, c
            if c['phase'] not in ('idle','stopped') or c['may_be_running'] or c['storage']['failed']:
                return 409, {'error':'板卡已有采集或尚未完成保存'}
            self._command('BEGIN '+session, r'LFN OK BEGIN', timeout=12)
            self._command('STARTADC 1 0 0', r'LFN OK id=\d+')
            return 202, self._capture()
        if path == '/api/capture/stop':
            c = self._capture()
            if c['phase'] not in ('idle','stopping','stopped'):
                self._command('STOP', r'LFN OK id=\d+')
                c = self._capture()
            return 200, c
        if path not in ('/api/marker','/api/report'):
            return 404, {'error':'NOT_FOUND'}
        if not isinstance(data,dict) or not token(data.get('request_id')):
            raise ValueError('请求编号无效')
        req = data['request_id']
        if path == '/api/marker':
            if req in self.markers:
                return self.markers[req]
            strength, note = data.get('strength'), data.get('note','')
            if (type(strength) is not int or not 1 <= strength <= 5 or not isinstance(note,str)
                    or len(note.encode()) > 512 or any(ord(ch)<32 or ord(ch)==127 for ch in note)):
                raise ValueError('感受强度或备注无效；串口备注应为单行')
            if self._capture()['phase'] != 'running':
                return 409, {'error':'板卡未在采集'}
            # UART MARK has no request ID. A lost reply must never trigger another write.
            self.markers[req] = (503, {'error':'串口标记应答未确认，请在停止后的报告核对'})
            try:
                reply = self._command(f'MARK {strength} {note}', r'LFN OK MARK id=\d+ state=QUEUED')[-1]
            except BoardReplyError:
                del self.markers[req]
                raise
            self.markers[req] = (200, {'request_id':req,'state':'QUEUED','marker_id':re.search(r'id=(\d+)',reply)[1]})
            return self.markers[req]
        if self.last_report and req == self.last_report[0]:
            return 200, self.last_report[1]
        first, second = data.get('path',''), data.get('compare_path','')
        if any(not isinstance(p,str) or p and not SUMMARY.fullmatch(p) for p in (first,second)):
            raise ValueError('SD 汇总路径无效')
        c = self._capture()
        if c['may_be_running'] or c['storage']['failed']:
            return 409, {'error':'请先停止并完成 SD 保存'}
        if second and not first:
            epoch = c['storage'].get('epoch')
            if not c['storage']['closed'] or epoch is None:
                raise ValueError('请填写第一份 SD 汇总路径')
            first = f"SR{int(c['session']) & 0xFFFFFF:06X}/"
            if epoch:
                first += f'E{epoch:06X}/'
            first += 'BSUM.LFR' if epoch % 2 else 'ASUM.LFR'
        command = f'COMPARE {first} {second}' if second else ('REPORT '+first).rstrip()
        payload = self._report(command,c,current=not first)
        self.last_report = (req,payload)
        return 200, payload

    def request(self, method, path, data=None):
        with self.lock:
            try:
                if self.closed:
                    raise OSError('串口连接已关闭')
                if self.wire is None:
                    self.wire = open_port(self.port)
                code, value = self._request(method, path, data)
            except BoardReplyError as error:
                code, value = 409, {'error':str(error)}
            except ValueError as error:
                code, value = 400, {'error':str(error)}
            except OSError as error:
                self._disconnect()
                code, value = 503, {'error':f'{self.port}：{error}'}
            return code, value if isinstance(value,bytes) else encode(value)
