#!/usr/bin/env python3
"""Analyze benchmark results from run_comprehensive.sh output."""
import os, sys, re, json, glob
from collections import defaultdict

RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else "benchmark_results/comprehensive_standard_20260516_233613"

# Parse results: files named like:
#   stamp_{backend}_{subbench}_{threads}t_sample{sample}.txt
#   tpcc_{backend}_{threads}t_sample{sample}.txt
#   stmbench7_{backend}_{threads}t_sample{sample}.txt

pattern = re.compile(
    r'(?P<bench>stamp|tpcc|stmbench7)_'
    r'(?P<backend>[^_]+)_'
    r'(?:(?P<subbench>[^_]+)_)?'  # STAMP sub-benchmark (optional)
    r'(?P<threads>\d+)t_sample(?P<sample>\d+)\.txt'
)

data = []  # list of dicts

for fname in os.listdir(RESULTS_DIR):
    if not fname.endswith('.txt') or fname == 'SUMMARY.txt' or fname == 'results.csv':
        continue
    m = pattern.match(fname)
    if not m:
        continue
    
    fpath = os.path.join(RESULTS_DIR, fname)
    with open(fpath) as f:
        content = f.read()
    
    # Extract Ops/sec
    ops_match = re.search(r'Ops/sec\s*:\s*([\d.]+)', content)
    if not ops_match:
        ops_match = re.search(r'([\d.]+)\s*Ops/sec', content)
    if not ops_match:
        ops_match = re.search(r'Txns/sec\s*:\s*([\d.]+)', content)
    if not ops_match:
        ops_match = re.search(r'([\d.]+)\s*Txns/sec', content)
    if not ops_match:
        # Try TSXSGL stats format
        ops_match = re.search(r'Elapsed:\s+\d+\s+ms.*?Total ops:\s+(\d+)', content, re.DOTALL)
        if ops_match:
            elapsed_match = re.search(r'Elapsed:\s+(\d+)\s+ms', content)
            if elapsed_match:
                elapsed_ms = int(elapsed_match.group(1))
                total_ops = int(ops_match.group(1))
                ops_sec = total_ops / (elapsed_ms / 1000.0)
        else:
            ops_sec = None
    else:
        ops_sec = float(ops_match.group(1))
        # If there's also "Total ops: X" with "Elapsed", compute directly
        elapsed_match = re.search(r'Elapsed:\s+(\d+)\s+ms', content)
        total_match = re.search(r'Total ops:\s+(\d+)', content)
        if elapsed_match and total_match:
            elapsed_ms = int(elapsed_match.group(1))
            total_ops = int(total_match.group(1))
            ops_sec2 = total_ops / (elapsed_ms / 1000.0)
            if abs(ops_sec - ops_sec2) > 0.01:
                ops_sec = ops_sec2  # Use more precise computation
    
    if ops_sec is None or ops_sec == 0:
        continue
    
    entry = {
        'bench': m.group('bench'),
        'backend': m.group('backend'),
        'subbench': m.group('subbench') or '',
        'threads': int(m.group('threads')),
        'sample': int(m.group('sample')),
        'ops': ops_sec,
    }
    data.append(entry)

# Aggregate: mean & std per (bench, backend, subbench, threads)
# Also track TIMEOUTs (no data = timeout)
# Count total expected files vs actual data points
agg = defaultdict(list)
for d in data:
    key = (d['bench'], d['backend'], d['subbench'], d['threads'])
    agg[key].append(d['ops'])

# Compute stats
results = []
for key, values in agg.items():
    bench, backend, subbench, threads = key
    mean = sum(values) / len(values)
    if len(values) > 1:
        variance = sum((v - mean) ** 2 for v in values) / len(values)
        std = variance ** 0.5
    else:
        std = 0
    results.append((bench, backend, subbench, threads, mean, std, len(values)))

# Sort
results.sort(key=lambda r: (r[0], r[1], r[2], r[3]))

# Print tables
print(f"Results from {RESULTS_DIR}")
print(f"Data points: {len(data)}")
print()

current_bench = None
for r in results:
    bench, backend, subbench, threads, mean, std, n = r
    label = f"{bench}/{subbench}" if subbench else bench
    if label != current_bench:
        print(f"\n{'='*60}")
        print(f"  {label.upper()}")
        print(f"{'='*60}")
        current_bench = label
    
    if threads == 1:
        print(f"  {backend:12s} t={threads:2d}  mean={mean:8.1f}  std={std:6.2f}  n={n}")
    else:
        print(f"  {backend:12s} t={threads:2d}  mean={mean:8.1f}  std={std:6.2f}  n={n}")

# Summary comparison: TSXSGL vs SGL
print("\n\n")
print("="*60)
print("  TSXSGL vs SGL SUMMARY (mean throughput)")
print("="*60)

# Group by (bench, subbench, threads) and compare tsxsgl vs singlelock
comparisons = defaultdict(dict)
for r in results:
    bench, backend, subbench, threads, mean, std, n = r
    key = (bench, subbench, threads)
    comparisons[key][backend] = mean

print(f"{'Benchmark':20s} {'Threads':>8s} {'TSXSGL':>10s} {'SGL':>10s} {'Ratio':>8s}")
print("-"*60)
for key in sorted(comparisons.keys()):
    bench, subbench, threads = key
    tsxsgl = comparisons[key].get('tsxsgl')
    sgl = comparisons[key].get('singlelock')
    if tsxsgl and sgl:
        ratio = tsxsgl / sgl
        label = f"{bench}/{subbench}" if subbench else bench
        print(f"{label:20s} {threads:8d} {tsxsgl:10.1f} {sgl:10.1f} {ratio:8.2f}")
    elif tsxsgl:
        label = f"{bench}/{subbench}" if subbench else bench
        print(f"{label:20s} {threads:8d} {tsxsgl:10.1f} {'N/A':>10s} {'N/A':>8s}")
    elif sgl:
        label = f"{bench}/{subbench}" if subbench else bench
        print(f"{label:20s} {threads:8d} {'N/A':>10s} {sgl:10.1f} {'N/A':>8s}")

# TL2 comparison for TPCC
print("\n\n")
print("="*60)
print("  TPCC: TL2 comparison")
print("="*60)
for r in results:
    if r[0] == 'tpcc':
        bench, backend, subbench, threads, mean, std, n = r
        print(f"  {backend:12s} t={threads:2d}  mean={mean:8.1f}  std={std:6.2f}  n={n}")
