"""Run deterministic host-side tests, without hardware or network access."""
from pathlib import Path
import subprocess
import sys

src = Path(__file__).resolve().parents[1] / "src"
suites = [
    ("tests/python", "test_ps_hardware.py"),
    ("tests/python", "test_embedded_platform.py"),
    ("host_monitor", "test_serial_board.py"),
]
for directory, suite in suites:
    subprocess.run([sys.executable, "-B", "-m", "unittest", "discover", "-s", str(src / directory), "-p", suite], check=True)
print("PASS portable validation (XSA fixtures, platform contracts and serial transport)")
