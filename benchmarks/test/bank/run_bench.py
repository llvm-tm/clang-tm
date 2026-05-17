#!/usr/bin/env python3
"""Benchmark runner: sweeps thread counts × backends × samples, collects throughput + TSX stats."""

import subprocess
import sys
import os
import csv
import re
import time
from pathlib import Path

BIN_DIR = Path("bin")
BACKENDS = {
    "uninstrumented": BIN_DIR / "bank_uninstrumented",
    "singlelock":     BIN_DIR / "bank_singlelock",
    "tinystm_wbctl":  BIN_DIR / "bank_tinystm",
    "tsxsgl":         BIN_DIR / "bank_tsxsgl",
}
THREADS = [1, 2, 4, 7, 12, 14, 16, 21, 28, 35, 42, 49, 56]
SAMPLES = 10
DURATION_MS = 5000
ACCOUNTS = 1024
READ_ALL = 0

results = []
tsx_stats = []

def parse_tsxstats(stderr_text: str):
    """Parse TSXSTATS line from stderr."""
    m = re.search(r'TSXSTATS\s+started=(\d+)\s+committed=(\d+)\s+aborted=(\d+)\s+lock_busy=(\d+)\s+epoch_changed=(\d+)\s+other_abort=(\d+)\s+sgl=(\d+)\s+attempts_gt_1=(\d+)', stderr_text)
    if m:
        return {
            'started': int(m.group(1)),
            'committed': int(m.group(2)),
            'aborted': int(m.group(3)),
            'lock_busy': int(m.group(4)),
            'epoch_changed': int(m.group(5)),
            'other_abort': int(m.group(6)),
            'sgl': int(m.group(7)),
            'attempts_gt_1': int(m.group(8)),
        }
    return None

def parse_throughput(stdout_text: str):
    """Parse Txns/sec from stdout (handles scientific notation like 3.68e+06)."""
    m = re.search(r'Txns/sec:\s+([\d.eE+\-]+)', stdout_text)
    if m:
        return float(m.group(1))
    return None

def parse_transactions(stdout_text: str):
    """Parse total transactions count."""
    m = re.search(r'Total txns:\s+(\d+)', stdout_text)
    if m:
        return int(m.group(1))
    return None

def run_one(backend: str, threads: int, sample: int):
    binary = BACKENDS[backend]
    if not binary.exists():
        print(f"  SKIP: {binary} not found")
        return None
    cmd = [str(binary), "-t", str(threads), "-d", str(DURATION_MS), "-r", str(READ_ALL), "-a", str(ACCOUNTS)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=DURATION_MS // 1000 * 2 + 10)
        exit_code = proc.returncode
        stdout = proc.stdout
        stderr = proc.stderr

        tps = parse_throughput(stdout)
        total_txns = parse_transactions(stdout)
        stats = parse_tsxstats(stderr) if backend == "tsxsgl" else None
        passed = exit_code == 0

        return {
            'backend': backend,
            'threads': threads,
            'sample': sample,
            'passed': passed,
            'tps': tps,
            'total_txns': total_txns,
            'exit_code': exit_code,
            **({} if stats is None else stats)
        }
    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT")
        return None
    except FileNotFoundError:
        print(f"  NOT FOUND")
        return None

def main():
    os.chdir(Path(__file__).parent)

    total_runs = len(BACKENDS) * len(THREADS) * SAMPLES
    run_count = 0

    print(f"Running {total_runs} experiments ({len(BACKENDS)} backends × {len(THREADS)} thread counts × {SAMPLES} samples)")
    print(f"Each run: {DURATION_MS}ms, -r {READ_ALL}, -a {ACCOUNTS}")
    print()

    for backend in BACKENDS:
        print(f"\n{'='*60}")
        print(f"Backend: {backend}")
        print(f"{'='*60}")
        for threads in THREADS:
            print(f"  Threads={threads}: ", end="", flush=True)
            for s in range(SAMPLES):
                run_count += 1
                print(f"{s+1} ", end="", flush=True)
                r = run_one(backend, threads, s)
                if r:
                    results.append(r)
            print()

    # Write CSV results
    csv_path = "benchmark_results.csv"
    fieldnames = ['backend', 'threads', 'sample', 'passed', 'tps', 'total_txns', 'exit_code',
                  'started', 'committed', 'aborted', 'lock_busy', 'epoch_changed',
                  'other_abort', 'sgl', 'attempts_gt_1']
    with open(csv_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)

    print(f"\nResults written to {csv_path}")
    print(f"Total runs: {len(results)}")

    # Summary
    print(f"\n{'='*60}")
    print("Correctness Summary")
    print(f"{'='*60}")
    for backend in BACKENDS:
        runs = [r for r in results if r['backend'] == backend]
        total = len(runs)
        passed = sum(1 for r in runs if r['passed'])
        failed = total - passed
        print(f"  {backend:20s}: {passed}/{total} passed", end="")
        if failed:
            print(f"  FAILED runs:", [f"t={r['threads']} s={r['sample']}" for r in runs if not r['passed']], end="")
        print()

    print(f"\n{'='*60}")
    print("Throughput Summary (mean ± std across samples)")
    print(f"{'='*60}")
    for backend in BACKENDS:
        backend_runs = [r for r in results if r['backend'] == backend and r['passed']]
        for threads in THREADS:
            tps_vals = [r['tps'] for r in backend_runs if r['threads'] == threads and r['tps'] is not None]
            if tps_vals:
                mean = sum(tps_vals) / len(tps_vals)
                std = (sum((v - mean)**2 for v in tps_vals) / len(tps_vals))**0.5 if len(tps_vals) > 1 else 0
                print(f"  {backend:20s} t={threads:2d}: {mean:10.0f} ± {std:8.0f} txns/sec ({len(tps_vals)} samples)")

    # TSXSGL stats summary
    tsx_runs = [r for r in results if r['backend'] == 'tsxsgl' and r.get('started') is not None]
    if tsx_runs:
        print(f"\n{'='*60}")
        print("TSXSGL Abort Stats")
        print(f"{'='*60}")
        for threads in THREADS:
            runs = [r for r in tsx_runs if r['threads'] == threads]
            if not runs:
                continue
            avg_started = sum(r['started'] for r in runs) / len(runs)
            avg_committed = sum(r['committed'] for r in runs) / len(runs)
            avg_aborted = sum(r['aborted'] for r in runs) / len(runs)
            avg_lock_busy = sum(r['lock_busy'] for r in runs) / len(runs)
            avg_epoch_chg = sum(r['epoch_changed'] for r in runs) / len(runs)
            avg_other = sum(r['other_abort'] for r in runs) / len(runs)
            avg_sgl = sum(r['sgl'] for r in runs) / len(runs)
            avg_att_gt1 = sum(r['attempts_gt_1'] for r in runs) / len(runs)

            total_txns_tsx = avg_started + avg_sgl
            tsx_pct = 100 * avg_started / total_txns_tsx if total_txns_tsx > 0 else 0
            total_attempts = avg_started + avg_aborted
            abort_rate = 100 * avg_aborted / total_attempts if total_attempts > 0 else 0

            print(f"  t={threads:2d}: TSX={avg_started:8.0f} SGL={avg_sgl:8.0f} "
                  f"TSX%= {tsx_pct:4.1f}% abort_rate={abort_rate:5.1f}% "
                  f"lock_busy={avg_lock_busy:8.0f} epoch_chg={avg_epoch_chg:8.0f} "
                  f"other={avg_other:8.0f} att>1={avg_att_gt1:8.0f}")

    # Generate plotting script
    create_plot_script(results)

def create_plot_script(results):
    """Write a Gnuplot script for throughput and abort rate plots."""
    plot_script = """set terminal pngcairo size 1200,800 enhanced font 'DejaVu Sans,11'
set style data linespoints
set grid

# Throughput plot
set output 'throughput.png'
set title 'Bank Benchmark Throughput (-r 0, 1024 accounts, 5s)'
set xlabel 'Threads'
set ylabel 'Throughput (txns/sec)'
set key top left
plot \\
"""
    backends = ['singlelock', 'tinystm_wbctl', 'tsxsgl', 'uninstrumented']
    labels = {'singlelock': 'SingleGlobalLock', 'tinystm_wbctl': 'TinySTM WBCTL',
              'tsxsgl': 'TSXSGL', 'uninstrumented': 'Uninstrumented'}
    # Write data files for each backend
    for backend in backends:
        datafile = f'/tmp/thru_{backend}.dat'
        label = labels[backend]
        with open(datafile, 'w') as f:
            for t in THREADS:
                vals = [r['tps'] for r in results if r['backend'] == backend and r['threads'] == t and r['tps'] is not None and r['passed']]
                if vals:
                    mean = sum(vals) / len(vals)
                    f.write(f"{t} {mean}\n")
        if backend != backends[-1]:
            plot_script += f"  '{datafile}' using 1:2 title '{label}' lw 2, \\\n"
        else:
            plot_script += f"  '{datafile}' using 1:2 title '{label}' lw 2\n"

    # Abort rate plot (TSXSGL only)
    datafile = '/tmp/tsx_abort.dat'
    with open(datafile, 'w') as f:
        for t in THREADS:
            runs = [r for r in results if r['backend'] == 'tsxsgl' and r['threads'] == t and r.get('aborted') is not None]
            if runs:
                avg_started = sum(r['started'] for r in runs) / len(runs)
                avg_aborted = sum(r['aborted'] for r in runs) / len(runs)
                avg_sgl = sum(r['sgl'] for r in runs) / len(runs)
                total_attempts = avg_started + avg_aborted
                abort_rate = 100 * avg_aborted / total_attempts if total_attempts > 0 else 0
                sgl_pct = 100 * avg_sgl / (avg_started + avg_sgl) if (avg_started + avg_sgl) > 0 else 0
                tsx_pct = 100 * avg_started / (avg_started + avg_sgl) if (avg_started + avg_sgl) > 0 else 0
                f.write(f"{t} {abort_rate} {sgl_pct} {tsx_pct}\n")

    plot_script += """
# Abort rate plot
set output 'tsx_abort_rate.png'
set title 'TSXSGL: TSX Abort Rate vs Thread Count'
set xlabel 'Threads'
set ylabel 'Rate (%)'
plot '/tmp/tsx_abort.dat' using 1:2 title 'TSX abort rate (per attempt)' lw 2 with lines, \\
     '/tmp/tsx_abort.dat' using 1:3 title 'SGL fallback rate (per TX)' lw 2 with lines, \\
     '/tmp/tsx_abort.dat' using 1:4 title 'TSX success rate (per TX)' lw 2 with lines
"""
    with open('benchmark_plots.gp', 'w') as f:
        f.write(plot_script)
    print("\nPlot script written to benchmark_plots.gp")
    print("Run: gnuplot benchmark_plots.gp")

if __name__ == "__main__":
    main()
