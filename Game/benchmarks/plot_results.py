#!/usr/bin/env python3
"""CSC 581 performance checkpoint: generates benchmark_summary.csv and the report-ready
graphs from benchmark_raw.csv. Every number in benchmark_summary.csv and every plotted
point is computed here, deterministically, from the raw per-repetition rows written by
NetworkPerformanceBenchmark -- nothing in this script is hand-typed benchmark data.

Dependencies: Python standard library (csv, statistics) + matplotlib only -- no pandas/
numpy, to keep this a small plotting script rather than a data-analytics dependency.

Usage:
    python3 plot_results.py [results_dir]

Defaults to the directory this script lives in (Game/benchmarks) + "results".
"""

import csv
import statistics
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

FULL_COLOR = "#1f77b4"
DELTA_COLOR = "#d95f02"


def load_raw(raw_path):
    rows = []
    with open(raw_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(
                {
                    "experiment": row["experiment"],
                    "strategy": row["strategy"],
                    "client_count": int(row["client_count"]),
                    "static_object_count": int(row["static_object_count"]),
                    "moving_object_count": int(row["moving_object_count"]),
                    "iterations": int(row["iterations"]),
                    "repetition": int(row["repetition"]),
                    "elapsed_ms": float(row["elapsed_ms"]),
                    "total_messages_sent": int(row["total_messages_sent"]),
                    "total_bytes_sent": int(row["total_bytes_sent"]),
                }
            )
    return rows


def group_key(row):
    return (
        row["experiment"],
        row["strategy"],
        row["client_count"],
        row["static_object_count"],
        row["moving_object_count"],
        row["iterations"],
    )


def summarize(rows):
    groups = {}
    for row in rows:
        groups.setdefault(group_key(row), []).append(row)

    summary = []
    for key, group_rows in groups.items():
        experiment, strategy, client_count, static_count, moving_count, iterations = key
        elapsed = [r["elapsed_ms"] for r in group_rows]
        bytes_sent = [r["total_bytes_sent"] for r in group_rows]
        messages_sent = [r["total_messages_sent"] for r in group_rows]

        n = len(elapsed)
        mean_elapsed = statistics.fmean(elapsed)
        # Sample variance (n - 1 denominator); 0 for a single-sample group rather than
        # calling statistics.variance, which raises for n < 2.
        variance_elapsed = statistics.variance(elapsed) if n > 1 else 0.0
        stddev_elapsed = statistics.stdev(elapsed) if n > 1 else 0.0
        mean_bytes = statistics.fmean(bytes_sent)
        mean_messages = statistics.fmean(messages_sent)
        throughput_iters_per_sec = iterations / (mean_elapsed / 1000.0) if mean_elapsed > 0 else float("inf")
        bytes_per_iteration = mean_bytes / iterations

        summary.append(
            {
                "experiment": experiment,
                "strategy": strategy,
                "client_count": client_count,
                "static_object_count": static_count,
                "moving_object_count": moving_count,
                "iterations": iterations,
                "n": n,
                "mean_elapsed_ms": mean_elapsed,
                "variance_elapsed_ms2": variance_elapsed,
                "stddev_elapsed_ms": stddev_elapsed,
                "mean_bytes_sent": mean_bytes,
                "mean_messages_sent": mean_messages,
                "throughput_iterations_per_sec": throughput_iters_per_sec,
                "bytes_per_iteration": bytes_per_iteration,
            }
        )

    # Stable, readable ordering: by experiment, then by the varying dimension, then strategy.
    summary.sort(
        key=lambda s: (
            s["experiment"],
            s["moving_object_count"],
            s["client_count"],
            s["static_object_count"],
            s["strategy"],
        )
    )
    return summary


def write_summary(summary, out_path):
    fieldnames = [
        "experiment",
        "strategy",
        "client_count",
        "static_object_count",
        "moving_object_count",
        "iterations",
        "n",
        "mean_elapsed_ms",
        "variance_elapsed_ms2",
        "stddev_elapsed_ms",
        "mean_bytes_sent",
        "mean_messages_sent",
        "throughput_iterations_per_sec",
        "bytes_per_iteration",
    ]
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for row in summary:
            writer.writerow(row)


def find(summary, **filters):
    """Rows matching every key/value in `filters`, sorted by the filters' implicit order
    is not guaranteed -- callers sort by whatever x-axis field they need."""
    result = []
    for row in summary:
        if all(row[k] == v for k, v in filters.items()):
            result.append(row)
    return result


def plot_object_count_scaling(summary, out_dir):
    moving_counts = sorted({r["moving_object_count"] for r in summary if r["experiment"] == "ObjectCountScaling"})

    fig, ax = plt.subplots(figsize=(7, 5))
    for strategy, color in (("Full", FULL_COLOR), ("Delta", DELTA_COLOR)):
        rows = sorted(
            find(summary, experiment="ObjectCountScaling", strategy=strategy), key=lambda r: r["moving_object_count"]
        )
        x = [r["moving_object_count"] for r in rows]
        y = [r["mean_elapsed_ms"] for r in rows]
        yerr = [r["stddev_elapsed_ms"] for r in rows]
        ax.errorbar(x, y, yerr=yerr, marker="o", capsize=4, label=strategy, color=color)

    ax.set_xscale("log")
    ax.set_xticks(moving_counts)
    ax.set_xticklabels([str(c) for c in moving_counts])
    ax.set_xlabel("Moving objects (clients = 4, static = 0)")
    ax.set_ylabel("Mean elapsed time (ms, 20000 iterations)")
    ax.set_title("Experiment 1: Object-Count Scaling")
    ax.legend(title="Strategy (error bars = sample std dev)")
    ax.grid(True, which="both", linestyle=":", alpha=0.6)
    fig.tight_layout()
    fig.savefig(out_dir / "experiment1_object_count_scaling.png", dpi=150)
    plt.close(fig)


def plot_client_count_scaling(summary, out_dir):
    client_counts = sorted({r["client_count"] for r in summary if r["experiment"] == "ClientCountScaling"})

    fig, ax = plt.subplots(figsize=(7, 5))
    for strategy, color in (("Full", FULL_COLOR), ("Delta", DELTA_COLOR)):
        rows = sorted(find(summary, experiment="ClientCountScaling", strategy=strategy), key=lambda r: r["client_count"])
        x = [r["client_count"] for r in rows]
        y = [r["mean_elapsed_ms"] for r in rows]
        yerr = [r["stddev_elapsed_ms"] for r in rows]
        ax.errorbar(x, y, yerr=yerr, marker="o", capsize=4, label=strategy, color=color)

    ax.set_xticks(client_counts)
    ax.set_xlabel("Client (subscriber) count (moving objects = 100, static = 0)")
    ax.set_ylabel("Mean elapsed time (ms, 20000 iterations)")
    ax.set_title("Experiment 2: Client-Count Scaling")
    ax.legend(title="Strategy (error bars = sample std dev)")
    ax.grid(True, linestyle=":", alpha=0.6)
    fig.tight_layout()
    fig.savefig(out_dir / "experiment2_client_count_scaling.png", dpi=150)
    plt.close(fig)


def _composition_label(static_count, moving_count):
    return f"{static_count}s / {moving_count}m"


def plot_scene_composition(summary, out_dir):
    compositions = [(500, 0), (450, 50), (250, 250), (0, 500)]
    labels = [_composition_label(s, m) for s, m in compositions]
    x = list(range(len(compositions)))

    fig, ax = plt.subplots(figsize=(7, 5))
    for strategy, color in (("Full", FULL_COLOR), ("Delta", DELTA_COLOR)):
        y = []
        yerr = []
        for static_count, moving_count in compositions:
            rows = find(
                summary,
                experiment="SceneComposition",
                strategy=strategy,
                static_object_count=static_count,
                moving_object_count=moving_count,
            )
            y.append(rows[0]["mean_elapsed_ms"])
            yerr.append(rows[0]["stddev_elapsed_ms"])
        ax.errorbar(x, y, yerr=yerr, marker="o", capsize=4, label=strategy, color=color)

    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_xlabel("Scene composition (static objects s / moving objects m, total = 500, clients = 4)")
    ax.set_ylabel("Mean elapsed time (ms, 20000 iterations)")
    ax.set_title("Experiment 3: Static vs Moving Scene Composition")
    ax.legend(title="Strategy (error bars = sample std dev)", loc="upper left")
    ax.grid(True, linestyle=":", alpha=0.6)
    fig.tight_layout()
    fig.savefig(out_dir / "experiment3_scene_composition_time.png", dpi=150)
    plt.close(fig)


def plot_scene_composition_bytes(summary, out_dir):
    """Bonus graph: bytes sent is where the Full-vs-Delta difference is most direct --
    time also reflects per-message ZeroMQ/thread overhead, bytes isolates the actual
    data-format difference the two strategies exist to compare."""
    compositions = [(500, 0), (450, 50), (250, 250), (0, 500)]
    labels = [_composition_label(s, m) for s, m in compositions]
    x = list(range(len(compositions)))

    fig, ax = plt.subplots(figsize=(7, 5))
    for strategy, color in (("Full", FULL_COLOR), ("Delta", DELTA_COLOR)):
        y = []
        for static_count, moving_count in compositions:
            rows = find(
                summary,
                experiment="SceneComposition",
                strategy=strategy,
                static_object_count=static_count,
                moving_object_count=moving_count,
            )
            y.append(rows[0]["mean_bytes_sent"] / 1_000_000.0)  # MB
        ax.plot(x, y, marker="o", label=strategy, color=color)

    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_xlabel("Scene composition (static objects s / moving objects m, total = 500, clients = 4)")
    ax.set_ylabel("Mean total bytes sent (MB, wire-level, 20000 iterations x 4 clients)")
    ax.set_title("Experiment 3: Total Bytes Sent by Scene Composition")
    ax.legend(title="Strategy")
    ax.grid(True, linestyle=":", alpha=0.6)
    fig.tight_layout()
    fig.savefig(out_dir / "experiment3_scene_composition_bytes.png", dpi=150)
    plt.close(fig)


def main():
    results_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "results"
    raw_path = results_dir / "benchmark_raw.csv"
    summary_path = results_dir / "benchmark_summary.csv"
    plots_dir = results_dir / "plots"
    plots_dir.mkdir(parents=True, exist_ok=True)

    rows = load_raw(raw_path)
    if not rows:
        sys.exit(f"No rows found in {raw_path}")

    summary = summarize(rows)
    write_summary(summary, summary_path)
    print(f"Wrote {summary_path} ({len(summary)} condition/strategy rows)")

    plot_object_count_scaling(summary, plots_dir)
    plot_client_count_scaling(summary, plots_dir)
    plot_scene_composition(summary, plots_dir)
    plot_scene_composition_bytes(summary, plots_dir)
    print(f"Wrote 4 plots to {plots_dir}")


if __name__ == "__main__":
    main()
