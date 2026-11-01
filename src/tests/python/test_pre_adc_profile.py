"""Exercise the independent board profile in plain Tcl, without Vivado or hardware."""
import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROFILE = ROOT / "bd/ax7021_pre_adc_profile.tcl"
TCLSH = os.environ.get(
    "LFN_TCLSH",
    "tclsh",
)


def run_tcl(body):
    script = "if {[catch {\nsource {%s}\n%s\n} message]} {puts stderr $message; exit 1}\n" % (
        PROFILE.as_posix(), body
    )
    return subprocess.run([TCLSH], input=script, text=True, capture_output=True,
                          encoding="utf-8", timeout=15)


class PreAdcProfileTests(unittest.TestCase):
    def query(self, body):
        result = run_tcl(body)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout.strip()

    def test_default_is_offline_and_has_no_selected_uart_or_sd(self):
        self.assertEqual(self.query("""
set p [::lfn_pre_adc_profile::make]
::lfn_pre_adc_profile::validate $p
puts [list [dict get $p uart] [dict get $p sd0] [dict get $p gate board_ready] [dict get $p gate init_execution_authorized]]
"""), "none 0 0 0")

    def test_all_six_explicit_variants_validate(self):
        self.assertEqual(self.query("""
foreach uart {none carrier core} {
    foreach sd {0 1} {::lfn_pre_adc_profile::validate [::lfn_pre_adc_profile::make $uart $sd]}
}
puts PASS
"""), "PASS")

    def test_carrier_uart_exact_wiring(self):
        self.assertEqual(self.query("""
set c [dict get [::lfn_pre_adc_profile::make carrier] ps_config]
puts [list [dict get $c CONFIG.PCW_EN_UART0] [dict get $c CONFIG.PCW_EN_UART1] [dict get $c CONFIG.PCW_UART1_UART1_IO] [dict get $c CONFIG.PCW_MIO_12_IOTYPE] [dict get $c CONFIG.PCW_UART1_BAUD_RATE]]
"""), "0 1 {MIO 12 .. 13} {LVCMOS 3.3V} 115200")

    def test_core_uart_is_separate(self):
        self.assertEqual(self.query("""
set c [dict get [::lfn_pre_adc_profile::make core] ps_config]
puts [list [dict get $c CONFIG.PCW_EN_UART0] [dict get $c CONFIG.PCW_EN_UART1] [dict get $c CONFIG.PCW_UART0_UART0_IO] [dict get $c CONFIG.PCW_MIO_14_IOTYPE]]
"""), "1 0 {MIO 14 .. 15} {LVCMOS 3.3V}")

    def test_sd_controller_bus_card_detect_and_voltages(self):
        self.assertEqual(self.query("""
set c [dict get [::lfn_pre_adc_profile::make none 1] ps_config]
puts [list [dict get $c CONFIG.PCW_EN_SDIO0] [dict get $c CONFIG.PCW_SD0_SD0_IO] [dict get $c CONFIG.PCW_SD0_GRP_CD_IO] [dict get $c CONFIG.PCW_MIO_10_IOTYPE] [dict get $c CONFIG.PCW_MIO_40_IOTYPE] [dict get $c CONFIG.PCW_PRESET_BANK1_VOLTAGE] [dict get $c CONFIG.PCW_SD0_GRP_WP_ENABLE]]
"""), "1 {MIO 40 .. 45} {MIO 10} {LVCMOS 3.3V} {LVCMOS 1.8V} {LVCMOS 1.8V} 0")

    def test_part_ddr_and_clocks_preserve_official_values(self):
        self.assertEqual(self.query("""
set p [::lfn_pre_adc_profile::make]
set c [dict get $p ps_config]
puts [list [dict get $p part] [dict get $c CONFIG.PCW_UIPARAM_DDR_PARTNO] [dict get $c CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH] [dict get $c CONFIG.PCW_UIPARAM_DDR_FREQ_MHZ] [dict get $c CONFIG.PCW_CRYSTAL_PERIPHERAL_FREQMHZ] [dict get $p clocks pl_ref_pin] [dict get $p clocks pl_ref_mhz]]
"""), "xc7z020clg484-2 {MT41J256M16 RE-125} {32 Bit} 533.333333 33.333333 Y9 50")

    def test_corrupted_electrical_and_functional_configuration_is_rejected(self):
        mutations = [
            ("part", "xc7z020clg400-1"),
            ("ps_config CONFIG.PCW_PRESET_BANK1_VOLTAGE", "LVCMOS 3.3V"),
            ("ps_config CONFIG.PCW_MIO_40_IOTYPE", "LVCMOS 3.3V"),
            ("ps_config CONFIG.PCW_MIO_10_IOTYPE", "LVCMOS 1.8V"),
            ("ps_config CONFIG.PCW_UART1_UART1_IO", "MIO 14 .. 15"),
            ("ps_config CONFIG.PCW_EN_UART0", "1"),
            ("ps_config CONFIG.PCW_SD0_SD0_IO", "MIO 28 .. 33"),
            ("ps_config CONFIG.PCW_SD0_GRP_CD_IO", "MIO 47"),
            ("ps_config CONFIG.PCW_UIPARAM_DDR_PARTNO", "UNKNOWN"),
            ("ps_config CONFIG.PCW_UIPARAM_DDR_FREQ_MHZ", "800"),
            ("ps_config CONFIG.PCW_UIPARAM_DDR_BUS_WIDTH", "16 Bit"),
            ("clocks pl_ref_pin", "E3"),
        ]
        for path, value in mutations:
            with self.subTest(path=path):
                result = run_tcl("set p [::lfn_pre_adc_profile::make carrier 1]\n"
                                 "dict set p %s {%s}\n::lfn_pre_adc_profile::validate $p" % (path, value))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("mismatch", result.stderr)

    def test_missing_facts_cannot_be_promoted_to_ready(self):
        for path, value in [("gate board_ready", "1"),
                            ("gate init_execution_authorized", "1"),
                            ("gate missing_facts", ""),
                            ("facts physical_revision", "CONFIRMED"),
                            ("audio skew_min_ns", "0"),
                            ("audio skew_max_ns", "0")]:
            with self.subTest(path=path):
                result = run_tcl("set p [::lfn_pre_adc_profile::make]\n"
                                 "dict set p %s {%s}\n::lfn_pre_adc_profile::validate $p" % (path, value))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("mismatch", result.stderr)

    def test_missing_unknown_and_duplicate_keys_rejected(self):
        for mutation in ["dict unset p ps_config CONFIG.PCW_PRESET_BANK1_VOLTAGE",
                         "dict set p ps_config CONFIG.PCW_EN_USB0 1",
                         "dict set p surprise 1", "lappend p part xc7z020clg484-2"]:
            with self.subTest(mutation=mutation):
                result = run_tcl("set p [::lfn_pre_adc_profile::make]\n%s\n::lfn_pre_adc_profile::validate $p" % mutation)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("mismatch", result.stderr)

    def test_invalid_selections_rejected(self):
        for args in ["auto 0", "carrier 2", "core true", "none -1"]:
            with self.subTest(args=args):
                result = run_tcl("::lfn_pre_adc_profile::make " + args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("selection", result.stderr)

    def test_sources_include_pinned_primary_evidence(self):
        self.assertEqual(self.query("""
set s [dict get [::lfn_pre_adc_profile::make] sources]
puts [list [dict get $s core_schematic sha256] [dict get $s sd_example member_sha256]]
"""), "bc5ab9c88f28fa10a45cfa0aea091c3eb6109e9a297a1f82bd819dd075d00162 3c8f974643863310592ff780f7d0fe4a9b56496e55b53f98364bd232d5bfea18")

    def test_overlay_replaces_disabled_peripheral_controls(self):
        self.assertEqual(self.query("""
foreach uart {none carrier core} {
    foreach sd {0 1} {
        set o [::lfn_pre_adc_profile::overlay [::lfn_pre_adc_profile::make $uart $sd]]
        foreach name {UART0 UART1} {
            if {[dict get $o CONFIG.PCW_${name}_PERIPHERAL_ENABLE] != [dict get $o CONFIG.PCW_EN_$name]} {error {UART enable mismatch}}
        }
        if {[dict get $o CONFIG.PCW_SD0_PERIPHERAL_ENABLE] != $sd} {error {SD enable mismatch}}
    }
}
puts PASS
"""), "PASS")

    def test_overlay_contains_board_electrical_and_ddr_configuration(self):
        self.assertEqual(self.query("""
set o [::lfn_pre_adc_profile::overlay [::lfn_pre_adc_profile::make carrier 1]]
foreach {name expected} {
    PCW_PRESET_BANK0_VOLTAGE {LVCMOS 3.3V}
    PCW_PRESET_BANK1_VOLTAGE {LVCMOS 1.8V}
    PCW_CRYSTAL_PERIPHERAL_FREQMHZ 33.333333
    PCW_EN_DDR 1 PCW_UIPARAM_DDR_ENABLE 1
    PCW_UIPARAM_DDR_PARTNO {MT41J256M16 RE-125}
    PCW_UIPARAM_DDR_BUS_WIDTH {32 Bit}
    PCW_UIPARAM_DDR_FREQ_MHZ 533.333333
} {
    if {[dict get $o CONFIG.$name] ne $expected} {error "incorrect $name"}
}
puts PASS
"""), "PASS")

    def test_sourcing_is_data_only_and_has_no_vivado_commands(self):
        self.assertEqual(self.query("""
if {[llength [info commands ::lfn_pre_adc_profile::*]] != 4} {error {unexpected public surface}}
puts [llength [info commands create_project]]
"""), "0")


if __name__ == "__main__":
    unittest.main()
