"""Exercise the public checker CLI with positive and negative fixtures."""

from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parent


def expect(file, code, expected, marker="TB_DONE_OK"):
    run = subprocess.run(
        [sys.executable, str(ROOT / "verify_xsim.py"), str(file),
         "--exit-code", str(code), "--pass-line", marker],
        capture_output=True, text=True,
    )
    if run.returncode != expected:
        raise AssertionError(f"{file.name}: expected {expected}, got {run.returncode}: {run.stderr}")


def main():
    expect(ROOT / "examples/pass.txt", 0, 0)
    expect(ROOT / "examples/fault.txt", 0, 1)
    expect(ROOT / "examples/pass.txt", 1, 1)
    cases = [
        ("no_marker", "INFO: run stopped\n", 1),
        ("duplicate", "TB_DONE_OK\nTB_DONE_OK\n", 1),
        ("echo", "puts TB_DONE_OK\n", 1),
        ("explicit_fail", "SIM_FAIL input mismatch\nTB_DONE_OK\n", 1),
        ("tool_error", "# ERROR: failed assertion\nTB_DONE_OK\n", 1),
        ("with_prefix", "# TB_DONE_OK\n", 0),
    ]
    with TemporaryDirectory() as directory:
        for name, text, expected in cases:
            path = Path(directory) / (name + ".txt")
            path.write_text(text, encoding="utf-8")
            expect(path, 0, expected)
    marker = "PASS spectrum_shared_store"
    expect(ROOT / "examples/fft_before.txt", 0, 1, marker)
    expect(ROOT / "examples/fft_after.txt", 0, 0, marker)
    print(f"PASS: {len(cases) + 3} checker cases and 2 archived FFT log excerpts")


if __name__ == "__main__":
    main()
