"""Run sealed 3-tier external blind benchmarks for flashdb-edge-tsdb."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CLI = REPO_ROOT / "build" / ("fdb_edge_cli.exe" if os.name == "nt" else "fdb_edge_cli")
DEFAULT_REPORT = REPO_ROOT / "docs" / "benchmarks_report.json"

NAB_FILES = (
    "ambient_temperature_system_failure.csv",
    "cpu_utilization_asg_misconfiguration.csv",
    "ec2_request_latency_system_failure.csv",
    "machine_temperature_system_failure.csv",
    "nyc_taxi.csv",
    "rogue_agent_key_hold.csv",
    "rogue_agent_key_updown.csv",
)


def run_cli_json(cli: Path, args: list[str]) -> dict[str, Any]:
    proc = subprocess.run(
        [str(cli), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=True,
    )
    line = proc.stdout.strip().splitlines()[-1]
    return json.loads(line)


def resolve_dataset_root(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).resolve()
    env_val = os.environ.get("FDB_EDGE_BENCHMARK_ROOT")
    if env_val:
        return Path(env_val).resolve()
    return (REPO_ROOT / ".fdb_benchmarks").resolve()


def main() -> int:
    parser = argparse.ArgumentParser(description="Run 3-tier blind benchmarks for flashdb-edge-tsdb")
    parser.add_argument("--dataset-root", default=None, help="Directory holding nab/ and fusion_imu/ datasets")
    parser.add_argument("--cli", default=str(DEFAULT_CLI), help="Path to compiled fdb_edge_cli binary")
    parser.add_argument("--out", default=str(DEFAULT_REPORT), help="Output JSON report path")
    args = parser.parse_args()

    cli = Path(args.cli).resolve()
    if not cli.is_file():
        print(f"ERROR: CLI binary not found: {cli.name}", file=sys.stderr)
        return 2

    dataset_root = resolve_dataset_root(args.dataset_root)
    nab_dir = dataset_root / "nab"
    imu_csv = dataset_root / "fusion_imu" / "sensor_data.csv"

    for fname in NAB_FILES:
        if not (nab_dir / fname).is_file():
            print(f"ERROR: missing NAB dataset file: nab/{fname}", file=sys.stderr)
            return 2
    if not imu_csv.is_file():
        print("ERROR: missing Fusion IMU dataset file: fusion_imu/sensor_data.csv", file=sys.stderr)
        return 2

    repro = run_cli_json(cli, ["repro-upstream"])

    # Tier 1: All 7 NAB realKnownCause traces (batch=128 on 1-channel telemetry)
    nab_results: list[dict[str, Any]] = []
    nab_tot_rows = 0
    nab_up_ok = 0
    nab_up_drop = 0
    nab_edge_ok = 0
    nab_edge_match = 0
    nab_up_bytes = 0
    nab_edge_bytes = 0
    nab_up_erases = 0
    nab_edge_erases = 0
    nab_rollbacks_natural = 0
    nab_nor_violations = 0
    nab_bit_exact_all = True

    for fname in NAB_FILES:
        res = run_cli_json(
            cli,
            [
                "bench-csv",
                "--csv",
                str(nab_dir / fname),
                "--mode",
                "nab",
                "--sectors",
                "512",
                "--sector-size",
                "4096",
                "--batch",
                "128",
            ],
        )
        nab_results.append({"file": fname, **res})
        nab_tot_rows += int(res["rows"])
        nab_up_ok += int(res["upstream"]["written_ok"])
        nab_up_drop += int(res["upstream"]["dropped"])
        nab_edge_ok += int(res["edge"]["written_ok"])
        nab_edge_match += int(res["edge"]["queried_matched"])
        nab_up_bytes += int(res["upstream"]["flash_bytes_written"])
        nab_edge_bytes += int(res["edge"]["flash_bytes_written"])
        nab_up_erases += int(res["upstream"]["erases"])
        nab_edge_erases += int(res["edge"]["erases"])
        nab_rollbacks_natural += int(res["edge"]["rollbacks"])
        nab_nor_violations += int(res["edge"]["nor_violations"])
        nab_bit_exact_all = nab_bit_exact_all and bool(res["edge"]["bit_exact"]) and int(res["edge"]["mismatch"]) == 0

    # Rollback stress test on largest NAB file (1800 rows of 1-minute/5-minute rollback scenario via -1h)
    nab_rollback = run_cli_json(
        cli,
        [
            "bench-csv",
            "--csv",
            str(nab_dir / "machine_temperature_system_failure.csv"),
            "--mode",
            "nab",
            "--sectors",
            "512",
            "--sector-size",
            "4096",
            "--batch",
            "128",
            "--rollback-at",
            "11000",
        ],
    )

    # Tier 2: Fusion 9-DOF IMU multi-channel + 1000 power-cut fault injections + 2000-sample WAL sync
    imu_full = run_cli_json(
        cli,
        [
            "bench-csv",
            "--csv",
            str(imu_csv),
            "--mode",
            "imu",
            "--sectors",
            "512",
            "--sector-size",
            "4096",
            "--batch",
            "64",
        ],
    )
    faults = run_cli_json(cli, ["fault-inject", "--trials", "1000", "--seed", "42"])
    sync = run_cli_json(cli, ["sync-demo", "--samples", "2000", "--corrupt-pct", "5", "--seed", "7"])

    # Tier 3: OOD Privacy + DoS Guardrails
    privacy = run_cli_json(cli, ["privacy-bench"])

    tier1_passed = (
        nab_edge_ok == nab_tot_rows
        and nab_edge_match == nab_tot_rows
        and nab_bit_exact_all
        and nab_nor_violations == 0
        and nab_rollback["edge"]["written_ok"] == nab_rollback["rows"]
    )
    tier2_passed = (
        imu_full["edge"]["queried_matched"] == imu_full["rows"]
        and imu_full["edge"]["bit_exact"]
        and imu_full["edge"]["nor_violations"] == 0
        and faults["passed"] == faults["trials"]
        and faults["committed_loss_count"] == 0
        and faults["nor_bit_violations"] == 0
        and sync["synced_exact"] is True
    )
    tier3_passed = (
        privacy["secret_recall"] == 15
        and privacy["benign_fp"] == 0
        and privacy["dos_checks_passed"] == 4
    )

    report = {
        "status": "verified" if (tier1_passed and tier2_passed and tier3_passed) else "failed",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "upstream_repo": "armink/FlashDB",
        "upstream_commit": "6c722f169f27f99f96670ee00c27b2f59b1505e2",
        "upstream_repro": repro,
        "tier1_nab_real_known_cause": {
            "dataset": "numenta/NAB (MIT, data/realKnownCause, 7 CSVs)",
            "verdict": "PASSED" if tier1_passed else "FAILED",
            "total_rows": nab_tot_rows,
            "upstream_written_ok": nab_up_ok,
            "upstream_dropped_natural": nab_up_drop,
            "edge_written_ok": nab_edge_ok,
            "edge_queried_matched": nab_edge_match,
            "edge_rollbacks_natural": nab_rollbacks_natural,
            "upstream_flash_bytes_written": nab_up_bytes,
            "edge_flash_bytes_written": nab_edge_bytes,
            "overall_write_reduction_x": round(nab_up_bytes / max(1, nab_edge_bytes), 4),
            "upstream_erases": nab_up_erases,
            "edge_erases": nab_edge_erases,
            "edge_bit_exact_all": nab_bit_exact_all,
            "edge_nor_violations": nab_nor_violations,
            "rollback_1h_injection_test": nab_rollback,
            "per_file": nab_results,
        },
        "tier2_imu_fault_and_wal_sync": {
            "dataset": "xioTechnologies/Fusion (MIT, Examples/Python/sensor_data.csv, 9-DOF IMU)",
            "verdict": "PASSED" if tier2_passed else "FAILED",
            "fusion_imu_9dof": imu_full,
            "power_cut_torn_write_1000_trials": faults,
            "stm32_to_rpi_noisy_wal_sync": sync,
        },
        "tier3_privacy_and_dos_audit": {
            "dataset": "Synthetic OOD cellular/credential fixtures & DoS bounds",
            "verdict": "PASSED" if tier3_passed else "FAILED",
            "metrics": privacy,
        },
    }

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(
        json.dumps(
            {
                "status": report["status"],
                "tier1_verdict": report["tier1_nab_real_known_cause"]["verdict"],
                "tier1_rows": nab_tot_rows,
                "tier1_up_drop": nab_up_drop,
                "tier1_write_reduction_x": report["tier1_nab_real_known_cause"]["overall_write_reduction_x"],
                "tier2_verdict": report["tier2_imu_fault_and_wal_sync"]["verdict"],
                "tier2_imu_rows": imu_full["rows"],
                "tier2_imu_write_reduction_x": imu_full["write_reduction_x"],
                "tier2_faults_passed": f"{faults['passed']}/{faults['trials']}",
                "tier2_sync_exact": sync["synced_exact"],
                "tier3_verdict": report["tier3_privacy_and_dos_audit"]["verdict"],
            },
            indent=2,
        )
    )
    return 0 if report["status"] == "verified" else 1


if __name__ == "__main__":
    raise SystemExit(main())
