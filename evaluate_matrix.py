#!/usr/bin/env python3

import argparse
import csv
import itertools
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Tuple


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["read", "write", "both"], default="read")
    parser.add_argument("--step", type=int, default=10)
    parser.add_argument("--operations", type=int, default=4000)
    parser.add_argument("--warmup", type=int, default=1000)
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--output", default="matrix_results.csv")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    return parser.parse_args()


def generate_matrix(step: int) -> Iterable[Tuple[int, int, int, int]]:
    points = range(0, 101, step)
    for l1, l2, l3 in itertools.product(points, repeat=3):
        subtotal = l1 + l2 + l3
        if subtotal > 100:
            continue
        dram = 100 - subtotal
        if dram % step != 0:
            continue
        yield (l1, l2, l3, dram)


def run_case(args: argparse.Namespace, target: Tuple[int, int, int, int], temp_output: Path) -> Dict[str, str]:
    l1, l2, l3, dram = target
    cmd = [
        "./run_bench.py",
        "--mode",
        args.mode,
        "--target-l1",
        str(l1),
        "--target-l2",
        str(l2),
        "--target-l3",
        str(l3),
        "--target-dram",
        str(dram),
        "--operations",
        str(args.operations),
        "--warmup",
        str(args.warmup),
        "--trials",
        str(args.trials),
        "--cpu",
        str(args.cpu),
        "--output",
        str(temp_output),
    ]
    if args.build:
        cmd.append("--build")

    proc = subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    sys.stdout.write(proc.stdout)
    if proc.stderr:
        sys.stderr.write(proc.stderr)

    with temp_output.open("r", encoding="ascii", newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise RuntimeError(f"no rows produced for target {target}")
    return rows[0]


def main() -> int:
    args = parse_args()
    if args.step <= 0 or 100 % args.step != 0:
        raise SystemExit("--step must be a positive divisor of 100")

    output_path = Path(args.output)
    temp_output = output_path.with_suffix(".tmp.csv")

    matrix = list(generate_matrix(args.step))
    if args.limit > 0:
        matrix = matrix[:args.limit]
    if not matrix:
        raise SystemExit("matrix is empty")

    rows: List[Dict[str, str]] = []
    for index, target in enumerate(matrix, start=1):
        print(f"[{index}/{len(matrix)}] target={target[0]}/{target[1]}/{target[2]}/{target[3]}")
        row = run_case(args, target, temp_output)
        row["matrix_step"] = str(args.step)
        rows.append(row)

    fieldnames = list(rows[0].keys())
    with output_path.open("w", encoding="ascii", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    if temp_output.exists():
        temp_output.unlink()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
