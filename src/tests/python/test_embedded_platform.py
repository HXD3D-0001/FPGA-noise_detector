"""Receipt and generated-artifact gates; isolated fixtures never execute init."""
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("platform_builder", ROOT / "scripts/build_embedded_platform.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
spec = importlib.util.spec_from_file_location("platform_xsa_fixture", ROOT / "tests/python/test_ps_hardware.py")
xsa_fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xsa_fixture)
NS = "http://www.xilinx.com/sdx"

class CandidateOptInTest(unittest.TestCase):
    def test_profiles_cannot_be_combined(self):
        with self.assertRaisesRegex(ValueError, 'mutually exclusive'):
            builder.validate_input(Path('not-used.xsa'),allow_rtos_timer=True,allow_candidate_dma=True)


def archive():
    payload = b"fixture member\n"
    header = ("fixture.o/".ljust(16) + "0".ljust(12) + "0".ljust(6) +
              "0".ljust(6) + "100644".ljust(8) + str(len(payload)).ljust(10) + "`\n").encode()
    return b"!<arch>\n" + header + payload + (b"\n" if len(payload) % 2 else b"")


class FixtureClient:
    def __init__(self, workspace, xsa, mutation=None, status=0, supported=True):
        self.workspace, self.xsa = Path(workspace), xsa
        self.mutation, self.status, self.supported = mutation, status, supported
        self.kwargs = None
        self.domains = []

    def info(self):
        return True

    def get_processor_os_list(self, xsa):
        cpu = "ps7_cortexa9_0" if self.supported else "ps7_cortexa9_1"
        return SimpleNamespace(os_processors=[SimpleNamespace(os=os, cpuInstance=[cpu])
                                              for os in ("standalone", "freertos")])

    def create_platform_component(self, **kwargs):
        self.kwargs = kwargs
        self.domains.append({"name": kwargs["domain_name"], "processor": kwargs["cpu"],
                             "os": kwargs["os"], "isBootDomain": False})
        return self

    def add_domain(self, **kwargs):
        self.domains.append({"name": kwargs["name"], "processor": kwargs["cpu"],
                             "os": kwargs["os"], "isBootDomain": False})
        return SimpleNamespace(name=kwargs["name"])

    def build(self):
        component = self.workspace / self.kwargs["name"]
        exported = component / "export" / self.kwargs["name"]
        config = {"configuration": {"domains": self.domains, "isCustomFSBL": True, "fsblPath": ""}}
        component.mkdir()
        (component / "vitis-comp.json").write_text(json.dumps(config))
        for directory in (component / "hw", exported / "hw"):
            directory.mkdir(parents=True)
            shutil.copyfile(self.xsa, directory / self.xsa.name)
        for domain in self.domains:
            bsp = component / "ps7_cortexa9_0" / domain["name"] / "bsp"
            export_bsp = exported / "sw" / domain["name"]
            for prefix in (bsp, export_bsp):
                (prefix / "lib").mkdir(parents=True)
                (prefix / "include").mkdir()
                libs = ["libxil.a", "libxilstandalone.a" if domain["os"] == "standalone" else "libfreertos.a"]
                headers = ["xparameters.h", "xil_assert.h"]
                if domain["os"] == "freertos":
                    headers += ["FreeRTOS.h", "FreeRTOSConfig.h"]
                for lib in libs:
                    (prefix / "lib" / lib).write_bytes(archive())
                for header in headers:
                    (prefix / "include" / header).write_text("#define FIXTURE_HEADER 1\n")
        platform = ET.Element("{" + NS + "}platform", {"{" + NS + "}name": self.kwargs["name"]})
        ET.SubElement(platform, "{" + NS + "}hardwarePlatform", {"{" + NS + "}name": self.xsa.name, "{" + NS + "}path": "hw"})
        ET.SubElement(platform, "{" + NS + "}softwarePlatform", {"{" + NS + "}name": self.kwargs["name"] + ".spfm", "{" + NS + "}path": "sw"})
        (exported / (self.kwargs["name"] + ".xpfm")).write_bytes(ET.tostring(platform))
        software = ET.Element("{" + NS + "}softwarePlatform")
        for domain in self.domains:
            group = ET.SubElement(software, "{" + NS + "}processorGroup", {
                "{" + NS + "}name": domain["name"], "{" + NS + "}cpuInstance": domain["processor"]})
            ET.SubElement(group, "{" + NS + "}os", {"{" + NS + "}name": domain["os"]})
        (exported / "sw" / (self.kwargs["name"] + ".spfm")).write_bytes(ET.tostring(software))
        if self.mutation:
            self.mutation(component)
        return self.status


class PlatformTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.xsa = self.root / "ps_only.xsa"
        xsa_fixture.mock_xsa(self.xsa)
        self.output = self.root / "output"
        self.clients, self.closed = [], []

    def run_build(self, **options):
        def factory(workspace):
            client = FixtureClient(workspace, self.xsa, **options)
            self.clients.append(client)
            return client
        return builder.build_platform(self.xsa, self.output, client_factory=factory,
                                      disposer=lambda: self.closed.append(True))

    def assert_fail(self, **options):
        record = self.run_build(**options)
        self.assertEqual(record["status"], "FAIL")
        self.assertEqual(json.loads((self.output / "status.json").read_text())["status"], "FAIL")
        self.assertFalse(record["init_execution_authorized"])
        return record

    def test_build_checks_real_files_and_records_absolute_platform(self):
        record = self.run_build()
        self.assertEqual(record["status"], "PASS", record)
        self.assertIn("platform_dir", record)
        self.assertTrue(Path(record["platform_dir"]).is_absolute())
        self.assertTrue(record["artifacts"])
        self.assertEqual(self.closed, [True])
        kwargs = self.clients[0].kwargs
        self.assertIs(kwargs["no_boot_bsp"], True)
        self.assertIs(kwargs["generate_dtb"], False)
        self.assertNotIn("architecture", kwargs)
        self.assertFalse(record["board_execution_authorized"])

    def test_missing_xsa_replaces_stale_pass_without_starting_client(self):
        self.output.mkdir()
        (self.output / "status.json").write_text('{"status":"PASS"}')
        self.xsa.unlink()
        self.assert_fail()
        self.assertEqual(self.clients, [])

    def test_fresh_workspace_name_keeps_windows_cmake_paths_short(self):
        first, second = self.run_build(), self.run_build()
        self.assertNotEqual(first["workspace"], second["workspace"])
        self.assertLessEqual(len(Path(second["workspace"]).name), 10)

    def test_named_xsa_and_output_paths_with_spaces_are_preserved(self):
        renamed = self.root / "ps rtos.xsa"
        self.xsa.rename(renamed)
        self.xsa = renamed
        self.output = self.root / "output with spaces"
        record = self.run_build()
        self.assertEqual(record["status"], "PASS", record)
        self.assertTrue((self.output / "status.json").is_file())
        self.assertEqual(Path(record["artifacts"]["export_xsa"]["path"]).name, "ps rtos.xsa")

    def test_rtos_timer_profile_requires_explicit_opt_in(self):
        xsa_fixture.mock_xsa(self.xsa, changes={"PCW_EN_TTC0": "1", "PCW_TTC0_PERIPHERAL_ENABLE": "1",
            "PCW_EN_EMIO_TTC0": "0", "PCW_TTC0_TTC0_IO": "EMIO",
            **{"PCW_TTC0_CLK" + str(i) + "_PERIPHERAL_CLKSRC": "CPU_1X" for i in range(3)}})
        self.assert_fail()
        record = builder.build_platform(self.xsa, self.output,
            client_factory=lambda workspace: FixtureClient(workspace, self.xsa),
            disposer=lambda: None, allow_rtos_timer=True)
        self.assertEqual(record["status"], "PASS", record)

    def test_workspace_collision_fails_without_reusing_old_files(self):
        with patch.object(builder.uuid, "uuid4", return_value=SimpleNamespace(hex="a" * 32)):
            first = self.run_build()
            self.assertEqual(first["status"], "PASS")
            receipt_path = Path(first["workspace"]) / "receipt.json"
            receipt = receipt_path.read_bytes()
            self.assert_fail()
            self.assertEqual(receipt_path.read_bytes(), receipt)
        self.assertEqual(len(self.clients), 1)

    def test_actual_freertos_dependency_paths_fit_windows_max_path(self):
        self.output = self.root / "build/embedded_platform"
        record = self.run_build()
        self.assertEqual(record["status"], "PASS", record)
        working = Path(record["platform_dir"]) / "ps7_cortexa9_0/freertos_cpu0/bsp/libsrc/build_configs/gen_bsp"
        # Relative -MF arguments from the actual failed FreeRTOS compile log.
        for name in ("port_asm_vectors.S.obj.d", "portZynq7000.c.obj.d"):
            dependency = working / "libsrc/freertos10_xilinx/src/CMakeFiles/freertos.dir/Source/portable/GCC/ARM_CA9" / name
            self.assertLess(len(str(dependency)), 260, str(dependency))
        self.assertEqual(Path(record["workspace"]).parent, self.output.parent / "e")

    def test_xsa_with_enabled_ddr_rejected_before_client(self):
        xsa_fixture.mock_xsa(self.xsa, changes={"PCW_EN_DDR": "1"})
        self.assert_fail()
        self.assertEqual(self.clients, [])

    def test_wrong_api_cpu_rejected_and_owned_client_closed(self):
        self.assert_fail(supported=False)
        self.assertEqual(self.closed, [True])

    def test_integer_failure_and_in_progress_are_rejected(self):
        for value in (1, 2, -1):
            with self.subTest(value=value):
                self.assert_fail(status=value)

    def test_boolean_false_and_true_and_none_are_not_success_enum(self):
        for value in (False, True, None, "0"):
            with self.subTest(value=value):
                self.assert_fail(status=value)

    def test_missing_library_is_not_masked_by_prior_workspace(self):
        old = self.run_build()
        def remove(component):
            (component / "ps7_cortexa9_0/standalone_cpu0/bsp/lib/libxilstandalone.a").unlink()
        failed = self.assert_fail(mutation=remove)
        self.assertNotEqual(old["workspace"], failed["workspace"])

    def test_corrupt_archive_rejected(self):
        self.assert_fail(mutation=lambda p: (p / "ps7_cortexa9_0/freertos_cpu0/bsp/lib/libxil.a").write_bytes(b"old PASS"))

    def test_missing_freertos_header_rejected(self):
        self.assert_fail(mutation=lambda p: (p / "ps7_cortexa9_0/freertos_cpu0/bsp/include/FreeRTOS.h").unlink())

    def change_config(self, action):
        def mutation(component):
            path = component / "vitis-comp.json"
            config = json.loads(path.read_text())
            action(config["configuration"])
            path.write_text(json.dumps(config))
        return mutation

    def test_boot_bsp_enabled_metadata_rejected(self):
        self.assert_fail(mutation=self.change_config(lambda c: c.update(isCustomFSBL=False)))

    def test_domain_cpu_and_os_mismatch_metadata_rejected(self):
        for field, value in (("processor", "ps7_cortexa9_1"), ("os", "linux"), ("isBootDomain", True)):
            with self.subTest(field=field):
                self.assert_fail(mutation=self.change_config(lambda c: c["domains"][0].update({field: value})))

    def test_wrong_export_xsa_rejected(self):
        self.assert_fail(mutation=lambda p: (p / "export/ps_only_build/hw/ps_only.xsa").write_bytes(b"wrong hardware"))

    def test_wrong_xpfm_reference_rejected(self):
        self.assert_fail(mutation=lambda p: (p / "export/ps_only_build/ps_only_build.xpfm").write_text("<platform/>"))

    def test_dispose_failure_prevents_pass(self):
        def disposer():
            raise RuntimeError("fixture cleanup failed")
        record = builder.build_platform(self.xsa, self.output,
            client_factory=lambda workspace: FixtureClient(workspace, self.xsa), disposer=disposer)
        self.assertEqual(record["status"], "FAIL")



def fixture_platform(root):
    xsa = root / "ps_only.xsa"
    xsa_fixture.mock_xsa(xsa)
    workspace = root / "ws"
    workspace.mkdir(parents=True, exist_ok=True)
    client = FixtureClient(workspace, xsa)
    client.create_platform_component(name="ps_only_build", hw_design=str(xsa), cpu="ps7_cortexa9_0",
                                     os="standalone", domain_name="standalone_cpu0",
                                     no_boot_bsp=True, generate_dtb=False, compiler="gcc")
    client.add_domain(cpu="ps7_cortexa9_0", os="freertos", name="freertos_cpu0",
                      generate_dtb=False, compiler="gcc")
    client.build()
    return workspace / "ps_only_build", xsa


def add_xilffs(component, config):
    bsp = component / "ps7_cortexa9_0/freertos_cpu0/bsp"
    export = component / "export/ps_only_build/sw/freertos_cpu0"
    for prefix in (bsp, export):
        (prefix / "lib").mkdir(parents=True, exist_ok=True)
        (prefix / "include").mkdir(parents=True, exist_ok=True)
        (prefix / "lib/libxilffs.a").write_bytes(archive())
        for name in ("ff.h", "ffconf.h"):
            (prefix / "include" / name).write_text("#define FIXTURE_FFS 1\n")
        (prefix / "include/xilffs_config.h").write_text(config)
        (prefix / "include/ffconf.h").write_text(FIXTURE_FFCONF)


FIXTURE_FFCONF = ("#define FF_USE_MKFS\t\t0\n"
                  "#define FF_FS_READONLY\t0\n"
                  "#define FF_USE_LFN\t0\n"
                  "#define FF_FS_MINIMIZE\t0\n")
FIXTURE_FFS_CONFIG = ("#define XPAR_XSDPS_0_BASEADDR 0xE0100000\n"
                      "#define FILE_SYSTEM_INTERFACE_SD\n"
                      "#define FILE_SYSTEM_NUM_LOGIC_VOL 1\n"
                      "#define FILE_SYSTEM_WORD_ACCESS\n"
                      "/* #undef FILE_SYSTEM_USE_MKFS */\n"
                      "/* #undef FILE_SYSTEM_USE_LFN */\n")


class CaptureLibraryTests(unittest.TestCase):
    class Domain:
        def __init__(self, accept=True):
            self.accept, self.libs, self.configs = accept, [], []

        def set_lib(self, lib_name):
            self.libs.append(lib_name)
            return self.accept

        def set_config(self, option, param, value, lib_name):
            self.configs.append((option, param, value, lib_name))
            return self.accept

    def test_capture_domain_adds_xilffs_with_sd_no_mkfs_settings(self):
        record, domain = {}, self.Domain()
        builder.configure_capture_libraries(domain, record)
        self.assertEqual(domain.libs, ["xilffs"])
        self.assertEqual({param.removeprefix("XILFFS_"): value for _, param, value, _ in domain.configs},
                         builder.CAPTURE_XILFFS_SETTINGS)
        self.assertTrue(all(option == "lib" and lib == "xilffs" for option, _, _, lib in domain.configs))
        self.assertTrue(all(param.startswith("XILFFS_")
                            for _, param, _, _ in domain.configs))
        self.assertEqual(record["freertos_libraries"], {"xilffs": builder.CAPTURE_XILFFS_SETTINGS})

    def test_rejected_library_or_parameter_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "library"):
            builder.configure_capture_libraries(self.Domain(accept=False), {})

    def test_xilffs_config_requires_sd_volume_and_no_mkfs(self):
        with tempfile.TemporaryDirectory() as temp:
            include = Path(temp)
            (include / "ffconf.h").write_text(FIXTURE_FFCONF)
            config = include / "xilffs_config.h"
            config.write_text(FIXTURE_FFS_CONFIG)
            info = builder.check_xilffs_config(include)
            self.assertEqual(info["checks"], "SD_ONE_VOLUME_NO_MKFS_NO_LFN_READ_WRITE")
            for bad in ("#define FILE_SYSTEM_USE_MKFS\n",
                        "#define FILE_SYSTEM_USE_LFN 2\n",
                        "#define FILE_SYSTEM_READ_ONLY\n",
                        "#define FILE_SYSTEM_NUM_LOGIC_VOL 2\n"):
                with self.subTest(bad=bad.strip()):
                    config.write_text(FIXTURE_FFS_CONFIG + bad)
                    with self.assertRaises(ValueError):
                        builder.check_xilffs_config(include)
            config.write_text(FIXTURE_FFS_CONFIG.replace("FILE_SYSTEM_INTERFACE_SD", "FILE_SYSTEM_INTERFACE_RAM"))
            with self.assertRaisesRegex(ValueError, "SD interface"):
                builder.check_xilffs_config(include)
            config.write_text(FIXTURE_FFS_CONFIG)
            (include / "ffconf.h").write_text(FIXTURE_FFCONF.replace("#define FF_USE_MKFS\t\t0", "#define FF_USE_MKFS\t\t1"))
            with self.assertRaisesRegex(ValueError, "expected branch"):
                builder.check_xilffs_config(include)

    def test_artifact_inspection_requires_xilffs_archive_and_config(self):
        with tempfile.TemporaryDirectory() as temp:
            component, xsa = fixture_platform(Path(temp))
            sha = __import__("hashlib").sha256(xsa.read_bytes()).hexdigest()
            add_xilffs(component, FIXTURE_FFS_CONFIG)
            with patch.dict("os.environ", {"XILINX_VITIS": ""}):
                _, artifacts = builder.inspect_artifacts(component, sha, xsa.name, "ps_only_build",
                                                         require_xilffs=True)
            self.assertIn("freertos_cpu0/libxilffs.a", artifacts)
            self.assertEqual(artifacts["freertos_cpu0/ff.h"]["size"] > 0, True)
            self.assertIn("freertos_cpu0/xilffs_config", artifacts)
            (component / "ps7_cortexa9_0/freertos_cpu0/bsp/lib/libxilffs.a").unlink()
            with patch.dict("os.environ", {"XILINX_VITIS": ""}), self.assertRaises(FileNotFoundError):
                builder.inspect_artifacts(component, sha, xsa.name, "ps_only_build", require_xilffs=True)

class RuntimeTests(unittest.TestCase):
    extension = "cygrpc.cp313-win_amd64.pyd"

    def test_vendor_python_313_64_bit_matches_grpc_extension(self):
        info = builder.validate_runtime((3, 13), 64, [self.extension])
        self.assertEqual(info["required_python"], "3.13")

    def test_python_312_rejected_before_vendor_import(self):
        with self.assertRaisesRegex(ValueError, "requires Python 3.13.*3.12"):
            builder.validate_runtime((3, 12), 64, [self.extension])

    def test_wrong_bitness_rejected(self):
        with self.assertRaisesRegex(ValueError, "64-bit"):
            builder.validate_runtime((3, 13), 32, [self.extension])

    def test_missing_or_unknown_vendor_extension_rejected(self):
        for names in ([], ["cygrpc.pyd"], [self.extension, "cygrpc.cp312-win_amd64.pyd"]):
            with self.subTest(names=names), self.assertRaises(ValueError):
                builder.validate_runtime((3, 13), 64, names)


if __name__ == "__main__":
    unittest.main()