#!/usr/bin/env python3
"""Parse comprehensive benchmark results into summary tables."""
import os, re, sys
from collections import defaultdict
from statistics import mean

RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else "."
if not os.path.isdir(RESULTS_DIR):
    files = sorted(os.path.join(RESULTS_DIR, f) for f in os.listdir(RESULTS_DIR))
else:
    results_dir = RESULTS_DIR

outlines = []

files = sorted(os.path.join(RESULTS_DIR, f) for f in os.listdir(RESULTS_DIR) if f.endswith('.txt'))

# Parse each file
# Filenames: {suite}_{backend}_{...}_{t}t_s{sample}.txt
# For STAMP: stamp_{backend}_genome_{t}t_s{s}.txt
# For TPCC:  tpcc_{backend}_{t}t_s{s}.txt
# For YCSB:  ycsb_{backend}_{t}t_s{s}.txt
# For STM7:  stmbench7_{backend}_{t}t_s{s}.txt

results = {}  # key: (suite, backend, threads, sample) -> {metric: value}

for f in files:
    basename = os.path.basename(f)
    if not basename.endswith('.txt'):
        continue
    base = basename[:-4]
    parts = base.split('_')
    
    # Determine suite
    suite = parts[0]  # stamp, tpcc, ycsb, stmbench7
    
    # Parse backend, thread count, sample
    if suite == 'stmbench7':
        # stmbench7_{backend}_{t}t_s{s}
        backend = parts[1]
        # find thread count part
        thread_part = None
        sample_part = None
        for p in parts[2:]:
            if p.endswith('t') and p[:-1].isdigit():
                thread_part = int(p[:-1])
            elif p.startswith('s') and p[1:].isdigit():
                sample_part = int(p[1:])
    elif suite == 'stamp':
        # stamp_{backend}_genome_{t}t_s{s}
        backend = parts[1]
        thread_part = None
        sample_part = None
        for p in parts[3:]:
            if p.endswith('t') and p[:-1].isdigit():
                thread_part = int(p[:-1])
            elif p.startswith('s') and p[1:].isdigit():
                sample_part = int(p[1:])
    else:
        # tpcc_{backend}_{t}t_s{s} or ycsb_{backend}_{t}t_s{s}
        backend = parts[1]
        thread_part = None
        sample_part = None
        for p in parts[2:]:
            if p.endswith('t') and p[:-1].isdigit():
                thread_part = int(p[:-1])
            elif p.startswith('s') and p[1:].isdigit():
                sample_part = int(p[1:])
    
    if thread_part is None or sample_part is None:
        continue
    
    try:
        text = open(f).read()
    except:
        continue
    
    metrics = {}
    
    # STAMP: Time = X or Time: X
    m = re.search(r'Time\s*[=:]\s*([0-9.]+)', text)
    if m:
        metrics['time'] = float(m.group(1))
    
    # TPCC/YCSB: Ops/sec (handles int, float, and scientific notation like 1.13335e+06)
    m = re.search(r'Ops/sec[:\s]*([0-9.eE+\-]+)', text)
    if m:
        try:
            metrics['ops'] = float(m.group(1))
        except ValueError:
            pass
    
    # STMbench7: Read-only / Update
    m = re.search(r'Read-only:\s*(\d+)', text)
    if m:
        metrics['ro'] = int(m.group(1))
    m = re.search(r'Update:\s*(\d+)', text)
    if m:
        metrics['upd'] = int(m.group(1))
    
    # Aborts (TinySTM)
    m = re.search(r'total aborts = (\d+)', text)
    if m:
        metrics['aborts'] = int(m.group(1))
    
    # TSX stats
    m = re.search(r'TSXSTATS started=(\d+) committed=(\d+) aborted=(\d+)', text)
    if m:
        metrics['tsx_started'] = int(m.group(1))
        metrics['tsx_committed'] = int(m.group(2))
        metrics['tsx_aborted'] = int(m.group(3))
    
    # Check for crash/timeout
    if os.path.getsize(f) == 0:
        metrics['status'] = 'EMPTY'
    elif 'Segmentation fault' in text or 'SIGSEGV' in text:
        metrics['status'] = 'CRASH'
    elif 'Aborted' in text or 'SIGABRT' in text:
        metrics['status'] = 'ABORT'
    
    # Flag as CRASH if no metrics extracted and file is basically empty (just header)
    if not metrics and os.path.getsize(f) < 200:
        metrics['status'] = 'CRASH'
    
    key = (suite, backend, thread_part, sample_part)
    results[key] = metrics

# Aggregate by (suite, backend, threads)
aggregated = defaultdict(lambda: defaultdict(list))
for (suite, backend, threads, sample), metrics in results.items():
    aggregate_key = (suite, backend, threads)
    for k, v in metrics.items():
        if k != 'status':
            aggregated[aggregate_key][k].append(v)

# Group by suite
suites_order = ['stamp', 'tpcc', 'ycsb', 'stmbench7']
suite_labels = {'stamp': 'STAMP (genome)', 'tpcc': 'TPCC', 'ycsb': 'YCSB (A)', 'stmbench7': 'STMbench7 (w1)'}

backends_by_suite = {}
threads_by_suite = {}
for (s, bk, t, smp) in results:
    backends_by_suite.setdefault(s, set()).add(bk)
    threads_by_suite.setdefault(s, set()).add(t)

for suite in suites_order:
    print(f"\n{'='*80}")
    print(f"  {suite_labels.get(suite, suite)}")
    print(f"{'='*80}")
    
    # Collect backends and thread counts for this suite
    backends = set()
    threads_set = set()
    for (s, bk, t, smp) in results:
        if s == suite:
            backends.add(bk)
            threads_set.add(t)
    
    backends = sorted(backends)
    threads = sorted(threads_set)
    
    if not backends:
        print("  No results")
        continue
    
    # Print header
    header = f"{'Threads':>8}"
    for bk in backends:
        header += f"  {bk:>14}"
    print(header)
    print('-' * len(header))
    
    for t in threads:
        row = f"{t:>8}"
        for bk in backends:
            key = (suite, bk, t)
            if key in aggregated:
                metrics = aggregated[key]
                if 'ops' in metrics:
                    vals = metrics['ops']
                    avg = mean(vals)
                    if len(vals) > 1:
                        spread = max(vals) - min(vals)
                        if avg > 1000:
                            row += f"  {avg:>8.0f} ±{spread:>4.0f}"
                        else:
                            row += f"  {avg:>8.1f} ±{spread:>4.1f}"
                    else:
                        if avg > 1000:
                            row += f"  {avg:>8.0f}     "
                        else:
                            row += f"  {avg:>8.1f}     "
                elif 'time' in metrics:
                    vals = metrics['time']
                    avg = mean(vals)
                    if len(vals) > 1:
                        row += f"  {avg:>8.3f}s±{max(vals)-min(vals):.3f}"
                    else:
                        row += f"  {avg:>8.3f}s     "
                elif 'ro' in metrics and 'upd' in metrics:
                    ro_vals = metrics['ro']
                    upd_vals = metrics['upd']
                    avg_ro = mean(ro_vals)
                    avg_upd = mean(upd_vals)
                    row += f"  {avg_ro+avg_upd:>4.0f}tx  "
                else:
                    row += f"  {'N/A':>14}"
            else:
                row += f"  {'MISSING':>14}"
        print(row)
    
    # Print aborts if available (only TinySTM reports them)
    if suite in ('tpcc', 'ycsb') and any(('tinystm' in bk for bk in backends)):
        print()
        print("  Aborts (TinySTM):")
        row = f"{'Threads':>8}  {'aborts':>10}"
        print(row)
        for t in threads:
            key = (suite, 'tinystm', t)
            if key in aggregated and 'aborts' in aggregated[key]:
                aborts = aggregated[key]['aborts']
                avg_ab = mean(aborts)
                print(f"{t:>8}  {avg_ab:>10.0f}")

# Print crash/timeout summary
print(f"\n{'='*80}")
print("  CRASH / TIMEOUT / MISSING SUMMARY")
print(f"{'='*80}")
crash_count = 0
missing_backends = defaultdict(set)
for (suite, backend, threads, sample), metrics in sorted(results.items()):
    status = metrics.get('status', '')
    if status:
        crash_count += 1
        if crash_count <= 10:
            print(f"  {suite:>12} {backend:>12} t={threads:>2} s={sample}: {status}")

# Check for missing data points
for suite in suites_order:
    for bk in backends_by_suite.get(suite, []):
        for t in sorted(threads_by_suite.get(suite, set())):
            key = (suite, bk, t)
            if key not in aggregated or ('ops' not in aggregated[key] and 'time' not in aggregated[key] and 'ro' not in aggregated[key]):
                missing_backends[(suite, bk)].add(t)

if missing_backends:
    print(f"\n  Missing data points (no usable metrics):")
    for (suite, bk), threads in sorted(missing_backends.items()):
        print(f"  {suite:>12} {bk:>12}: {sorted(threads)}")
