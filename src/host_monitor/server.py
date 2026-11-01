"""Local monitor service for Ethernet and USB serial boards."""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timezone
import webbrowser
from http.client import HTTPException
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import ProxyHandler, Request, build_opener
from capture_control import CaptureControl
from discovery import find_boards
from serial_board import SerialBoard, list_ports

ROOT = Path(__file__).resolve().parent
STATIC = {"/": ("index.html", "text/html"), "/app.js": ("app.js", "text/javascript"),
          "/capture_ui.js": ("capture_ui.js", "text/javascript"),
          "/model.js": ("model.js", "text/javascript"), "/style.css": ("style.css", "text/css")}
MAX_RESPONSE = 256 * 1024
MAX_REQUEST = 4096
MAX_EXPORT_REQUEST = 16 * 1024 * 1024
MAX_ARCHIVE_POINTS = 240
SERVICE_VERSION = "1.5.1"


def connection_state(mode, board=None, port=None):
    if mode == 'serial':
        if not isinstance(port, str) or not re.fullmatch(r'COM[1-9][0-9]{0,3}', port, re.I | re.ASCII):
            raise ValueError('请选择有效的 COM 口')
        port = port.upper()
        return {'mode':'serial','board':port,'port':port}
    if mode not in ("board", "offline"):
        raise ValueError("Invalid connection mode")
    if mode == "board":
        if not isinstance(board, str) or not board or any(c.isspace() or ord(c) < 32 or ord(c) == 127 for c in board):
            raise ValueError("Invalid board origin")
        address = urlsplit(board)
        if (address.scheme not in ("http", "https") or not address.hostname
                or address.username is not None or address.password is not None
                or address.path not in ("", "/") or address.query or address.fragment
                or address.netloc.endswith(":") or address.port == 0):
            raise ValueError("Board must be an http(s) origin with a valid port")
        board = board.rstrip("/")
    else:
        board = None
    return {"mode": mode, "board": board}


def read_board(opener, board, path, data=None):
    """Read one fixed-route JSON response, preserving useful board HTTP errors."""
    try:
        try:
            request = Request(board + path, data=json.dumps(data, ensure_ascii=False).encode() if data is not None else None,
                              headers={'Content-Type': 'application/json'} if data is not None else {})
            response = opener.open(request, timeout=2)
        except HTTPError as error:
            response = error
        with response:
            payload = response.read(MAX_RESPONSE + 1)
            code = response.code
        if len(payload) > MAX_RESPONSE:
            return 502, b'{"error":"BOARD_RESPONSE_TOO_LARGE"}'
        json.loads(payload)
        return code, payload
    except (ValueError, UnicodeError):
        return 502, b'{"error":"BOARD_INVALID_RESPONSE"}'
    except (URLError, TimeoutError, OSError, HTTPException):
        return 502, b'{"error":"BOARD_UNAVAILABLE"}'


def check_status(data):
    capture, storage = data.get("capture"), data.get("storage")
    if (not isinstance(data.get("have_snapshot"), bool) or not isinstance(capture, dict)
            or not {"state", "runtime_state", "failure", "source_mode"} <= capture.keys()
            or "storage" not in data or storage is not None and not isinstance(storage, dict)):
        return "fail", "状态接口格式错误或缺少必要字段"
    failure = capture["failure"]
    if (any(capture[key] is not None and type(capture[key]) is not int for key in ("state", "runtime_state", "failure"))
            or capture["source_mode"] not in (None, "TEST", "ADC")):
        return "fail", "采集状态或故障码格式错误"
    if failure:
        return "fail", f"板端采集故障，故障码 {failure}"
    if storage is not None:
        if not all(isinstance(storage.get(key), bool) for key in ("active", "closed", "failed")):
            return "fail", "存储状态格式错误"
        if storage["failed"]:
            return "fail", "板端存储报告故障"
    if not data["have_snapshot"] or any(capture[key] is None for key in ("state", "runtime_state", "failure", "source_mode")) or storage is None:
        return "wait", "板端已响应，状态快照尚未完整就绪"
    return "pass", "板端状态可读，采集与存储未报告故障"


def finite(value):
    return type(value) in (int, float) and abs(value) <= sys.float_info.max


def valid_band(band):
    if not isinstance(band, dict):
        return False
    low, high, dbfs = (band.get(key) for key in ('low_hz', 'high_hz', 'dbfs'))
    return (finite(low) and finite(high) and 0 < low < high and type(band.get('below_floor')) is bool
            and (finite(dbfs) or band['below_floor'] and dbfs is None))


def check_spectrum(data, serial=False):
    if (data.get("status") == "NO_SPECTRUM" and data.get("data_source") == "LIVE_SNAPSHOT"
            and "live" in data and data["live"] is None):
        return "wait", "板端已连接，当前暂无频谱；等待采集快照"
    if (data.get("schema_version") != 1 or data.get("kind") != "noise_report"
            or data.get("data_source") != "LIVE_SNAPSHOT" or data.get("source_mode") not in ("TEST", "ADC")
            or type(data.get("config_id")) is not int or data["config_id"] <= 0):
        return "fail", "频谱接口格式错误或缺少必要字段"
    progress = ('host_observation_seq',) if serial else ('frame_seq', 'sample_raw')
    for field in ('session_id','capture_id',*progress):
        value = data.get(field)
        if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or len(value) > 20 or int(value) > 2**64 - 1:
            return "fail", f"频谱 {field} 不是有效的 64 位十进制字符串"
    live = data.get("live")
    bands = live.get("bands") if isinstance(live, dict) else None
    if not isinstance(bands, list) or len(bands) != 10:
        return "fail", "频谱缺少 10 个频带"
    if not all(valid_band(b) for b in bands):
        return "fail", "频带边界或 dBFS 数值无效"
    return "pass", "已收到串口频谱和主机读取序号" if serial else "10 频带与采样序号格式有效"


def check_capture_control(data):
    control = CaptureControl()
    try:
        phase = control._apply(data)
    except (OSError, TypeError, ValueError):
        return 'fail', '采集控制接口不兼容，请使用匹配固件'
    if phase == 'error':
        return 'fail', '板端采集或保存存在故障'
    return 'pass', '采集控制接口可读'


def selftest(connection, read):
    checks = []

    def add(identity, label, status, detail):
        checks.append({"id": identity, "label": label, "status": status, "detail": detail})

    add("runtime", "本地运行环境", "pass", f"服务 {SERVICE_VERSION} · Python {sys.version.split()[0]}")
    try:
        for name, _ in STATIC.values():
            if not (ROOT / name).read_bytes():
                raise OSError(f"{name} 为空")
        add("resources", "页面资源", "pass", "本地页面、脚本与样式可读取")
    except OSError as error:
        add("resources", "页面资源", "fail", f"页面资源不可读取：{error}")
    board = connection["board"]
    add("connection", "板卡地址", "pass" if board else "wait", board or "尚未配置板卡地址")
    for path, label, validate in (("status", "板端状态", check_status), ("spectrum", "板端频谱", check_spectrum),
                                  ("capture", "采集控制", check_capture_control)):
        if not board:
            state, detail = "wait", "未配置板卡；仅完成本地检查"
        else:
            code, payload = read(connection, "/api/" + path)
            data = json.loads(payload)
            if code == 503:
                state, detail = "wait", "板端服务暂未就绪（HTTP 503）"
            elif code != 200:
                reason = data.get("error") if isinstance(data, dict) else None
                detail = "板卡不可达或响应超时" if reason == "BOARD_UNAVAILABLE" else f"板端响应异常（HTTP {code} / {reason or '未知错误'}）"
                state = "fail"
            elif not isinstance(data, dict):
                state, detail = "fail", "板端响应格式错误：需要 JSON 对象"
            else:
                state, detail = (check_spectrum(data, serial=True) if path == 'spectrum' and connection['mode'] == 'serial'
                                 else validate(data))
        add("board_" + path, label, state, detail)
    overall = "fail" if any(c["status"] == "fail" for c in checks) else "attention" if any(c["status"] == "wait" for c in checks) else "pass"
    return {"overall": overall, "checked_at": datetime.now(timezone.utc).isoformat(), "checks": checks}


def default_reports_dir():
    """Return report folders, most preferred first.

    Path.home() raises RuntimeError when the account profile cannot be resolved
    (service accounts, stripped environment), which used to abort the whole
    service. Every candidate is therefore resolved defensively.
    """
    candidates = []
    override = os.environ.get('AX7021MONITOR_REPORTS')
    if override:
        candidates.append(Path(override))
    try:
        candidates.append(Path.home() / 'AX7021Monitor' / 'Reports')
    except (RuntimeError, OSError):
        pass
    local = os.environ.get('LOCALAPPDATA')
    if local:
        candidates.append(Path(local) / 'AX7021Monitor' / 'Reports')
    candidates.append(Path(tempfile.gettempdir()) / 'AX7021Monitor' / 'Reports')
    unique = []
    for candidate in candidates:
        if candidate not in unique:
            unique.append(candidate)
    return unique


def save_report(directory, data):
    kind, session, content = (data.get(key) for key in ('kind', 'session', 'content'))
    if kind not in ('json', 'csv', 'html') or not isinstance(content, str) or not content:
        raise ValueError('不支持的导出格式或内容为空')
    if not isinstance(session, str):
        raise ValueError('报告缺少会话标识')
    prefix = 'noise-report-' + re.sub(r'[^a-zA-Z0-9_-]', '_', session[:40]) + '-'
    payload = content.encode('utf-8')
    directory.mkdir(parents=True, exist_ok=True)
    # Each click gets its own filename. Publish only after the entire write closes.
    descriptor, name = tempfile.mkstemp(dir=directory, prefix=prefix, suffix='.tmp')
    partial = Path(name)
    try:
        with os.fdopen(descriptor, 'wb') as output:
            output.write(payload)
        result = partial.with_suffix('.' + kind)
        partial.rename(result)
    finally:
        partial.unlink(missing_ok=True)
    return {'path': str(result), 'dir': str(directory), 'bytes': len(payload)}


def reveal_folder(directory):
    """Open the report folder in the desktop file manager."""
    if not directory:
        raise OSError('还没有可用的保存目录')
    path = Path(directory)
    path.mkdir(parents=True, exist_ok=True)
    if sys.platform == 'win32':
        os.startfile(str(path))
    elif sys.platform == 'darwin':
        subprocess.Popen(['open', str(path)])
    else:
        subprocess.Popen(['xdg-open', str(path)])
    return {'dir': str(path), 'opened': True}


class ReportStore:
    """Write exports into the first folder of a fallback list that accepts writes."""

    def __init__(self, candidates):
        if isinstance(candidates, (str, Path)):
            candidates = [candidates]
        self.candidates = [Path(candidate) for candidate in candidates]
        self.lock = threading.Lock()
        self.index = 0
        self.directory = self._pick()

    def _pick(self):
        for offset in range(len(self.candidates)):
            index = (self.index + offset) % len(self.candidates)
            candidate = self.candidates[index]
            try:
                candidate.mkdir(parents=True, exist_ok=True)
                with tempfile.NamedTemporaryFile(dir=candidate, prefix='.ax7021-probe-'):
                    pass
            except OSError:
                continue
            self.index = index
            return candidate
        return None

    def save(self, data):
        errors = []
        with self.lock:
            for offset in range(len(self.candidates)):
                index = (self.index + offset) % len(self.candidates)
                directory = self.candidates[index]
                try:
                    result = save_report(directory, data)
                except OSError as error:
                    errors.append(f'{directory}（{error}）')
                    continue
                self.index = index
                self.directory = directory
                return result
        raise OSError('；'.join(errors) or '没有可写的保存目录')


class Archive:
    """Keep the newest live snapshot and recent trend points on this computer.

    The board only publishes a live snapshot while a capture runs. Archiving it
    here means pressing "stop and save", refreshing the page, or losing the
    network never discards the readings that were already received.
    """

    def __init__(self, directory):
        self.directory = Path(directory) if directory else None
        self.lock = threading.Lock()
        self.identity = None
        self.token = None
        self.report = None
        self.report_text = None
        self.received_at = None
        self.bands = None
        self.points = []
        self.last_flush = 0.0
        self._load()

    @staticmethod
    def _atomic_json(path, payload):
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, name = tempfile.mkstemp(dir=path.parent, prefix=path.name + '.', suffix='.tmp')
        partial = Path(name)
        try:
            with os.fdopen(descriptor, 'w', encoding='utf-8') as output:
                json.dump(payload, output, ensure_ascii=False, separators=(',', ':'))
            os.replace(partial, path)
        finally:
            partial.unlink(missing_ok=True)

    @staticmethod
    def _frame(report):
        if not isinstance(report, dict) or report.get('kind') != 'noise_report' or report.get('schema_version') != 1:
            return None
        section = report.get('live')
        bands = section.get('bands') if isinstance(section, dict) else None
        ids = [str(report.get(key, '')) for key in ('session_id', 'capture_id', 'config_id')]
        if not all(i.isascii() and i.isdecimal() for i in ids) or not isinstance(bands, list) or len(bands) != 10:
            return None
        if not all(valid_band(b) for b in bands):
            return None
        valid_ns = section.get('valid_ns', report.get('valid_ns'))
        adc = report.get('source_mode') == 'ADC' and not report.get('simulation') and valid_ns not in (0, '0')
        values = [b['estimated_spl_db'] if adc and not b['below_floor'] and finite(b.get('estimated_spl_db'))
                  else None for b in bands]
        progress = (report.get('frame_seq'), report.get('sample_raw'), section.get('frame_seq'), section.get('sample_raw'),
                    report.get('host_observation_seq'))
        token = str(next((v for v in progress if v is not None), ''))
        if not (token.isascii() and token.isdecimal() and len(token) <= 20):
            token = None
        return '/'.join(ids), token, bands, values

    def _load(self):
        if not self.directory:
            return
        try:
            data = json.loads((self.directory / 'latest.json').read_text(encoding='utf-8'))
            if not isinstance(data, dict):
                return
            text = data.get('report_text')
            # v1.4.0 stored a parsed Python object; serializing here preserves its uint64 integers.
            if text is None:
                text = json.dumps(data.get('report'), ensure_ascii=False)
            report = json.loads(text)
            frame = self._frame(report)
            if frame is None:
                return
            self.identity, self.token, self.bands, values = frame
            self.report, self.report_text = report, text
            self.received_at = data.get('received_at')
            history = data.get('history')
            # Older histories stored dBFS without a unit. The raw report remains usable.
            if (not self.token or not isinstance(history, dict) or history.get('unit') != 'spl_db'
                    or history.get('identity') != self.identity):
                return
            points = history.get('points')
            if not isinstance(points, list) or not points:
                return
            points = points[-MAX_ARCHIVE_POINTS:]
            previous = -1
            for p in points:
                if not isinstance(p, dict) or type(p.get('t')) is not int or p['t'] <= 0:
                    return
                token = str(p.get('token', ''))
                if not (token.isascii() and token.isdecimal() and len(token) <= 20) or int(token) <= previous:
                    return
                previous = int(token)
                vs = p.get('v')
                if not isinstance(vs, list) or len(vs) != 10 or not all(v is None or finite(v) for v in vs):
                    return
            if points[-1]['token'] == self.token and points[-1]['v'] == values:
                self.points = points
        except (OSError, ValueError, TypeError):
            pass

    def record(self, report, raw=None):
        frame = self._frame(report)
        if frame is None:
            return
        identity, token, bands, values = frame
        text = raw if raw is not None else json.dumps(report, ensure_ascii=False)
        with self.lock:
            if text == self.report_text:
                return
            new_identity = identity != self.identity
            if new_identity or token is None or self.token is not None and int(token) < int(self.token):
                self.points = []
            self.received_at = int(time.time() * 1000)
            if token is not None:
                if self.points and token == self.token:
                    self.points[-1] = dict(self.points[-1], v=values)
                else:
                    self.points.append({'t': self.received_at, 'token': token, 'v': values})
                    del self.points[:-MAX_ARCHIVE_POINTS]
            self.identity, self.token, self.bands = identity, token, bands
            self.report, self.report_text = report, text
            # Snapshots arrive about every two seconds; flushing every few
            # seconds keeps the file current without writing nonstop.
            if new_identity or time.monotonic() - self.last_flush >= 5:
                self._flush_locked()

    def _flush_locked(self):
        if not self.directory or self.report is None:
            return
        try:
            self._atomic_json(self.directory / 'latest.json',
                              {'received_at': self.received_at, 'report_text': self.report_text,
                               'history': {'identity': self.identity, 'unit': 'spl_db', 'points': self.points}})
            self.last_flush = time.monotonic()
        except OSError:
            pass

    def flush(self):
        with self.lock:
            self._flush_locked()

    def state(self):
        with self.lock:
            return {'report': self.report, 'report_text': self.report_text,
                    'received_at': self.received_at,
                    'history': {'identity': self.identity, 'unit': 'spl_db', 'bands': self.bands, 'points': list(self.points)},
                    'stored': self.directory is not None}


def make_server(host, port, board, capture_control=None, *, preference_file=None, export_dir=None):
    store = ReportStore(export_dir or default_reports_dir())
    archive = Archive(store.directory.parent / 'Sessions' if store.directory else None)
    connection = connection_state("board" if board else "offline", board)
    opener = build_opener(ProxyHandler({}))
    control = capture_control or CaptureControl(log_path=Path(tempfile.gettempdir())/'AX7021Monitor-capture.log')
    control.configure(connection['board'])
    known = {}
    if preference_file:
        try:
            saved = json.loads(preference_file.read_text(encoding='utf-8'))
            known = {'board':connection_state('board', saved['board'])['board'], 'mac':saved.get('mac')}
        except (OSError, KeyError, TypeError, ValueError):
            pass
    discovered_macs = {}
    selection_lock = threading.Lock()
    serial_client = None

    def read_selected(config, path, data=None):
        if config['mode'] != 'serial':
            return read_board(opener, config['board'], path, data)
        with selection_lock:
            device = serial_client if config is connection else None
        if device is None:
            return 409, b'{"error":"CONNECTION_CHANGED"}'
        try:
            return device.request('POST' if data is not None else 'GET', path, data)
        except OSError as error:
            return 502, json.dumps({'error':str(error)}, ensure_ascii=False).encode()

    def switch(next_connection):
        nonlocal connection, serial_client
        with selection_lock:
            if connection == next_connection:
                return
            if not control.can_close():
                raise ValueError('采集或保存尚未结束，请先在当前板卡停止并保存')
            port = next_connection.get('port')
            device = SerialBoard(port) if port else None
            try:
                verified = None
                if device is not None:
                    code, payload = device.request('GET','/api/capture')
                    if code != 200:
                        raise OSError(json.loads(payload).get('error','串口板卡无应答'))
                    probe = CaptureControl()
                    probe._apply(json.loads(payload))
                    verified = probe.snapshot()
                control.configure(next_connection['board'], serial_client=device, verified=verified)
            except Exception:
                if device is not None:
                    device.close()
                raise
            previous = serial_client
            serial_client, connection = device, next_connection
            if previous is not None:
                previous.close()

    def handshake(origin):
        code, raw = read_board(opener, origin, '/api/capture')
        if code != 200:
            return False
        try:
            CaptureControl()._apply(json.loads(raw))
            code, raw = read_board(opener, origin, '/api/status')
            data = json.loads(raw)
            return (code == 200 and isinstance(data, dict) and isinstance(data.get('have_snapshot'), bool)
                    and isinstance(data.get('capture'), dict)
                    and {'state','runtime_state','failure','source_mode'} <= data['capture'].keys()
                    and 'storage' in data)
        except (AttributeError, ValueError, TypeError, OSError):
            return False

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            if sys.stderr is not None:
                super().log_message(format, *args)

        def respond(self, code, data, content_type="application/json"):
            self.send_response(code)
            self.send_header("Content-Type", content_type + "; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def error(self, code, reason):
            self.respond(code, json.dumps({"error": reason}).encode())

        def do_POST(self):
            nonlocal connection, known
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length < 1 or length > (MAX_EXPORT_REQUEST if self.path == '/api/export' else MAX_REQUEST):
                    self.error(413, "INVALID_BODY_SIZE")
                    return
                # Consume the bounded body before rejecting: closing with unread POST
                # bytes can reset Windows sockets and hide the intended HTTP error.
                raw = self.rfile.read(length)
                if self.path not in ("/api/discovery", "/api/connection", "/api/capture", "/api/marker", "/api/report", "/api/export", "/api/reveal"):
                    self.error(404, "NOT_FOUND")
                    return
                local = f"127.0.0.1:{self.server.server_port}"
                if self.headers.get('Host') != local or self.headers.get('Origin') not in (None, 'http://' + local):
                    self.error(403, "只允许本软件窗口发起操作")
                    return
                if self.headers.get_content_type() != "application/json":
                    self.error(415, "JSON_REQUIRED")
                    return
                data = json.loads(raw)
                if not isinstance(data, dict):
                    raise ValueError("Expected object")
                if self.path == '/api/export':
                    try:
                        result = store.save(data)
                    except ValueError as error:
                        self.error(400, str(error))
                        return
                    except OSError as error:
                        self.error(500, f'无法保存报告（{error}）')
                        return
                    self.respond(201, json.dumps(result, ensure_ascii=False).encode())
                    return
                if self.path == '/api/reveal':
                    try:
                        self.respond(200, json.dumps(reveal_folder(store.directory), ensure_ascii=False).encode())
                    except OSError as error:
                        self.error(500, f'无法打开保存文件夹（{error}）')
                    return
                if self.path == '/api/discovery':
                    if connection['mode'] == 'serial':
                        self.respond(200, b'{"state":"waiting","boards":[],"board":null}')
                        return
                    result = find_boards(handshake, known, connection['board'], not control.can_close())
                    if result.get('mac'):
                        discovered_macs[result['board']] = result['mac']
                    self.respond(200, json.dumps(result).encode())
                    return
                if self.path in ('/api/marker', '/api/report'):
                    config = connection
                    if not config['board']:
                        self.error(503, 'BOARD_NOT_CONFIGURED')
                        return
                    code, payload = read_selected(config, self.path, data)
                    self.respond(code, payload)
                    return
                if self.path == "/api/capture":
                    with selection_lock:
                        state = control.submit(data.get('action'))
                    self.respond(202, json.dumps(state, ensure_ascii=False).encode())
                    return
                next_connection = connection_state(data.get("mode"), data.get("board"), data.get("port"))
            except (ValueError, TypeError) as error:
                self.error(400, str(error) if self.path in ('/api/capture', '/api/export') else "INVALID_CONNECTION")
                return
            try:
                switch(next_connection)
            except OSError as error:
                self.error(502, str(error))
                return
            except ValueError as error:
                self.error(409, str(error))
                return
            if next_connection['mode'] == 'board':
                origin = next_connection['board']
                mac = discovered_macs.get(origin) or (known.get('mac') if known.get('board') == origin else None)
                known = {'board':origin, 'mac':mac}
                if preference_file:
                    try:
                        preference_file.parent.mkdir(parents=True, exist_ok=True)
                        preference_file.write_text(json.dumps(known), encoding='utf-8')
                    except OSError:
                        pass
            self.respond(200, json.dumps(next_connection).encode())

        def do_GET(self):
            if self.path in STATIC:
                name, content_type = STATIC[self.path]
                self.respond(200, (ROOT / name).read_bytes(), content_type)
            elif self.path == "/api/connection":
                self.respond(200, json.dumps(connection).encode())
            elif self.path == '/api/ports':
                try:
                    self.respond(200, json.dumps({'ports':list_ports()}).encode())
                except OSError as error:
                    self.error(503, str(error))
            elif self.path == "/api/selftest":
                self.respond(200, json.dumps(selftest(connection, read_selected), ensure_ascii=False).encode())
            elif self.path == "/api/capture":
                self.respond(200, json.dumps(control.refresh(), ensure_ascii=False).encode())
            elif self.path == "/api/archive":
                self.respond(200, json.dumps(archive.state(), ensure_ascii=False).encode())
            elif self.path in ("/api/status", "/api/spectrum") or (
                    re.fullmatch(r'/api/(marker|report)/[1-9][0-9]{0,19}', self.path)
                    and int(self.path.rsplit('/', 1)[1]) <= 2**64 - 1):
                config = connection
                if config["mode"] == "offline":
                    self.error(503, "BOARD_NOT_CONFIGURED")
                    return
                code, payload = read_selected(config, self.path)
                if self.path == "/api/spectrum" and code == 200:
                    try:
                        snapshot = json.loads(payload)
                    except ValueError:
                        snapshot = None
                    if (isinstance(snapshot, dict) and snapshot.get("schema_version") == 1
                            and snapshot.get("kind") == "noise_report"
                            and snapshot.get("data_source") == "LIVE_SNAPSHOT"):
                        archive.record(snapshot, payload.decode(json.detect_encoding(payload)))
                self.respond(code, payload)
            else:
                self.error(404, "NOT_FOUND")

    class MonitorServer(ThreadingHTTPServer):
        def server_close(self):
            super().server_close()
            with selection_lock:
                if serial_client is not None:
                    serial_client.close()

    service = MonitorServer((host, port), Handler)
    service.capture_control = control
    service.store = store
    service.archive = archive
    return service


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", help="Board origin, e.g. http://192.168.1.10; omit for offline imports")
    parser.add_argument("--open", action="store_true", help="Open the local monitor in the default browser")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--export-dir", help="Folder for exported reports and the local session archive")
    args = parser.parse_args()
    try:
        server = make_server("127.0.0.1", args.port, args.board,
                             export_dir=Path(args.export_dir) if args.export_dir else None)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    url = f"http://127.0.0.1:{server.server_port}"
    print(f"Local monitor: {url} | source: {args.board or 'offline only'} | reports: {server.store.directory}",
          flush=True)
    if args.open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.archive.flush()
        server.server_close()
