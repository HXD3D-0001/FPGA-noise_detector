"""Test the public CLI offline; simulate only the PYNQ board adapter."""

import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest


ROOT = Path(__file__).resolve().parent
EXAMPLES = ROOT / "examples"
CHECKER = ROOT / "check_overlay.py"
FAKE_PYNQ = """
import json
from pathlib import Path

def record(event):
    with open('calls.txt', 'a') as stream:
        print(event, file=stream)

class Overlay:
    def __init__(self, bitfile_name, download=True):
        assert Path(bitfile_name).is_absolute()
        record('construct download=' + str(download))
        self.ip_dict = json.loads(Path('metadata.json').read_text())

    def download(self):
        record('download')

    def is_loaded(self):
        record('is_loaded')
        return not Path('not_loaded').exists()
"""


class OverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.contract = json.loads((EXAMPLES / "gpio_contract.json").read_text())
        self.metadata = json.loads((EXAMPLES / "gpio_metadata.json").read_text())

    def invoke(self, *args, code=0):
        for name, data in [('contract', self.contract), ('metadata', self.metadata)]:
            (self.work / (name + '.json')).write_text(json.dumps(data))
        env = dict(os.environ, PYTHONPATH=str(self.work))
        result = subprocess.run(
            [sys.executable, "-B", str(CHECKER), "contract.json", *args],
            cwd=self.work, env=env, text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, code, result.stderr or result.stdout)
        return json.loads(result.stdout) if code == 0 else result.stderr

    def offline(self, *args, **kwargs):
        return self.invoke("--metadata", "metadata.json", *args, **kwargs)

    def board(self, *args, **kwargs):
        (self.work / "board.bit").touch()
        (self.work / "pynq.py").write_text(FAKE_PYNQ)
        return self.invoke("--bit", "board.bit", *args, **kwargs)

    def calls(self):
        path = self.work / "calls.txt"
        return path.read_text().splitlines() if path.exists() else []

    def test_supplied_example(self):
        result = self.offline()
        self.assertEqual(result["instance"], "panel/buttons")
        self.assertEqual(result["scope"], "offline-metadata")

    def test_moved_instance_and_base(self):
        ip = self.metadata.pop("panel/buttons")
        ip["phys_addr"] = 0xA0100000
        self.metadata["io/keys"] = ip
        result = self.offline()
        self.assertEqual(result["instance"], "io/keys")
        self.assertEqual(result["base_address"], 0xA0100000)

    def test_different_design(self):
        self.contract = {"vlnv": "demo.org:hls:analyzer:1.0", "registers": {"STATUS": 16}}
        self.metadata = {"analyzer_0": {"type": self.contract["vlnv"],
                         "registers": {"STATUS": {"address_offset": "0x10"}}}}
        self.assertEqual(self.offline()["instance"], "analyzer_0")

    def test_wrong_vlnv_version(self):
        self.metadata["panel/buttons"]["type"] = "xilinx.com:ip:axi_gpio:1.0"
        self.assertIn("no IP", self.offline(code=1))

    def test_ambiguous_without_selector(self):
        self.metadata["panel/switches"] = self.metadata["panel/buttons"]
        self.assertIn("--ip", self.offline(code=1))
        self.assertEqual(self.offline("--ip", "panel/switches")["instance"], "panel/switches")

    def test_invalid_explicit_selector(self):
        self.assertIn("panel/missing", self.offline("--ip", "panel/missing", code=1))

    def test_missing_register(self):
        del self.metadata["panel/buttons"]["registers"]["GPIO_TRI"]
        self.assertIn("GPIO_TRI", self.offline(code=1))

    def test_wrong_offset(self):
        self.metadata["panel/buttons"]["registers"]["GPIO_TRI"]["address_offset"] = 12
        self.assertIn("GPIO_TRI", self.offline(code=1))

    def test_bad_contract(self):
        self.contract["registers"] = {}
        self.assertIn("register", self.offline(code=1))

    def test_offline_refuses_download(self):
        self.assertIn("--bit", self.offline("--download", code=2))

    def test_missing_hwh(self):
        self.assertIn(".hwh", self.board("--download", code=1))
        self.assertEqual(self.calls(), [])

    def test_default_on_board_does_not_download(self):
        (self.work / "board.hwh").touch()
        self.assertEqual(self.board()["scope"], "pynq-metadata")
        self.assertEqual(self.calls(), ["construct download=False"])

    def test_invalid_contract_never_downloads(self):
        (self.work / "board.hwh").touch()
        self.contract["registers"]["MISSING"] = 24
        self.board("--download", code=1)
        self.assertEqual(self.calls(), ["construct download=False"])

    def test_explicit_download_then_check(self):
        (self.work / "board.hwh").touch()
        self.assertEqual(self.board("--download")["scope"], "pynq-loaded")
        self.assertEqual(self.calls(), ["construct download=False", "download", "is_loaded"])

    def test_load_status_failure(self):
        (self.work / "board.hwh").touch()
        (self.work / "not_loaded").touch()
        self.assertIn("is_loaded", self.board("--download", code=1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
