"""Cross-check the anonymized competition evidence using Python's standard library."""
import csv
import json
from decimal import Decimal
from pathlib import Path

root = Path(__file__).resolve().parent

def read_csv(name):
    with (root / name).open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))

def require(condition, description):
    if not condition:
        raise SystemExit("FAIL: " + description)

def main():
    info = json.loads((root / "ten_minute_check.json").read_text(encoding="utf-8"))
    obs = read_csv("ten_minute_observations.csv")
    require(len(obs) == info["receipts"] == 208, "208 received observations")
    require([int(x["observation"]) for x in obs] == list(range(1, 209)), "observation order")
    keep = [int(x["observation"]) for x in obs if x["retained_at_stop"] == "1"]
    require(keep == list(range(28, 208)) and len(keep) == info["retained_points"], "retained window 28 through 207")
    receipt = [int(x["elapsed_since_first_receipt_ms"]) for x in obs]
    require(receipt[0] == 0 and all(b >= a for a, b in zip(receipt, receipt[1:])), "relative receipt time")
    require(max(b-a for a,b in zip(receipt, receipt[1:])) == round(info["max_receipt_gap_seconds"] * 1000), "maximum polling interval")
    require(round(info["sd_valid_seconds"]/info["sd_duration_seconds"]*100, 2) == info["coverage_percent"], "SD coverage")
    require(info["original_sd_sha256"] == info["original_monitor_report_sha256"], "byte-identical original reports")
    require(all(f["accepted"] == f["synced"] for f in info["sd_files"]) and len(info["sd_files"]) == 4, "four synchronized files")
    bands = read_csv("ten_minute_bands.csv")
    require([float(b["center_hz"]) for b in bands] == [31.5,40,50,63,80,100,125,160,200,250], "center frequencies")
    require(all(Decimal(b["sd_spl_db"]) == Decimal(b["export_spl_db"]) for b in bands), "export agrees with board readback")
    peak = max(bands, key=lambda b: float(b["sd_spl_db"]))
    require(float(peak["center_hz"]) == 100 and float(peak["sd_spl_db"]) == 72.5, "saved peak")
    steps = read_csv("speaker_steps.csv")
    require(len(steps) == 10 and all(abs(Decimal(r["steady_dbfs"])-Decimal(r["background_dbfs"])-Decimal(r["difference_db"])) <= Decimal("0.02") for r in steps), "per-tone difference (two-decimal source values)")
    print("PASS: 208 observations, 180 retained points, four files, ten exported bands and ten speaker steps.")

if __name__ == "__main__":
    main()
