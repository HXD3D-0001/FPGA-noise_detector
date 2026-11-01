"""Ethernet profile and actual-XSA mutation contracts; no board execution."""
import argparse
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

from test_pre_adc_profile import run_tcl

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from check_pre_adc_hardware import validate_variant

XSA = None


class NetworkProfile(unittest.TestCase):
    def test_explicit_network_preserves_sd_uart_ddr_overlay(self):
        result = run_tcl('''
set base [::lfn_pre_adc_profile::overlay [::lfn_pre_adc_profile::make carrier 1]]
set net [::lfn_pre_adc_profile::overlay [::lfn_pre_adc_profile::make carrier 1 1]]
dict for {k v} $base {if {[dict get $net $k] ne $v} {error "base changed $k"}}
foreach {k v} {PCW_EN_ENET0 1 PCW_ENET0_ENET0_IO {MIO 16 .. 27}
 PCW_ENET0_GRP_MDIO_IO {MIO 52 .. 53} PCW_ENET0_RESET_IO {MIO 7} PCW_EN_GPIO 1
 PCW_ENET_RESET_POLARITY {Active Low} PCW_ENET0_PERIPHERAL_CLKSRC {IO PLL}
 PCW_ENET0_PERIPHERAL_FREQMHZ {1000 Mbps}} {
 if {[dict get $net CONFIG.$k] ne $v} {error "wrong $k"}
}
puts PASS
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'PASS')

    def test_electrical_reset_clock_and_pin_mutations_fail(self):
        for key, value in [('PCW_MIO_16_IOTYPE', 'LVCMOS 3.3V'),
                           ('PCW_MIO_27_SLEW', 'slow'),
                           ('PCW_MIO_52_IOTYPE', 'LVCMOS 3.3V'),
                           ('PCW_ENET0_GRP_MDIO_IO', 'EMIO'),
                           ('PCW_ENET0_RESET_IO', 'MIO 8'),
                           ('PCW_ENET_RESET_POLARITY', 'Active High'),
                           ('PCW_ENET0_PERIPHERAL_CLKSRC', 'ARM PLL'),
                           ('PCW_GPIO_MIO_GPIO_ENABLE', '0')]:
            with self.subTest(key=key):
                result = run_tcl('set p [::lfn_pre_adc_profile::make carrier 1 1]\n'
                                 f'dict set p ps_config CONFIG.{key} {{{value}}}\n'
                                 '::lfn_pre_adc_profile::validate $p')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('mismatch', result.stderr)

    def test_actual_network_xsa_and_negative_controls(self):
        if XSA is None:
            self.skipTest('pass --xsa to inspect generated artifact')
        self.assertEqual(validate_variant(XSA, ethernet=True)['status'], 'PASS')
        with self.assertRaises(ValueError):
            validate_variant(XSA)
        with tempfile.TemporaryDirectory() as td, zipfile.ZipFile(XSA) as src:
            for key, value in [('PCW_EN_ENET0', '0'),
                               ('PCW_EN_ENET1', '1'),
                               ('PCW_ENET0_ENET0_IO', 'EMIO'),
                               ('PCW_ENET0_GRP_MDIO_IO', 'MIO 48 .. 49'),
                               ('PCW_MIO_16_IOTYPE', 'LVCMOS 3.3V'),
                               ('PCW_MIO_53_IOTYPE', 'LVCMOS 3.3V'),
                               ('PCW_ENET0_RESET_IO', 'MIO 8'),
                               ('PCW_ENET_RESET_POLARITY', 'Active High'),
                               ('PCW_ENET0_PERIPHERAL_CLKSRC', 'ARM PLL'),
                               ('PCW_ACT_ENET0_PERIPHERAL_FREQMHZ', '25.000000'),
                               ('PCW_EN_SDIO0', '0'), ('PCW_UART1_UART1_IO', 'MIO 14 .. 15'),
                               ('PCW_GPIO_MIO_GPIO_ENABLE', '0'), ('PCW_MIO_7_DIRECTION', 'in')]:
                with self.subTest(key=key):
                    tree = ET.fromstring(src.read('capture_dma.hwh'))
                    node = tree.find(f"./MODULES/MODULE[@INSTANCE='ps7']/PARAMETERS/PARAMETER[@NAME='{key}']")
                    self.assertIsNotNone(node)
                    node.set('VALUE', value)
                    path = Path(td) / 'mutated.xsa'
                    with zipfile.ZipFile(path, 'w') as dst:
                        for name in src.namelist():
                            dst.writestr(name, ET.tostring(tree) if name == 'capture_dma.hwh' else src.read(name))
                    with self.assertRaisesRegex(ValueError, key):
                        validate_variant(path, ethernet=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--xsa', type=Path)
    args, rest = parser.parse_known_args()
    XSA = args.xsa
    unittest.main(argv=[sys.argv[0]] + rest)
