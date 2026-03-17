#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description="Generate throughput and latency charts (safe vs broad) from benchmark CSV."
	)
	parser.add_argument(
		"--input",
		default="results_input.csv",
		help="Path to input CSV file.",
	)
	parser.add_argument(
		"--out-dir",
		default=".",
		help="Directory where chart PNG files will be saved.",
	)
	return parser.parse_args()


def prepare_data(input_csv: Path) -> pd.DataFrame:
	df = pd.read_csv(input_csv)

	required_columns = {
		"mode",
		"threads",
		"throughput_ops_s",
		"latency_us_op",
	}
	missing = required_columns - set(df.columns)
	if missing:
		raise ValueError(f"Missing required columns: {sorted(missing)}")

	filtered = df[df["mode"].isin(["safe", "broad"])].copy()
	if filtered.empty:
		raise ValueError("No rows for modes 'safe' or 'broad' in input CSV.")

	grouped = (
		filtered.groupby(["mode", "threads"], as_index=False)
		.agg(
			throughput_mean=("throughput_ops_s", "mean"),
			throughput_std=("throughput_ops_s", "std"),
			latency_mean=("latency_us_op", "mean"),
			latency_std=("latency_us_op", "std"),
		)
		.sort_values(["mode", "threads"])
	)
	return grouped


def plot_metric(
	grouped: pd.DataFrame,
	y_mean_col: str,
	y_std_col: str,
	title: str,
	y_label: str,
	output_path: Path,
	scale: float = 1.0,
) -> None:
	plt.figure(figsize=(10, 6))

	for mode, color in (("broad", "tab:red"), ("safe", "tab:green")):
		part = grouped[grouped["mode"] == mode].sort_values("threads")
		if part.empty:
			continue

		x = part["threads"]
		y = part[y_mean_col] / scale
		yerr = part[y_std_col].fillna(0) / scale

		plt.plot(x, y, marker="o", linewidth=2, label=mode, color=color)
		plt.fill_between(x, y - yerr, y + yerr, color=color, alpha=0.15)

	plt.title(title)
	plt.xlabel("Threads")
	plt.ylabel(y_label)
	plt.xscale("log", base=2)
	plt.xticks(sorted(grouped["threads"].unique()))
	plt.grid(True, linestyle="--", alpha=0.4)
	plt.legend()
	plt.tight_layout()
	plt.savefig(output_path, dpi=150)
	plt.close()


def main() -> None:
	args = parse_args()
	input_csv = Path(args.input)
	out_dir = Path(args.out_dir)
	out_dir.mkdir(parents=True, exist_ok=True)

	grouped = prepare_data(input_csv)

	throughput_path = out_dir / "throughput_plot.png"
	latency_path = out_dir / "latency_plot.png"

	plot_metric(
		grouped=grouped,
		y_mean_col="throughput_mean",
		y_std_col="throughput_std",
		title="Throughput vs Threads (safe vs broad)",
		y_label="Throughput [Mops/s]",
		output_path=throughput_path,
		scale=1_000_000.0,
	)

	plot_metric(
		grouped=grouped,
		y_mean_col="latency_mean",
		y_std_col="latency_std",
		title="Latency vs Threads (safe vs broad)",
		y_label="Latency [us/op]",
		output_path=latency_path,
	)

	print(f"Saved: {throughput_path}")
	print(f"Saved: {latency_path}")


if __name__ == "__main__":
	main()
