#!/usr/bin/env python3

import argparse
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd


AXES = ["l1", "l2", "l3", "dram"]
VALUE_COLUMNS = ["ops_per_cycle", "gbps", "cycles"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--x-axis", choices=AXES, required=True)
    parser.add_argument("--fix-l1", type=float)
    parser.add_argument("--fix-l2", type=float)
    parser.add_argument("--fix-l3", type=float)
    parser.add_argument("--fix-dram", type=float)
    parser.add_argument("--value", choices=VALUE_COLUMNS, default="ops_per_cycle")
    parser.add_argument("--mode", choices=["read", "write"], default="read")
    parser.add_argument("--output", default="matrix_slice.png")
    parser.add_argument("--title", default="")
    return parser.parse_args()


def fixed_axes(args: argparse.Namespace) -> Dict[str, float]:
    return {
        "l1": args.fix_l1,
        "l2": args.fix_l2,
        "l3": args.fix_l3,
        "dram": args.fix_dram,
    }


def validate_args(args: argparse.Namespace) -> Tuple[List[str], str]:
    fixed = fixed_axes(args)
    fixed_count = sum(value is not None for value in fixed.values())
    if fixed_count != 2:
        raise SystemExit("exactly two of --fix-l1/--fix-l2/--fix-l3/--fix-dram must be specified")
    if fixed[args.x_axis] is not None:
        raise SystemExit(f"--fix-{args.x_axis} conflicts with --x-axis {args.x_axis}")

    free_axes = [axis for axis in AXES if axis != args.x_axis and fixed[axis] is None]
    if len(free_axes) != 1:
        raise SystemExit("with two fixed axes and one x-axis, exactly one axis must remain implicit")
    return [axis for axis, value in fixed.items() if value is not None], free_axes[0]


def main() -> int:
    args = parse_args()
    fixed_names, implicit_axis = validate_args(args)

    df = pd.read_csv(args.input)
    df = df[df["mode"] == args.mode].copy()
    if df.empty:
        raise SystemExit(f"no rows for mode={args.mode}")

    fixed = fixed_axes(args)
    for axis in fixed_names:
        value = fixed[axis] / 100.0
        df = df[df[f"target_{axis}"].round(8) == round(value, 8)]

    if df.empty:
        raise SystemExit("no rows matched the requested fixed axes")

    df = df.sort_values(by=f"target_{args.x_axis}").copy()
    x_col = f"target_{args.x_axis}"
    implicit_col = f"target_{implicit_axis}"

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.plot(df[x_col] * 100.0, df[args.value], marker="o")

    for _, row in df.iterrows():
        ax.annotate(
            f"{row[implicit_col] * 100:.0f}",
            (row[x_col] * 100.0, row[args.value]),
            textcoords="offset points",
            xytext=(0, 6),
            ha="center",
            fontsize=8,
        )

    ax.set_xlabel(f"{args.x_axis.upper()} target (%)")
    ax.set_ylabel(args.value)
    ax.grid(True, alpha=0.3)

    if args.title:
        title = args.title
    else:
        fixed_text = ", ".join(f"{axis.upper()}={fixed[axis]:.0f}%" for axis in fixed_names)
        title = f"{args.value} vs {args.x_axis.upper()} ({args.mode}, {fixed_text}, implicit {implicit_axis.upper()})"
    ax.set_title(title)

    output_path = Path(args.output)
    fig.tight_layout()
    fig.savefig(output_path, dpi=160)
    plt.close(fig)
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
