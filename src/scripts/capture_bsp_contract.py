"""Bind CAP1/DAT1/CTR1 BSP metadata to current hardware; never execute init."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_capture_hardware import validate
from check_pre_adc_hardware import validate_variant
from check_dma_hardware import check_bsp_interrupts

ROOT = Path(__file__).resolve().parents[1]
PROFILE = "capture_candidate"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def required_sources():
    return {p.resolve() for p in [*ROOT.glob("rtl/*.v"), *ROOT.glob("generated/*.mem"),
        ROOT / "bd/create_capture_bd.tcl", ROOT / "scripts/export_capture_hardware.ps1",
        ROOT / "scripts/check_capture_hardware.py", ROOT / "scripts/check_dma_hardware.py",
        ROOT / "tests/python/test_ps_hardware.py", ROOT / "tests/python/test_capture_hardware.py",
        ROOT / "build/fft_ip/lfn_fft/lfn_fft.xci"]}


def pre_adc_required_sources():
    return {p.resolve() for p in [*ROOT.glob("rtl/*.v"), *ROOT.glob("generated/*.mem"),
        ROOT / "bd/create_pre_adc_bd.tcl", ROOT / "bd/ax7021_pre_adc_profile.tcl",
        ROOT / "scripts/export_pre_adc_hardware.ps1", ROOT / "scripts/check_pre_adc_hardware.py",
        ROOT / "scripts/check_capture_hardware.py", ROOT / "scripts/check_dma_hardware.py",
        ROOT / "tests/python/test_ps_hardware.py",
        ROOT / "tests/python/test_pre_adc_network_hardware.py",
        ROOT / "build/fft_ip/lfn_fft/lfn_fft.xci"]}


# Receipt scope selects the exact source set, fresh validator and profile
# label; each variant keeps its own fail-closed negative baseline.
HARDWARE_INPUTS = {
    "CAP1_DAT1_CTR1_CANDIDATE_XSA_ONLY": {
        "sources": required_sources, "validate": validate, "profile": "capture_candidate"},
    "PRE_ADC_PROFILE_CARRIER_SD0_XSA_ONLY": {
        "sources": pre_adc_required_sources, "validate": validate_variant,
        "profile": "pre_adc_carrier_sd0"},
    "PRE_ADC_PROFILE_CARRIER_SD0_ETHERNET_XSA_ONLY": {
        "sources": pre_adc_required_sources, "validate": lambda xsa: validate_variant(xsa, ethernet=True),
        "profile": "pre_adc_carrier_sd0_ethernet"},
}


def validate_input(xsa, hardware_receipt=None):
    xsa = Path(xsa).resolve()
    receipt = Path(hardware_receipt or xsa.with_name("status.json")).resolve()
    evidence = json.loads(receipt.read_text(encoding="utf-8-sig"))
    spec = HARDWARE_INPUTS.get(evidence.get("scope"))
    if (evidence.get("status") != "PASS" or spec is None or
        evidence.get("board_execution_authorized") is not False or
        evidence.get("init_execution_authorized") is not False):
        raise ValueError("Require non-executable capture hardware PASS receipt")
    hashes = evidence.get("source_hashes", {})
    if {Path(p).resolve() for p in hashes} != spec["sources"]():
        raise ValueError("Hardware source set differs from current export inputs")
    for path, expected in hashes.items():
        if sha(path).lower() != expected.lower():
            raise ValueError("Hardware source changed: " + path)
    if sha(xsa).lower() != evidence.get("xsa_sha256", "").lower():
        raise ValueError("Hardware receipt XSA fingerprint mismatch")
    validation_file = xsa.with_name("validation.json")
    if sha(validation_file).lower() != evidence.get("validation_sha256", "").lower():
        raise ValueError("Hardware validation fingerprint mismatch")
    original = json.loads(validation_file.read_text(encoding="utf-8-sig"))
    current = spec["validate"](xsa)
    if current != original:
        raise ValueError("Fresh hardware validation differs from recorded validation")
    current.update(hardware_profile=spec["profile"], hardware_receipt=str(receipt),
                   hardware_receipt_sha256=sha(receipt), hardware_validation_sha256=sha(validation_file),
                   hardware_source_hashes=hashes)
    return current


def check_sdt(path, validation):
    """Parse only balanced literal DTS nodes; require identity-mapped 32-bit PL bus."""
    path = Path(path)
    text = path.read_text(encoding="utf-8")
    # Preserve quoted paths/strings while removing comments. Braces/semicolons in
    # strings must not change the tree. Unsupported expressions fail below.
    text = re.sub(r'"(?:\\.|[^"\\])*"|/\*[\s\S]*?\*/|//[^\n]*',
                  lambda m: m[0] if m[0].startswith('"') else "", text)
    virtual = {"header": "", "properties": [], "parent": None}
    stack, nodes, pending = [virtual], [], []
    for token in re.split(r'("(?:\\.|[^"\\])*"|[{};])', text):
        if token == "{":
            node = {"header": "".join(pending).strip(), "properties": [], "parent": stack[-1]}
            if not node["header"]:
                raise ValueError("SDT node header missing")
            nodes.append(node); stack.append(node); pending = []
        elif token == "}":
            if len(stack) == 1 or "".join(pending).strip():
                raise ValueError("Unbalanced or unsupported SDT node")
            stack.pop(); pending = []
        elif token == ";":
            prop = "".join(pending).strip()
            if prop:
                stack[-1]["properties"].append(prop)
            pending = []
        else:
            pending.append(token)
    if len(stack) != 1 or "".join(pending).strip():
        raise ValueError("Truncated SDT")

    def property_value(node, name):
        matches = [re.fullmatch(re.escape(name) + r"\s*(?:=\s*([\s\S]*))?", prop)
                   for prop in node["properties"]]
        matches = [match for match in matches if match]
        if len(matches) != 1:
            raise ValueError("Missing or duplicate SDT property: " + name)
        return (matches[0][1] or "").strip()

    matches = [(node, re.fullmatch(r"(?:processing\s*:\s*)?capture_platform_top@([0-9a-fA-F]+)", node["header"]))
               for node in nodes if "capture_platform_top@" in node["header"]]
    if len(matches) != 1 or matches[0][1] is None:
        raise ValueError("Require one literal capture SDT node")
    node, match = matches[0]
    parent = node["parent"]
    if (not re.fullmatch(r"(?:amba_pl\s*:\s*)?amba_pl", parent["header"]) or
            parent["parent"]["header"] != "/" or
            property_value(parent, "ranges") != "" or
            property_value(parent, "compatible") != '"simple-bus"'):
        raise ValueError("SDT capture bus must use identity ranges")
    for name in ("#address-cells", "#size-cells"):
        if property_value(parent, name) not in ("<1>", "<0x1>"):
            raise ValueError("Unknown SDT cell width")
    for name, expected in (("compatible", '"xlnx,capture-platform-top-1.0"'),
                           ("xlnx,ip-name", '"capture_platform_top"'), ("status", '"okay"')):
        if property_value(node, name) != expected:
            raise ValueError("Wrong SDT capture identity/status: " + name)
    reg = re.fullmatch(r"<([^<>]+)>", property_value(node, "reg"))
    if reg is None:
        raise ValueError("Unsupported SDT capture reg")
    tokens = reg[1].split()
    if len(tokens) != 6 or any(not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", t) for t in tokens):
        raise ValueError("Require three literal SDT register windows")
    cells = [int(token, 0) for token in tokens]
    expected = []
    for key in ("s_cap_axi", "s_dat_axi", "s_ctr_axi"):
        region = validation["topology"]["registers"][key]
        expected += [region["base"], region["bytes"]]
    if cells != expected or int(match[1], 16) != expected[0]:
        raise ValueError("SDT register windows differ from actual XSA")
    return {"path": str(path.resolve()), "sha256": sha(path), "reg_cells": cells,
            "scope": "GENERATED_SDT_THREE_WINDOWS_MATCH_XSA"}


def check_bsp(include, validation, sdt_proof=None):
    top = validation["topology"]
    result = check_bsp_interrupts(include, {"interrupts": top["dma_irqs"], "registers": top["registers"]})
    # Custom module-reference macros are not named here: discover complete numeric
    # BASEADDR/HIGHADDR pairs and require all three actual HWH windows. Unknown
    # vendor output must fail explicitly rather than quietly borrow old addresses.
    text = (Path(include) / "xparameters.h").read_text(encoding="utf-8")
    pairs, seen = {}, set()
    for name, value in re.findall(r"^\s*#define\s+(XPAR_\w+_(?:BASEADDR|HIGHADDR))\s+([^\r\n]+)", text, re.M):
        if name in seen:
            raise ValueError("Duplicate BSP address macro: " + name)
        seen.add(name)
        token = value.split("/*")[0].strip().rstrip("UuLl")
        try:
            pairs[name] = int(token, 0)
        except ValueError:
            continue  # aliases cannot establish an independent numeric address pair
    windows = {(base, pairs[name[:-8] + "HIGHADDR"]) for name, base in pairs.items()
               if name.endswith("BASEADDR") and name[:-8] + "HIGHADDR" in pairs}
    expected_windows = {(top["registers"][key]["base"], top["registers"][key]["high"])
                        for key in ("s_cap_axi", "s_dat_axi", "s_ctr_axi")}
    if not expected_windows <= windows:
        # Observed 2026.1 module-reference generator emits both aliases for only
        # reg[0]. Missing DAT/CTR macros are acceptable only with full SDT proof
        # and these exact vendor aliases. Never fabricate xparameters.h entries.
        first = top["registers"]["s_cap_axi"]
        for prefix in ("XPAR_PROCESSING_", "XPAR_CAPTURE_PLATFORM_TOP_0_"):
            if (pairs.get(prefix + "BASEADDR") != first["base"] or
                    pairs.get(prefix + "HIGHADDR") != first["high"] or sdt_proof is None):
                raise ValueError("Missing or incorrect BSP register window proof")
        result["register_header_format"] = "VITIS_2026_1_MODULE_REF_FIRST_WINDOW_WITH_SDT"
    else:
        result["register_header_format"] = "ALL_THREE_NUMERIC_WINDOWS"
    result["register_windows"] = {key: top["registers"][key]
                                  for key in ("s_cap_axi", "s_dat_axi", "s_ctr_axi")}
    return result


def inspect_platform(platform_dir, validation, domains):
    platform_dir = Path(platform_dir)
    result = {}
    common_sdt = check_sdt(platform_dir / "hw/sdt/pl.dtsi", validation)
    for domain in domains:
        include = platform_dir / "ps7_cortexa9_0" / domain / "bsp/include"
        sdt_file = include.parent / "hw_artifacts/ps7_cortexa9_0_baremetal.dts"
        domain_sdt = check_sdt(sdt_file, validation)
        result[domain] = check_bsp(include, validation, domain_sdt)
        result[domain]["common_sdt"] = common_sdt
        result[domain]["domain_sdt"] = domain_sdt
        export = platform_dir / "export" / platform_dir.name / "sw" / domain / "include"
        exported_sdt = export.parent / "hw_artifacts/ps7_cortexa9_0_baremetal.dts"
        if sha(sdt_file) != sha(exported_sdt):
            raise ValueError("Exported BSP SDT differs: " + domain)
        result[domain]["export_sdt"] = {"path": str(exported_sdt.resolve()), "sha256": sha(exported_sdt)}
        for name in ("xparameters.h", "xinterrupt_wrap.h"):
            if sha(include / name) != sha(export / name):
                raise ValueError("Exported BSP header differs: " + domain + "/" + name)
    return result


def map_header(validation):
    lines = ["/* Generated from validated capture XSA; candidate metadata only. */",
             "#ifndef LFN_CAPTURE_MAP_H", "#define LFN_CAPTURE_MAP_H",
             '#define LFN_CAPTURE_XSA_SHA256 "' + validation["xsa_sha256"] + '"',
             "#define LFN_CAPTURE_BOARD_VERIFIED 0U"]
    top = validation["topology"]
    for key, label in (("s_cap_axi", "CAP"), ("s_dat_axi", "DAT"), ("s_ctr_axi", "CTR"),
                       ("dma_spectrum", "DMA_SPECTRUM"), ("dma_pcm", "DMA_PCM")):
        for field in ("base", "high"):
            lines.append(f"#define LFN_CAPTURE_{label}_{field.upper()} 0x{top['registers'][key][field]:08x}U")
    for key, irq in top["dma_irqs"].items():
        for field in ("gic_id", "sdt_encoded"):
            lines.append(f"#define LFN_CAPTURE_{key.upper()}_{field.upper()} 0x{irq[field]:x}U")
    for key, value in top["memory_plan"].items():
        lines.append(f"#define LFN_CAPTURE_{key.upper()} 0x{value:x}U")
    lines.extend([f"#define LFN_CAPTURE_CLOCK_HZ {top['clock_hz']}U", "#endif", ""])
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xsa", type=Path, required=True)
    parser.add_argument("--hardware-receipt", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    record = {"status": "RUNNING", "scope": "XSA_CONTRACT_ONLY", "bsp_status": "NOT_BUILT",
              "board_execution_authorized": False, "init_execution_authorized": False}
    args.output.write_text(json.dumps(record) + "\n", encoding="utf-8")
    try:
        record["contract"] = validate_input(args.xsa, args.hardware_receipt)
        record["status"] = "PASS"
    except Exception as error:
        record.update(status="FAIL", error=type(error).__name__ + ": " + str(error))
    args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(record["status"] + " capture_bsp_input XSA_CONTRACT_ONLY")
    raise SystemExit(0 if record["status"] == "PASS" else 1)
