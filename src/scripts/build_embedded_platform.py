"""Generate CPU0 standalone/FreeRTOS BSPs; never execute generated initialization."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import uuid
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
CPU = "ps7_cortexa9_0"
NAME = "ps_only_build"
DOMAINS = {"standalone_cpu0": "standalone", "freertos_cpu0": "freertos"}
NS = "{http://www.xilinx.com/sdx}"
# Capture profile only: FatFs on SD with formatting compiled out. Keys mirror
# vendor xilffs.mld parameters and receive the Vitis "XILFFS_" prefix; values
# use the vendor BSP token forms (strings numerics, ON/OFF booleans).
CAPTURE_FREERTOS_LIBRARY = "xilffs"
CAPTURE_XILFFS_PREFIX = "XILFFS_"
CAPTURE_XILFFS_SETTINGS = {"fs_interface": "1", "read_only": "false", "use_lfn": "0",
                           "use_mkfs": "false", "num_logical_vol": "1"}
CAPTURE_XILFFS_HEADERS = ("ff.h", "ffconf.h", "xilffs_config.h")


def validate_runtime(version, bits, extension_names):
    if len(extension_names) != 1:
        raise ValueError("Require exactly one vendor cygrpc Python ABI extension")
    match = re.fullmatch(r"cygrpc\.cp(\d)(\d+)-win_amd64\.pyd", extension_names[0])
    if not match:
        raise ValueError("Unknown vendor cygrpc Python ABI extension")
    required = tuple(int(v) for v in match.groups())
    required_text = ".".join(map(str, required))
    actual_text = ".".join(map(str, version))
    if tuple(version) != required:
        raise ValueError("Vitis vendor cygrpc requires Python " + required_text +
                         " 64-bit; selected runtime is Python " + actual_text +
                         ". Use the suite's bundled Python executable.")
    if bits != 64:
        raise ValueError("Vitis vendor cygrpc requires a 64-bit Python runtime")
    return {"required_python": required_text, "python": actual_text, "bits": bits,
            "extension": extension_names[0]}


def runtime_preflight(vitis_root):
    extension_dir = Path(vitis_root) / "cli/python-packages/win64/grpc/_cython"
    result = validate_runtime(sys.version_info[:2], struct.calcsize("P") * 8,
                              [p.name for p in extension_dir.glob("cygrpc*.pyd")])
    # Importing modules loads the native extension, but starts no Vitis client.
    import grpc
    import vitis
    result.update(native_import="PASS", executable=str(Path(sys.executable).resolve()))
    return result


def validate_input(xsa, *, allow_rtos_timer=False, allow_candidate_dma=False, allow_capture=False, hardware_receipt=None):
    if sum((allow_rtos_timer, allow_candidate_dma, allow_capture)) > 1:
        raise ValueError('Hardware profile opt-ins are mutually exclusive')
    if allow_capture:
        sys.path.insert(0, str(ROOT / "scripts"))
        import capture_bsp_contract
        return capture_bsp_contract.validate_input(xsa, hardware_receipt)
    if allow_candidate_dma:
        spec = importlib.util.spec_from_file_location('dma_xsa_validation',ROOT/'scripts/check_dma_hardware.py')
        validator = importlib.util.module_from_spec(spec);spec.loader.exec_module(validator)
        result = validator.validate(xsa)
        if result.get('init_execution_authorized') is not False or result.get('board_execution_authorized') is not False:
            raise ValueError('Candidate cannot authorize execution')
        result['hardware_profile']='dma_candidate'
        return result
    spec = importlib.util.spec_from_file_location("embedded_xsa_validation", ROOT / "tests/python/test_ps_hardware.py")
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    result = (validator.validate_xsa(xsa, allow_rtos_timer=True) if allow_rtos_timer
              else validator.validate_xsa(xsa))
    if result.get("init_execution_authorized") is not False:
        raise ValueError("Input validator must forbid generated initialization execution")
    return result


def file_evidence(path, *, archive=False):
    data = path.read_bytes()
    if not data:
        raise ValueError("Empty generated artifact: " + str(path))
    if archive:
        if not data.startswith(b"!<arch>\n"):
            raise ValueError("Invalid BSP archive: " + str(path))
        offset, members = 8, 0
        while offset < len(data):
            header = data[offset:offset + 60]
            if len(header) != 60 or header[58:60] != b"`\n":
                raise ValueError("Truncated BSP archive member: " + str(path))
            size = int(header[48:58].decode("ascii").strip())
            if size < 0 or offset + 60 + size > len(data):
                raise ValueError("Invalid BSP archive bounds: " + str(path))
            offset += 60 + size + size % 2
            members += 1
        if offset != len(data) or not members:
            raise ValueError("Empty or malformed BSP archive: " + str(path))
    return {"path": str(path.resolve()), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def configure_capture_libraries(domain, record):
    """Add the FatFs library to the FreeRTOS domain before the platform build."""
    if domain.set_lib(lib_name=CAPTURE_FREERTOS_LIBRARY) is not True:
        raise ValueError("Vitis rejected library: " + CAPTURE_FREERTOS_LIBRARY)
    for param, value in CAPTURE_XILFFS_SETTINGS.items():
        if domain.set_config(option="lib", param=CAPTURE_XILFFS_PREFIX + param, value=value,
                             lib_name=CAPTURE_FREERTOS_LIBRARY) is not True:
            raise ValueError("Vitis rejected xilffs parameter: " + param)
    record["freertos_libraries"] = {CAPTURE_FREERTOS_LIBRARY: dict(CAPTURE_XILFFS_SETTINGS)}


def configure_network(domain, record):
    """Dedicated Ethernet profile; keep the original storage BSP unchanged."""
    if domain.set_lib(lib_name="lwip220") is not True:
        raise ValueError("Vitis rejected lwip220")
    settings = {"api_mode": "SOCKET_API", "dhcp": "true", "socket_mode_thread_prio": "2"}
    for key, value in settings.items():
        if domain.set_config(option="lib", param="lwip220_" + key, value=value, lib_name="lwip220") is not True:
            raise ValueError("Vitis rejected lwip220 parameter: " + key)
    if domain.set_config(option="os", param="freertos_total_heap_size", value="524288") is not True:
        raise ValueError("Vitis rejected network FreeRTOS heap")
    record["freertos_libraries"]["lwip220"] = settings
    record["network_freertos_heap_bytes"] = 524288


def network_repository(output_root):
    """Local descriptor encoding fix for the Windows frozen empyro loader.

    Runtime sources and license remain verbatim; only the UTF-8 BOM is removed
    from our copy of the library descriptor. Never edit the installed vendor.
    """
    vendor = Path(os.environ["XILINX_VITIS"]) / "data/embeddedsw/ThirdParty/sw_services/lwip220_v1_4"
    repository = output_root.parent / "network_vendor"
    copied = repository / "lwip220"
    def copy_source(source, destination):
        target = Path(destination)
        if target.exists():
            target.chmod(0o666)  # Only our generated copy; vendor stays read-only.
        return shutil.copyfile(source, destination)
    shutil.copytree(vendor, copied, dirs_exist_ok=True, copy_function=copy_source)
    descriptor = copied / "data/lwip220.yaml"
    original = (vendor / "data/lwip220.yaml").read_bytes()
    descriptor.write_bytes(original.removeprefix(b"\xef\xbb\xbf"))
    return repository, {"vendor": str(vendor), "local": str(copied),
        "change": "Remove UTF-8 BOM from local data/lwip220.yaml only",
        "vendor_descriptor_sha256": hashlib.sha256(original).hexdigest(),
        "local_descriptor_sha256": hashlib.sha256(descriptor.read_bytes()).hexdigest()}


def check_xilffs_config(include_dir):
    """Require SD FatFs with one volume and f_mkfs/LFN/read-only compiled out."""
    include_dir = Path(include_dir)
    text = (include_dir / "xilffs_config.h").read_text(encoding="utf-8")
    if not re.search(r"^#define\s+FILE_SYSTEM_INTERFACE_SD\b", text, re.M):
        raise ValueError("xilffs is not configured for the SD interface")
    if re.search(r"^#define\s+FILE_SYSTEM_INTERFACE_RAM\b", text, re.M):
        raise ValueError("xilffs must not enable the RAM interface")
    volumes = re.findall(r"^#define\s+FILE_SYSTEM_NUM_LOGIC_VOL\s+(\S+)", text, re.M)
    if volumes != ["1"]:
        raise ValueError("xilffs must define exactly one logical volume")
    for name in ("FILE_SYSTEM_USE_MKFS", "FILE_SYSTEM_USE_LFN", "FILE_SYSTEM_READ_ONLY"):
        if re.search(r"^#define\s+" + name + r"\b", text, re.M):
            raise ValueError("xilffs parameter must stay disabled: " + name)
    # Generated ffconf.h keeps both branches; the else-branch values must be the
    # ones selected by the xilffs_config.h above (mkfs disabled, read-write, 8.3).
    ffconf = (include_dir / "ffconf.h").read_text(encoding="utf-8")
    for name in ("FF_USE_MKFS", "FF_FS_READONLY", "FF_USE_LFN"):
        if not re.search(r"^#define\s+" + name + r"\s+0\b", ffconf, re.M):
            raise ValueError("generated FatFs configuration is not the expected branch: " + name)
    return {"path": str((include_dir / "xilffs_config.h").resolve()),
            "sha256": hashlib.sha256((include_dir / "xilffs_config.h").read_bytes()).hexdigest(),
            "ffconf_sha256": hashlib.sha256((include_dir / "ffconf.h").read_bytes()).hexdigest(),
            "checks": "SD_ONE_VOLUME_NO_MKFS_NO_LFN_READ_WRITE"}


def check_xilffs_symbols(archive):
    """Prove the linked archive exposes FatFs file IO but no format function."""
    if not os.environ.get("XILINX_VITIS"):
        return {"checked": False, "reason": "REQUIRES_VITIS_ENVIRONMENT"}
    nm = shutil.which("arm-none-eabi-nm")
    if nm is None:
        raise ValueError("ARM nm is not on PATH for the xilffs archive symbol gate")
    result = subprocess.run([nm, "-g", str(archive)], capture_output=True, text=True)
    if result.returncode != 0:
        raise ValueError("Cannot inspect libxilffs.a symbols: " + result.stderr.strip())
    names = {line.split()[-1] for line in result.stdout.splitlines() if line.strip()}
    for name in ("f_mount", "f_open", "f_sync", "f_close"):
        if name not in names:
            raise ValueError("libxilffs.a lacks required symbol: " + name)
    if "f_mkfs" in names:
        raise ValueError("libxilffs.a must be built without f_mkfs")
    return {"path": str(Path(archive).resolve()),
            "sha256": hashlib.sha256(Path(archive).read_bytes()).hexdigest(),
            "checked": True, "nm": str(Path(nm).resolve()),
            "required": ["f_mount", "f_open", "f_sync", "f_close"], "forbidden": ["f_mkfs"]}


def inspect_artifacts(platform_dir, xsa_sha256, xsa_name="ps_only.xsa", platform_name=NAME,
                      require_xilffs=False, require_network=False):
    config = json.loads((platform_dir / "vitis-comp.json").read_text(encoding="utf-8"))["configuration"]
    domains = config["domains"]
    if len(domains) != len(DOMAINS) or {d["name"] for d in domains} != set(DOMAINS):
        raise ValueError("Require exactly the two requested CPU0 domains")
    for domain in domains:
        if (domain["processor"] != CPU or domain["os"] != DOMAINS[domain["name"]] or
                domain.get("isBootDomain") is not False):
            raise ValueError("Unexpected CPU/OS/boot domain metadata")
    if config.get("isCustomFSBL") is not True or config.get("fsblPath") != "":
        raise ValueError("Automatic boot BSP must be disabled")
    export = platform_dir / "export" / platform_name
    artifacts = {"metadata": file_evidence(platform_dir / "vitis-comp.json")}
    for label, directory in (("component_xsa", platform_dir / "hw"), ("export_xsa", export / "hw")):
        item = file_evidence(directory / xsa_name)
        if item["sha256"] != xsa_sha256:
            raise ValueError("Generated hardware differs from validated input")
        artifacts[label] = item
    for domain, os_name in DOMAINS.items():
        bsp = platform_dir / CPU / domain / "bsp"
        libraries = ["libxil.a", "libxilstandalone.a" if os_name == "standalone" else "libfreertos.a"]
        headers = ["xparameters.h", "xil_assert.h"]
        if os_name == "freertos":
            headers += ["FreeRTOS.h", "FreeRTOSConfig.h"]
            if require_xilffs:
                libraries += ["libxilffs.a"]
                headers += list(CAPTURE_XILFFS_HEADERS)
            if require_network:
                libraries += ["liblwip220.a"]
                headers += ["lwipopts.h", "xemacps.h"]
        for library in libraries:
            source = file_evidence(bsp / "lib" / library, archive=True)
            copied = file_evidence(export / "sw" / domain / "lib" / library, archive=True)
            if copied["sha256"] != source["sha256"]:
                raise ValueError("Exported library differs from generated BSP")
            artifacts[domain + "/" + library] = source
            artifacts[domain + "/export/" + library] = copied
        for header in headers:
            artifacts[domain + "/" + header] = file_evidence(bsp / "include" / header)
        if require_xilffs and os_name == "freertos":
            artifacts[domain + "/xilffs_config"] = check_xilffs_config(bsp / "include")
            artifacts[domain + "/xilffs_symbols"] = check_xilffs_symbols(bsp / "lib" / "libxilffs.a")
    xpfm_path = export / (platform_name + ".xpfm")
    platform = ET.parse(xpfm_path).getroot()
    if platform.tag != NS + "platform" or platform.get(NS + "name") != platform_name:
        raise ValueError("Wrong XPFM platform")
    hardware = list(platform.iter(NS + "hardwarePlatform"))
    software = list(platform.iter(NS + "softwarePlatform"))
    if (len(hardware) != 1 or hardware[0].get(NS + "name") != xsa_name or
            hardware[0].get(NS + "path") != "hw" or len(software) != 1 or
            software[0].get(NS + "name") != platform_name + ".spfm" or software[0].get(NS + "path") != "sw"):
        raise ValueError("Unexpected XPFM hardware/software reference")
    spfm_path = export / "sw" / (platform_name + ".spfm")
    groups = list(ET.parse(spfm_path).getroot().iter(NS + "processorGroup"))
    if len(groups) != len(DOMAINS) or {g.get(NS + "name") for g in groups} != set(DOMAINS):
        raise ValueError("Unexpected exported software domains")
    for group in groups:
        os_nodes = list(group.iter(NS + "os"))
        if (group.get(NS + "cpuInstance") != CPU or len(os_nodes) != 1 or
                os_nodes[0].get(NS + "name") != DOMAINS[group.get(NS + "name")]):
            raise ValueError("Exported CPU/OS metadata mismatch")
    artifacts["xpfm"] = file_evidence(xpfm_path)
    artifacts["spfm"] = file_evidence(spfm_path)
    return domains, artifacts


def build_platform(xsa, output_root, *, client_factory=None, disposer=None, launcher_run_id=None, allow_rtos_timer=False, allow_candidate_dma=False, allow_capture=False, hardware_receipt=None, network=False):
    xsa, output_root = Path(xsa).resolve(), Path(output_root).resolve()
    if network and (not allow_capture or output_root == ROOT / "build/capture_bsp"):
        raise ValueError("Network requires capture profile and separate BSP output")
    if allow_capture and output_root in {ROOT / "build/embedded_platform", ROOT / "build/dma_platform"}:
        raise ValueError("Capture platform requires isolated output directory")
    output_root.mkdir(parents=True, exist_ok=True)
    run_id = uuid.uuid4().hex
    platform_name = "capture_build" if allow_capture else NAME
    # FreeRTOS dependency-file paths need a shorter prefix than receipt storage.
    # mkdir below rejects collisions; the complete UUID remains in the receipt.
    workspace_root = output_root.parent / "e"
    workspace = workspace_root / ("w_" + run_id[:8])
    record = {"status": "RUNNING", "stage": "input_validation", "run_id": run_id,
              "launcher_run_id": launcher_run_id, "checked_at_utc": datetime.now(timezone.utc).isoformat(),
              "scope": "BSP_BUILD_ONLY", "board_execution_authorized": False,
              "init_execution_authorized": False, "workspace": str(workspace),
              "platform_dir": str(workspace / platform_name), "no_boot_bsp": True,
              "allow_rtos_timer": allow_rtos_timer,
              "allow_candidate_dma": allow_candidate_dma,
              "allow_capture": allow_capture,
              "network": network,
              "client_mode": "TEST_INJECTED" if client_factory is not None else "VITIS",
              "client_disposed": False}
    client, drain_thread, workspace_created = None, None, False
    managed_server = None

    def save():
        data = (json.dumps(record, indent=2) + "\n").encode("utf-8")
        temporary = output_root / (run_id + ".status.tmp")
        temporary.write_bytes(data)
        temporary.replace(output_root / "status.json")
        (output_root / "status.txt").write_bytes((record["status"] + " embedded_platform BUILD_ONLY\n").encode("ascii"))

    save()
    try:
        validation = validate_input(xsa, allow_rtos_timer=allow_rtos_timer,allow_candidate_dma=allow_candidate_dma,
                                    allow_capture=allow_capture, hardware_receipt=hardware_receipt)
        record["xsa_sha256"] = validation["xsa_sha256"]
        record["input_profile"] = validation.get("hardware_profile", "ps_only")
        if network != (record["input_profile"] == "pre_adc_carrier_sd0_ethernet"):
            raise ValueError("Ethernet hardware and BSP network option must match")
        record["input_timer_fields"] = validation.get("timer_fields", {})
        record["input_init_risks"] = validation["init_risks"]
        if allow_candidate_dma:
            record['memory_plan']=validation['memory_plan']
            record['board_profile_status']=validation['board_profile_status']
        if allow_capture:
            record["capture_contract"] = validation
            tracked = [Path(__file__), ROOT / "scripts/capture_bsp_contract.py",
                       ROOT / "scripts/build_embedded_platform.ps1"]
            record["source_hashes"] = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
        workspace_root.mkdir(parents=True, exist_ok=True)
        workspace.mkdir()
        workspace_created = True
        if client_factory is None:
            if allow_capture:
                record["stage"] = "embedded_validation"; save()
                from check_embedded_tools import check_installation
                record["embedded_tools"] = check_installation(Path(os.environ["XILINX_VITIS"]).parent,
                                                               output_root / "embedded_tools")
                if record["embedded_tools"]["status"] != "PASS":
                    raise ValueError("Complete Embedded installation and ARM compiler proof required")
            record["stage"] = "runtime_validation"; save()
            record["runtime"] = runtime_preflight(os.environ["XILINX_VITIS"])
            import vitis
            import platform_pb2
            # Installed API returns the protobuf enum's Python int, not bool.
            if platform_pb2.PlatformResponse.ResponseStatusType.Value("SUCCESS") != 0:
                raise ValueError("Unknown installed platform success enum")
            client_factory, disposer = vitis.create_client, vitis.dispose
            # This installation's vitis-server.bat emits a GBK console message
            # for a missing lopper environment script path (long-known install
            # mismatch, see the workspace report); create_client()'s in-process
            # utf-8 stdout port discovery crashes on that first line. With
            # LFN_VITIS_SERVER_PORT we start the server ourselves (output
            # binary-redirected to a log), wait for the gRPC port, then attach
            # by port so no server stdout is ever decoded.
            server_port = os.environ.get("LFN_VITIS_SERVER_PORT", "")
            if server_port.isdigit():
                server_port = int(server_port)
                import socket
                import subprocess
                import time
                with socket.socket() as probe:
                    already = probe.connect_ex(("127.0.0.1", server_port)) == 0
                if not already:
                    vitis_bin = Path(os.environ["XILINX_VITIS"]) / "bin" / "vitis-server.bat"
                    server_log = (output_root / "vitis_server.log").open("wb")
                    managed_server = subprocess.Popen(f'"{vitis_bin}" -p {server_port}',
                                                      shell=True, stdout=server_log,
                                                      stderr=subprocess.STDOUT)
                    deadline = time.monotonic() + 180
                    while time.monotonic() < deadline:
                        with socket.socket() as wait:
                            if wait.connect_ex(("127.0.0.1", server_port)) == 0:
                                break
                        if managed_server.poll() is not None:
                            raise ValueError("vitis-server exited during startup; see vitis_server.log")
                        time.sleep(2)
                    else:
                        raise ValueError("vitis-server did not become ready in time; see vitis_server.log")
                client_factory = lambda workspace: vitis.create_client(port=server_port, workspace=workspace)
        if disposer is None:
            raise ValueError("An owned-client disposer is required")
        record["stage"] = "client"; save()
        client = client_factory(workspace=str(workspace))
        if client is None or client.info() is not True:
            raise ValueError("Vitis client did not confirm connection")
        if network:
            repository, record["network_repository"] = network_repository(output_root)
            if client.add_embedded_sw_repo(level="LOCAL", path=str(repository)) is not True:
                raise ValueError("Could not register workspace-local lwIP repository")
        stream = getattr(getattr(getattr(client, "_serverObj", None), "cp", None), "stdout", None)
        if stream is not None:
            def drain_server():
                with (workspace / "server.log").open("wb") as log:
                    for line in iter(stream.readline, b""):
                        log.write(line); log.flush()
            drain_thread = threading.Thread(target=drain_server, daemon=True)
            drain_thread.start()
        record["stage"] = "processor_os"; save()
        supported = client.get_processor_os_list(xsa=str(xsa))
        pairs = {(entry.os, cpu) for entry in supported.os_processors for cpu in entry.cpuInstance}
        if not {(os_name, CPU) for os_name in DOMAINS.values()} <= pairs:
            raise ValueError("XSA does not support CPU0 standalone and FreeRTOS")
        record["processor_os"] = [{"os": os_name, "cpu": cpu} for os_name, cpu in sorted(pairs)]
        record["stage"] = "platform_create"; save()
        platform = client.create_platform_component(name=platform_name, hw_design=str(xsa),
            cpu=CPU, os="standalone", domain_name="standalone_cpu0", no_boot_bsp=True,
            generate_dtb=False, compiler="gcc")
        if platform is None:
            raise ValueError("No platform component returned")
        freertos_domain = platform.add_domain(cpu=CPU, os="freertos", name="freertos_cpu0",
                                              generate_dtb=False, compiler="gcc")
        if freertos_domain is None:
            raise ValueError("No FreeRTOS domain returned")
        if allow_capture:
            record["stage"] = "capture_libraries"; save()
            configure_capture_libraries(freertos_domain, record)
            if network:
                configure_network(freertos_domain, record)
        record["stage"] = "platform_build"; save()
        result = platform.build()
        record["platform_build_status"] = result if type(result) in (int, bool, str, type(None)) else repr(result)
        if type(result) is not int or result != 0:
            raise ValueError("Platform build must return integer SUCCESS=0")
        record["stage"] = "artifact_validation"; save()
        if hashlib.sha256(xsa.read_bytes()).hexdigest() != record["xsa_sha256"]:
            raise ValueError("Input XSA changed during build")
        record["domains"], record["artifacts"] = inspect_artifacts(workspace / platform_name, record["xsa_sha256"], xsa.name, platform_name, require_xilffs=allow_capture, require_network=network)
        if allow_capture:
            record["stage"] = "capture_bsp_validation"; save()
            import capture_bsp_contract
            fresh = capture_bsp_contract.validate_input(xsa, hardware_receipt)
            if fresh != validation:
                raise ValueError("Capture hardware evidence changed during build")
            for path, expected in record["source_hashes"].items():
                if hashlib.sha256(Path(path).read_bytes()).hexdigest() != expected:
                    raise ValueError("Platform builder source changed during build: " + path)
            record["capture_bsp_checks"] = capture_bsp_contract.inspect_platform(workspace / platform_name, validation, DOMAINS)
            header = workspace / platform_name / "capture_map.h"
            header.write_text(capture_bsp_contract.map_header(validation), encoding="utf-8")
            record["artifacts"]["capture_map"] = file_evidence(header)
        record.update(status="PASS", stage="complete")
    except BaseException as error:
        record.update(status="FAIL", error=type(error).__name__ + ": " + str(error))
    finally:
        if client is not None:
            try:
                disposer()
                record["client_disposed"] = True
            except BaseException as error:
                record.update(status="FAIL", cleanup_error=type(error).__name__ + ": " + str(error))
        if drain_thread is not None:
            drain_thread.join(timeout=1)
        if managed_server is not None:
            managed_server.terminate()
        record["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        save()
        if workspace_created:
            (workspace / "receipt.json").write_bytes((json.dumps(record, indent=2) + "\n").encode("utf-8"))
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xsa", type=Path)
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--launcher-run-id")
    parser.add_argument("--allow-rtos-timer", action="store_true")
    parser.add_argument("--allow-candidate-dma", action="store_true")
    parser.add_argument("--allow-capture", action="store_true")
    parser.add_argument("--network", action="store_true")
    parser.add_argument("--hardware-receipt", type=Path)
    parser.add_argument("--runtime-preflight", action="store_true")
    parser.add_argument("--vitis-root", type=Path)
    args = parser.parse_args()
    if args.runtime_preflight:
        try:
            if args.vitis_root is None:
                raise ValueError("Runtime preflight requires --vitis-root")
            print(json.dumps(runtime_preflight(args.vitis_root)), flush=True)
            print("PASS embedded_runtime BUILD_ONLY", flush=True)
        except Exception as error:
            print("FAIL embedded_runtime BUILD_ONLY: " + type(error).__name__ + ": " + str(error), flush=True)
            raise SystemExit(1)
        raise SystemExit(0)
    if args.xsa is None:
        args.xsa = ROOT / ("build/capture_hardware/capture_dma.xsa" if args.allow_capture else "build/ps_hardware/ps_only.xsa")
    if args.output_root is None:
        args.output_root = ROOT / ("build/capture_network_bsp" if args.network else "build/capture_bsp" if args.allow_capture else "build/embedded_platform")
    receipt = build_platform(args.xsa, args.output_root, launcher_run_id=args.launcher_run_id,
                             allow_rtos_timer=args.allow_rtos_timer,allow_candidate_dma=args.allow_candidate_dma,
                             allow_capture=args.allow_capture, hardware_receipt=args.hardware_receipt, network=args.network)
    print(receipt["status"] + " embedded_platform BUILD_ONLY", flush=True)
    raise SystemExit(0 if receipt["status"] == "PASS" else 1)
