"""Host the monitor for its desktop window or the optional system browser."""
import ctypes
import json
import os
from pathlib import Path
import re
import tempfile
import sys
import threading
import time
import traceback
import webbrowser
from urllib.error import URLError
from urllib.request import ProxyHandler, build_opener

from server import make_server


def run_window_service():
    """Serve the packaged window until its owning process closes the pipe."""
    service = make_server('127.0.0.1', 0, None, preference_file=config_dir()/'connection.json')
    worker = threading.Thread(target=service.serve_forever, daemon=True)
    worker.start()
    try:
        print(json.dumps({'url': f'http://127.0.0.1:{service.server_port}'}), flush=True)
        for line in sys.stdin:
            command = line.strip()
            try:
                value = json.loads(command)
            except ValueError:
                value = command
            command = value.get('command') if isinstance(value, dict) else value
            if command == 'quit':
                break
            if command == 'close':
                allowed = service.capture_control.request_close()
                state = service.capture_control.snapshot()
                print(json.dumps({'can_close': allowed, 'phase': state['phase'],
                                  'may_be_running': state['may_be_running']}), flush=True)
    finally:
        service.shutdown()
        service.server_close()
        worker.join()
        service.archive.flush()


def config_dir():
    """Return a writable folder for preferences and the single-instance marker."""
    candidates = []
    local = os.environ.get('LOCALAPPDATA')
    if local:
        candidates.append(Path(local))
    try:
        candidates.append(Path.home())
    except (RuntimeError, OSError):
        pass
    candidates.append(Path(tempfile.gettempdir()))
    for base in candidates:
        try:
            folder = base / 'AX7021Monitor'
            folder.mkdir(parents=True, exist_ok=True)
            return folder
        except OSError:
            continue
    return Path(tempfile.gettempdir()) / 'AX7021Monitor'


def message_box(message, flags):
    return ctypes.windll.user32.MessageBoxW(None, message, '住宅低频噪声监测', flags)


def open_page(url):
    try:
        return webbrowser.open(url, new=2)
    except (OSError, webbrowser.Error):
        return False


def running_url(marker):
    """Return the address of a live copy, or None when the marker is stale."""
    try:
        data = json.loads(marker.read_text(encoding='utf-8'))
        url = data.get('url') if isinstance(data, dict) else None
        if not isinstance(url, str) or not re.fullmatch(r'http://127\.0\.0\.1:[1-9]\d{0,4}', url):
            return None
        opener = build_opener(ProxyHandler({}))
        with opener.open(url + '/api/connection', timeout=1.5) as response:
            config = json.loads(response.read(4096))
            return url if isinstance(config, dict) and config.get('mode') in ('offline', 'board', 'serial') else None
    except (OSError, ValueError, TypeError, URLError):
        return None


def claim_instance(marker):
    """Return 'owned' when this copy may start, 'running' when another copy serves the page."""
    try:
        descriptor = os.open(marker, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        url = running_url(marker)
        if not url:
            # 另一个实例可能刚创建标记、服务尚未开始监听，稍等再探测一次。
            time.sleep(1.2)
            url = running_url(marker)
        if url:
            open_page(url)
            message_box('监测服务已经在运行，已为您重新打开页面。\n\n' + url
                        + '\n\n如需退出，请在原来的“住宅低频噪声监测”窗口选择“是”。', 0x40)
            return 'running'
        try:
            marker.unlink()
        except OSError:
            return 'unavailable'
        try:
            descriptor = os.open(marker, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except OSError:
            return 'unavailable'
    except OSError:
        return 'unavailable'
    os.close(descriptor)
    return 'owned'


def run_browser():
    folder = config_dir()
    marker = folder / 'instance.json'
    if claim_instance(marker) == 'running':
        return
    service = make_server('127.0.0.1', 0, None, preference_file=folder / 'connection.json')
    worker = threading.Thread(target=service.serve_forever, daemon=True)
    worker.start()
    url = f'http://127.0.0.1:{service.server_port}'
    try:
        marker.write_text(json.dumps({'pid': os.getpid(), 'url': url, 'started': time.time()}),
                          encoding='utf-8')
    except OSError:
        pass
    reports = service.store.directory or '（暂不可用）'
    try:
        opened = open_page(url)
        while True:
            prompt = ('监测页已在默认浏览器打开。' if opened else '请将下方地址复制到浏览器。')
            answer = message_box(f'{prompt}\n\n{url}\n\n报告与留档位置：{reports}'
                                 '\n\n请保留本窗口。需要重新打开监测页时选择“否”。'
                                 '\n选择“是”退出本地服务。退出前请先停止并保存。\n\n现在退出吗？',
                                 0x124)  # MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2
            if answer != 6:  # IDYES
                opened = open_page(url)
                continue
            if service.capture_control.request_close():
                return
            state = service.capture_control.snapshot()
            if state['phase'] == 'error' and state['may_be_running']:
                answer = message_box('板卡采集或 SD 保存尚未确认结束。请保持板卡供电。'
                                     '\n选择“否”返回监测页，恢复连接后重试停止并保存。'
                                     '\n\n仍要退出本地服务吗？', 0x134)
                if answer == 6:
                    return
            else:
                message_box('采集或保存正在进行，本地服务将继续运行。请在监测页点击“停止并保存”。', 0x40)
    finally:
        try:
            marker_data = json.loads(marker.read_text(encoding='utf-8'))
            if isinstance(marker_data, dict) and marker_data.get('pid') == os.getpid():
                marker.unlink(missing_ok=True)
        except (OSError, ValueError, TypeError):
            pass
        try:
            service.archive.flush()
        except (AttributeError, OSError):
            pass
        service.shutdown()
        service.server_close()
        worker.join()


def main():
    try:
        run_browser()
    except Exception:
        log = Path(tempfile.gettempdir())/'AX7021Monitor-error.log'
        try:
            log.write_text(traceback.format_exc(), encoding='utf-8')
            detail = f'错误详情：{log}'
        except OSError:
            detail = traceback.format_exc()
        message_box(f'本地监测服务未能启动。请确认已完整解压软件。\n{detail}', 0x10)
        return 1
    return 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--window-service']:
        sys.stdin.reconfigure(encoding='utf-8')
        sys.stdout.reconfigure(encoding='utf-8')
        run_window_service()
        raise SystemExit(0)
    raise SystemExit(main())
