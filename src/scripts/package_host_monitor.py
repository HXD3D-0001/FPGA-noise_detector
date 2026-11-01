"""Build the standalone Windows x64 monitor from pinned Python and Electron releases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_NAME = 'python-3.13.16-embed-amd64.zip'
RUNTIME_URL = 'https://www.python.org/ftp/python/3.13.16/' + RUNTIME_NAME
RUNTIME_SHA256 = '97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297'
WINDOW_NAME = 'electron-v45.2.1-win32-x64.zip'
WINDOW_SHA256 = '7c342aee45d5b4ccafe5dfe9037028796b082735a576e21e2b4e2d8422ebb727'
WINDOW_URL = 'https://github.com/electron/electron/releases/download/v45.2.1/' + WINDOW_NAME
SERIAL_WHEEL = 'pyserial-3.5-py2.py3-none-any.whl'
SERIAL_SHA256 = 'c4451db6ba391ca6ca299fb3ec7bae67a5c55dde170964c7a14ceefec02f2cf0'
APP_FILES = ('desktop.py', 'server.py', 'discovery.py', 'capture_control.py',
             'serial_board.py', 'session_recording.py', 'session_export.py',
             'capture_ui.js', 'index.html', 'app.js', 'model.js', 'style.css')


def sha256(content):
    return hashlib.sha256(content).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=ROOT/'build'/RUNTIME_NAME)
    parser.add_argument('--window-runtime', type=Path, default=ROOT/'build'/WINDOW_NAME)
    parser.add_argument('--serial-wheel', type=Path, default=ROOT/'build'/SERIAL_WHEEL)
    parser.add_argument('--output', type=Path, default=ROOT/'build/host_monitor')
    args = parser.parse_args()
    if not args.runtime.is_file():
        parser.error(f'Download {RUNTIME_URL} to {args.runtime} or specify --runtime.')
    if sha256(args.runtime.read_bytes()) != RUNTIME_SHA256:
        parser.error('Unexpected SHA-256 for the official embedded Python archive.')
    if not args.window_runtime.is_file():
        parser.error(f'Download {WINDOW_URL} to {args.window_runtime} or specify --window-runtime.')
    with args.window_runtime.open('rb') as binary:
        if hashlib.file_digest(binary, 'sha256').hexdigest() != WINDOW_SHA256:
            parser.error('Unexpected SHA-256 for the Electron archive.')
    output = args.output.resolve()
    if not args.serial_wheel.is_file() or sha256(args.serial_wheel.read_bytes()) != SERIAL_SHA256:
        parser.error(f'Expected the official {SERIAL_WHEEL} wheel in FPGA/build or via --serial-wheel.')
    output.mkdir(parents=True, exist_ok=True)
    monitor = ROOT/'host_monitor'
    window_files = ('window/main.cjs', 'window/package.json')
    license_name = 'licenses/pyserial-license.txt'
    source = {name: (monitor/name).read_bytes() for name in (*APP_FILES, 'README.md', 'start_monitor.cmd', license_name, *window_files)}
    archive = output/'AX7021Monitor_Windows_x64.zip'
    staged = archive.with_suffix('.zip.tmp')
    prefix = 'AX7021Monitor/'
    try:
        with zipfile.ZipFile(staged, 'w', zipfile.ZIP_DEFLATED, compresslevel=5) as bundle, zipfile.ZipFile(args.runtime) as runtime, zipfile.ZipFile(args.window_runtime) as window:
            for item in window.infolist():
                if item.is_dir() or item.filename == 'resources/default_app.asar':
                    continue
                name = 'AX7021Monitor.exe' if item.filename == 'electron.exe' else item.filename
                bundle.writestr(prefix + name, window.read(item))
            for name in window_files:
                bundle.writestr(prefix + 'resources/app/' + Path(name).name, source[name])
            for name in runtime.namelist():
                content = runtime.read(name)
                if name == 'python313.zip':
                    # Some distribution workflows omit nested .zip entries.
                    name = 'python313.stdlib'
                elif name == 'python313._pth':
                    content = content.replace(b'python313.zip', b'python313.stdlib')
                bundle.writestr(prefix + 'runtime/' + name, content)
            for name in APP_FILES:
                bundle.writestr(prefix + 'runtime/' + name, source[name])
            with zipfile.ZipFile(args.serial_wheel) as serial_wheel:
                for name in serial_wheel.namelist():
                    if not name.endswith('/'):
                        bundle.writestr(prefix + 'runtime/' + name, serial_wheel.read(name))
            bundle.writestr(prefix + license_name, source[license_name])
            bundle.writestr(prefix + 'Start.cmd', source['start_monitor.cmd'])
            bundle.writestr(prefix + 'README.md', source['README.md'])
        with zipfile.ZipFile(staged) as bundle:
            bad_file = bundle.testzip()
            if bad_file:
                raise RuntimeError('Corrupt archive member: ' + bad_file)
            if os.name == 'nt':
                with tempfile.TemporaryDirectory(dir=output) as check:
                    bundle.extractall(check)
                    interpreter = Path(check)/prefix/'runtime/python.exe'
                    subprocess.run([interpreter, '-I', '-B', '-c', 'import desktop, encodings, serial_board; from serial.tools import list_ports'],
                                   cwd=check, check=True, timeout=30)
        staged.replace(archive)
    finally:
        staged.unlink(missing_ok=True)
    receipt = {'status': 'PACKAGED', 'entry': prefix + 'AX7021Monitor.exe',
               'window_runtime': WINDOW_NAME, 'window_sha256': WINDOW_SHA256, 'window_url': WINDOW_URL,
               'serial_wheel': SERIAL_WHEEL, 'serial_wheel_sha256': SERIAL_SHA256,
               'runtime': RUNTIME_NAME, 'runtime_url': RUNTIME_URL, 'runtime_sha256': RUNTIME_SHA256,
               'files_sha256': {name: sha256(data) for name, data in source.items()},
               'archive': str(archive), 'archive_sha256': sha256(archive.read_bytes())}
    (output/'manifest.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(archive)


if __name__ == '__main__':
    main()
