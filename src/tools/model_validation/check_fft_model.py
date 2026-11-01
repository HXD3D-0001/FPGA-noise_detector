#!/usr/bin/env python3
"""Run the generated AMD FFT IP's bit-accurate C model through ctypes (Windows).

Python 3 + NumPy are required. No installation or persistent PATH change occurs.
The FFT DLL and dependencies are read from the generated IP's existing ZIP into
a temporary directory and removed after unloading the model. Original vendor
files and generated IP files are never modified.

python check_fft_model.py
Exit 0: listed model checks passed. Exit 1: failed checks. Exit 2: model unavailable.
This is arithmetic C-model evidence, NOT RTL/AXI handshake or timing verification.
"""

from __future__ import annotations

import argparse
import ctypes as ct
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import tempfile
import zipfile

import numpy as np


N = 4096
FS = 1000
SCALE = 1 << 23
COEFF_SCALE = 1 << 17
DEPENDENCIES = ("libgmp.dll", "libmpfr.dll",
                "libIp_floating_point_v7_1_bitacc_cmodel.dll")
GENERIC_NAMES = ("C_NFFT_MAX", "C_ARCH", "C_HAS_NFFT", "C_USE_FLT_PT",
                 "C_INPUT_WIDTH", "C_TWIDDLE_WIDTH", "C_HAS_SCALING",
                 "C_HAS_BFP", "C_HAS_ROUNDING", "C_NSSR", "C_SYSTOLICFFT_INV")
EXPECTED_GENERICS = dict(zip(GENERIC_NAMES, (12, 1, 0, 0, 24, 24, 1, 1, 1, 1, 0)))
EXPECTED_COMPONENT = {
    "channels": "1", "transform_length": "4096",
    "implementation_options": "radix_4_burst_io", "data_format": "fixed_point",
    "input_width": "24", "phase_factor_width": "24",
    "scaling_options": "block_floating_point", "rounding_modes": "convergent_rounding",
    "output_ordering": "natural_order", "throttle_scheme": "nonrealtime",
    "aresetn": "true", "xk_index": "true", "ovflo": "false",
    "run_time_configurable_transform_length": "false",
    "super_sample_rates": "1", "systolicfft_inv": "false",
}
EXPECTED_MODEL = dict(EXPECTED_GENERICS, C_HAS_ARESETN=1, C_HAS_NATURAL_OUTPUT=1,
                      C_THROTTLE_SCHEME=1, C_HAS_XK_INDEX=1, C_OUTPUT_WIDTH=24,
                      C_CHANNELS=1)
COMPLEX_RELATIVE_L2_LIMIT = 1e-3
COMPLEX_MAX_ABSOLUTE_LIMIT = 1e-3
DOUBLE_PTR = ct.POINTER(ct.c_double)
INT_PTR = ct.POINTER(ct.c_int)


class Generics(ct.Structure):
    _fields_ = [(name, ct.c_int) for name in GENERIC_NAMES]


class Inputs(ct.Structure):
    _fields_ = [("nfft", ct.c_int), ("xn_re", DOUBLE_PTR), ("xn_re_size", ct.c_int),
                ("xn_im", DOUBLE_PTR), ("xn_im_size", ct.c_int),
                ("scaling_sch", INT_PTR), ("scaling_sch_size", ct.c_int),
                ("direction", ct.c_int)]


class Outputs(ct.Structure):
    _fields_ = [("xk_re", DOUBLE_PTR), ("xk_re_size", ct.c_int),
                ("xk_im", DOUBLE_PTR), ("xk_im_size", ct.c_int),
                ("blk_exp", ct.c_int), ("overflow", ct.c_int)]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_xci(path):
    """Bind the C-model configuration to effective generated JSON XCI values.

    The forward transform is selected by the runtime direction=1 below, not an
    XCI forward-only flag. SYSTOLICFFT_INV is also validated independently.
    Reject missing, ambiguous or drifted parameters before loading any DLL.
    """
    try:
        instance = json.loads(Path(path).read_text(encoding="utf-8"))["ip_inst"]
        if instance["component_reference"] != "xilinx.com:ip:xfft:9.1":
            raise ValueError("FFT IP must be xilinx.com:ip:xfft:9.1")
        observed = {}
        for section, expected in (("component_parameters", EXPECTED_COMPONENT),
                                  ("model_parameters", EXPECTED_MODEL)):
            values = instance["parameters"][section]
            observed[section] = {}
            for name, wanted in expected.items():
                records = values[name]
                if len(records) != 1:
                    raise ValueError(f"Ambiguous XCI parameter {section}.{name}")
                actual = records[0]["value"]
                if str(actual) != str(wanted):
                    raise ValueError(f"XCI drift {section}.{name}: expected {wanted}, got {actual}")
                observed[section][name] = actual
        generics = {name: int(observed["model_parameters"][name]) for name in GENERIC_NAMES}
        return {"status": "PASS", "xci_sha256": sha256(path),
                "ip_vlnv": instance["component_reference"], "generics": generics,
                "effective_parameters": observed, "runtime_forward_direction": 1}
    except (KeyError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError(f"Invalid or incomplete FFT XCI: {exc}") from exc


def complex_error_checks(actual, reference):
    """Require complex fidelity in addition to scalar power conservation."""
    error = np.asarray(actual) - np.asarray(reference)
    norm = float(np.linalg.norm(reference))
    relative = float(np.linalg.norm(error)) / norm if norm else float(np.linalg.norm(error))
    maximum = float(np.max(abs(error)))
    return [{"name": "relative_complex_l2", "status": "PASS" if relative < COMPLEX_RELATIVE_L2_LIMIT else "FAIL",
             "actual": relative, "limit": f"<{COMPLEX_RELATIVE_L2_LIMIT}"},
            {"name": "maximum_restored_complex_error", "status": "PASS" if maximum < COMPLEX_MAX_ABSOLUTE_LIMIT else "FAIL",
             "actual": maximum, "limit": f"<{COMPLEX_MAX_ABSOLUTE_LIMIT} normalized complex units"}]


def quantize(value, scale=SCALE):
    value = np.asarray(value, dtype=np.float64) * scale
    rounded = np.copysign(np.floor(abs(value) + 0.5), value)
    return np.clip(rounded, -scale, scale - 1).astype(np.int64)


def window_pcm(pcm, coefficients):
    product = pcm * coefficients
    magnitude = (abs(product) + (1 << 16)) >> 17
    return np.where(product < 0, -magnitude, magnitude)


def one_sided_power(spectrum, denominator):
    power = abs(spectrum[:N // 2 + 1]) ** 2
    power[1:-1] *= 2
    return float(np.sum(power) / (N * denominator))


def power_error_db(actual, expected):
    return None if actual == 0 or expected == 0 else float(10 * np.log10(actual / expected))


def bind(dll):
    dll.xilinx_ip_xfft_v9_1_get_default_generics.argtypes = []
    dll.xilinx_ip_xfft_v9_1_get_default_generics.restype = Generics
    dll.xilinx_ip_xfft_v9_1_create_state.argtypes = [Generics]
    dll.xilinx_ip_xfft_v9_1_create_state.restype = ct.c_void_p
    dll.xilinx_ip_xfft_v9_1_destroy_state.argtypes = [ct.c_void_p]
    dll.xilinx_ip_xfft_v9_1_destroy_state.restype = None
    dll.xilinx_ip_xfft_v9_1_bitacc_simulate.argtypes = [ct.c_void_p, Inputs, ct.POINTER(Outputs)]
    dll.xilinx_ip_xfft_v9_1_bitacc_simulate.restype = ct.c_int


def simulate(dll, state, real_q, imaginary_q=None):
    real = np.ascontiguousarray(real_q / SCALE, dtype=np.float64)
    imaginary = (np.zeros(N, dtype=np.float64) if imaginary_q is None else
                 np.ascontiguousarray(imaginary_q / SCALE, dtype=np.float64))
    output_real = np.zeros(N, dtype=np.float64)
    output_imaginary = np.zeros(N, dtype=np.float64)
    scaling = np.zeros(6, dtype=np.int32)  # Ignored for BFP; valid storage provided.
    inputs = Inputs(12, real.ctypes.data_as(DOUBLE_PTR), N,
                    imaginary.ctypes.data_as(DOUBLE_PTR), N,
                    scaling.ctypes.data_as(INT_PTR), 6, 1)  # direction=1 means forward.
    outputs = Outputs(output_real.ctypes.data_as(DOUBLE_PTR), N,
                      output_imaginary.ctypes.data_as(DOUBLE_PTR), N, 0, -1)
    code = int(dll.xilinx_ip_xfft_v9_1_bitacc_simulate(state, inputs, ct.byref(outputs)))
    if code != 0 or outputs.xk_re_size != N or outputs.xk_im_size != N:
        raise RuntimeError(f"C model returned code={code}, sizes={outputs.xk_re_size}/{outputs.xk_im_size}")
    mantissa = output_real + 1j * output_imaginary
    exponent = int(outputs.blk_exp)
    restored = mantissa * (2.0 ** exponent)
    reference = np.fft.fft(real + 1j * imaginary)
    reference_norm = float(np.linalg.norm(reference))
    error_norm = float(np.linalg.norm(restored - reference))
    integer_real = output_real * SCALE
    integer_imaginary = output_imaginary * SCALE
    metadata = {
        "model_return_code": code, "output_length": int(outputs.xk_re_size),
        "blk_exp": exponent,
        "overflow_field_not_valid_for_bfp": int(outputs.overflow),
        "maximum_restored_complex_error": float(np.max(abs(restored - reference))),
        "relative_complex_l2_error": None if reference_norm == 0 else error_norm / reference_norm,
        "complex_fidelity_checks": complex_error_checks(restored, reference),
        "mantissa_integer_alignment_max_error": float(max(np.max(abs(integer_real - np.rint(integer_real))),
                                                          np.max(abs(integer_imaginary - np.rint(integer_imaginary))))),
        "mantissa_integer_min": float(min(np.min(integer_real), np.min(integer_imaginary))),
        "mantissa_integer_max": float(max(np.max(integer_real), np.max(integer_imaginary))),
        "input_sha256_le_complex_int32": hashlib.sha256(
            np.column_stack((real_q, np.zeros(N, dtype=np.int64) if imaginary_q is None else imaginary_q))
            .astype("<i4").tobytes()).hexdigest(),
        "output_mantissa_sha256_le_complex_int32": hashlib.sha256(
            np.column_stack((np.rint(integer_real), np.rint(integer_imaginary))).astype("<i4").tobytes()).hexdigest(),
    }
    return restored, reference, mantissa, metadata


def run_checks(dll, validated_generics):
    bind(dll)
    defaults = dll.xilinx_ip_xfft_v9_1_get_default_generics()
    generics = Generics(**validated_generics)
    state = dll.xilinx_ip_xfft_v9_1_create_state(generics)
    if not state:
        raise RuntimeError("AMD model create_state returned NULL")
    rows = []
    checks = []

    def check(name, passed, actual, limit):
        checks.append({"name": name, "status": "PASS" if passed else "FAIL",
                       "actual": actual, "limit": limit})

    try:
        index = np.arange(N)
        window_q = quantize(0.5 - 0.5 * np.cos(2 * np.pi * index / N), COEFF_SCALE)
        window_energy = float(np.sum((window_q / COEFF_SCALE) ** 2))
        rng = np.random.default_rng(20261003)
        noise_q = quantize(rng.uniform(-0.25, 0.25, N))
        vectors = [
            ("zero", np.zeros(N, dtype=np.int64), None),
            ("dc_quarter_scale", np.full(N, SCALE // 4, dtype=np.int64), None),
            ("impulse_quarter_scale", np.pad(np.array([SCALE // 4], dtype=np.int64), (0, N - 1)), None),
            ("nyquist_quarter_scale", np.where(index % 2, -SCALE // 4, SCALE // 4), None),
            ("seeded_uniform_noise", noise_q, None),
            ("complex_forward_bin77", quantize(0.125 * np.cos(2 * np.pi * 77 * index / N)),
             quantize(0.125 * np.sin(2 * np.pi * 77 * index / N))),
        ]
        for name, real_q, imaginary_q in vectors:
            restored, reference, mantissa, row = simulate(dll, state, real_q, imaginary_q)
            row.update({"case": name, "window": "none", "complex_input": imaginary_q is not None})
            row["restored_peak_bin"] = int(np.argmax(abs(restored)))
            if imaginary_q is None:
                actual_power = one_sided_power(restored, N)
                reference_power = one_sided_power(reference, N)
                row.update({"restored_one_sided_mean_square": actual_power,
                            "reference_mean_square": reference_power,
                            "restored_power_error_db": power_error_db(actual_power, reference_power),
                            "zero_output_exact": bool(np.all(restored == 0)) if name == "zero" else None})
            rows.append(row)

        for frequency in (31.5, 50.0, 63.0, 100.0, 125.0, 250.0, 300.0,
                          128 * FS / N, 128.5 * FS / N):
            levels = (-6.0, -20.0, -40.0, -60.0) if frequency in (31.5, 50., 63., 100., 125., 250., 300.) else (0.,)
            for level in levels:
                pcm = quantize(10 ** (level / 20) * np.sin(2 * np.pi * frequency / FS * index + 0.37))
                windowed_q = window_pcm(pcm, window_q)
                restored, reference, mantissa, row = simulate(dll, state, windowed_q)
                actual_power = one_sided_power(restored, window_energy)
                reference_power = one_sided_power(reference, window_energy)
                uncorrected_power = one_sided_power(mantissa, window_energy)
                freq_axis = np.fft.fftfreq(N, 1 / FS)
                in_main_lobe = abs(freq_axis - frequency) < 2 * FS / N
                main_lobe_power = float(2 * np.sum(abs(restored[in_main_lobe]) ** 2) / (N * window_energy))
                expected_power = 0.5 * 10 ** (level / 10)
                row.update({
                    "case": "windowed_tone", "window": "periodic Hann Q1.17, ties-away product Q1.23",
                    "frequency_hz": frequency, "input_dbfs": level,
                    "restored_power": actual_power, "reference_power": reference_power,
                    "restored_power_error_db": power_error_db(actual_power, reference_power),
                    "restored_dbfs": 10 * math.log10(actual_power / 0.5),
                    "main_lobe_power_error_vs_ideal_tone_db": power_error_db(main_lobe_power, expected_power),
                    "uncorrected_mantissa_power_error_db": power_error_db(uncorrected_power, reference_power),
                    "expected_power_error_without_blk_exp_db": -20 * math.log10(2) * row["blk_exp"],
                    "peak_bin": int(np.argmax(abs(restored[:N // 2 + 1]))),
                })
                rows.append(row)

        # A repeated frame verifies that state reuse has not changed its arithmetic.
        _, _, _, repeat = simulate(dll, state, noise_q)
        original_noise = next(r for r in rows if r["case"] == "seeded_uniform_noise")
        repeat_equal = (repeat["blk_exp"] == original_noise["blk_exp"] and
                        repeat["output_mantissa_sha256_le_complex_int32"] == original_noise["output_mantissa_sha256_le_complex_int32"])
        check("zero_input_exact_zero", rows[0]["zero_output_exact"], rows[0]["maximum_restored_complex_error"], "0")
        dc = next(r for r in rows if r["case"] == "dc_quarter_scale")
        check("dc_power_and_natural_bin_order", dc["restored_peak_bin"] == 0 and abs(dc["restored_power_error_db"]) <= 0.2,
              {"peak_bin": dc["restored_peak_bin"], "power_error_db": dc["restored_power_error_db"]}, "bin0, <=0.2 dB")
        nyquist = next(r for r in rows if r["case"] == "nyquist_quarter_scale")
        check("nyquist_endpoint_not_doubled", nyquist["restored_peak_bin"] == N // 2 and abs(nyquist["restored_power_error_db"]) <= 0.2,
              {"peak_bin": nyquist["restored_peak_bin"], "power_error_db": nyquist["restored_power_error_db"]}, "bin2048, <=0.2 dB")
        complex_tone = next(r for r in rows if r["case"] == "complex_forward_bin77")
        check("forward_direction_and_natural_order", complex_tone["restored_peak_bin"] == 77 and complex_tone["relative_complex_l2_error"] < 1e-3,
              {"peak_bin": complex_tone["restored_peak_bin"], "relative_l2_error": complex_tone["relative_complex_l2_error"]}, "bin77, relative L2 <1e-3")
        tones = [r for r in rows if r["case"] == "windowed_tone"]
        regular_tones = [r for r in tones if r["input_dbfs"] >= -40]
        max_power_error = max(abs(r["restored_power_error_db"]) for r in regular_tones)
        check("restored_tone_power_vs_numpy", max_power_error <= 0.2, max_power_error,
              "<=0.2 dB, input >=-40 dBFS; -60 dBFS reported separately")
        max_lobe_error = max(abs(r["main_lobe_power_error_vs_ideal_tone_db"]) for r in regular_tones)
        check("restored_main_lobe_power", max_lobe_error <= 0.2, max_lobe_error, "<=0.2 dB, input >=-40 dBFS")
        check("restored_noise_power_vs_numpy", abs(original_noise["restored_power_error_db"]) <= 0.2,
              original_noise["restored_power_error_db"], "<=0.2 dB")
        impulse = next(r for r in rows if r["case"] == "impulse_quarter_scale")
        check("restored_impulse_fft_vs_numpy", impulse["relative_complex_l2_error"] < 1e-3,
              impulse["relative_complex_l2_error"], "relative L2 <1e-3")
        all_integer_and_in_range = all(r["mantissa_integer_alignment_max_error"] == 0 and
                                       r["mantissa_integer_min"] >= -SCALE and
                                       r["mantissa_integer_max"] < SCALE for r in rows)
        check("output_mantissa_is_signed_q1_23", all_integer_and_in_range,
              max(r["mantissa_integer_alignment_max_error"] for r in rows), "integer-aligned and within signed24 range")
        exponents = sorted({r["blk_exp"] for r in tones})
        check("multiple_bfp_exponents_exercised", len(exponents) >= 2, exponents, "at least 2 distinct BLK_EXP values")
        scale_error = max(abs(r["uncorrected_mantissa_power_error_db"]
                              - r["expected_power_error_without_blk_exp_db"]
                              - r["restored_power_error_db"]) for r in tones)
        check("omitted_exponent_negative_control", scale_error < 1e-10, scale_error,
              "power error = -6.020599913*BLK_EXP + numerical error, within 1e-10 dB")
        check("reused_state_repeat_frame", repeat_equal, repeat_equal, "identical exponent and mantissa hash")
        for number, row in enumerate(rows):
            for gate in row["complex_fidelity_checks"]:
                check(f"case_{number}_{row['case']}_{gate['name']}", gate["status"] == "PASS",
                      gate["actual"], gate["limit"])
        return {
            "default_generics": {name: getattr(defaults, name) for name in GENERIC_NAMES},
            "configured_generics": validated_generics, "window_sum_squares": window_energy,
            "cases": rows, "checks": checks,
            "summary": {"status": "PASS" if all(c["status"] == "PASS" for c in checks) else "FAIL",
                        "case_count": len(rows), "additional_repeat_case_count": 1,
                        "check_count": len(checks), "failed_check_count": sum(c["status"] == "FAIL" for c in checks),
                        "tone_blk_exp_values": exponents,
                        "max_tone_power_error_db_at_or_above_minus40": max_power_error,
                        "max_tone_power_error_db_at_minus60": max(abs(r["restored_power_error_db"]) for r in tones if r["input_dbfs"] == -60),
                        "max_main_lobe_error_db_at_or_above_minus40": max_lobe_error},
        }
    finally:
        dll.xilinx_ip_xfft_v9_1_destroy_state(state)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vivado-root", type=Path, default=Path(os.environ.get("XILINX_VIVADO", ".")))
    parser.add_argument("--xci", type=Path, default=Path(__file__).parent / "ip_probe/fft_probe/fft_probe.xci")
    parser.add_argument("--cmodel-zip", type=Path, default=Path(__file__).parent / "ip_probe/fft_probe/cmodel/xfft_v9_1_bitacc_cmodel_nt64.zip")
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("fft_model_results.json"))
    args = parser.parse_args()
    dll_path = args.vivado_root / "win64/tools/fft_v9_1/libIp_xfft_v9_1_bitacc_cmodel.dll"
    header_path = dll_path.with_name("xfft_v9_1_bitacc_cmodel.h")
    configuration_path = Path(__file__).with_name("ip_configuration.txt")
    report = {
        "schema_version": 1, "evidence_class": "AMD-official-bit-accurate-FFT-C-model",
        "evidence_exclusions": ["RTL simulation", "AXI handshakes/backpressure", "TLAST/TUSER packing",
                                "RTL output ordering configuration wiring", "synthesis/timing/resources", "ADC/acoustic measurement"],
        "python_version": platform.python_version(), "numpy_version": np.__version__,
        "abi": "ctypes.CDLL cdecl; native Windows x64 structure alignment",
        "ctypes_structure_sizes": {"Generics": ct.sizeof(Generics), "Inputs": ct.sizeof(Inputs), "Outputs": ct.sizeof(Outputs)},
        "script_sha256": sha256(__file__),
        "installed_model_dll_sha256": sha256(dll_path) if dll_path.is_file() else None,
        "installed_model_header_sha256": sha256(header_path) if header_path.is_file() else None,
        "generated_cmodel_zip_sha256": sha256(args.cmodel_zip) if args.cmodel_zip.is_file() else None,
        "generated_ip_configuration_sha256": sha256(configuration_path) if configuration_path.is_file() else None,
        "intended_generics": EXPECTED_GENERICS, "direction_input": 1,
        "normalization": {"c_model_output": "normalized Q1.23 mantissa double; X_unscaled = model_output * 2**BLK_EXP",
                          "rtl_integer_output": "X_unscaled = signed24_integer * 2**(BLK_EXP - 23)",
                          "power": "magnitude-squared restoration uses 2**(2*BLK_EXP); one-sided PSD doubles interior bins only",
                          "dbfs_reference_mean_square": 0.5,
                          "overflow_note": "C model overflow output is invalid in BFP mode and is not used as an overflow pass criterion."},
        "dependency_policy": "Existing ZIP dependencies extracted to process temporary directory; temporary DLL search handles only; no installation or persistent PATH edits.",
        "load_attempts": [],
    }
    dll = None
    handles = []
    exit_code = 2
    try:
        report["generated_xci_sha256"] = sha256(args.xci)
        report["xci_validation"] = validate_xci(args.xci)
        with tempfile.TemporaryDirectory(prefix="lf_fft_model_") as temporary:
            try:
                candidates = (dll_path.parent, args.vivado_root / "win64/lib", args.vivado_root / "lib/win64.o",
                              args.vivado_root / "tps/win64", args.vivado_root / "win64/tools")
                report["checked_dependency_directories"] = [{"directory": str(p), "exists": p.is_dir()} for p in candidates]
                for directory in candidates:
                    if directory.is_dir():
                        handles.append(os.add_dll_directory(str(directory)))
                try:
                    dll = ct.CDLL(str(dll_path))
                    report["load_attempts"].append({"method": "installed directories", "status": "LOAD_OK"})
                except OSError as exc:
                    report["load_attempts"].append({"method": "installed directories", "status": "LOAD_FAILED",
                                                    "error_type": type(exc).__name__, "winerror": getattr(exc, "winerror", None), "message": str(exc)})
                if dll is not None:
                    import _ctypes
                    _ctypes.FreeLibrary(dll._handle)
                    dll = None
                # Always test the model packaged with this generated IP. The
                # installation's similarly named DLL can be a different build.
                with zipfile.ZipFile(args.cmodel_zip) as archive:
                    report["temporary_dependency_sha256"] = {}
                    for name in (*DEPENDENCIES, dll_path.name):
                        data = archive.read(name)
                        Path(temporary, name).write_bytes(data)
                        report["temporary_dependency_sha256"][name] = hashlib.sha256(data).hexdigest()
                    report["model_dll_sha256"] = report["temporary_dependency_sha256"][dll_path.name]
                    report["model_header_sha256"] = hashlib.sha256(archive.read(header_path.name)).hexdigest()
                    report["installed_fft_dll_matches_generated_ip_zip_dll"] = (
                        report["installed_model_dll_sha256"] == report["model_dll_sha256"])
                report["loaded_model_source"] = "The generated FFT IP's cmodel ZIP, temporarily extracted with its bundled dependencies"
                handles.append(os.add_dll_directory(temporary))
                dll = ct.CDLL(str(Path(temporary, dll_path.name)))
                report["load_attempts"].append({"method": "generated IP ZIP model and dependencies in temporary directory", "status": "LOAD_OK"})
                print("Official FFT C model loaded; running arithmetic cases...", flush=True)
                report.update(run_checks(dll, report["xci_validation"]["generics"]))
                exit_code = 0 if report["summary"]["failed_check_count"] == 0 else 1
            finally:
                if dll is not None:
                    import _ctypes
                    _ctypes.FreeLibrary(dll._handle)
                    dll = None
                for handle in handles:
                    handle.close()
    except ValueError as exc:
        report["summary"] = {"status": "FAIL", "failed_check_count": 1,
                             "reason": str(exc), "error_type": type(exc).__name__,
                             "arithmetic_checks_complete": False}
        exit_code = 1
    except (OSError, RuntimeError, KeyError, zipfile.BadZipFile) as exc:
        report["summary"] = {"status": "BLOCKED", "failed_check_count": None,
                             "reason": str(exc), "error_type": type(exc).__name__,
                             "winerror": getattr(exc, "winerror", None), "arithmetic_checks_complete": False}
        exit_code = 2
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "load_attempts": report["load_attempts"],
                      "summary": report["summary"], "checks": report.get("checks", [])}, indent=2, ensure_ascii=False))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
