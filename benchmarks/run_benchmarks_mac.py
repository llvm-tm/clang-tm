#!/usr/bin/env python3
"""Run all benchmarks locally and plot results.  Writes results incrementally."""

import subprocess, csv, os, sys, re, time
from pathlib import Path

BENCH_DIR = Path(__file__).parent
DURATION_MS = 3000
THREADS = [1, 2, 4, 6, 8, 12, 16]
SAMPLES = 3
TIMEOUT = 60

OUTPUT = BENCH_DIR / "results_mac.csv"
PLOT = BENCH_DIR / "results_mac.png"

# {(benchmark, subbenchmark, backend): binary_path}
BINARIES = {
    ("STAMP", b, "singlelock"): BENCH_DIR / "STAMP" / "bin" / "stamp_singlelock"
    for b in ["genome", "intruder", "kmeans", "labyrinth", "ssca2", "vacation", "yada"]
}
BINARIES.update({
    ("TPCC", "tpcc", "singlelock"): BENCH_DIR / "TPCC" / "bin" / "tpcc_singlelock",
    ("TPCC", "tpcc", "tl2"):        BENCH_DIR / "TPCC" / "bin" / "tpcc_tl2",
    ("STMbench7", "stmbench7", "singlelock"): BENCH_DIR / "STMbench7" / "bin" / "stmbench_singlelock",
    ("STMbench7", "stmbench7", "tl2"):        BENCH_DIR / "STMbench7" / "bin" / "stmbench_tl2",
})

def parse_throughput(output):
    for line in output.splitlines():
        m = re.search(r"Ops/sec:\s+([0-9.]+)", line)
        if m:
            return float(m.group(1))
    return None

def build_cmd(benchmark, subbenchmark, backend, threads):
    bp = BINARIES[(benchmark, subbenchmark, backend)]
    if benchmark == "STAMP":
        return [str(bp), "-b", subbenchmark[0], "-t", str(threads), "-d", str(DURATION_MS)]
    else:
        return [str(bp), "-t", str(threads), "-d", str(DURATION_MS)]

def run_one(cmd):
    for s in range(SAMPLES):
        try:
            out = subprocess.check_output(
                ["gtimeout", str(TIMEOUT)] + cmd,
                stderr=subprocess.STDOUT, timeout=TIMEOUT + 10
            ).decode()
        except (subprocess.TimeoutExpired, subprocess.CalledProcessError):
            return None
        val = parse_throughput(out)
        if val is None:
            return None
    return val

def main():
    fieldnames = ["benchmark", "subbenchmark", "backend", "threads", "throughput"]
    rows = []

    total = len(BINARIES) * len(THREADS)
    done = 0

    for (bm, sub, bk), _ in sorted(BINARIES.items()):
        bp = BINARIES[(bm, sub, bk)]
        if not os.path.exists(bp):
            print(f"SKIP {bm}/{bk}/{sub}: binary not found")
            continue
        for t in THREADS:
            done += 1
            cmd = build_cmd(bm, sub, bk, t)
            desc = f"{bm}/{bk}/{sub} t={t} ({done}/{total})"
            print(f"  {desc}...", end=" ", flush=True)
            avg = run_one(cmd)
            row = {"benchmark": bm, "subbenchmark": sub, "backend": bk,
                   "threads": t, "throughput": avg if avg is not None else 0}
            rows.append(row)
            print(f"{avg:.0f}" if avg else "FAIL")

            # Write incrementally
            with open(OUTPUT, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=fieldnames)
                w.writeheader()
                w.writerows(rows)

    print(f"\nResults written to {OUTPUT}")

    # ---- Plot ----
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import pandas as pd

    df = pd.read_csv(OUTPUT)

    fig, axes = plt.subplots(1, 3, figsize=(24, 8))
    fig.suptitle("TM Benchmark Throughput — Apple Silicon (ARM macOS)", fontsize=16, y=1.02)

    colors = {"singlelock": "#2196F3", "tl2": "#FF9800"}

    # STAMP — one line per sub-benchmark
    ax = axes[0]
    ax.set_title("STAMP (SingleGlobalLock)")
    stamp_df = df[df["benchmark"] == "STAMP"]
    for bench in ["genome", "intruder", "kmeans", "labyrinth", "ssca2", "vacation", "yada"]:
        sub = stamp_df[stamp_df["subbenchmark"] == bench].sort_values("threads")
        if sub.empty or (sub["throughput"] == 0).all():
            continue
        ax.plot(sub["threads"], sub["throughput"], marker="o", label=bench)
    ax.set_xlabel("Threads")
    ax.set_ylabel("Throughput (ops/sec)")
    ax.legend(fontsize=7)
    ax.grid(True, alpha=0.3)

    # TPCC
    ax = axes[1]
    ax.set_title("TPCC")
    tpcc_df = df[df["benchmark"] == "TPCC"]
    for backend in ["singlelock", "tl2"]:
        sub = tpcc_df[tpcc_df["backend"] == backend].sort_values("threads")
        if sub.empty or (sub["throughput"] == 0).all():
            continue
        ax.plot(sub["threads"], sub["throughput"], marker="o",
                color=colors.get(backend), label=backend)
    ax.set_xlabel("Threads")
    ax.set_ylabel("Throughput (ops/sec)")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # STMbench7
    ax = axes[2]
    ax.set_title("STMbench7")
    s7_df = df[df["benchmark"] == "STMbench7"]
    for backend in ["singlelock", "tl2"]:
        sub = s7_df[s7_df["backend"] == backend].sort_values("threads")
        if sub.empty or (sub["throughput"] == 0).all():
            continue
        ax.plot(sub["threads"], sub["throughput"], marker="o",
                color=colors.get(backend), label=backend)
    ax.set_xlabel("Threads")
    ax.set_ylabel("Throughput (ops/sec)")
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(PLOT, dpi=150, bbox_inches="tight")
    print(f"Plot saved to {PLOT}")

if __name__ == "__main__":
    main()
