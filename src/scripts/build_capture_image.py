"""Build a separate CAP1/DAT1/CTR1 FreeRTOS ELF; never execute it."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import re
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_bsp_contract as contract
from check_runtime_elf import inspect_loads, inspect_symbols
ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["ps/app/capture_uart_start.c", "ps/app/storage_rotation_port.c", "ps/app/capture_control.c", "ps/app/capture_request.c", "ps/app/pre_adc_controller.c", "ps/app/pre_adc_diag.c", "ps/app/capture_storage_route.c", "ps/app/capture_image_main.c", "ps/capture/history.c","ps/app/capture_application.c", "ps/app/capture_runtime.c", "ps/app/capture_time_pair.c",
 "ps/app/raw_capture.c", "ps/app/service_bridge.c", "ps/app/analysis_service.c", "ps/app/block_pool.c", "ps/capture/capture_task.c",
 "ps/drivers/capture_regs.c", "ps/drivers/capture_contract.c", "ps/drivers/capture_data.c",
 "ps/drivers/dma_rx.c", "ps/drivers/dma_rx_xilinx.c", "ps/drivers/dma_rx_xilinx_freertos.c",
 "ps/dsp/spectrum.c", "ps/time/time_map.c", "shared/protocol.c",
 "ps/app/marker.c", "ps/app/level_compare.c", "ps/app/noise_report.c"]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def owner_manifest(data, symbols, inspection, plan, count):
    match = re.search(r'^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+\w\s+lfn_storage_owner_sizes$', symbols, re.M)
    if not match or int(match[2], 16) != 20:
        raise ValueError("Missing actual ARM owner size manifest")
    address = int(match[1], 16)
    phoff = struct.unpack_from('<I', data, 28)[0]
    phnum = struct.unpack_from('<H', data, 44)[0]
    raw = None
    for k in range(phnum):
        kind, offset, virtual, _, filesz, _, _, _ = struct.unpack_from('<8I', data, phoff + 32*k)
        if kind == 1 and virtual <= address and address + 20 <= virtual + filesz:
            raw = data[offset + address - virtual:offset + address - virtual + 20]
    if raw is None:
        raise ValueError("Owner size manifest is not in initialized ELF data")
    sizes = dict(zip(('files', 'owner_bytes', 'diagnostics_bytes', 'arena_bytes', 'groups'), struct.unpack('<5I', raw)))
    if sizes['files'] != count or sizes['arena_bytes'] != 4*1024*1024 or sizes['groups'] != 2:
        raise ValueError("Owner count/arena contract mismatch")
    end = max(load['end'] for load in inspection['loads'] if load['region'] == 'dedicated_noload')
    budget = plan['data_end'] - plan['data_base']
    if budget != 32*1024*1024 or end > plan['data_end']:
        raise ValueError("Owner image exceeds fixed 32 MiB data budget")
    return dict(**sizes, data_used_bytes=end-plan['data_base'], data_budget_bytes=budget,
                data_remaining_bytes=plan['data_end']-end, evidence='ACTUAL_LINKED_ARM_ELF')


def platform_input(receipt_path):
    record = json.loads(Path(receipt_path).read_text(encoding="utf-8-sig"))
    known_profiles = {spec["profile"] for spec in contract.HARDWARE_INPUTS.values()}
    if (record.get("status") != "PASS" or record.get("stage") != "complete" or
            record.get("client_mode") != "VITIS" or record.get("client_disposed") is not True or
            record.get("input_profile") not in known_profiles or record.get("allow_capture") is not True or
            record.get("board_execution_authorized") is not False or record.get("init_execution_authorized") is not False):
        raise ValueError("Require actual, closed, build-only capture platform PASS")
    for path, expected in record["source_hashes"].items():
        if sha(path).lower() != expected.lower():
            raise ValueError("Platform source fingerprint mismatch: " + path)
    for name, item in record["artifacts"].items():
        if sha(item["path"]).lower() != item["sha256"].lower():
            raise ValueError("Platform artifact fingerprint mismatch: " + name)
    xsa = Path(record["artifacts"]["component_xsa"]["path"])
    # Resolve the hardware export from the chain state instead of a hardcoded
    # candidate path: the hardware receipt recorded by the current BSP build
    # names its own directory, and exactly one XSA must sit beside it.
    hardware_receipt = Path(record["capture_contract"]["hardware_receipt"])
    hardware_xsas = sorted(hardware_receipt.parent.glob("*.xsa"))
    if len(hardware_xsas) != 1:
        raise ValueError("Expect exactly one XSA beside the hardware receipt: " + str(hardware_receipt.parent))
    validation = contract.validate_input(hardware_xsas[0], hardware_receipt)
    if sha(xsa) != validation["xsa_sha256"]:
        raise ValueError("Copied platform XSA differs from current hardware export")
    if validation != record["capture_contract"] or validation["xsa_sha256"] != record["xsa_sha256"]:
        raise ValueError("Platform contract differs from current hardware")
    directory = Path(record["platform_dir"])
    if contract.inspect_platform(directory, validation, ("standalone_cpu0", "freertos_cpu0")) != record["capture_bsp_checks"]:
        raise ValueError("Platform BSP/SDT evidence drift")
    if (directory / "capture_map.h").read_text(encoding="utf-8") != contract.map_header(validation):
        raise ValueError("Platform map header differs from actual XSA contract")
    return record, validation


def build(suite_root, receipt_path, *, storage_task=False, sd_probe=False, sd_rec=False, event_fixture=False, network=False):
    suite_root, receipt_path = Path(suite_root).resolve(), Path(receipt_path).resolve()
    if sum((storage_task, sd_probe, sd_rec, event_fixture, network)) > 1:
        raise ValueError("storage_task, sd_probe, sd_rec and event_fixture are separate build targets")
    if network:
        output = ROOT / "build/capture_network/current"
    elif storage_task:
        output = ROOT / "build/storage_task/current"
    elif sd_probe:
        output = ROOT / "build/sd_probe/current"
    elif sd_rec:
        output = ROOT / "build/sd_rec/current"
    elif event_fixture:
        output = ROOT / "build/event_fixture/current"
    else:
        output = ROOT / "build/capture_image/current"
    sources = list(SOURCES)
    if sd_probe:
        sources += ["ps/app/sd_probe.c", "ps/storage/fatfs_backend.c", "ps/storage/record_codec.c", "ps/storage/metadata_json.c"]
    if sd_rec:
        sources += ["ps/storage/" + name + ".c" for name in ("storage_service", "storage_writer", "storage_queue", "storage_metadata", "metadata_json", "storage_records", "metadata_builder", "storage_pump", "storage_task_freertos", "record_codec", "fatfs_backend")]
        sources += ["ps/app/sd_probe.c", "ps/app/sd_rec.c"]
        sources += ["ps/app/" + name + ".c" for name in ("storage_owner", "storage_owner_freertos", "storage_cold_start", "storage_rotation", "storage_segment", "storage_capture_epoch", "storage_multi_sink", "minute_summary")]
        sources += ["ps/storage/" + name + ".c" for name in ("storage_provision", "storage_namespace", "storage_lifecycle", "storage_factory", "storage_inventory", "storage_file_handles", "storage_engine", "storage_delete", "storage_epoch", "storage_catalog_replay", "storage_bootstrap", "storage_history", "storage_query", "storage_archive_verify", "storage_binding", "storage_family_index", "storage_family", "storage_family_queue", "storage_file_catalog", "storage_retention", "boot_journal", "fatfs_file_port", "fatfs_namespace_port", "fatfs_provision_port")]
        sources += ["ps/time/time_service.c"]
    if storage_task:
        sources += ["ps/storage/" + name + ".c" for name in ("storage_service", "storage_writer", "storage_queue", "storage_metadata", "metadata_json", "storage_records", "metadata_builder", "storage_pump", "storage_task_freertos", "record_codec", "fatfs_backend")]
        # Multi-file owner assembly (P4/E) linked but not yet scheduled: forcing
        # the owner symbols proves the complete target link/callgraph/resource
        # contract before runtime integration.
        sources += ["ps/app/" + name + ".c" for name in ("storage_owner", "storage_owner_freertos", "storage_cold_start", "storage_rotation", "storage_segment", "storage_capture_epoch", "storage_multi_sink", "minute_summary")]
        sources += ["ps/storage/" + name + ".c" for name in ("storage_provision", "storage_namespace", "storage_lifecycle", "storage_factory", "storage_inventory", "storage_file_handles", "storage_engine", "storage_delete", "storage_epoch", "storage_catalog_replay", "storage_bootstrap", "storage_history", "storage_query", "storage_archive_verify", "storage_binding", "storage_family_index", "storage_family", "storage_family_queue", "storage_file_catalog", "storage_retention", "boot_journal")]
        sources += ["ps/time/time_service.c"]
        sources += ["ps/storage/" + name + ".c" for name in ("fatfs_file_port", "fatfs_namespace_port", "fatfs_provision_port")]
    if not (storage_task or sd_probe or sd_rec):
        # Production runtime image: capture owns the route producer, the storage
        # coordinator task owns the mounted SD volume and the multi-file owner.
        sources += ["ps/app/storage_runtime.c", "ps/app/summary_runtime.c", "ps/app/event_runtime.c", "ps/events/event_engine.c", "ps/events/event_feed.c", "ps/app/storage_owner.c", "ps/app/storage_owner_freertos.c",
                    "ps/app/storage_cold_start.c", "ps/app/storage_rotation.c", "ps/app/storage_segment.c",
                    "ps/app/storage_capture_epoch.c", "ps/app/storage_multi_sink.c", "ps/app/minute_summary.c"]
        sources += ["ps/storage/" + name + ".c" for name in ("storage_provision", "storage_namespace", "storage_lifecycle",
                    "storage_factory", "storage_inventory", "storage_file_handles", "storage_engine", "storage_delete",
                    "storage_epoch", "storage_catalog_replay", "storage_bootstrap", "storage_history", "storage_query",
                    "storage_archive_verify", "storage_binding", "storage_family_index", "storage_family",
                    "storage_family_queue", "storage_file_catalog", "storage_retention", "boot_journal",
                    "fatfs_file_port", "fatfs_namespace_port", "fatfs_provision_port", "storage_service",
                    "storage_writer", "storage_queue", "storage_metadata", "metadata_json", "storage_records",
                    "metadata_builder", "storage_pump", "record_codec", "fatfs_backend")]
        sources += ["ps/time/time_service.c"]
    if not sd_probe:
        sources += ["ps/storage/storage_event_roll.c", "ps/storage/marker_record.c"]
    if not (storage_task or sd_probe or sd_rec or event_fixture):
        sources += ["ps/app/marker_runtime.c", "ps/app/saved_report.c", "ps/app/stored_report_io.c"]
    if event_fixture:
        sources.remove("ps/app/storage_runtime.c")
        sources += ["ps/app/event_fixture.c", "ps/app/event_fixture_image.c"]
    if network:
        vendor = suite_root / "Vitis/data/embeddedsw/ThirdParty/sw_services/lwip220_v1_4/src/lwip-2.2.0/contrib/ports/xilinx/netif/xemacpsif_physpeed.c"
        patched = ROOT / "build/network_profile/xemacpsif_physpeed.c"
        subprocess.run([sys.executable, "-B", str(ROOT / "scripts/generate_network_phy.py"), "--vendor", str(vendor), "--output", str(patched)], check=True)
        sources += ["ps/net/network.c", "ps/net/http_request.c", "ps/net/ksz9031.c", "ps/app/network_snapshot.c", "ps/app/network_capture.c", "ps/app/network_extra.c", "build/network_profile/xemacpsif_physpeed.c", "build/network_profile/xadapter.c"]
    output.mkdir(parents=True, exist_ok=True)
    scope = ("CAPTURE_ETHERNET_ELF_BUILD_ONLY" if network else "STORAGE_TASK_CANDIDATE_ELF_BUILD_ONLY" if storage_task else
             "SD_PROBE_ELF_BUILD_ONLY" if sd_probe else
             "SD_RECORD_PROBE_ELF_BUILD_ONLY" if sd_rec else
             "PS_SYNTHETIC_EVENT_FIXTURE_ELF_BUILD_ONLY" if event_fixture else "ACTUAL_CAPTURE_ELF_BUILD_ONLY")
    status = {"status": "RUNNING", "stage": "inputs", "scope": scope,
              "board_execution_authorized": False, "init_execution_authorized": False,
              "scheduler": "NOT_RUN", "physical_dma": "NOT_RUN", "adc": "NOT_RUN",
              "checked_at_utc": datetime.now(timezone.utc).isoformat()}
    def save():
        (output / "status.json").write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")
    save()
    try:
        for name in ("capture_image.elf", "inspection.json"):
            (output / name).unlink(missing_ok=True)
        if storage_task or sd_rec or not (storage_task or sd_probe or sd_rec):
            for script in ("generate_metadata_fixtures.py", "generate_storage_profile.py"):
                result = subprocess.run([sys.executable, "-B", str(ROOT / "scripts" / script)], cwd=ROOT, capture_output=True, text=True, check=True)
                (output / (script + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
        platform, validation = platform_input(receipt_path)
        if bool(platform.get("network")) != network:
            raise ValueError("Network target and BSP profile must match")
        bsp = Path(platform["platform_dir"]) / "ps7_cortexa9_0/freertos_cpu0/bsp"
        include = bsp / "include"
        bin_dir = suite_root / "gnu/aarch32/nt/gcc-arm-none-eabi/bin"
        compiler = bin_dir / "arm-none-eabi-gcc.exe"
        tools = {name: bin_dir / ("arm-none-eabi-" + name + ".exe") for name in ("gcc", "nm", "readelf", "objdump")}
        environment = dict(os.environ)
        environment["PATH"] = str(bin_dir) + os.pathsep + environment.get("PATH", "")
        def run(label, command):
            result = subprocess.run([str(x) for x in command], cwd=ROOT, env=environment,
                                    capture_output=True, text=True, timeout=180)
            (output / (label + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode != 0:
                raise ValueError(label + " failed; inspect " + label + ".log")
            return result.stdout
        if run("compiler_target", [compiler, "-dumpmachine"]).strip() != "arm-xilinx-eabi":
            raise ValueError("Wrong ARM compiler target")
        status["compiler_version"] = run("compiler_version", [compiler, "--version"]).splitlines()[0]
        library_names = ["libfreertos.a", "libxil.a", "libxilstandalone.a", "libxiltimer.a", "libxilffs.a"]
        if network:
            library_names += ["liblwip220.a"]
        libraries = [bsp / "lib" / name for name in library_names]
        flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-DSDT", "-DFREERTOS_BSP", "-mcpu=cortex-a9",
                 "-marm", "-mfpu=vfpv3", "-mfloat-abi=hard", "-O2", "-fstack-usage", "-fcallgraph-info=su", "-ffunction-sections", "-fdata-sections", "-I" + str(include),
                 "-I" + platform["platform_dir"], "-I" + str(ROOT / "shared")]
        owner_files = 2 if storage_task or sd_probe or sd_rec else 4
        flags += ["-DLFN_STORAGE_OWNER_FILES=" + str(owner_files)]
        if network:
            flags += ["-DLFN_NETWORK", "-I" + str(ROOT / "ps/net")]
        if not (storage_task or sd_probe or sd_rec):
            flags += ["-DLFN_EVENT_FIXTURE" if event_fixture else "-DLFN_STORAGE_RUNTIME", "-I" + str(ROOT / "build/storage_profile"), "-I" + str(ROOT / "build/storage_metadata/fixtures")]
        if storage_task or sd_rec:
            flags += ["-I" + str(ROOT / "build/storage_profile"), "-I" + str(ROOT / "build/storage_metadata/fixtures")]
        if storage_task or sd_rec:
            flags += ["-DLFN_STORAGE_OWNER_INSTANCE"]
        if sd_probe or sd_rec:
            flags += ["-DLFN_SD_PROBE"]
        if sd_rec:
            flags += ["-DLFN_SD_REC"]
        crt = []
        for name in ("crti.o", "crtn.o"):
            path = Path(run(name + "_path", [compiler, *flags, "-print-file-name=" + name]).strip())
            if not path.is_file():
                raise ValueError("Missing matching hard-float startup object")
            crt.append(path)
        toolchain_libraries = []
        for name in ("libc.a", "libm.a", "libgcc.a"):
            path = Path(run(name + "_path", [compiler, *flags, "-print-file-name=" + name]).strip())
            if not path.is_file():
                raise ValueError("Missing matching compiler runtime library: " + name)
            toolchain_libraries.append(path)
        tracked = [receipt_path, Path(__file__), ROOT / "scripts/check_runtime_elf.py", ROOT / "scripts/capture_bsp_contract.py",
                   ROOT / "ps/app/capture_memory.ld", *tools.values(), *libraries, *toolchain_libraries, *crt,
                   Path(platform["platform_dir"]) / "capture_map.h"]
        tracked += [ROOT / path for path in sources]
        if network:
            tracked += [ROOT / "scripts/generate_network_phy.py", vendor, vendor.with_name("xadapter.c")]
        tracked += list((ROOT / "ps").rglob("*.h")) + list((ROOT / "shared").rglob("*.h")) + list(include.rglob("*.h"))
        if not sd_probe:
            tracked += [ROOT / name for name in ("scripts/generate_metadata_fixtures.py", "scripts/generate_storage_profile.py", "scripts/storage_target_contract.py", "generated/fir_coefficients.h", "generated/parameters.json", "generated/hann4096.mem", "build/storage_profile/storage_profile.h", "build/storage_metadata/fixtures/record_manifest.h")]
        fingerprints = {str(path.resolve()): sha(path) for path in tracked}
        status.update(platform_receipt=str(receipt_path), platform_dir=platform["platform_dir"],
                      xsa_sha256=validation["xsa_sha256"], source_hashes=fingerprints,
                      owner_files=owner_files, compiler_flags=flags, stage="compile")
        save(); objects = []
        for source in sources:
            obj = output / (source.replace("/", "_") + ".o")
            extra = ["-Wno-unused-function"] if source.startswith("build/network_profile/") else []
            run(obj.name, [compiler, *flags, *extra, "-c", ROOT / source, "-o", obj])
            objects.append(obj)
        plan = validation["topology"]["memory_plan"]
        elf, mapping = output / "capture_image.elf", output / "capture_image.map"
        status["stage"] = "link"; save()
        link = ["-mcpu=cortex-a9", "-marm", "-mfpu=vfpv3", "-mfloat-abi=hard", "-nostartfiles",
                "-Wl,--build-id=none,--fatal-warnings,--no-undefined,--gc-sections,-u,_vector_table,-u,lfn_capture_image_request_start,-u,lfn_capture_image_request_stop,-u,lfn_capture_image_snapshot_copy,-u,lfn_capture_image_energy_take,-u,lfn_capture_image_raw_read,-u,lfn_capture_image_storage_attach,-u,lfn_capture_image_storage_close",
                "-T" + str(ROOT / "ps/app/capture_memory.ld"), "-Wl,-Map=" + str(mapping)]
        link += ["-Wl,-u,lfn_capture_image_rotation_port", "-Wl,-u,lfn_capture_image_cold_port"]
        if network:
            link += ["-Wl,--wrap=XEmacPs_PhyRead,--wrap=XEmacPs_PhyWrite"]
        link += ["-Wl,-u,lfn_capture_image_control_" + name for name in ("submit", "attach", "copy", "ack", "barrier")]
        link += ["-Wl,--defsym,LFN_" + key.upper() + "=" + str(plan[key]) for key in ("app_base", "app_end", "data_base", "data_end")]
        if sd_probe:
            link += ["-Wl,-u,lfn_sd_probe_run"]
        if sd_rec:
            link += ["-Wl,-u,lfn_sd_probe_run", "-Wl,-u,lfn_sd_rec_run"]
            link += ["-Wl,-u,lfn_storage_task_" + name for name in ("start_sd", "sd_closed", "metadata", "energy", "psd", "poll", "ack", "close", "health")]
            link += ["-Wl,-u,storage_build_" + name for name in ("session", "config", "capture")]
            link += ["-Wl,-u,lfn_storage_resource_sizes"]
            link += ["-Wl,-u,storage_owner_" + name for name in ("prepared", "prepare", "attach_ports")]
            link += ["-Wl,-u,lfn_storage_owner_freertos_mount", "-Wl,-u,lfn_storage_owner_freertos_unmount"]
            link += ["-Wl,-u,lfn_storage_task_start_sd_multifile", "-Wl,-u,lfn_storage_task_owner_health"]
        if event_fixture:
            link += ["-Wl,-u,lfn_event_fixture_image_start", "-Wl,-u,lfn_event_fixture_image_health"]
        if not (storage_task or sd_probe or sd_rec or event_fixture):
            link += ["-Wl,-u,lfn_storage_runtime_" + name for name in ("start", "session", "stop", "health", "capture_close_requested")]
            link += ["-Wl,-u,storage_owner_prepare", "-Wl,-u,storage_owner_attach_ports", "-Wl,-u,lfn_storage_owner_freertos_mount", "-Wl,-u,lfn_storage_owner_freertos_unmount"]
        if storage_task:
            names = ("connect_capture", "source_facts", "start", "start_sd", "sd_closed", "metadata", "energy", "psd", "pcm", "poll", "ack", "abort", "close", "recover", "health")
            link += ["-Wl,-u,lfn_storage_task_" + name for name in names]
            link += ["-Wl,-u,storage_build_" + name for name in ("session", "config", "capture", "time")]
            link += ["-Wl,-u,lfn_storage_resource_sizes"]
            link += ["-Wl,-u,storage_owner_" + name for name in ("init", "begin_cold", "segment", "step", "request_stop", "begin_rotation", "state", "error", "route", "prepare", "attach_ports")]
            link += ["-Wl,-u,lfn_storage_owner_freertos_" + name for name in ("mount", "unmount", "mounted")]
        if not sd_probe:
            link += ["-Wl,-u,lfn_storage_owner_sizes"]
        run("link", [compiler, crt[0], *objects, *link, "-Wl,--start-group", *libraries, "-lm", "-lc", "-lgcc",
                     "-Wl,--end-group", crt[1], "-o", elf])
        symbols = run("symbols", [tools["nm"], "-S", "--defined-only", elf])
        attributes = run("attributes", [tools["readelf"], "-A", elf])
        run("segments", [tools["readelf"], "-lW", elf])
        run("disassembly", [tools["objdump"], "-d", elf])
        status["stage"] = "inspect"; save()
        inspection = inspect_loads(elf.read_bytes(), **{key: plan[key] for key in ("app_base", "app_end", "data_base", "data_end")})
        inspection.update(inspect_symbols(symbols, plan, inspection["loads"], inspection["entry"], profile="capture"))
        if "Tag_ABI_VFP_args: VFP registers" not in attributes:
            raise ValueError("Missing hard-float argument attribute")
        if any(segment["flags"] & 3 == 3 for segment in inspection["loads"]):
            raise ValueError("Writable executable segment in capture image")
        if storage_task:
            from storage_target_contract import inspect_storage
            inspection["storage"] = inspect_storage(elf.read_bytes(), symbols, plan, inspection, output, len(sources))
        if not sd_probe:
            inspection["storage_owner"] = owner_manifest(elf.read_bytes(), symbols, inspection, plan, owner_files)
        platform_input(receipt_path)
        for path, expected in fingerprints.items():
            if sha(path) != expected:
                raise ValueError("Build input changed: " + path)
        inspection.update(status="PASS", scope="CAPTURE_ELF_LINK_ONLY_NO_EXECUTION")
        inspected = output / "inspection.json"
        inspected.write_text(json.dumps(inspection, indent=2) + "\n", encoding="utf-8")
        status.update(status="PASS", stage="complete", elf=str(elf), memory_plan=plan,
                      artifacts={str(path): sha(path) for path in (elf, mapping, inspected, *objects,
                      *(output / (name + ".log") for name in ("symbols", "attributes", "segments", "disassembly")), *(list(output.glob("*.su")) + list(output.glob("*.ci")) if storage_task else []))})
    except Exception as error:
        status.update(status="FAIL", error=type(error).__name__ + ": " + str(error))
        (output / "capture_image.elf").unlink(missing_ok=True)
    status["finished_at_utc"] = datetime.now(timezone.utc).isoformat(); save()
    return status


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite-root", type=Path, required=True)
    parser.add_argument("--platform-receipt", type=Path)
    parser.add_argument("--network", action="store_true", help="Build separate capture+Ethernet image")
    parser.add_argument("--storage-task", action="store_true", help="Build separate storage task candidate, no execution")
    parser.add_argument("--sd-probe", action="store_true", help="Build read-only SD probe image, no execution")
    parser.add_argument("--sd-rec", action="store_true", help="Build SD record-persistence probe image, no execution")
    parser.add_argument("--event-fixture", action="store_true", help="Build explicit PS synthetic event fixture image, no execution")
    args = parser.parse_args()
    if args.platform_receipt is None:
        args.platform_receipt = ROOT / ("build/capture_network_bsp/status.json" if args.network else "build/capture_bsp/status.json")
    result = build(args.suite_root, args.platform_receipt, storage_task=args.storage_task, sd_probe=args.sd_probe, sd_rec=args.sd_rec, event_fixture=args.event_fixture, network=args.network)
    print(result["status"] + " capture_image BUILD_ONLY")
    if result["status"] != "PASS":
        print(result.get("error", ""))
    raise SystemExit(0 if result["status"] == "PASS" else 1)
