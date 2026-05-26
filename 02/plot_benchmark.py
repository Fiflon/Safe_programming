#!/usr/bin/env python3
"""
Generate comparison charts for the three BST synchronization strategies.

Usage:
    python plot_benchmark.py [--input results.csv] [--out-dir plots]
"""
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


VARIANT_LABELS = {
    "singleLock": "Single Lock (mutex)",
    "handOver":   "Hand-over-hand",
    "lockFree":   "Lock-free (CAS)",
}

VARIANT_COLORS = {
    "singleLock": "tab:red",
    "handOver":   "tab:orange",
    "lockFree":   "tab:green",
}

VARIANT_ORDER = ["singleLock", "handOver", "lockFree"]


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="BST benchmark charts")
    p.add_argument("--input", default="results.csv", help="Input CSV file")
    p.add_argument("--out-dir", default="plots", help="Output directory for PNGs")
    return p.parse_args()


def load(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    agg = (
        df.groupby(["variant", "threads"], as_index=False)
        .agg(
            throughput_mean=("throughput_ops_s", "mean"),
            throughput_std=("throughput_ops_s", "std"),
            time_mean=("time_us", "mean"),
            time_std=("time_us", "std"),
        )
        .sort_values(["variant", "threads"])
    )
    return agg


# ---------------------------------------------------------------------------
# Chart 1 : Throughput vs Threads
# ---------------------------------------------------------------------------
def plot_throughput(agg: pd.DataFrame, out: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 6))

    for v in VARIANT_ORDER:
        part = agg[agg["variant"] == v].sort_values("threads")
        if part.empty:
            continue
        x = part["threads"]
        y = part["throughput_mean"] / 1e6        # Mops/s
        yerr = part["throughput_std"].fillna(0) / 1e6

        ax.plot(x, y, marker="o", linewidth=2,
                label=VARIANT_LABELS.get(v, v), color=VARIANT_COLORS.get(v))
        ax.fill_between(x, y - yerr, y + yerr,
                        color=VARIANT_COLORS.get(v), alpha=0.15)

    ax.set_title("Throughput vs Number of Threads")
    ax.set_xlabel("Threads")
    ax.set_ylabel("Throughput  [Mops/s]")
    ax.set_xscale("log", base=2)
    ax.set_xticks(sorted(agg["threads"].unique()))
    ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / "throughput_vs_threads.png", dpi=150)
    plt.close(fig)
    print(f"  [ok] {out / 'throughput_vs_threads.png'}")


# ---------------------------------------------------------------------------
# Chart 2 : Latency (avg time per total ops) vs Threads
# ---------------------------------------------------------------------------
def plot_latency(agg: pd.DataFrame, out: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 6))

    for v in VARIANT_ORDER:
        part = agg[agg["variant"] == v].sort_values("threads")
        if part.empty:
            continue
        x = part["threads"]
        y = part["time_mean"] / 1e3               # ms
        yerr = part["time_std"].fillna(0) / 1e3

        ax.plot(x, y, marker="s", linewidth=2,
                label=VARIANT_LABELS.get(v, v), color=VARIANT_COLORS.get(v))
        ax.fill_between(x, y - yerr, y + yerr,
                        color=VARIANT_COLORS.get(v), alpha=0.15)

    ax.set_title("Total Execution Time vs Number of Threads")
    ax.set_xlabel("Threads")
    ax.set_ylabel("Execution time  [ms]")
    ax.set_xscale("log", base=2)
    ax.set_xticks(sorted(agg["threads"].unique()))
    ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / "time_vs_threads.png", dpi=150)
    plt.close(fig)
    print(f"  [ok] {out / 'time_vs_threads.png'}")


# ---------------------------------------------------------------------------
# Chart 3 : Speedup vs single-thread baseline (per variant)
# ---------------------------------------------------------------------------
def plot_speedup(agg: pd.DataFrame, out: Path) -> None:
    fig, ax = plt.subplots(figsize=(10, 6))

    max_t = 1
    for v in VARIANT_ORDER:
        part = agg[agg["variant"] == v].sort_values("threads")
        if part.empty:
            continue
        base = part[part["threads"] == 1]["throughput_mean"].values
        if len(base) == 0:
            continue
        base = base[0]
        x = part["threads"]
        y = part["throughput_mean"] / base

        ax.plot(x, y, marker="^", linewidth=2,
                label=VARIANT_LABELS.get(v, v), color=VARIANT_COLORS.get(v))
        max_t = max(max_t, x.max())

    # Ideal linear speedup reference
    ideal_x = sorted(agg["threads"].unique())
    ax.plot(ideal_x, ideal_x, linestyle="--", color="gray",
            alpha=0.5, label="Ideal linear")

    ax.set_title("Speedup vs Number of Threads  (relative to 1 thread of same variant)")
    ax.set_xlabel("Threads")
    ax.set_ylabel("Speedup  (×)")
    ax.set_xscale("log", base=2)
    ax.set_xticks(sorted(agg["threads"].unique()))
    ax.get_xaxis().set_major_formatter(plt.ScalarFormatter())
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / "speedup_vs_threads.png", dpi=150)
    plt.close(fig)
    print(f"  [ok] {out / 'speedup_vs_threads.png'}")


# ---------------------------------------------------------------------------
# Chart 4 : Bar chart – throughput at selected thread counts
# ---------------------------------------------------------------------------
def plot_bars(agg: pd.DataFrame, out: Path) -> None:
    thread_counts = sorted(agg["threads"].unique())
    n_groups = len(thread_counts)
    n_bars = len(VARIANT_ORDER)
    width = 0.25

    fig, ax = plt.subplots(figsize=(10, 6))
    import numpy as np
    x_pos = np.arange(n_groups)

    for i, v in enumerate(VARIANT_ORDER):
        part = agg[agg["variant"] == v].sort_values("threads")
        if part.empty:
            continue
        vals = []
        errs = []
        for tc in thread_counts:
            row = part[part["threads"] == tc]
            if row.empty:
                vals.append(0)
                errs.append(0)
            else:
                vals.append(row["throughput_mean"].values[0] / 1e6)
                errs.append(row["throughput_std"].fillna(0).values[0] / 1e6)
        ax.bar(x_pos + i * width, vals, width, yerr=errs,
               label=VARIANT_LABELS.get(v, v), color=VARIANT_COLORS.get(v),
               capsize=3)

    ax.set_title("Throughput by Variant and Thread Count")
    ax.set_xlabel("Threads")
    ax.set_ylabel("Throughput  [Mops/s]")
    ax.set_xticks(x_pos + width)
    ax.set_xticklabels(thread_counts)
    ax.grid(True, axis="y", linestyle="--", alpha=0.4)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out / "throughput_bars.png", dpi=150)
    plt.close(fig)
    print(f"  [ok] {out / 'throughput_bars.png'}")


def main() -> None:
    args = parse_args()
    csv_path = Path(args.input)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    agg = load(csv_path)

    print("Generating charts...")
    plot_throughput(agg, out_dir)
    plot_latency(agg, out_dir)
    plot_speedup(agg, out_dir)
    plot_bars(agg, out_dir)
    print("All charts saved to:", out_dir)


if __name__ == "__main__":
    main()
