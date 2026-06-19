#!/usr/bin/env python3

import argparse
import os
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", str(Path(".matplotlib").resolve()))

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator
import pandas as pd


ACTUAL_COLUMNS = ["actual_l1", "actual_l2", "actual_l3", "actual_dram"]
VALUE_COLUMNS = ["gbps", "ops_per_cycle", "instructions_per_op", "pmu_instructions"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot a selected metric against each actual_* column from matrix_results.csv."
    )
    parser.add_argument("--input", default="matrix_results.csv")
    parser.add_argument("--mode", choices=["read", "write"], default="read")
    parser.add_argument("--value", choices=VALUE_COLUMNS, default="gbps")
    parser.add_argument("--output", default="actual_throughput.png")
    parser.add_argument("--title", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    df = pd.read_csv(args.input)
    df = df[df["mode"] == args.mode].copy()
    if df.empty:
        raise SystemExit(f"no rows for mode={args.mode}")

    fig, axes = plt.subplots(2, 2, figsize=(12, 8), sharey=True)
    axes = axes.flatten()

    for ax, column in zip(axes, ACTUAL_COLUMNS):
        plot_df = df[[column, args.value]].sort_values(by=column)
        ax.scatter(
            plot_df[column] * 100.0,
            plot_df[args.value],
            s=28,
            alpha=0.8,
            color="#1f77b4",
            edgecolors="none",
        )
        ax.set_title(column.replace("actual_", "").upper())
        ax.set_xlabel(f"{column} (%)")
        ax.grid(True, color="#c7d0d9", alpha=0.45, linewidth=0.8)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.xaxis.set_major_locator(MaxNLocator(nbins=6))
        ax.yaxis.set_major_locator(MaxNLocator(nbins=6))

    axes[0].set_ylabel(args.value)
    axes[2].set_ylabel(args.value)

    title = args.title or f"{args.value} vs actual_* ({args.mode})"
    fig.suptitle(title)

    output_path = Path(args.output)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(output_path, dpi=160)
    plt.close(fig)
    print(output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
