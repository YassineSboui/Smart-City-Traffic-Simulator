#!/usr/bin/env python3
import csv
import sys


def read_results(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def write_text(rows):
    with open("benchmark.txt", "w", encoding="utf-8") as handle:
        handle.write("SmartCross benchmark summary\n")
        handle.write("strategy,elapsed_s,avg_wait_ms,throughput,speedup\n")
        for row in rows:
            handle.write(
                f"{row['strategy']},{row['elapsed_s']},{row['avg_wait_ms']},"
                f"{row['throughput_cars_s']},{row['speedup']}\n"
            )
    print("benchmark.txt generated (matplotlib not available)")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "results.csv"
    rows = read_results(path)
    if not rows:
        print("No benchmark data found", file=sys.stderr)
        return 1

    try:
        import matplotlib.pyplot as plt
    except Exception:
        write_text(rows)
        return 0

    strategies = [row["strategy"] for row in rows]
    elapsed = [float(row["elapsed_s"]) for row in rows]
    wait = [float(row["avg_wait_ms"]) for row in rows]

    fig, axes = plt.subplots(1, 2, figsize=(11, 4))
    axes[0].bar(strategies, elapsed, color="#2f80ed")
    axes[0].set_title("Temps total")
    axes[0].set_ylabel("secondes")
    axes[0].tick_params(axis="x", rotation=25)

    axes[1].bar(strategies, wait, color="#27ae60")
    axes[1].set_title("Attente moyenne")
    axes[1].set_ylabel("ms / voiture")
    axes[1].tick_params(axis="x", rotation=25)

    fig.suptitle("SmartCross - comparaison des strategies")
    fig.tight_layout()
    fig.savefig("benchmark.png", dpi=140)
    print("benchmark.png generated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
