"""Validate one completed, UTF-8 XSim transcript using an explicit bench marker."""

import argparse
import re
from pathlib import Path

FAILURE = re.compile(r"^(?:(?:fatal|error)\s*:|sim_fail\b|\$fatal\b)", re.IGNORECASE)


def check_log(lines, exit_code, pass_line):
    if exit_code != 0:
        return f"simulator exited with {exit_code}"
    matches = 0
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if line.startswith("#"):
            line = line[1:].lstrip()
        if FAILURE.match(line):
            return f"failure diagnostic on line {number}"
        if line == pass_line:
            matches += 1
    if matches != 1:
        return f"expected exactly one completion marker, found {matches}"
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--exit-code", required=True, type=int)
    parser.add_argument("--pass-line", required=True)
    args = parser.parse_args()
    if not args.pass_line.strip() or len(args.pass_line.splitlines()) != 1:
        parser.error("--pass-line must be one nonempty line")
    try:
        with args.log.open(encoding="utf-8-sig") as source:
            error = check_log(source, args.exit_code, args.pass_line.strip())
    except (OSError, UnicodeError) as exc:
        parser.exit(2, f"Unable to read log: {exc}\n")
    if error:
        parser.exit(1, f"FAIL: {error}\n")
    print("PASS: simulation completed with the required marker")


if __name__ == "__main__":
    main()
