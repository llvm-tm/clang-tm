#!/usr/bin/env python3
"""
Detailed STAMP profiling with TM metrics collection.

1. Builds plugin STAMP with metrics-patched TinySTM
2. Runs each benchmark with paper-matched params
3. Parses TM_STATS: commits, avg/min/max reads/writes, aborts
4. Parses benchmark timing and operation counts
5. Compares with Table VI characterization data
6. Generates a report
"""

import csv
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PLUGIN_DIR = REPO_ROOT / "benchmarks" / "plugin" / "STAMP"
STAMP_CSV = REPO_ROOT / "benchmarks" / "stamp_characterization.csv"

SHARED = "shared"
CLI_ARGS = {
    "bayes":      {SHARED: "-v 32 -r 1024 -n 2 -c 20 -i 2 -e 2"},
    "genome":     {SHARED: "-g 256 -s 16 -n 16384"},
    "intruder":   {SHARED: "-a 10 -l 4 -n 2048 -s 1"},
    "kmeans-high":  {"plugin": "-m 16 -n 2048 -t 0.05"},
    "kmeans-low":   {"plugin": "-m 16 -n 2048 -t 0.05"},
    "labyrinth":  {SHARED: "-x 32 -y 32 -z 3 -n 96"},
    "ssca2":      {SHARED: "-s 13 -i 1 -u 1.0 -l 3 -p 3"},
    "vacation-high":  {"plugin": "-n 4 -q 60 -u 90 -r 16384 -t 4096"},
    "vacation-low":   {"plugin": "-n 2 -q 90 -u 98 -r 16384 -t 4096"},
    "yada":      {SHARED: "-a 20 -j 0.5"},
}

def get_params(bench: str) -> str:
    entry = CLI_ARGS.get(bench, {})
    return entry.get(SHARED, entry.get("plugin", ""))

def bname(bench: str) -> str:
    for b in ["bayes","genome","intruder","kmeans","labyrinth","ssca2","vacation","yada"]:
        if b in bench:
            return b
    return bench

BENCHMARKS = ["bayes","genome","intruder","kmeans-high","kmeans-low",
              "labyrinth","ssca2","vacation-high","vacation-low","yada"]

@dataclass
class TMStats:
    commits: int = 0
    avg_reads: float = 0.0
    min_reads: int = 0
    max_reads: int = 0
    avg_writes: float = 0.0
    min_writes: int = 0
    max_writes: int = 0
    total_reads: int = 0
    total_writes: int = 0
    aborts: int = 0

@dataclass
class BenchResult:
    benchmark: str
    threads: int
    elapsed_ms: Optional[float] = None
    ops: Optional[int] = None
    tm_stats: Optional[TMStats] = None
    passed: bool = False
    error: str = ""

# ── Parsers ────────────────────────────────────────────────

RE_TIME = re.compile(
    r"(?:Time|Elapsed|Learn time)\s*[=:]\s*([\d.]+)\s*(?:ms|sec|seconds)?"
    r"|Time taken for all is\s*([\d.]+)\s*sec"
    r"|Results\s*\((\d+)\s*ms\)"
    r"|Elapsed:\s+(\d+)\s+ms", re.IGNORECASE)
RE_OPS = re.compile(
    r"(?:Total ops|Operations|Num found|Unique segments|Paths routed|Total edges learned)"
    r"[=:\s]*(\d+)", re.IGNORECASE)
RE_CHECK = re.compile(r"(?:PASS|Verification passed|done\.)", re.IGNORECASE)

RE_TM_STATS = re.compile(
    r"TM_STATS:\s*commits=(\d+)"
    r"\s+avg_reads=([\d.]+)\s+min_reads=(\d+)\s+max_reads=(\d+)"
    r"\s+avg_writes=([\d.]+)\s+min_writes=(\d+)\s+max_writes=(\d+)"
    r"\s+aborts=(\d+)"
)

def parse_output(out: str) -> Tuple[Optional[float], Optional[int], bool]:
    elapsed = None
    ops = None
    for m in RE_TIME.finditer(out):
        if m.group(1):
            elapsed = float(m.group(1)) if "ms" in m.group(0) else float(m.group(1)) * 1000
        if m.group(2): elapsed = float(m.group(2)) * 1000
        if m.group(3): elapsed = float(m.group(3))
        if m.group(4): elapsed = float(m.group(4))
    for m in RE_OPS.finditer(out):
        ops = int(m.group(1))
    passed = bool(RE_CHECK.search(out))
    return elapsed, ops, passed

def parse_tm_stats(out: str) -> Optional[TMStats]:
    m = RE_TM_STATS.search(out)
    if not m:
        return None
    s = TMStats()
    s.commits = int(m.group(1))
    s.avg_reads = float(m.group(2))
    s.min_reads = int(m.group(3))
    s.max_reads = int(m.group(4))
    s.avg_writes = float(m.group(5))
    s.min_writes = int(m.group(6))
    s.max_writes = int(m.group(7))
    s.aborts = int(m.group(8))
    s.total_reads = int(s.avg_reads * s.commits + 0.5)
    s.total_writes = int(s.avg_writes * s.commits + 0.5)
    return s

# ── Runner ──────────────────────────────────────────────────

def run(bench: str, threads: int, timeout: int = 120) -> BenchResult:
    binary = PLUGIN_DIR / "bin" / "stamp_tinystm"
    if not binary.exists():
        return BenchResult(bench, threads, error="binary not found")
    params = get_params(bench)
    cmd = [str(binary), "-p", str(threads), "-b", bname(bench)] + params.split()
    try:
        proc = subprocess.Popen(cmd, cwd=PLUGIN_DIR,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stdout, stderr = proc.communicate(timeout=timeout)
        out = stdout + stderr
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return BenchResult(bench, threads, error="TIMEOUT")
    except Exception as e:
        return BenchResult(bench, threads, error=str(e))

    elapsed, ops, passed = parse_output(out)
    tm = parse_tm_stats(out)
    if rc != 0 and not passed:
        tail = stderr.strip().split("\n")[-3:] if stderr.strip() else [f"exit {rc}"]
        return BenchResult(bench, threads, error="; ".join(tail))
    return BenchResult(bench, threads, elapsed, ops, tm, passed or rc == 0, "")

# ── Table VI Loader ─────────────────────────────────────────

def load_table_vi() -> Dict[str, dict]:
    """Load characterization data from stamp_characterization.csv.
    Returns dict keyed by benchmark name."""
    if not STAMP_CSV.exists():
        return {}
    data = {}
    with open(STAMP_CSV) as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = row.get("Application", "").strip().lower()
            if key:
                data[key] = row
    return data

# ── Report ──────────────────────────────────────────────────

def print_report(results: List[BenchResult], table_vi: Dict[str, dict]):
    print(f"\n{'='*90}")
    print(f"STAMP Profiling Report — Detailed TM Metrics")
    print(f"{'='*90}")
    print(f"{'Benchmark':<20s} {'Time':>8s} {'Ops':>8s} {'Com':>6s} "
          f"{'AvgR':>6s} {'MinR':>5s} {'MaxR':>5s} "
          f"{'AvgW':>6s} {'MinW':>5s} {'MaxW':>5s} {'Abrt':>5s}")
    print(f"{'-'*90}")

    for r in results:
        if r.error:
            print(f"{r.benchmark:<20s} {'FAIL':>8s}  {r.error}")
            continue
        tm = r.tm_stats
        if tm:
            print(f"{r.benchmark:<20s} {r.elapsed_ms:>7.0f}ms {r.ops if r.ops else 0:>8d} "
                  f"{tm.commits:>6d} {tm.avg_reads:>6.1f} {tm.min_reads:>5d} {tm.max_reads:>5d} "
                  f"{tm.avg_writes:>6.1f} {tm.min_writes:>5d} {tm.max_writes:>5d} {tm.aborts:>5d}")
        else:
            print(f"{r.benchmark:<20s} {r.elapsed_ms:>7.0f}ms {r.ops if r.ops else 0:>8d}  "
                  f"(no TM stats)")

    # Compare with Table VI
    if table_vi:
        print(f"\n{'='*90}")
        print(f"Comparison with Table VI (IISWC 2008)")
        print(f"{'='*90}")
        print(f"{'Benchmark':<20s} {'Source':>8s} {'Trans':>7s} {'AvgR':>6s} {'AvgW':>6s} "
              f"{'TotR':>8s} {'TotW':>8s} {'Aborts':>7s}")
        print(f"{'-'*90}")
        for r in results:
            if r.error or not r.tm_stats:
                continue
            tm = r.tm_stats
            vi = table_vi.get(r.benchmark, {})
            if vi:
                vi_commits = vi.get("Transactions (%)", vi.get("Transactions", "?")).replace(",", "").replace("~", "")
                vi_avg_r = vi.get("Read Set (90 pctile)", vi.get("Read Set", "?"))
                vi_avg_w = vi.get("Write Set (90 pctile)", vi.get("Write Set", "?"))
                vi_tot_r = vi.get("Read Barrier (90 pctile)", vi.get("Read Barriers", "?")).replace(",", "").replace("~", "")
                vi_tot_w = vi.get("Write Barrier (90 pctile)", vi.get("Write Barriers", "?")).replace(",", "").replace("~", "")
                print(f"{r.benchmark:<20s} {'Measured':>8s} {tm.commits:>7d} {tm.avg_reads:>6.1f} "
                      f"{tm.avg_writes:>6.1f} {tm.total_reads:>8d} {tm.total_writes:>8d} {tm.aborts:>7d}")
                print(f"{'':20s} {'Table VI':>8s} {vi_commits:>7s} {vi_avg_r:>6s} {vi_avg_w:>6s} "
                      f"{vi_tot_r:>8s} {vi_tot_w:>8s} {'':>7s}")
            else:
                print(f"{r.benchmark:<20s} {'Measured':>8s} {tm.commits:>7d} {tm.avg_reads:>6.1f} "
                      f"{tm.avg_writes:>6.1f} {tm.total_reads:>8d} {tm.total_writes:>8d} {tm.aborts:>7d}")
                print(f"{'':20s} {'Table VI':>8s} {'N/A':>7s}")
            print()

    n_ok = sum(1 for r in results if not r.error)
    n_fail = sum(1 for r in results if r.error)
    print(f"{'='*90}")
    print(f"SUMMARY  Total: {len(results)}  OK: {n_ok}  Failed: {n_fail}")
    print(f"{'='*90}")

# ── Main ────────────────────────────────────────────────────

def main():
    import argparse
    ap = argparse.ArgumentParser(description="Detailed STAMP Profiling")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--benchmarks", nargs="+", default=BENCHMARKS)
    ap.add_argument("--rebuild", action="store_true", default=True)
    args = ap.parse_args()

    if args.rebuild:
        print("[BUILD] Building plugin with metrics...", flush=True)
        rc = subprocess.call(["make", "-j", "stamp_tinystm"], cwd=PLUGIN_DIR,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if rc != 0:
            print("BUILD FAILED")
            sys.exit(1)

    table_vi = load_table_vi()
    results = []
    for bench in args.benchmarks:
        print(f"\n[{bench}]", flush=True)
        r = run(bench, args.threads, args.timeout)
        if r.error:
            print(f"  FAIL: {r.error}")
        else:
            tm = r.tm_stats
            if tm:
                print(f"  {r.elapsed_ms:.0f}ms  ops={r.ops}  "
                      f"commits={tm.commits}  reads={tm.avg_reads:.1f}({tm.min_reads}-{tm.max_reads})  "
                      f"writes={tm.avg_writes:.1f}({tm.min_writes}-{tm.max_writes})  aborts={tm.aborts}")
            else:
                print(f"  {r.elapsed_ms:.0f}ms  ops={r.ops}  (no TM stats)")
        results.append(r)

    print_report(results, table_vi)

if __name__ == "__main__":
    main()
