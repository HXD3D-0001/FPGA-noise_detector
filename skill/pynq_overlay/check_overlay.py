"""Check one PYNQ IP's VLNV and required register offsets before optional download."""

import argparse
from collections.abc import Mapping
import json
from pathlib import Path
import sys


def byte_offset(value):
    if isinstance(value, str):
        value = int(value, 0)
    if type(value) is not int or value < 0:
        raise ValueError("register offset must be a non-negative integer")
    return value


def read_contract(path):
    contract = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(contract, dict) or set(contract) != {"vlnv", "registers"}:
        raise ValueError("contract requires exactly vlnv and registers")
    vlnv = contract["vlnv"]
    parts = vlnv.split(":") if isinstance(vlnv, str) else []
    if len(parts) != 4 or not all(parts):
        raise ValueError("vlnv must be vendor:library:name:version")
    registers = contract["registers"]
    if not isinstance(registers, dict) or not registers or not all(registers):
        raise ValueError("registers must map at least one register name to its byte offset")
    return vlnv, {name: byte_offset(offset) for name, offset in registers.items()}


def match_interface(ip_dict, vlnv, required, instance):
    if not isinstance(ip_dict, Mapping):
        raise ValueError("metadata must be an ip_dict object keyed by instance name")
    candidates = {name: desc for name, desc in ip_dict.items()
                  if isinstance(desc, Mapping) and desc.get("type") == vlnv}
    if instance is not None:
        if instance not in candidates:
            raise ValueError(f"no IP named {instance!r} with VLNV {vlnv}")
    else:
        if not candidates:
            raise ValueError(f"no IP with VLNV {vlnv}")
        if len(candidates) > 1:
            raise ValueError("multiple matching IPs; pass --ip with one of: "
                             + ", ".join(sorted(candidates)))
        instance = next(iter(candidates))
    desc = candidates[instance]
    register_map = desc.get("registers")
    if not isinstance(register_map, Mapping):
        raise ValueError(f"{instance}: register metadata unavailable")
    for name, expected in required.items():
        register = register_map.get(name)
        if not isinstance(register, Mapping) or "address_offset" not in register:
            raise ValueError(f"{instance}: missing register or offset for {name}")
        actual = byte_offset(register["address_offset"])
        if actual != expected:
            raise ValueError(f"{instance}.{name}: expected {expected:#x}, found {actual:#x}")
    return {"instance": instance, "vlnv": vlnv, "base_address": desc.get("phys_addr"),
            "register_offsets": required}


def inspect(args):
    vlnv, required = read_contract(args.contract)
    if args.metadata is not None:
        ip_dict = json.loads(args.metadata.read_text(encoding="utf-8-sig"))
        result = match_interface(ip_dict, vlnv, required, args.ip)
        result["scope"] = "offline-metadata"
        return result

    bit = args.bit.resolve()
    if bit.suffix.lower() != ".bit" or not bit.is_file():
        raise ValueError(f"expected an existing .bit file: {bit.name}")
    if not bit.with_suffix(".hwh").is_file():
        raise ValueError(f"missing matching metadata file: {bit.with_suffix('.hwh').name}")
    from pynq import Overlay

    overlay = Overlay(str(bit), download=False)
    result = match_interface(overlay.ip_dict, vlnv, required, args.ip)
    result["scope"] = "pynq-metadata"
    if args.download:
        overlay.download()
        if not overlay.is_loaded():
            raise RuntimeError("download returned but is_loaded() is false")
        result["scope"] = "pynq-loaded"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("contract", type=Path, help="JSON contract with vlnv and registers")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--metadata", type=Path, help="offline JSON projection of ip_dict")
    source.add_argument("--bit", type=Path, help="board-specific .bit and adjacent .hwh")
    parser.add_argument("--ip", help="exact instance path when multiple IPs match")
    parser.add_argument("--download", action="store_true", help="program PL after the interface matches")
    args = parser.parse_args()
    if args.download and args.bit is None:
        parser.error("--download requires --bit on a PYNQ device")
    try:
        result = inspect(args)
    except Exception as exc:
        # Board-specific drivers and PYNQ metadata parsers can raise their own exceptions.
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
