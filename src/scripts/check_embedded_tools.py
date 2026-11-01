"""Current Embedded compiler capability audit; no client/server or board access."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import uuid


def inspect_object(data):
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("Expected complete ELF32 little-endian object")
    fields = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    _, kind, machine, version, _, phoff, shoff, flags, ehsize, _, phnum, shsize, shnum, shstr = fields
    if kind != 1 or machine != 40 or version != 1 or ehsize != 52:
        raise ValueError("Expected ARM relocatable object")
    if flags >> 24 != 5:
        raise ValueError("Expected ARM EABI5")
    if phnum != 0 or phoff != 0 or shsize != 40 or shnum == 0 or shstr >= shnum:
        raise ValueError("Unexpected relocatable-object tables")
    if shoff < 52 or shoff + shsize * shnum > len(data):
        raise ValueError("Truncated ELF section table")
    sections = [struct.unpack_from("<10I", data, shoff + index * shsize) for index in range(shnum)]
    for section in sections:
        _, section_type, _, _, offset, size, _, _, _, _ = section
        # SHT_NOBITS owns memory size but contains no stored file payload.
        if section_type != 8 and (offset > len(data) or size > len(data) - offset):
            raise ValueError("ELF section payload exceeds object file")
    names_section = sections[shstr]
    if names_section[1] != 3 or names_section[5] == 0:
        raise ValueError("Invalid section-name string table")
    names_data = data[names_section[4]:names_section[4] + names_section[5]]
    if names_data[0] != 0 or names_data[-1] != 0:
        raise ValueError("Section-name string table must be NUL bounded")
    names = []
    for section in sections:
        name_offset = section[0]
        if name_offset >= len(names_data):
            raise ValueError("Section-name index exceeds string table")
        name_end = names_data.find(b"\0", name_offset)
        if name_end < 0:
            raise ValueError("Unterminated section name")
        names.append(names_data[name_offset:name_end])
    executable = [{"name": names[index].decode("ascii", errors="backslashreplace"), "size": section[5]}
                  for index, section in enumerate(sections)
                  if section[1] == 1 and section[2] & 4 and section[5] > 0]
    if not executable:
        raise ValueError("No nonempty executable PROGBITS section")
    if not any(name == b".ARM.attributes" and section[1] == 0x70000003 and section[5] > 0
               for name, section in zip(names, sections)):
        raise ValueError("Missing nonempty ARM attributes section")
    return {"class": "ELF32", "endian": "little", "machine": "ARM", "type": "REL",
            "eabi": 5, "section_count": shnum, "section_payload_bounds_verified": True,
            "executable_sections": executable, "arm_attributes_section_present": True,
            "sha256": hashlib.sha256(data).hexdigest()}


def check_installation(suite_root, output_dir, runner=subprocess.run):
    suite_root, output_dir = Path(suite_root).resolve(), Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    record = {"status": "RUNNING", "run_id": str(uuid.uuid4()),
              "checked_at_utc": datetime.now(timezone.utc).isoformat(),
              "compiler_status": "NOT_VERIFIED", "project_bsp_status": "NOT_VERIFIED",
              "modern_platform_status": "NOT_VERIFIED", "legacy_sdk_diagnostic": "NOT_RUN",
              "scope": "INSTALLATION_AND_ARM_OBJECT_ONLY"}

    def save():
        (output_dir / "status.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        (output_dir / "status.txt").write_text(record["status"] + " embedded_tools\n", encoding="ascii")

    save()
    try:
        for name in ("probe.o", "capabilities.json"):
            (output_dir / name).unlink(missing_ok=True)
        vitis = suite_root / "Vitis"
        record["product_selection"] = {
            "embedded_marker": (vitis / ".vitis_embedded").is_file(),
            "embedded_settings": (vitis / ".settings64-Vitis_Embedded_Development.bat").is_file(),
            "hls_marker": (vitis / ".vitis_for_hls").is_file(),
        }
        if not record["product_selection"]["embedded_marker"] or not record["product_selection"]["embedded_settings"]:
            raise ValueError("Current Embedded marker/settings selection not present")
        record["launcher_lopper_environment_script_present"] = (
            vitis / "tps/win64/lopper-1.1.0-packages/min_sdk/environment-setup.bat").is_file()
        bin_dir = suite_root / "gnu/aarch32/nt/gcc-arm-none-eabi/bin"
        candidates = sorted(bin_dir.glob("*-gcc.exe"))
        if len(candidates) != 1:
            raise ValueError("Require exactly one ARM GCC candidate in the selected package")
        compiler = candidates[0]
        readelf = compiler.with_name(compiler.name.replace("-gcc.exe", "-readelf.exe"))
        if not readelf.is_file():
            raise ValueError("Matching GNU readelf not found")
        child_env = dict(os.environ)
        child_env["PATH"] = str(bin_dir) + os.pathsep + child_env.get("PATH", "")

        def run(label, command):
            result = runner([str(arg) for arg in command], cwd=output_dir, env=child_env,
                            capture_output=True, text=True, timeout=30)
            (output_dir / (label + ".log")).write_text(
                result.stdout + result.stderr, encoding="utf-8")
            if result.returncode != 0:
                raise ValueError(label + " returned nonzero status")
            return result.stdout

        version = run("gcc_version", [compiler, "--version"])
        match = re.search(r"\b(\d+\.\d+\.\d+)\b", version)
        if not match or "gcc" not in version.lower():
            raise ValueError("Unrecognized GNU GCC version output")
        target = run("gcc_target", [compiler, "-dumpmachine"]).strip()
        if target != "arm-xilinx-eabi":
            raise ValueError("Unexpected compiler target; require arm-xilinx-eabi")
        source, obj = output_dir / "probe.c", output_dir / "probe.o"
        source.write_text("unsigned capability_probe(unsigned x) { return (x ^ 0x7021U) + 17U; }\n", encoding="ascii")
        flags = ["-mcpu=cortex-a9", "-marm", "-mfpu=vfpv3", "-mfloat-abi=soft",
                 "-ffreestanding", "-fno-common", "-fno-pic", "-O2", "-c"]
        run("gcc_compile", [compiler, *flags, source.resolve(), "-o", obj.resolve()])
        if not obj.is_file():
            raise ValueError("Compiler did not produce a fresh object")
        object_info = inspect_object(obj.read_bytes())
        attributes = run("readelf_attributes", [readelf, "-A", obj.resolve()])
        cpu_name = re.search(r'Tag_CPU_name:\s*"([^"]+)"', attributes)
        arch = re.search(r"Tag_CPU_arch:\s*([^\r\n]+)", attributes)
        if not cpu_name or cpu_name.group(1).lower() not in ("cortex-a9", "7-a") or not arch or arch.group(1).strip() != "v7":
            raise ValueError("Object attributes do not match ARMv7 Cortex-A9 compilation")
        record["compiler"] = {"executable": compiler.name, "version": match.group(1),
                              "target": target, "compile_flags": flags}
        record["object"] = {**object_info, "cpu_name_attribute": cpu_name.group(1),
                            "cpu_arch_attribute": arch.group(1).strip()}
        record["compiler_status"] = "INSTALLED_WORKING"
        record["status"] = "PASS"
        (output_dir / "capabilities.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        record["status"] = "FAIL"
        record["error"] = type(error).__name__ + ": " + str(error)
    finally:
        save()
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/embedded_tools")
    args = parser.parse_args()
    receipt = check_installation(args.suite_root, args.output_dir)
    if receipt["status"] != "PASS":
        print("FAIL embedded_tools; inspect the ignored capability receipt")
        raise SystemExit(1)
    print("PASS embedded_tools")
    print("ARM_GNU=" + receipt["compiler"]["version"] + " TARGET=" + receipt["compiler"]["target"])
    print("PROJECT_BSP=NOT_VERIFIED MODERN_PLATFORM=NOT_VERIFIED")
