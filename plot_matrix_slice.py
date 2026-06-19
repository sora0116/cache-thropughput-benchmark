#!/usr/bin/env python3

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator
import pandas as pd


AXES = ["l1", "l2", "l3", "dram"]
VALUE_COLUMNS = ["ops_per_cycle", "gbps", "cycles"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--x-axis", choices=AXES, required=True)
    parser.add_argument("--color-axis", choices=AXES, required=True)
    parser.add_argument("--value", choices=VALUE_COLUMNS, default="ops_per_cycle")
    parser.add_argument("--mode", choices=["read", "write"], default="read")
    parser.add_argument("--output", default="matrix_actual_scatter.png")
    parser.add_argument("--title", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.x_axis == args.color_axis:
        raise SystemExit("--x-axis and --color-axis must be different")

    df = pd.read_csv(args.input)
    df = df[df["mode"] == args.mode].copy()
    if df.empty:
        raise SystemExit(f"no rows for mode={args.mode}")

    x_col = f"actual_{args.x_axis}"
    color_col = f"actual_{args.color_axis}"
    df = df.sort_values(by=x_col).copy()

    fig, ax = plt.subplots(figsize=(10.5, 6.5))
    scatter = ax.scatter(
        df[x_col] * 100.0,
        df[args.value],
        c=df[color_col] * 100.0,
        cmap="cividis",
        s=52,
        alpha=0.9,
        edgecolors="white",
        linewidths=0.7,
    )

    ax.set_xlabel(f"{args.x_axis.upper()} actual (%)")
    ax.set_ylabel(args.value)
    ax.grid(True, which="major", axis="both", color="#c7d0d9", alpha=0.45, linewidth=0.8)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.xaxis.set_major_locator(MaxNLocator(nbins=10))
    ax.yaxis.set_major_locator(MaxNLocator(nbins=7))

    if args.title:
        title = args.title
    else:
        title = f"{args.value} vs {args.x_axis.upper()} actual ({args.mode})"
    ax.set_title(title)

    remaining_axes = [axis.upper() for axis in AXES if axis not in (args.x_axis, args.color_axis)]
    colorbar = fig.colorbar(scatter, ax=ax, pad=0.02)
    colorbar.set_label(f"{args.color_axis.upper()} actual (%)")

    fig.text(
        0.12,
        0.02,
        f"Each point is one measured row. Color shows {args.color_axis.upper()} actual. "
        f"Remaining actual axes: {', '.join(remaining_axes)}.",
        fontsize=9,
        color="#4b5b6a",
    )

    output_path = Path(args.output)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    fig.savefig(output_path, dpi=160)
    plt.close(fig)
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
