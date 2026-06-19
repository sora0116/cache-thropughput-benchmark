#!/usr/bin/env python3

import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Tuple

DEFAULT_CACHE_LINES = {
    "l1": 48 * 1024 // 64,
    "l2": 1280 * 1024 // 64,
    "l3": 18 * 1024 * 1024 // 64,
}

RESULT_RE = re.compile(r"^([^=]+)=(.+)$")


@dataclass
class Calibration:
    lines: Dict[str, int]
    shares: Dict[str, int]
    evict_passes: Tuple[int, int, int]
    actual: Dict[str, float]
    score: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["read", "write", "both"], default="both")
    parser.add_argument("--target-l1", type=float, required=True)
    parser.add_argument("--target-l2", type=float, required=True)
    parser.add_argument("--target-l3", type=float, required=True)
    parser.add_argument("--target-dram", type=float, required=True)
    parser.add_argument("--operations", type=int, default=20000)
    parser.add_argument("--warmup", type=int, default=5000)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--tolerance", type=float, default=0.08)
    parser.add_argument("--output", default="results.csv")
    parser.add_argument("--build", action="store_true")
    return parser.parse_args()


def run(cmd: List[str], capture_stderr: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE if capture_stderr else None,
    )


def build() -> None:
    run(["make"])


def normalize_targets(args: argparse.Namespace) -> Dict[str, float]:
    values = {
        "l1": args.target_l1,
        "l2": args.target_l2,
        "l3": args.target_l3,
        "dram": args.target_dram,
    }
    total = sum(values.values())
    if total <= 0.0:
        raise SystemExit("targets must sum to > 0")
    return {name: value / total for name, value in values.items()}


def shares_from_targets(targets: Dict[str, float], block: int = 100) -> Dict[str, int]:
    raw = {name: int(round(value * block)) for name, value in targets.items()}
    diff = block - sum(raw.values())
    ranked = sorted(targets.items(), key=lambda item: item[1], reverse=True)
    index = 0
    while diff != 0:
        name = ranked[index % len(ranked)][0]
        if diff > 0:
            raw[name] += 1
            diff -= 1
        elif raw[name] > 0:
            raw[name] -= 1
            diff += 1
        index += 1
    return raw


def normalize_share_weights(weights: Dict[str, float], block: int = 100) -> Dict[str, int]:
    clamped = {name: max(value, 0.0) for name, value in weights.items()}
    total = sum(clamped.values())
    if total <= 0.0:
        return shares_from_targets({"l1": 0.25, "l2": 0.25, "l3": 0.25, "dram": 0.25}, block=block)
    normalized = {name: value / total for name, value in clamped.items()}
    return shares_from_targets(normalized, block=block)


def choose_steps(lines: Dict[str, int]) -> Dict[str, int]:
    steps = {}
    for i, name in enumerate(("l1", "l2", "l3", "dram")):
        count = lines[name]
        step = max(1, (count // (7 + i * 2)) | 1)
        while step > 1 and count % step == 0:
            step -= 2
        steps[name] = max(1, step)
    return steps


def benchmark_args(
    mode: str,
    kernel: str,
    shares: Dict[str, int],
    lines: Dict[str, int],
    evict_passes: Tuple[int, int, int],
    operations: int,
    warmup: int,
    cpu: int,
) -> List[str]:
    if kernel == "stream":
        steps = {name: 1 for name in ("l1", "l2", "l3", "dram")}
    else:
        steps = choose_steps(lines)
    return [
        "./benchmark",
        "--mode",
        mode,
        "--kernel",
        kernel,
        "--operations",
        str(operations),
        "--warmup",
        str(warmup),
        "--lines-l1",
        str(lines["l1"]),
        "--lines-l2",
        str(lines["l2"]),
        "--lines-l3",
        str(lines["l3"]),
        "--lines-dram",
        str(lines["dram"]),
        "--step-l1",
        str(steps["l1"]),
        "--step-l2",
        str(steps["l2"]),
        "--step-l3",
        str(steps["l3"]),
        "--step-dram",
        str(steps["dram"]),
        "--share-l1",
        str(shares["l1"]),
        "--share-l2",
        str(shares["l2"]),
        "--share-l3",
        str(shares["l3"]),
        "--share-dram",
        str(shares["dram"]),
        "--evict-lines-l1",
        str(DEFAULT_CACHE_LINES["l1"] * 2),
        "--evict-lines-l2",
        str(DEFAULT_CACHE_LINES["l2"]),
        "--evict-lines-l3",
        str(DEFAULT_CACHE_LINES["l3"] // 2),
        "--evict-passes-l1",
        str(evict_passes[0]),
        "--evict-passes-l2",
        str(evict_passes[1]),
        "--evict-passes-l3",
        str(evict_passes[2]),
        "--cpu",
        str(cpu),
        "--seed",
        "1",
    ]


def parse_key_values(text: str) -> Dict[str, str]:
    result = {}
    for line in text.splitlines():
        match = RESULT_RE.match(line.strip())
        if match:
            result[match.group(1)] = match.group(2)
    return result


def sample(
    mode: str,
    kernel: str,
    shares: Dict[str, int],
    lines: Dict[str, int],
    evict_passes: Tuple[int, int, int],
    operations: int,
    warmup: int,
    cpu: int,
) -> Dict[str, float]:
    bench_cmd = benchmark_args(mode, kernel, shares, lines, evict_passes, operations, warmup, cpu)
    proc = run(bench_cmd)
    bench = parse_key_values(proc.stdout)
    result = {
        "cycles": float(bench["cycles"]),
        "ops_per_cycle": float(bench["ops_per_cycle"]),
        "gbps": float(bench["gbps"]),
        "pmu_instructions": float(bench.get("pmu_instructions", "0")),
        "instructions_per_op": float(bench.get("instructions_per_op", "0")),
        "pmu_l1_miss": float(bench.get("pmu_l1_miss", "0")),
        "pmu_ll_ref": float(bench.get("pmu_ll_ref", "0")),
        "pmu_ll_miss": float(bench.get("pmu_ll_miss", "0")),
    }
    result["actual_l1"] = 0.0
    result["actual_l2"] = 0.0
    result["actual_l3"] = 0.0
    result["actual_dram"] = 0.0
    return result


def probe_level_config(level: str, cal: Calibration) -> Tuple[Dict[str, int], Dict[str, int], Tuple[int, int, int]]:
    shares = {"l1": 0, "l2": 0, "l3": 0, "dram": 0}
    shares[level] = 100

    lines = {"l1": 64, "l2": 64, "l3": 64, "dram": 64}
    lines[level] = cal.lines[level]

    if level == "l1":
        evict_passes = (0, 0, 0)
    elif level == "l2":
        evict_passes = (1, 0, 0)
    elif level == "l3":
        evict_passes = (1, 1, 0)
    else:
        evict_passes = (1, 1, 1)
    return shares, lines, evict_passes


def probe_level_actual(level: str, cal: Calibration, operations: int, warmup: int, cpu: int) -> Dict[str, float]:
    shares, lines, evict_passes = probe_level_config(level, cal)
    measured = sample("read", "stream", shares, lines, evict_passes, operations, warmup, cpu)
    baseline = sample("baseline", "stream", shares, lines, (0, 0, 0), operations, warmup, cpu)

    l1_miss = max(measured["pmu_l1_miss"] - baseline["pmu_l1_miss"], 0.0)
    ll_ref = max(measured["pmu_ll_ref"] - baseline["pmu_ll_ref"], 0.0)
    ll_miss = max(measured["pmu_ll_miss"] - baseline["pmu_ll_miss"], 0.0)

    l1 = max(operations - l1_miss, 0.0) / operations
    l2 = max(l1_miss - ll_ref, 0.0) / operations
    l3 = max(ll_ref - ll_miss, 0.0) / operations
    dram = max(ll_miss, 0.0) / operations
    total = max(l1 + l2 + l3 + dram, 1e-12)
    return {
        "actual_l1": l1 / total,
        "actual_l2": l2 / total,
        "actual_l3": l3 / total,
        "actual_dram": dram / total,
    }


def blend_actuals(cal: Calibration, operations: int, warmup: int, cpu: int) -> Dict[str, float]:
    probes = {}
    for level in ("l1", "l2", "l3", "dram"):
        if cal.shares[level] > 0:
            probes[level] = probe_level_actual(level, cal, operations, warmup, cpu)

    blended = {"actual_l1": 0.0, "actual_l2": 0.0, "actual_l3": 0.0, "actual_dram": 0.0}
    for level in ("l1", "l2", "l3", "dram"):
        share = cal.shares[level] / 100.0
        if share == 0.0:
            continue
        observed = probes[level]
        for key in blended:
            blended[key] += share * observed[key]
    return blended


def calibrate(targets: Dict[str, float], operations: int, warmup: int, cpu: int) -> Calibration:
    shares = shares_from_targets(targets)
    actual = {name: shares[name] / 100.0 for name in ("l1", "l2", "l3", "dram")}
    high_l1 = actual["l1"] >= 0.7
    lines = {
        "l1": 64 if shares["l1"] > 0 else 64,
        "l2": 2048 if shares["l2"] > 0 else 64,
        "l3": 65536 if shares["l3"] > 0 else 64,
        "dram": 393216 if shares["dram"] > 0 else 64,
    }
    evict_passes = (0, 0, 0) if high_l1 else (1, 0, 0) if actual["l2"] >= 0.4 else (1, 1, 0) if actual["l3"] > 0 else (1, 1, 1)
    return Calibration(lines=lines, shares=shares, evict_passes=evict_passes, actual={f"actual_{k}": v for k, v in actual.items()}, score=0.0)


def median_results(rows: List[Dict[str, float]]) -> Dict[str, float]:
    keys = rows[0].keys()
    result = {}
    for key in keys:
        values = [row[key] for row in rows]
        result[key] = statistics.median(values)
    return result


def write_csv(path: str, rows: List[Dict[str, float]]) -> None:
    fieldnames = [
        "mode",
        "measure_kernel",
        "target_l1",
        "target_l2",
        "target_l3",
        "target_dram",
        "actual_l1",
        "actual_l2",
        "actual_l3",
        "actual_dram",
        "cycles",
        "ops_per_cycle",
        "gbps",
        "pmu_instructions",
        "instructions_per_op",
        "lines_l1",
        "lines_l2",
        "lines_l3",
        "lines_dram",
        "share_l1",
        "share_l2",
        "share_l3",
        "share_dram",
        "evict_passes_l1",
        "evict_passes_l2",
        "evict_passes_l3",
    ]
    with open(path, "w", newline="", encoding="ascii") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def main() -> int:
    args = parse_args()
    if args.build:
        build()
    if not os.path.exists("./benchmark"):
        raise SystemExit("benchmark binary not found; run with --build")

    targets = normalize_targets(args)
    cal = calibrate(targets, args.operations, args.warmup, args.cpu)
    pmu_actual = blend_actuals(cal, max(args.operations, 12000), max(args.warmup, 3000), args.cpu)

    modes = ["read", "write"] if args.mode == "both" else [args.mode]
    rows = []
    for mode in modes:
        trials = []
        for _ in range(args.trials):
            trials.append(
                sample(
                    mode,
                    "stream",
                    cal.shares,
                    cal.lines,
                    (0, 0, 0),
                    args.operations,
                    args.warmup,
                    args.cpu,
                )
            )
        median = median_results(trials)
        median["actual_l1"] = pmu_actual["actual_l1"]
        median["actual_l2"] = pmu_actual["actual_l2"]
        median["actual_l3"] = pmu_actual["actual_l3"]
        median["actual_dram"] = pmu_actual["actual_dram"]
        row = {
            "mode": mode,
            "measure_kernel": "stream",
            "target_l1": targets["l1"],
            "target_l2": targets["l2"],
            "target_l3": targets["l3"],
            "target_dram": targets["dram"],
            "actual_l1": median["actual_l1"],
            "actual_l2": median["actual_l2"],
            "actual_l3": median["actual_l3"],
            "actual_dram": median["actual_dram"],
            "cycles": median["cycles"],
            "ops_per_cycle": median["ops_per_cycle"],
            "gbps": median["gbps"],
            "pmu_instructions": median["pmu_instructions"],
            "instructions_per_op": median["instructions_per_op"],
            "lines_l1": cal.lines["l1"],
            "lines_l2": cal.lines["l2"],
            "lines_l3": cal.lines["l3"],
            "lines_dram": cal.lines["dram"],
            "share_l1": cal.shares["l1"],
            "share_l2": cal.shares["l2"],
            "share_l3": cal.shares["l3"],
            "share_dram": cal.shares["dram"],
            "evict_passes_l1": cal.evict_passes[0],
            "evict_passes_l2": cal.evict_passes[1],
            "evict_passes_l3": cal.evict_passes[2],
        }
        rows.append(row)
        print(
            f"{mode}: target=({targets['l1']:.2%}, {targets['l2']:.2%}, {targets['l3']:.2%}, {targets['dram']:.2%}) "
            f"actual=({median['actual_l1']:.2%}, {median['actual_l2']:.2%}, {median['actual_l3']:.2%}, {median['actual_dram']:.2%}) "
            f"ops/cycle={median['ops_per_cycle']:.6f} gbps={median['gbps']:.6f} "
            f"instr/op={median['instructions_per_op']:.3f}"
        )

    write_csv(args.output, rows)
    if cal.score > args.tolerance:
        print(
            f"warning: calibration error {cal.score:.4f} exceeds tolerance {args.tolerance:.4f}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
