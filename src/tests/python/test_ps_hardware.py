"""Independent XSA inspection and negative fixtures; no vendor code execution."""
import argparse
import json
import hashlib
import re
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

PART = "xc7z020clg484-2"
DISABLED = ("PCW_UIPARAM_DDR_ENABLE", "PCW_EN_DDR") + tuple(
    "PCW_USE_" + name for name in (
        "M_AXI_GP0", "M_AXI_GP1", "S_AXI_GP0", "S_AXI_GP1", "S_AXI_ACP",
        "S_AXI_HP0", "S_AXI_HP1", "S_AXI_HP2", "S_AXI_HP3")) + tuple(
    "PCW_EN_" + name for name in (
        "UART0", "UART1", "USB0", "USB1", "ENET0", "ENET1", "SDIO0", "SDIO1",
        "SPI0", "SPI1", "I2C0", "I2C1", "CAN0", "CAN1", "GPIO", "QSPI", "SMC",
        "TRACE", "TTC0", "TTC1", "WDT", "PJTAG", "RST0_PORT", "RST1_PORT",
        "RST2_PORT", "RST3_PORT", "CLK1_PORT", "CLK2_PORT", "CLK3_PORT"))


def inspect_init(name, text):
    """Conservative static classification of the known generated C/Tcl shapes.

    This is not a general language parser or an execution-safety proof. Unknown
    shapes are explicitly reported and no classification grants authorization.
    """
    is_c = name.endswith(".c")
    comments = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"' if is_c else r"(?m)^\s*#[^\n]*"
    code = re.sub(comments, lambda m: "\n" * m.group().count("\n"), text, flags=re.S if is_c else 0)
    line_at = lambda pos: code.count("\n", 0, pos) + 1
    function = re.search(r"\bps7_init\s*\([^)]*\)\s*\{" if is_c else
                         r"\bproc\s+ps7_init\s+\{[^}]*\}\s*\{", code)
    start = function.end() - 1 if function else None
    end = None
    if start is not None:
        depth = 0
        for pos in range(start, len(code)):
            depth += (code[pos] == "{") - (code[pos] == "}")
            if depth == 0:
                end = pos
                break
    arrays = []
    if is_c:
        for match in re.finditer(r"\b(ps7_ddr_init_data\w*)\s*\[\s*\]\s*=\s*\{([^}]*)\}\s*;", code):
            body = match.group(2).strip()
            arrays.append({"name": match.group(1), "line": line_at(match.start()),
                           "empty": not body, "has_emit_exit": bool(re.search(r"\bEMIT_EXIT\s*\(", body))})
        calls = list(re.finditer(r"\bps7_config\s*\(\s*(ps7_ddr_init_data\w*)\s*\)", code))
    else:
        calls = list(re.finditer(r"\bps7_config\s+\$?(ps7_ddr_init_data\w*)|(?m:^\s*(ps7_ddr_init_data\w*)\b)", code))
    call_lines = [line_at(m.start()) for m in calls]
    unconditional = [line_at(m.start()) for m in calls if start is not None and end is not None and
                     start < m.start() < end and
                     code[start:m.start()].count("{") - code[start:m.start()].count("}") == 1]
    write_prefix = r"(?:EMIT_(?:MASKWRITE|WRITE)\s*\(\s*|mask_write\s+|mwr\s+)"
    writes = list(re.finditer(write_prefix + r"(0x[0-9a-f]{8})\b", code, re.I))
    write_lines = lambda predicate: [line_at(m.start()) for m in writes if predicate(int(m.group(1), 16))]
    controller_writes = write_lines(lambda addr: 0xF8006000 <= addr <= 0xF8006FFF)
    pll_writes = write_lines(lambda addr: addr == 0xF8000104)
    clock_writes = write_lines(lambda addr: addr == 0xF8000124)
    training = [line_at(m.start()) for m in re.finditer(r"\bddr_train\w*\s*\(", code)]
    unknown = end is None or (is_c and "ps7_ddr_init_data" in code and not arrays)
    risks = []
    add_risk = lambda kind, lines, **extra: risks.append({"code": kind, "file": name,
                                                        "lines": sorted(set(lines)), **extra})
    if unknown:
        add_risk("UNKNOWN_INIT_STRUCTURE", [])
    if calls or controller_writes or training:
        add_risk("DDR_CONTROLLER_FLOW_PRESENT", call_lines + controller_writes + training)
    if pll_writes or clock_writes:
        add_risk("GENERIC_DDR_PLL_CLOCK_WRITES", pll_writes + clock_writes)
    if is_c and calls:
        tokens = {m.group(1) for m in calls}
        reachable = [a for a in arrays if a["name"] in tokens or "ps7_ddr_init_data" in tokens]
        if not reachable:
            add_risk("UNKNOWN_INIT_STRUCTURE", call_lines)
            unknown = True
        for empty, kind in ((True, "EMPTY_DDR_ARRAY_WITHOUT_EXIT_CALLED"),
                            (False, "DDR_ARRAY_WITHOUT_EXIT_CALLED")):
            bad = [a for a in reachable if a["empty"] == empty and not a["has_emit_exit"]]
            if bad:
                add_risk(kind, call_lines + [a["line"] for a in bad], arrays=[a["name"] for a in bad])
    flow = bool(calls or controller_writes or training)
    return {"format": "C" if is_c else "Tcl", "analysis_status": "UNKNOWN" if unknown else "KNOWN",
            "ddr_arrays": arrays, "ddr_config_call_lines": call_lines,
            "unconditional_ddr_config_call_lines": unconditional,
            "ddr_controller_write_lines": controller_writes, "ddr_training_call_lines": training,
            "ddr_pll_write_lines": pll_writes, "ddr_clock_write_lines": clock_writes,
            "controller_flow_present": flow,
            "controller_flow_omitted": not flow and not unknown}, risks


def validate_xsa(path, *, allow_rtos_timer=False):
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        require(len(names) == len(set(names)), "Duplicate ZIP member")
        require(not any(n.endswith((".bit", ".elf")) for n in names), "Unexpected executable payload")
        hwhs = [n for n in names if n.endswith(".hwh")]
        require(len(hwhs) == 1, "Require exactly one HWH")
        metadata = [n for n in ("hwdef.xml", "sysdef.xml") if n in names]
        require(len(metadata) == 1, "Require exactly one hardware metadata XML")
        for needed in ("xsa.xml", "ps7_init.tcl", "ps7_init.c", "ps7_init.h"):
            require(needed in names, "Missing " + needed)
        hwh = ET.fromstring(z.read(hwhs[0]))
        sysdef = ET.fromstring(z.read(metadata[0]))
        xsa_xml = ET.fromstring(z.read("xsa.xml"))
        require(hwh.get("VIVADOVERSION") == "2026.1", "Unexpected Vivado version")
        device = xsa_xml.find(".//Device")
        require(device is not None and device.get("FpgaDevice") == "zynq:xc7z020:clg484:-2",
                "Wrong XSA device")
        board = xsa_xml.find(".//Board")
        require(board is not None and not any(board.get(k) for k in ("Name", "Vendor", "Part")),
                "Unexpected board preset metadata")
        info = hwh.find("SYSTEMINFO")
        require(info is not None and
                (info.get("DEVICE"), info.get("PACKAGE"), info.get("SPEEDGRADE")) ==
                ("7z020", "clg484", "-2"), "Wrong HWH part")
        require(sysdef.find("SYSTEMINFO").get("PART") == PART, "Wrong sysdef part")
        mods = list(hwh.iter("MODULE"))
        require(len(mods) == 1 and mods[0].get("VLNV") ==
                "xilinx.com:ip:processing_system7:5.5", "Require sole real PS7 module")
        interfaces = [e.get("NAME") for e in mods[0].iter("BUSINTERFACE")]
        require(interfaces == ["FIXED_IO"], "Unexpected PS7 bus interface")
        require(not any(e.get("NAME", "").startswith(("TTC0_", "TTC1_"))
                        for e in mods[0].iter("PORT")), "Unexpected TTC module pin")
        external = list(hwh.findall("./EXTERNALPORTS/PORT"))
        clocks = [e for e in external if "FCLK_CLK" in e.get("SIGNAME", "")]
        require(len(clocks) == 1 and clocks[0].get("SIGNAME", "").endswith("FCLK_CLK0") and
                clocks[0].get("CLKFREQUENCY") == "50000000", "Wrong exported FCLK port")
        require(all(e in clocks or e.get("NAME", "").startswith("FIXED_IO_") for e in external),
                "Unexpected external port")
        pairs = [(e.get("NAME"), e.get("VALUE")) for e in mods[0].iter("PARAMETER")]
        params = dict(pairs)
        require(len(params) == len(pairs), "Duplicate PS7 parameter")
        require(params.get("PCW_IMPORT_BOARD_PRESET") == "None", "Unexpected PS7 board preset")
        for name in DISABLED:
            if allow_rtos_timer and name == "PCW_EN_TTC0":
                continue
            require(params.get(name) == "0", "Required disabled field: " + name)
        allowed = {"PCW_EN_CLK0_PORT": "1", "PCW_FPGA_FCLK0_ENABLE": "1",
                   "PCW_USE_CR_FABRIC": "1"}
        timer_fields = {}
        if allow_rtos_timer:
            timer_fields = {"PCW_EN_TTC0": "1", "PCW_TTC0_PERIPHERAL_ENABLE": "1",
                            "PCW_EN_EMIO_TTC0": "0", "PCW_TTC0_TTC0_IO": "EMIO",
                            "PCW_TTC0_CLK0_PERIPHERAL_CLKSRC": "CPU_1X",
                            "PCW_TTC0_CLK1_PERIPHERAL_CLKSRC": "CPU_1X",
                            "PCW_TTC0_CLK2_PERIPHERAL_CLKSRC": "CPU_1X"}
            for name, value in timer_fields.items():
                require(params.get(name) == value, "Wrong RTOS timer field: " + name)
            allowed.update({"PCW_EN_TTC0": "1", "PCW_TTC0_PERIPHERAL_ENABLE": "1"})
        for name, value in params.items():
            if (name.startswith(("PCW_EN_", "PCW_USE_")) or
                    (name.endswith("_ENABLE") and value in ("0", "1")) or
                    name.startswith("PCW_UIPARAM_DDR_TRAIN_")):
                require(value == allowed.get(name, "0"), "Unexpected enabled field: " + name)
        for name in ("PCW_EN_CLK0_PORT", "PCW_FPGA_FCLK0_ENABLE", "PCW_USE_CR_FABRIC"):
            require(params.get(name) == "1", "Missing clock-export feature: " + name)
        for name, expected in (("PCW_FPGA0_PERIPHERAL_FREQMHZ", 50),
                               ("PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ", 50),
                               ("PCW_CRYSTAL_PERIPHERAL_FREQMHZ", 33.333333)):
            require(name in params and abs(float(params[name]) - expected) <= 0.000001,
                    "Wrong clock: " + name)
        mio = params.get("PCW_MIO_TREE_PERIPHERALS", "").split("#")
        require(len(mio) == 54 and set(mio) == {"unassigned"}, "MIO must all be unassigned")
        for name, value in params.items():
            if re.fullmatch(r"PCW_MIO_\d+_PERIPHERAL", name):
                require(value == "unassigned", "Assigned MIO: " + name)
        refs, summaries, init_risks = {}, {}, []
        for name in (n for n in names if n.startswith("ps7_init") and n.endswith((".tcl", ".c"))):
            lines = z.read(name).decode("utf-8").splitlines()
            refs[name] = [i for i, line in enumerate(lines, 1)
                          if re.search(r"ps7_ddr_init|ddr_training|ddr_train|0xF8006[0-9a-f]{3}", line, re.I)]
            summaries[name], risks = inspect_init(name, "\n".join(lines))
            init_risks.extend(risks)
        return {"part": PART, "vivado_version": hwh.get("VIVADOVERSION"),
                "hardware_profile": "ps_rtos_timer" if allow_rtos_timer else "ps_only",
                "timer_fields": timer_fields,
                "ps7_vlnv": mods[0].get("VLNV"), "fclk0_mhz": 50,
                "ps_clk_mhz": 33.333333, "ddr_enabled": False, "mio_assignments": 0,
                "axi_interfaces_enabled": 0, "zip_members": names,
                "ddr_init_or_training_reference_lines": refs,
                "ddr_controller_flow_present": any(s["controller_flow_present"] for s in summaries.values()),
                "init_summary": summaries, "init_risks": init_risks,
                "init_execution_authorized": False,
                "xsa_sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest()}


def mock_xsa(path, changes=None, missing=None, init=None, part=PART, extra_bus=None, init_c=None,
             extra_port=None, internal_port=None):
    config = {name: "0" for name in DISABLED}
    config.update({
        "PCW_UIPARAM_DDR_ENABLE": "0", "PCW_EN_DDR": "0",
        "PCW_USE_M_AXI_GP0": "0", "PCW_EN_UART1": "0",
        "PCW_EN_CLK0_PORT": "1", "PCW_FPGA_FCLK0_ENABLE": "1",
        "PCW_FPGA0_PERIPHERAL_FREQMHZ": "50",
        "PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ": "50.000000",
        "PCW_CRYSTAL_PERIPHERAL_FREQMHZ": "33.333333",
        "PCW_USE_CR_FABRIC": "1",
        "PCW_IMPORT_BOARD_PRESET": "None",
        "PCW_MIO_TREE_PERIPHERALS": "#".join(["unassigned"] * 54),
        "PCW_MIO_12_PERIPHERAL": "unassigned",
    })
    config.update(changes or {})
    root = ET.Element("EDKSYSTEM", VIVADOVERSION="2026.1")
    ET.SubElement(root, "SYSTEMINFO", DEVICE="7z020", PACKAGE="clg484", SPEEDGRADE="-2")
    mod = ET.SubElement(ET.SubElement(root, "MODULES"), "MODULE", INSTANCE="ps7",
                       MODTYPE="processing_system7", VLNV="xilinx.com:ip:processing_system7:5.5")
    interfaces = ET.SubElement(mod, "BUSINTERFACES")
    ET.SubElement(interfaces, "BUSINTERFACE", NAME="FIXED_IO")
    if extra_bus:
        ET.SubElement(interfaces, "BUSINTERFACE", NAME=extra_bus)
    if internal_port:
        ET.SubElement(ET.SubElement(mod, "PORTS"), "PORT", NAME=internal_port)
    ports = ET.SubElement(root, "EXTERNALPORTS")
    ET.SubElement(ports, "PORT", NAME="FCLK_CLK0_0", SIGNAME="ps7_FCLK_CLK0", CLKFREQUENCY="50000000")
    if extra_port:
        ET.SubElement(ports, "PORT", NAME=extra_port, SIGNAME="ps7_" + extra_port)
    params = ET.SubElement(mod, "PARAMETERS")
    for key, val in config.items():
        ET.SubElement(params, "PARAMETER", NAME=key, VALUE=val)
    sysdef = ET.Element("Project")
    ET.SubElement(sysdef, "SYSTEMINFO", PART=part)
    xsa_xml = ET.Element("Root")
    ET.SubElement(xsa_xml, "Device", FpgaDevice="zynq:xc7z020:clg484:-2")
    ET.SubElement(xsa_xml, "Board", Name="", Vendor="", Part="")
    files = {"ps_only.hwh": ET.tostring(root), "sysdef.xml": ET.tostring(sysdef),
             "xsa.xml": ET.tostring(xsa_xml),
             "ps7_init.tcl": (init or "proc ps7_init {} {return}\n").encode(),
             "ps7_init.c": (init_c or "int ps7_init(void) { return 0; }").encode(),
             "ps7_init.h": b"/* init */"}
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files.items():
            if name != missing:
                z.writestr(name, data)


class XsaValidationTests(unittest.TestCase):
    actual_xsa = None
    actual_allow_rtos_timer = False

    def classify(self, **kw):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "init.xsa"
            mock_xsa(path, **kw)
            return validate_xsa(path)

    def check_rejected(self, **kw):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "bad.xsa"
            mock_xsa(path, **kw)
            with self.assertRaises(ValueError):
                validate_xsa(path)

    def test_mock_minimal_passes(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "good.xsa"
            mock_xsa(path)
            validate_xsa(path)

    def test_ddr_enable_rejected(self):
        self.check_rejected(changes={"PCW_UIPARAM_DDR_ENABLE": "1"})

    def test_axi_enable_rejected(self):
        self.check_rejected(changes={"PCW_USE_M_AXI_GP0": "1"})

    def test_unused_peripheral_rejected(self):
        self.check_rejected(changes={"PCW_EN_UART1": "1"})

    def test_assigned_mio_rejected(self):
        self.check_rejected(changes={"PCW_MIO_12_PERIPHERAL": "uart1"})

    def test_wrong_clock_rejected(self):
        self.check_rejected(changes={"PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ": "100"})

    def test_wrong_part_rejected(self):
        self.check_rejected(part="xc7z020clg400-1")

    def test_missing_hwh_rejected(self):
        self.check_rejected(missing="ps_only.hwh")

    def test_missing_init_rejected(self):
        self.check_rejected(missing="ps7_init.tcl")

    def test_generated_ddr_flow_is_not_safe_to_execute(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "ddr_calls.xsa"
            mock_xsa(path, init="proc ps7_init {} {ps7_config $ps7_ddr_init_data_3_0}\n")
            self.assertTrue(validate_xsa(path)["ddr_controller_flow_present"])

    def test_direct_ddr_register_flow_is_reported(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "ddr_writes.xsa"
            mock_xsa(path, init="proc ps7_init {} {mask_write 0xF8006000 0xFFFFFFFF 1}\n")
            self.assertTrue(validate_xsa(path)["ddr_controller_flow_present"])

    def test_unexpected_axi_bus_rejected(self):
        self.check_rejected(extra_bus="M_AXI_GP0")

    def test_declaration_only_is_not_controller_execution(self):
        result = self.classify(init_c="unsigned long ps7_ddr_init_data_1_0[] = {};\nint ps7_init(void) { return 0; }")
        self.assertFalse(result["ddr_controller_flow_present"])
        self.assertEqual(result["init_summary"]["ps7_init.c"]["ddr_config_call_lines"], [])

    def test_empty_called_array_is_explicit_risk(self):
        result = self.classify(init_c="unsigned long ps7_ddr_init_data_1_0[] = {};\nint ps7_init(void) { return ps7_config(ps7_ddr_init_data_1_0); }")
        codes = {r["code"] for r in result["init_risks"]}
        self.assertIn("EMPTY_DDR_ARRAY_WITHOUT_EXIT_CALLED", codes)
        summary = result["init_summary"]["ps7_init.c"]
        self.assertEqual(summary["unconditional_ddr_config_call_lines"], [2])
        self.assertTrue(summary["ddr_arrays"][0]["empty"])
        self.assertFalse(summary["ddr_arrays"][0]["has_emit_exit"])
        self.assertFalse(result["init_execution_authorized"])

    def test_sentinel_array_is_not_empty_array_risk(self):
        result = self.classify(init_c="unsigned long ps7_ddr_init_data_1_0[] = {EMIT_EXIT()};\nint ps7_init(void) { return ps7_config(ps7_ddr_init_data_1_0); }")
        self.assertNotIn("EMPTY_DDR_ARRAY_WITHOUT_EXIT_CALLED", {r["code"] for r in result["init_risks"]})
        self.assertTrue(result["init_summary"]["ps7_init.c"]["ddr_arrays"][0]["has_emit_exit"])

    def test_tcl_pll_only_has_separate_risk(self):
        result = self.classify(init="proc ps7_init {} {\nmask_write 0xF8000104 0xFFFFFFFF 1\nmask_write 0xF8000124 0xFFFFFFFF 1\n}")
        summary = result["init_summary"]["ps7_init.tcl"]
        self.assertFalse(result["ddr_controller_flow_present"])
        self.assertTrue(summary["controller_flow_omitted"])
        self.assertEqual(summary["ddr_pll_write_lines"], [2])
        self.assertEqual(summary["ddr_clock_write_lines"], [3])
        self.assertIn("GENERIC_DDR_PLL_CLOCK_WRITES", {r["code"] for r in result["init_risks"]})

    def test_unknown_structure_is_classified_unknown(self):
        result = self.classify(init_c="void alternate_startup(void) { opaque_initialize(); }")
        self.assertEqual(result["init_summary"]["ps7_init.c"]["analysis_status"], "UNKNOWN")
        self.assertIn("UNKNOWN_INIT_STRUCTURE", {r["code"] for r in result["init_risks"]})
        self.assertFalse(result["init_execution_authorized"])

    def test_comments_are_not_execution(self):
        result = self.classify(init_c="/* ps7_config(ps7_ddr_init_data); */\nint ps7_init(void) { return 0; }")
        self.assertFalse(result["ddr_controller_flow_present"])

    def test_actual_archive_init_classification(self):
        if self.actual_xsa is None:
            self.skipTest("Pass --actual-xsa after a real export")
        result = validate_xsa(self.actual_xsa, allow_rtos_timer=self.actual_allow_rtos_timer)
        for name in ("ps7_init.c", "ps7_init_gpl.c"):
            summary = result["init_summary"][name]
            self.assertEqual(summary["analysis_status"], "KNOWN")
            self.assertEqual(len(summary["ddr_arrays"]), 3)
            self.assertTrue(all(a["empty"] and not a["has_emit_exit"] for a in summary["ddr_arrays"]))
            self.assertEqual(len(summary["unconditional_ddr_config_call_lines"]), 1)
            self.assertTrue(summary["ddr_pll_write_lines"])
            self.assertTrue(summary["ddr_clock_write_lines"])
            self.assertTrue(any(r["file"] == name and r["code"] == "EMPTY_DDR_ARRAY_WITHOUT_EXIT_CALLED" for r in result["init_risks"]))
        tcl = result["init_summary"]["ps7_init.tcl"]
        self.assertTrue(tcl["controller_flow_omitted"])
        self.assertTrue(tcl["ddr_pll_write_lines"])
        self.assertTrue(tcl["ddr_clock_write_lines"])
        self.assertFalse(result["init_execution_authorized"])


class RtosTimerValidationTests(unittest.TestCase):
    # Hand-derived PS7 property contract: no pins, CPU_1X clocks only.
    timer = {"PCW_EN_TTC0": "1", "PCW_TTC0_PERIPHERAL_ENABLE": "1",
             "PCW_EN_EMIO_TTC0": "0", "PCW_TTC0_TTC0_IO": "EMIO",
             "PCW_TTC0_CLK0_PERIPHERAL_CLKSRC": "CPU_1X",
             "PCW_TTC0_CLK1_PERIPHERAL_CLKSRC": "CPU_1X",
             "PCW_TTC0_CLK2_PERIPHERAL_CLKSRC": "CPU_1X"}

    def validate_timer(self, changes=None, **kw):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "timer.xsa"
            config = dict(self.timer)
            config.update(changes or {})
            mock_xsa(path, changes=config, **kw)
            return validate_xsa(path, allow_rtos_timer=True)

    def test_explicit_internal_timer_profile_passes(self):
        result = self.validate_timer()
        self.assertEqual(result["hardware_profile"], "ps_rtos_timer")
        self.assertEqual(result["timer_fields"], self.timer)
        self.assertFalse(result["init_execution_authorized"])
        self.assertEqual(result["mio_assignments"], 0)
        self.assertFalse(result["ddr_enabled"])

    def test_default_rejects_timer_profile(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "timer.xsa"
            mock_xsa(path, changes=self.timer)
            with self.assertRaises(ValueError):
                validate_xsa(path)

    def test_explicit_profile_requires_enabled_timer(self):
        for field in ("PCW_EN_TTC0", "PCW_TTC0_PERIPHERAL_ENABLE"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate_timer({field: "0"})

    def test_other_peripheral_ddr_axi_mio_and_emio_stay_rejected(self):
        for field in ("PCW_EN_TTC1", "PCW_TTC1_PERIPHERAL_ENABLE", "PCW_EN_UART1",
                      "PCW_UIPARAM_DDR_ENABLE", "PCW_EN_DDR", "PCW_USE_M_AXI_GP0",
                      "PCW_EN_EMIO_TTC0", "PCW_EN_EMIO_TTC1"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate_timer({field: "1"})
        with self.assertRaises(ValueError):
            self.validate_timer({"PCW_MIO_12_PERIPHERAL": "ttc0"})

    def test_external_clocks_and_io_selection_rejected(self):
        for field in ("PCW_TTC0_CLK0_PERIPHERAL_CLKSRC", "PCW_TTC0_CLK1_PERIPHERAL_CLKSRC",
                      "PCW_TTC0_CLK2_PERIPHERAL_CLKSRC"):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate_timer({field: "External"})
        for selection in ("<Select>", "MIO 18 .. 20"):
            with self.subTest(selection=selection), self.assertRaises(ValueError):
                self.validate_timer({"PCW_TTC0_TTC0_IO": selection})

    def test_external_port_rejected(self):
        with self.assertRaises(ValueError):
            self.validate_timer(extra_port="TTC0_WAVE0_OUT")

    def test_unexported_ttc_module_pins_rejected(self):
        for pin in ("TTC0_WAVE0_OUT", "TTC0_CLK0_IN", "TTC1_WAVE1_OUT"):
            with self.subTest(pin=pin), self.assertRaises(ValueError):
                self.validate_timer(internal_port=pin)

    def test_timer_init_risks_remain_classified(self):
        result = self.validate_timer(init="proc ps7_init {} {mask_write 0xF8000104 0xFFFFFFFF 1}\n")
        self.assertIn("GENERIC_DDR_PLL_CLOCK_WRITES", {r["code"] for r in result["init_risks"]})
        self.assertFalse(result["init_execution_authorized"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--validate", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--actual-xsa", type=Path)
    parser.add_argument("--allow-rtos-timer", action="store_true")
    args = parser.parse_args()
    if args.validate:
        result = validate_xsa(args.validate, allow_rtos_timer=args.allow_rtos_timer)
        if args.output:
            args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print("PASS ps_hardware_validation")
    else:
        XsaValidationTests.actual_xsa = args.actual_xsa
        XsaValidationTests.actual_allow_rtos_timer = args.allow_rtos_timer
        unittest.main(argv=[__file__])
