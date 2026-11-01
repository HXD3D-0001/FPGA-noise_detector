"""ADC operations over the selected board connection."""
import json
import secrets
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import ProxyHandler, Request, build_opener


def board_origin(value):
    if not isinstance(value,str) or not value or any(c.isspace() or ord(c)<32 for c in value):
        raise ValueError('请先连接有效的板卡地址')
    try:
        parsed=urlsplit(value)
        if (parsed.scheme not in ('http','https') or not parsed.hostname or parsed.username is not None
                or parsed.password is not None or parsed.path not in ('','/') or parsed.query or parsed.fragment
                or parsed.netloc.endswith(':') or parsed.port==0):raise ValueError()
    except ValueError:raise ValueError('板卡地址应为 http(s)://地址[:端口]') from None
    return value.rstrip('/')


class BoardClient:
    def __init__(self, board):
        self.board=board_origin(board);self.opener=build_opener(ProxyHandler({}))

    def request(self, method, path):
        request=Request(self.board+path,data=b'' if method=='POST' else None,method=method)
        try:
            try:response=self.opener.open(request,timeout=2)
            except HTTPError as error:response=error
            with response:
                payload=response.read(256*1024+1);code=response.code
            if len(payload)>256*1024:raise ValueError('板端响应过长')
            data=json.loads(payload)
            if not isinstance(data,dict):raise ValueError('板端响应应为JSON对象')
            return code,data
        except (ValueError,UnicodeError) as error:
            raise OSError('板端响应格式错误，请加载匹配固件：'+str(error)) from error


class CaptureControl:
    def __init__(self, client_factory=BoardClient, poll_interval=.3, operation_timeout=35, log_path=None):
        self.client_factory=client_factory;self.poll_interval=poll_interval;self.operation_timeout=operation_timeout
        self.log_path=Path(log_path) if log_path else None
        self.lock=threading.Lock();self.poll_lock=threading.Lock();self.worker=None
        self.serial_client=None
        self.state={'phase':'idle','board':None,'message':'连接板卡后可开始采集',
                    'may_be_running':False,'session':None,'config':None,'confirmed':False}

    def snapshot(self):
        with self.lock:return dict(self.state)

    def update(self,**values):
        with self.lock:self.state.update(values)

    def _busy(self):return self.worker is not None and self.worker.is_alive()

    def configure(self, board, serial_client=None, verified=None):
        if serial_client is None:
            board=board_origin(board) if board else None
        with self.lock:
            if board==self.state['board']:return
            if self._busy() or self.state['may_be_running']:
                raise ValueError('采集或保存尚未确认结束，请先在当前板卡停止并保存，再切换地址')
        # Finish any in-flight status read before swapping the selected peer.
        with self.poll_lock:
            with self.lock:
                if self._busy() or self.state['may_be_running']:
                    raise ValueError('采集或保存尚未确认结束，请先在当前板卡停止并保存，再切换地址')
                self.serial_client=serial_client
                self.state.update(phase='idle',session=None,config=None,confirmed=False,
                                  message='正在确认板端采集状态' if board else '连接板卡后可开始采集')
                if verified is not None:
                    self.state.update(verified)
                self.state['board']=board

    def wait(self,timeout):
        if self.worker:self.worker.join(timeout)
        return not self._busy()

    def can_close(self):
        with self.lock:return not self._busy() and not self.state['may_be_running']

    def request_close(self):
        if self.can_close():return True
        self.update(message='请先点击“停止并保存”，确认完成后再关闭软件');return False

    def submit(self,action,board=None):
        if action not in ('start','stop'):raise ValueError('不支持的采集操作')
        if board is not None:self.configure(board)
        with self.lock:
            target=self.state['board']
            if not target:raise ValueError('请先填写板卡地址并连接')
            if self._busy():raise ValueError('上一项采集操作尚未完成，请稍候')
            if action=='start' and self.state['may_be_running']:raise ValueError('采集可能仍在运行，请先停止并保存')
            self.state.update(phase='starting' if action=='start' else 'stopping',
                              message='正在准备 ADC 采集…' if action=='start' else '正在停止并等待 SD 文件关闭…')
            self.worker=threading.Thread(target=self._operate,args=(action,target),daemon=True);self.worker.start()
        return self.snapshot()

    def _apply(self,data):
        phase=data.get('phase');storage=data.get('storage')
        required=('session','source','channel','config_id','capture_id','failure','may_be_running')
        if (phase not in ('idle','starting','running','stopping','stopped','error') or not isinstance(storage,dict)
                or any(k not in data for k in required) or any(type(storage.get(k)) is not bool for k in ('available','active','closed','failed'))
                or type(data['may_be_running']) is not bool):raise OSError('板端采集状态字段不完整，请加载本次固件')
        active=data['may_be_running'] or phase in ('starting','running','stopping')
        if active:self.update(may_be_running=True)
        if phase=='running' and (data['source']!='ADC' or data['channel']!=0 or not data['config_id']
                or str(data['capture_id'])=='0' or str(data['session'])=='0' or not storage['available'] or not storage['active']):
            raise OSError('板端源或采集身份不匹配，未确认真实 ADC 左声道运行')
        if phase=='stopped' and (not storage['available'] or storage['active'] or not storage['closed'] or storage['failed']):
            raise OSError('板端尚未确认 SD 文件完整关闭')
        if storage['failed'] or data['failure'] or phase=='error':
            self.update(phase='error',confirmed=True,session=str(data['session']),config=data['config_id'],
                        message='板端采集或存储故障，保存结果未确认；可重试停止并保存')
            return 'error'
        messages={'idle':'板卡已就绪，可以开始采集','starting':'正在准备 ADC 采集，等待板端运行确认…',
                  'running':'ADC 左声道正在采集',
                  'stopping':'正在停止并等待 SD 文件关闭…','stopped':'采集已停止，SD 文件已关闭，可以断电或关闭软件'}
        self.update(phase=phase,may_be_running=active,confirmed=True,session=str(data['session']),config=data['config_id'],message=messages[phase])
        return phase

    @staticmethod
    def _request(client,method,path):
        code,data=client.request(method,path)
        return code,json.loads(data) if isinstance(data,bytes) else data

    def _read(self,client):
        code,data=self._request(client,'GET','/api/capture')
        if code!=200:raise OSError(f'板端采集状态暂不可用（HTTP {code}）')
        previous=self.snapshot()
        if (client is self.serial_client and previous['may_be_running'] and previous['session'] not in (None,'0')
                and str(data.get('session')) != previous['session']):
            raise OSError('串口板卡的会话已改变，请接回原板确认保存状态')
        return self._apply(data)

    def refresh(self):
        state=self.snapshot()
        if not state['board'] or self._busy() or not self.poll_lock.acquire(blocking=False):return state
        try:
            board=self.snapshot()['board']
            if board:self._read(self.serial_client or self.client_factory(board))
        except Exception as error:self.update(phase='error',confirmed=False,message='连接或状态未确认：'+str(error)+'；请保持板卡供电，可重连后停止并保存')
        finally:self.poll_lock.release()
        return self.snapshot()

    def _operate(self,action,board):
        try:
            with self.poll_lock:
                client=self.serial_client or self.client_factory(board)
                if action=='start':
                    phase=self._read(client)
                    if phase in ('starting','running','stopping'):return
                    if phase=='error':raise OSError('板卡仍有故障，请先停止并保存，排查后重新加载固件')
                    token=str(secrets.randbits(63) or 1);path='/api/capture/start/'+token
                else:
                    if client is self.serial_client:
                        self._read(client)
                    path='/api/capture/stop'
                if action=='start' and client is self.serial_client:
                    self.update(session=token)
                self.update(may_be_running=True)
                code,data=self._request(client,'POST',path)
                if code not in (200,202):raise OSError(f'板卡未接受采集操作（HTTP {code}）')
                phase=self._apply(data)
                deadline=time.monotonic()+self.operation_timeout
                while True:
                    if phase=='error':return
                    if action=='start' and phase=='running':
                        if self.snapshot()['session']!=token:raise OSError('开始请求与板端会话不匹配')
                        return
                    if action=='stop' and phase in ('idle','stopped'):return
                    if time.monotonic()>=deadline:raise TimeoutError('等待板端确认超时；操作可能仍在继续，请保持供电并重试停止')
                    time.sleep(self.poll_interval);phase=self._read(client)
        except Exception as error:self.update(phase='error',confirmed=False,message=str(error) or type(error).__name__)
        finally:
            if self.log_path:
                try:
                    with self.log_path.open('a',encoding='utf-8') as f:f.write(json.dumps({**self.snapshot(),'action':action,'at':datetime.now(timezone.utc).isoformat()},ensure_ascii=False)+'\n')
                except OSError:pass
