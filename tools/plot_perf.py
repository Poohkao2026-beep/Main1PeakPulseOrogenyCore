#!/usr/bin/env python3
"""Plot throughput and latency from peak_pulse_perf schema-version-1 JSON."""

import argparse
import json
from pathlib import Path

import matplotlib.pyplot as plt


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_json", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("docs/img"))
    args = parser.parse_args()

    data = json.loads(args.input_json.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise SystemExit("unsupported performance JSON schema")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    records = data["results"]

    fig, axis = plt.subplots(figsize=(8, 5), constrained_layout=True)
    for queue in sorted({record["queue"] for record in records}):
        selected = [
            record
            for record in records
            if record["queue"] == queue
        ]
        selected.sort(key=lambda record: record["producers"] + record["consumers"])
        if selected:
            axis.plot(
                [record["producers"] + record["consumers"] for record in selected],
                [record["items_per_second_median"] for record in selected],
                marker="o",
                label=queue,
            )
    axis.set(
        title="Throughput by workload thread count",
        xlabel="Producer + consumer threads",
    )
    axis.set_ylabel("Items per second")
    axis.grid(True, alpha=0.3)
    axis.legend()
    fig.savefig(args.output_dir / "throughput_vs_threads.png", dpi=180)
    plt.close(fig)

    fig, axis = plt.subplots(figsize=(8, 5), constrained_layout=True)
    for record in records:
        if record["scenario"] != "mpmc":
            continue
        latencies = record["latency"]
        axis.plot(
            [25, 50, 75, 90],
            [latency["p99_ns"] for latency in latencies],
            marker="o",
            label=f'{record["queue"]} p99',
        )
    axis.set(
        title="MPMC p99 latency under offered load",
        xlabel="Offered load (% of measured max throughput)",
    )
    axis.set_ylabel("Latency (ns)")
    axis.grid(True, alpha=0.3)
    axis.legend()
    fig.savefig(args.output_dir / "latency_vs_load.png", dpi=180)
    plt.close(fig)


if __name__ == "__main__":
    main()
