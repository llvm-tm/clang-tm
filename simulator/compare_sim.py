#!/usr/bin/env python3
"""
compare_sim.py — Compare real C++ TM runs against the simulator.

Given a raw C++ TM_TRACE_PATH trace file, this script:

  1. Parses the trace to count real transactions, commits, and aborts.
  2. Converts the trace to JSONL (via tm-trace2jsonl).
  3. Runs tm-check (WBCTL model) on the JSONL.
  4. Runs tm-sim (real Rust backend) on the JSONL.
  5. Compares all counts and reports differences.

Usage:
  # Compare a pre-recorded trace
  python3 compare_sim.py --trace /path/to/raw.trace

  # Run a benchmark first, then compare
  python3 compare_sim.py --bench ./bank --bench-args "-t 4 -d 100 -a 64"

  # Compare against tm-check (model) only
  python3 compare_sim.py --trace file.trc --model-only

  # Compare against a specific Rust backend
  python3 compare_sim.py --trace file.trc --backend tl2
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict

EXTENDED_RE = re.compile(
    r'^(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)'
)

BASIC_RE = re.compile(
    r'^(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*(\S*)\s*(\S*)'
)


class TraceStats:
    def __init__(self):
        self.tx_begin = 0
        self.tx_end = 0
        self.abort_events = 0
        self.other = 0
        self.total_raw = 0

    def abort_count(self):
        """Best-guess abort count: explicit aborts + unmatched TxBegins."""
        unmatched = max(0, self.tx_begin - self.tx_end)
        return self.abort_events + unmatched

    def commit_count(self):
        return self.tx_end

    def total_tx(self):
        return self.tx_begin


def parse_raw_trace(path: str) -> TraceStats:
    stats = TraceStats()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            stats.total_raw += 1
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                type_code = int(parts[2])
            except (ValueError, IndexError):
                stats.other += 1
                continue
            if type_code == 2:
                stats.tx_begin += 1
            elif type_code == 3:
                stats.tx_end += 1
            elif type_code == 6:
                stats.abort_events += 1
            else:
                stats.other += 1
    return stats


def parse_tm_check_output(output: str) -> dict:
    result = {}
    m = re.search(r'Commits:\s+(\d+)\s+Aborts:\s+(\d+)', output)
    if m:
        result['model_commits'] = int(m.group(1))
        result['model_aborts'] = int(m.group(2))
        result['model_total'] = result['model_commits'] + result['model_aborts']
    m = re.search(r'Events processed:\s+(\d+)', output)
    if m:
        result['model_events'] = int(m.group(1))
    m = re.search(r'NO MEMORY VIOLATIONS', output)
    result['model_clean'] = m is not None
    m = re.search(r'CONFLICTS DETECTED', output)
    result['model_conflicts'] = m is not None
    return result


def parse_tm_sim_output(output: str) -> dict:
    result = {}
    m = re.search(r'Commits:\s+(\d+)\s+Aborts:\s+(\d+)', output)
    if m:
        result['sim_commits'] = int(m.group(1))
        result['sim_aborts'] = int(m.group(2))
        result['sim_total'] = result['sim_commits'] + result['sim_aborts']
    m = re.search(r'NO MEMORY VIOLATIONS', output)
    result['sim_clean'] = m is not None
    return result


def run_command(cmd: list, desc: str) -> tuple[int, str]:
    print(f"  [{desc}] $ {' '.join(cmd)}", file=sys.stderr)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    stderr = proc.stderr.strip() if proc.stderr else ''
    stdout = proc.stdout.strip() if proc.stdout else ''
    combined = stderr + '\n' + stdout
    if proc.returncode != 0:
        print(f"  WARNING: {desc} exited with code {proc.returncode}", file=sys.stderr)
        if stderr:
            for line in stderr.splitlines()[:5]:
                print(f"    {line}", file=sys.stderr)
    return proc.returncode, combined


def compare_counts(label: str, real_val: int, sim_val: int):
    if real_val == sim_val:
        return f"  {label}: {real_val} == {sim_val}  ✅"
    else:
        diff = real_val - sim_val
        return f"  {label}: {real_val} vs {sim_val}  ❌ (Δ={diff:+d})"


def format_rate(commits: int, aborts: int) -> str:
    total = commits + aborts
    if total == 0:
        return "0.0%"
    return f"{100.0 * aborts / total:.1f}%"


def is_jsonl(path: str) -> bool:
    """Detect if a trace file is JSONL (starts with '{') or raw C++ format."""
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                return line.startswith('{')
    return False


def parse_jsonl_trace(path: str) -> TraceStats:
    """Parse JSONL trace to count transaction events."""
    stats = TraceStats()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            stats.total_raw += 1
            if '"TxBegin"' in line:
                stats.tx_begin += 1
            elif '"TxEnd"' in line:
                stats.tx_end += 1
            elif '"Abort"' in line:
                stats.abort_events += 1
            else:
                stats.other += 1
    return stats


def main():
    parser = argparse.ArgumentParser(
        description="Compare real C++ TM runs against the simulator"
    )
    parser.add_argument('--trace', '-t', help='Raw C++ TM_TRACE_PATH or JSONL trace file')
    parser.add_argument('--bench', '-b', help='Benchmark binary to run (alternative to --trace)')
    parser.add_argument('--bench-args', '-a', default='',
                        help='Arguments for benchmark binary')
    parser.add_argument('--backend', default='norec',
                        choices=['norec', 'tl2', 'tinystm'],
                        help='Rust backend for tm-sim (default: norec)')
    parser.add_argument('--model-only', action='store_true',
                        help='Compare against tm-check (model) only, skip tm-sim')
    parser.add_argument('--sim-only', action='store_true',
                        help='Compare against tm-sim only, skip tm-check')
    parser.add_argument('--cargo-profile', default='release',
                        choices=['release', 'debug'],
                        help='Cargo build profile (default: release)')
    parser.add_argument('--project-dir', default=os.path.dirname(os.path.abspath(__file__)),
                        help='Simulator project directory')
    args = parser.parse_args()

    if not args.trace and not args.bench:
        print("Error: provide --trace or --bench", file=sys.stderr)
        sys.exit(1)

    proj = args.project_dir
    cargo_flag = '--release' if args.cargo_profile == 'release' else ''
    target_dir = os.path.join(proj, 'target', args.cargo_profile)
    tm_trace2jsonl_bin = os.path.join(target_dir, 'tm-trace2jsonl')
    tm_check_bin = os.path.join(target_dir, 'tm-check')
    tm_sim_bin = os.path.join(target_dir, 'tm-sim')

    trace_path = args.trace

    # ── Step 0: Build if needed ──────────────────────────────────
    for bin_name in ['tm-trace2jsonl', 'tm-check', 'tm-sim']:
        bin_path = os.path.join(target_dir, bin_name)
        if not os.path.exists(bin_path):
            print("Building simulator binaries...", file=sys.stderr)
            subprocess.run(
                ['cargo', 'build'] + ([cargo_flag] if cargo_flag else []),
                cwd=proj, check=True
            )
            break

    # ── Step 1: Capture trace from benchmark ─────────────────────
    if args.bench and not trace_path:
        fd, trace_path = tempfile.mkstemp(suffix='.trace', prefix='tm_compare_')
        os.close(fd)
        bench_env = os.environ.copy()
        bench_env['TM_TRACE_PATH'] = trace_path
        print(f"Running benchmark: {args.bench} {args.bench_args}", file=sys.stderr)
        print(f"  TM_TRACE_PATH={trace_path}", file=sys.stderr)
        rc, _ = run_command(
            [args.bench] + args.bench_args.split(),
            'benchmark'
        )
        if rc != 0:
            print(f"Warning: benchmark exited with code {rc}", file=sys.stderr)

    # ── Step 2: Parse real trace ────────────────────────────────
    if not trace_path or not os.path.exists(trace_path):
        print(f"Error: trace file not found: {trace_path}", file=sys.stderr)
        sys.exit(1)

    print(f"\nParsing trace: {trace_path}", file=sys.stderr)
    trace_is_jsonl = is_jsonl(trace_path)
    if trace_is_jsonl:
        real = parse_jsonl_trace(trace_path)
    else:
        real = parse_raw_trace(trace_path)

    if real.total_raw == 0:
        print("Error: trace is empty", file=sys.stderr)
        sys.exit(1)

    print(f"  Raw events: {real.total_raw}", file=sys.stderr)
    print(f"  TxBegin: {real.tx_begin}", file=sys.stderr)
    print(f"  TxEnd:   {real.tx_end}", file=sys.stderr)
    if real.abort_events:
        print(f"  Abort:   {real.abort_events} (explicit)", file=sys.stderr)

    # ── Step 3: Convert to JSONL (if raw) ───────────────────────
    jsonl_path = trace_path
    if not trace_is_jsonl:
        fd_jsonl, jsonl_path = tempfile.mkstemp(suffix='.jsonl', prefix='tm_compare_')
        os.close(fd_jsonl)
        rc, _ = run_command(
            [tm_trace2jsonl_bin, '--input', trace_path, '--output', jsonl_path],
            'tm-trace2jsonl'
        )
        if rc != 0:
            print("Error: trace conversion failed", file=sys.stderr)
            sys.exit(1)

    # ── Step 4: Run simulator(s) and compare ────────────────────
    comparisons = []

    if not args.sim_only:
        print("\n--- tm-check (WBCTL model) ---", file=sys.stderr)
        rc, check_out = run_command(
            [tm_check_bin, '--trace', jsonl_path],
            'tm-check'
        )
        model = parse_tm_check_output(check_out)
        if 'model_total' in model:
            comparisons.append(('Model (WBCTL)', model, 'model_'))

    if not args.model_only:
        print(f"\n--- tm-sim (backend={args.backend}) ---", file=sys.stderr)
        rc, sim_out = run_command(
            [tm_sim_bin, '--backend', args.backend, '--trace', jsonl_path],
            'tm-sim'
        )
        sim = parse_tm_sim_output(sim_out)
        if 'sim_total' in sim:
            comparisons.append((f'Sim ({args.backend})', sim, 'sim_'))

    # ── Step 5: Report ──────────────────────────────────────────
    print("\n" + "=" * 55, file=sys.stderr)
    print("COMPARISON REPORT", file=sys.stderr)
    print("=" * 55, file=sys.stderr)

    for label, counts, prefix in comparisons:
        s_commits = counts.get(f'{prefix}commits', 0)
        s_aborts = counts.get(f'{prefix}aborts', 0)
        s_total = counts.get(f'{prefix}total', 0)

        print(f"\nReal run  — {label}:", file=sys.stderr)
        print(f"  Total transactions: {real.total_tx()}", file=sys.stderr)
        print(f"  Commits:  {real.commit_count()}  ({format_rate(real.commit_count(), real.abort_count())})", file=sys.stderr)
        print(f"  Aborts:   {real.abort_count()}", file=sys.stderr)

        print(f"\n{label}:", file=sys.stderr)
        print(f"  Total transactions: {s_total}", file=sys.stderr)
        print(f"  Commits:  {s_commits}  ({format_rate(s_commits, s_aborts)})", file=sys.stderr)
        print(f"  Aborts:   {s_aborts}", file=sys.stderr)

        print(f"\nComparison:", file=sys.stderr)
        line1 = compare_counts('Total TX', real.total_tx(), s_total)
        line2 = compare_counts('Commits', real.commit_count(), s_commits)
        line3 = compare_counts('Aborts', real.abort_count(), s_aborts)
        print(line1, file=sys.stderr)
        print(line2, file=sys.stderr)
        print(line3, file=sys.stderr)

        all_match = (real.total_tx() == s_total and
                     real.commit_count() == s_commits and
                     real.abort_count() == s_aborts)
        if all_match:
            print("\nResult: ✅ ALL COUNTS MATCH", file=sys.stderr)
        else:
            print(f"\nResult: ❌ COUNTS DIFFER", file=sys.stderr)

    # Clean up temp files
    if args.bench and trace_path and os.path.exists(trace_path):
        os.unlink(trace_path)
    if not trace_is_jsonl and os.path.exists(jsonl_path):
        os.unlink(jsonl_path)

    # Exit with non-zero if any comparison failed
    for _, counts, prefix in comparisons:
        s_commits = counts.get(f'{prefix}commits', -1)
        s_aborts = counts.get(f'{prefix}aborts', -1)
        s_total = counts.get(f'{prefix}total', -1)
        if (real.total_tx() != s_total or
                real.commit_count() != s_commits or
                real.abort_count() != s_aborts):
            sys.exit(1)

    sys.exit(0)


if __name__ == '__main__':
    main()
