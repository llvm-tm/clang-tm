#!/usr/bin/env python3
"""Run all benchmarks on local machine and plot results."""

import subprocess, csv, os, sys, time, re
from pathlib import Path

BENCH_DIR = Path(__file__).parent
STAMP_DIR = BENCH_DIR / "STAMP" / "bin"
TPCC_DIR  = BENCH_DIR / "TPCC" / "bin"
S7_DIR    = BENCH_DIR / "STMbench7" / "bin"

DURATION_MS = 3000
THREADS = [1, 2, 4, 6, 8, 12, 16]
SAMPLES = 10
TIMEOUT = 30

OUTPUT = BENCH_DIR / "results_mac.csv"
PLOT    = BENCH_DIR / "results_mac.png"

backgrounds = {
    "STAMP":     {"singlelock": str(STAMP_DIR / "stamp_singlelock")},
    "TPCC":      {"singlelock": str(TPCC_DIR / "tpcc_singlelock"),
                   "tl2":        str(TPCC_DIR / "tpcc_tl2")},
    "STMbench7": {"singlelock": str(S7_DIR / "stmbench_singlelock"),
                   "tl2":        str(S7_DIR / "stmbench_tl2")},
}

stamp_benchmarks = ["genome", "intruder", "kmeans", "labyrinth", "ssca2", "vacation", "yada"]

def parse_throughput(output):
    for line in output.splitlines():
        m = re.search(r"Ops/sec:\s+([0-9.]+)", line)
        if m:
            return float(m.group(1))
    return None

def run_one(cmd, desc):
    results = []
    for s in range(SAMPLES):
        try:
            out = subprocess.check_output(
                ["gtimeout", str(TIMEOUT)] + cmd,
                stderr=subprocess.STDOUT,
                timeout=TIMEOUT + 10
            ).decode()
        except subprocess.TimeoutExpired:
            return None
        except subprocess.CalledProcessError as e:
            out = e.output.decode() if e.output else ""
            if e.returncode in (124, 137):
                return None
            return None
        val = parse_throughput(out)
        if val is None:
            return None
        results.append(val)
    if not results:
        return None
    return sum(results) / len(results)

rows = []

# STAMP — run all sub-benchmarks
for backend, bpath in backgrounds["STAMP"].items():
    if not os.path.exists(bpath):
        print(f"SKIP stamp/{backend}: not found")
        continue
    for bench in stamp_benchmarks:
        for t in THREADS:
            desc = f"stamp_{backend}/{bench} t={t}"
            print(f"  {desc}...", end=" ", flush=True)
            avg = run_one([bpath, "-b", bench[0], "-t", str(t), "-d", str(DURATION_MS)], desc)
            rows.append({
                "benchmark": "STAMP",
                "subbenchmark": bench,
                "backend": backend,
                "threads": t,
                "throughput": avg if avg is not None else 0
            })
            print(f"{avg:.0f}" if avg else "FAIL")

# TPCC
for backend, bpath in backgrounds["TPCC"].items():
    if not os.path.exists(bpath):
        print(f"SKIP tpcc/{backend}: not found")
        continue
    for t in THREADS:
        desc = f"tpcc_{backend} t={t}"
        print(f"  {desc}...", end=" ", flush=True)
        avg = run_one([bpath, "-t", str(t), "-d", str(DURATION_MS)], desc)
        rows.append({
            "benchmark": "TPCC",
            "subbenchmark": "tpcc",
            "backend": backend,
            "threads": t,
            "throughput": avg if avg is not None else 0
        })
        print(f"{avg:.0f}" if avg else "FAIL")

# STMbench7
for backend, bpath in backgrounds["STMbench7"].items():
    if not os.path.exists(bpath):
        print(f"SKIP stmbench7/{backend}: not found")
        continue
    for t in THREADS:
        desc = f"stmbench7_{backend} t={t}"
        print(f"  {desc}...", end=" ", flush=True)
        avg = run_one([bpath, "-t", str(t), "-d", str(DURATION_MS)], desc)
        rows.append({
            "benchmark": "STMbench7",
            "subbenchmark": "stmbench7",
            "backend": backend,
            "threads": t,
            "throughput": avg if avg is not None else 0
        })
        print(f"{avg:.0f}" if avg else "FAIL")

# Write CSV
with open(OUTPUT, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["benchmark","subbenchmark","backend","threads","throughput"])
    w.writeheader()
    w.writerows(rows)
print(f"\nResults written to {OUTPUT}")

# ---- Plot ----
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

df = pd.read_csv(OUTPUT)

fig, axes = plt.subplots(1, 3, figsize=(24, 8))
fig.suptitle("TM Benchmark Throughput — Apple Silicon (ARM macOS)", fontsize=16, y=1.02)

colors = {"singlelock": "#2196F3", "tl2": "#FF9800"}

# STAMP — multi-line, one sub-benchmark per line
ax = axes[0]
ax.set_title("STAMP (SingleGlobalLock)")
stamp_df = df[df["benchmark"] == "STAMP"]
for bench in stamp_benchmarks:
    sub = stamp_df[stamp_df["subbenchmark"] == bench].sort_values("threads")
    if sub.empty:
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
for backend in backgrounds["TPCC"]:
    sub = tpcc_df[tpcc_df["backend"] == backend].sort_values("threads")
    if sub.empty:
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
for backend in backgrounds["STMbench7"]:
    sub = s7_df[s7_df["backend"] == backend].sort_values("threads")
    if sub.empty:
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
