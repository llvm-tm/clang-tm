#!/usr/bin/env python3
"""CSMV GPU profiling experiment runner.

Builds CSMV benchmarks with CSMV_PROFILE=1, runs them,
parses CSMV_STATS output, and writes calibration JSON.

Usage:
    python3 run_csmv_profiling.py [--build-dir BUILD] [--output-dir OUT]
"""

import subprocess, sys, os, re, json, time, csv, argparse
from pathlib import Path
from dataclasses import dataclass, asdict
from typing import Optional

# ── Regex for CSMV_STATS output line ─────────────────────────────
CSMV_STATS_RE = re.compile(
    r"CSMV_STATS:"
    r"\s+txns=(\d+)"
    r"\s+batches=(\d+)"
    r"\s+max_batch=(\d+)"
    r"\s+kernel_ns=(\d+)"
    r"\s+h2d_ns=(\d+)"
    r"\s+d2h_ns=(\d+)"
    r"\s+kernel_avg_ns=(\d+)"
    r"\s+tx_avg_ns=(\d+)"
    r"\s+occ_avg=(\d+)"
)

TIME_RE = re.compile(
    r"(?:Time|time|Duration|wall)\s*[=:]\s*([0-9.]+)\s*(s|ms|us|ns)?",
    re.IGNORECASE
)

@dataclass
class CsmvStats:
    txns: int = 0
    batches: int = 0
    max_batch: int = 0
    kernel_ns: int = 0
    h2d_ns: int = 0
    d2h_ns: int = 0
    kernel_avg_ns: int = 0
    tx_avg_ns: int = 0
    occ_avg: int = 0

    @classmethod
    def from_line(cls, line: str) -> Optional['CsmvStats']:
        m = CSMV_STATS_RE.match(line.strip())
        if not m:
            return None
        return cls(
            txns=int(m.group(1)),
            batches=int(m.group(2)),
            max_batch=int(m.group(3)),
            kernel_ns=int(m.group(4)),
            h2d_ns=int(m.group(5)),
            d2h_ns=int(m.group(6)),
            kernel_avg_ns=int(m.group(7)),
            tx_avg_ns=int(m.group(8)),
            occ_avg=int(m.group(9)),
        )

def parse_wall_time(line: str) -> Optional[float]:
    m = TIME_RE.search(line)
    if not m:
        return None
    val = float(m.group(1))
    unit = m.group(2) or 's'
    if unit == 'ms': val /= 1000.0
    elif unit == 'us': val /= 1_000_000.0
    elif unit == 'ns': val /= 1_000_000_000.0
    return val

def run_benchmark(cmd: list, timeout=300) -> tuple[Optional[CsmvStats], Optional[float], str]:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        output = r.stdout + r.stderr
        wall = None
        stats = None
        for line in output.split('\n'):
            if not wall:
                wall = parse_wall_time(line)
            if not stats:
                stats = CsmvStats.from_line(line)
        return stats, wall, output
    except subprocess.TimeoutExpired:
        return None, None, "(timeout)"
    except Exception as e:
        return None, None, f"({e})"

def write_results_csv(results: list, path: str):
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=[
            'benchmark', 'txns', 'wall_time_s',
            'kernel_ns', 'h2d_ns', 'd2h_ns',
            'tx_avg_ns', 'kernel_avg_ns', 'occ_avg',
            'batches', 'max_batch',
        ])
        w.writeheader()
        for r in results:
            w.writerow(r)

def generate_calibration(results: list) -> dict:
    if not results:
        return {}
    total_txns = sum(r['txns'] for r in results)
    if total_txns == 0:
        return {}
    weighted = {
        'tx_avg_ns': sum(r['tx_avg_ns'] * r['txns'] for r in results) / total_txns,
        'kernel_avg_ns': sum(r['kernel_avg_ns'] * r['txns'] for r in results) / total_txns,
        'h2d_ns': sum(r['h2d_ns'] for r in results) / len(results),
        'd2h_ns': sum(r['d2h_ns'] for r in results) / len(results),
        'occ_avg': sum(r['occ_avg'] for r in results) / len(results),
        'max_batch': max(r['max_batch'] for r in results),
        'samples': len(results),
    }
    return weighted

def main():
    parser = argparse.ArgumentParser(description='CSMV GPU profiling')
    parser.add_argument('--benchmarks-dir', default='gpu/benchmarks')
    parser.add_argument('--output-dir', default='profiling_data/csmv')
    parser.add_argument('--build', action='store_true', help='Build benchmarks first')
    args = parser.parse_args()

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    raw_dir = out_dir / 'raw'
    raw_dir.mkdir(exist_ok=True)
    cal_dir = out_dir / 'calibration'
    cal_dir.mkdir(exist_ok=True)

    benchmarks = []

    # ── GPU batch fuzz counter (varying batch sizes) ──────────
    for txns in [64, 128, 256, 512, 1024]:
        for incs in [4, 16, 64]:
            benchmarks.append({
                'name': f'gpu_fuzz_counter_{txns}t_{incs}i',
                'cmd': ['./gpu_fuzz_counter', str(txns), str(incs)],
            })

    # ── Run benchmarks ─────────────────────────────────────────
    results = []
    cwd = Path(args.benchmarks_dir).resolve()

    for b in benchmarks:
        name = b['name']
        print(f"  [{name}] ", end='', flush=True)
        stats, wall, output = run_benchmark(b['cmd'], timeout=120)
        if stats:
            r = {
                'benchmark': name,
                'txns': stats.txns,
                'wall_time_s': wall or 0,
                'kernel_ns': stats.kernel_ns,
                'h2d_ns': stats.h2d_ns,
                'd2h_ns': stats.d2h_ns,
                'tx_avg_ns': stats.tx_avg_ns,
                'kernel_avg_ns': stats.kernel_avg_ns,
                'occ_avg': stats.occ_avg,
                'batches': stats.batches,
                'max_batch': stats.max_batch,
            }
            results.append(r)
            print(f"OK  ({stats.txns} txns, {stats.kernel_avg_ns/1000:.1f} us/tx)")
        else:
            print(f"FAIL (no CSMV_STATS in output)")
            print(output[:500])

    # ── Write outputs ─────────────────────────────────────────
    if results:
        ts = time.strftime('%Y%m%d_%H%M%S')
        csv_path = raw_dir / f'csmv_profile_{ts}.csv'
        write_results_csv(results, csv_path)
        print(f"\nWrote CSV: {csv_path}")

        cal = generate_calibration(results)
        cal_path = cal_dir / f'calibration_{ts}.json'
        with open(cal_path, 'w') as f:
            json.dump(cal, f, indent=2)
        print(f"Wrote calibration: {cal_path}")

        # Summary
        print("\n═══ CSMV Profile Summary ═══")
        print(f"  Benchmarks run: {len(results)}")
        print(f"  Avg TX time:    {cal['tx_avg_ns']/1000:.1f} us")
        print(f"  Avg kernel:     {cal['kernel_avg_ns']/1000:.1f} us")
        print(f"  Avg occupancy:  {cal['occ_avg']:.0f} warps")
        print(f"  Max batch:      {cal['max_batch']} txns")

    return 0 if results else 1

if __name__ == '__main__':
    sys.exit(main())
