#!/usr/bin/env python3
"""
compare_sim.py — Compare real C++ TM runs against the simulator.

Given a raw C++ TM_TRACE_PATH trace or JSONL file, this script:

  1. Parses the trace to count real transactions, commits, and aborts.
  2. Converts raw → JSONL (via tm-trace2jsonl) if needed.
  3. Runs tm-check (WBCTL model) and/or tm-sim (real Rust backend).
  4. Computes fidelity metrics: agreement rate, MAE, abort delta.

Metrics:
  Fidelity Score: 100% when simulator reproduces every real commit/abort.
  MAE: Mean absolute error in commit count (|real - sim|).
  Abort Delta: sim_aborts - real_aborts (positive = over-detection).

Usage:
  python3 compare_sim.py --trace /path/to/trace
  python3 compare_sim.py --bench ./bank --bench-args "-t 4 -d 100 -a 64"
  python3 compare_sim.py --trace file.trc --backend tl2
  python3 compare_sim.py --trace file.jsonl --model-only
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile


class ScenarioStats:
    """Per-scenario or aggregate transaction counts."""
    def __init__(self, name: str = ""):
        self.name = name
        self.tx_begin = 0
        self.tx_end = 0
        self.abort_events = 0
        self.other_events = 0
        self.total_events = 0

    def abort_count(self) -> int:
        unmatched = max(0, self.tx_begin - self.tx_end)
        return self.abort_events + unmatched

    def commit_count(self) -> int:
        return self.tx_end

    def total_tx(self) -> int:
        return self.tx_begin

    def add(self, other: 'ScenarioStats'):
        self.tx_begin += other.tx_begin
        self.tx_end += other.tx_end
        self.abort_events += other.abort_events
        self.other_events += other.other_events
        self.total_events += other.total_events

    def __str__(self) -> str:
        return (f"events={self.total_events} "
                f"tx={self.tx_begin} commits={self.tx_end} "
                f"aborts={self.abort_count()}")


# ── Trace parsing ──────────────────────────────────────────


def parse_raw_trace(path: str) -> tuple[list[ScenarioStats], ScenarioStats]:
    """Parse raw C++ TM_TRACE_PATH trace.

    Returns (per_scenario, aggregate).  Scenarios are split by
    Checkpoint markers — currently detected as consecutive
    TxEnd → TxBegin on a clean thread, or by heuristic.
    """
    all_stats = ScenarioStats("all")
    scenarios = []
    cur = ScenarioStats("scenario_0")
    scenario_idx = 0

    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            all_stats.total_events += 1
            cur.total_events += 1
            parts = line.split()
            if len(parts) < 4:
                cur.other_events += 1
                all_stats.other_events += 1
                continue
            try:
                type_code = int(parts[2])
            except (ValueError, IndexError):
                cur.other_events += 1
                all_stats.other_events += 1
                continue
            if type_code == 2:
                cur.tx_begin += 1
                all_stats.tx_begin += 1
            elif type_code == 3:
                cur.tx_end += 1
                all_stats.tx_end += 1
            elif type_code == 6:
                cur.abort_events += 1
                all_stats.abort_events += 1
            else:
                cur.other_events += 1
                all_stats.other_events += 1

    scenarios.append(cur)
    return scenarios, all_stats


def parse_jsonl_trace(path: str) -> tuple[list[ScenarioStats], ScenarioStats]:
    """Parse JSONL trace, splitting scenarios by Checkpoint events."""
    all_stats = ScenarioStats("all")
    scenarios = []
    cur = ScenarioStats("scenario_0")
    scenario_idx = 0
    saw_checkpoint = True

    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            all_stats.total_events += 1
            cur.total_events += 1
            if '"TxBegin"' in line:
                cur.tx_begin += 1
                all_stats.tx_begin += 1
                saw_checkpoint = False
            elif '"TxEnd"' in line:
                cur.tx_end += 1
                all_stats.tx_end += 1
            elif '"Abort"' in line:
                cur.abort_events += 1
                all_stats.abort_events += 1
            elif '"Checkpoint"' in line:
                if not saw_checkpoint:
                    scenarios.append(cur)
                    scenario_idx += 1
                    cur = ScenarioStats(f"scenario_{scenario_idx}")
                    saw_checkpoint = True
            else:
                cur.other_events += 1
                all_stats.other_events += 1

    scenarios.append(cur)
    return scenarios, all_stats


# ── Simulator output parsing ───────────────────────────────


def parse_tm_check_output(output: str) -> dict:
    result = {}
    m = re.search(r'Commits:\s+(\d+)\s+Aborts:\s+(\d+)', output)
    if m:
        result['commits'] = int(m.group(1))
        result['aborts'] = int(m.group(2))
        result['total'] = result['commits'] + result['aborts']
    m = re.search(r'Events processed:\s+(\d+)', output)
    if m:
        result['events'] = int(m.group(1))
    m = re.search(r'NO MEMORY VIOLATIONS', output)
    result['clean'] = m is not None
    m = re.search(r'CONFLICTS DETECTED', output)
    result['conflicts'] = m is not None
    return result


def parse_tm_sim_output(output: str) -> dict:
    result = {}
    m = re.search(r'Commits:\s+(\d+)\s+Aborts:\s+(\d+)', output)
    if m:
        result['commits'] = int(m.group(1))
        result['aborts'] = int(m.group(2))
        result['total'] = result['commits'] + result['aborts']
    m = re.search(r'NO MEMORY VIOLATIONS', output)
    result['clean'] = m is not None
    return result


# ── Metrics ────────────────────────────────────────────────


def compute_metrics(real: ScenarioStats, sim: dict) -> dict:
    """Compute fidelity metrics between real trace and simulator output.

    Returns dict with keys: fidelity, commit_mae, abort_delta,
    real_commits, sim_commits, real_aborts, sim_aborts.
    """
    rc = real.commit_count()
    sc = sim.get('commits', 0)
    ra = real.abort_count()
    sa = sim.get('aborts', 0)
    total = max(real.total_tx(), 1)

    commit_diff = abs(rc - sc)
    fidelity = 100.0 * (1.0 - commit_diff / total)
    commit_mae = commit_diff
    abort_delta = sa - ra

    return {
        'fidelity': fidelity,
        'commit_mae': commit_mae,
        'abort_delta': abort_delta,
        'real_commits': rc,
        'sim_commits': sc,
        'real_aborts': ra,
        'sim_aborts': sa,
        'total_tx': real.total_tx(),
        'total_events': real.total_events,
    }


METRIC_LABELS = {
    'fidelity': ('Fidelity', '{:.1f}%', lambda v: v),
    'commit_mae': ('Commit MAE', '{}', lambda v: v),
    'abort_delta': ('Abort Δ', '{:+d}', lambda v: v),
}


def format_metric(key: str, value) -> str:
    label, fmt, _ = METRIC_LABELS[key]
    return f"  {label:15s}  {fmt.format(value)}"


def rating(fidelity: float) -> str:
    if fidelity >= 99.5:
        return "✅"
    elif fidelity >= 95.0:
        return "🟡"
    elif fidelity >= 80.0:
        return "🟠"
    return "🔴"


# ── Utility ────────────────────────────────────────────────


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


def is_jsonl(path: str) -> bool:
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                return line.startswith('{')
    return False


# ── Main ───────────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description='Compare real TM runs against the simulator'
    )
    parser.add_argument('--trace', '-t', help='Raw TM_TRACE_PATH or JSONL trace file')
    parser.add_argument('--bench', '-b', help='Benchmark binary to run')
    parser.add_argument('--bench-args', '-a', default='',
                        help='Arguments for benchmark binary')
    parser.add_argument('--backend', default=None,
                        choices=['norec', 'tl2', 'tinystm', 'romulus', 'swisstm'],
                        help='Rust backend for tm-sim (default: norec)')
    parser.add_argument('--backends', default=None,
                        help='Comma-separated list of backends to test')
    parser.add_argument('--model-only', action='store_true',
                        help='Compare against tm-check only, skip tm-sim')
    parser.add_argument('--sim-only', action='store_true',
                        help='Compare against tm-sim only, skip tm-check')
    parser.add_argument('--cargo-profile', default='release',
                        choices=['release', 'debug'],
                        help='Cargo build profile')
    parser.add_argument('--project-dir',
                        default=os.path.dirname(os.path.abspath(__file__)),
                        help='Simulator project directory')
    parser.add_argument('--csv', help='Append metrics to CSV file')
    parser.add_argument('--quiet', action='store_true',
                        help='Suppress per-step logging; only print metrics')
    args = parser.parse_args()

    if not args.trace and not args.bench:
        print("Error: provide --trace or --bench", file=sys.stderr)
        sys.exit(1)

    # Resolve backend list: --backends (plural) overrides --backend (singular).
    # If neither is given, default to norec.
    if args.backends:
        backend_list = [b.strip() for b in args.backends.split(',') if b.strip()]
    elif args.backend:
        backend_list = [args.backend]
    else:
        backend_list = ['norec']

    log = lambda msg: None if args.quiet else print(msg, file=sys.stderr)

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
            log("Building simulator binaries...")
            subprocess.run(
                ['cargo', 'build'] + ([cargo_flag] if cargo_flag else []),
                cwd=proj, check=True
            )
            break

    # ── Step 1: Capture trace from benchmark ─────────────────────
    if args.bench and not trace_path:
        fd, trace_path = tempfile.mkstemp(suffix='.trace', prefix='tm_compare_')
        os.close(fd)
        env = os.environ.copy()
        env['TM_TRACE_PATH'] = trace_path
        log(f"Running: {args.bench} {args.bench_args}")
        log(f"  TM_TRACE_PATH={trace_path}")
        rc, _ = run_command([args.bench] + args.bench_args.split(), 'benchmark')
        if rc != 0:
            log(f"  Warning: benchmark exited with code {rc}")

    # ── Step 2: Parse real trace ────────────────────────────────
    if not trace_path or not os.path.exists(trace_path):
        print(f"Error: trace not found: {trace_path}", file=sys.stderr)
        sys.exit(1)

    trace_is_jsonl = is_jsonl(trace_path)
    if trace_is_jsonl:
        scenarios, real_agg = parse_jsonl_trace(trace_path)
    else:
        scenarios, real_agg = parse_raw_trace(trace_path)

    if real_agg.total_events == 0:
        print("Error: trace is empty", file=sys.stderr)
        sys.exit(1)

    log(f"\nTrace: {trace_path}  ({'JSONL' if trace_is_jsonl else 'raw'}  "
        f"{real_agg.total_events} events, {len(scenarios)} scenarios)")
    for s in scenarios:
        log(f"  {s.name}: {s}")

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

    # ── Step 4: Run simulator(s) ────────────────────────────────
    comparisons = []

    if not args.sim_only:
        log("\n--- tm-check (WBCTL model) ---")
        rc, check_out = run_command(
            [tm_check_bin, '--trace', jsonl_path], 'tm-check')
        model = parse_tm_check_output(check_out)
        if 'total' in model:
            comparisons.append(('Model (WBCTL)', model))

    if not args.model_only:
        for bk in backend_list:
            log(f"\n--- tm-sim (backend={bk}) ---")
            rc, sim_out = run_command(
                [tm_sim_bin, '--backend', bk, '--trace', jsonl_path],
                f'tm-sim-{bk}')
            sim = parse_tm_sim_output(sim_out)
            if 'total' in sim:
                comparisons.append((f'Sim ({bk})', sim))

    # ── Step 5: Metrics ─────────────────────────────────────────
    print("\n" + "=" * 58)
    print("SIMULATOR FIDELITY REPORT")
    print("=" * 58)

    for label, counts in comparisons:
        m = compute_metrics(real_agg, counts)
        print(f"\n  {label}:")
        print(f"    Total TX:     real={m['real_commits']+m['real_aborts']:>4d}  "
              f"sim={m['total_tx']:>4d}")
        print(f"    Commits:      real={m['real_commits']:>4d}  "
              f"sim={m['sim_commits']:>4d}")
        print(f"    Aborts:       real={m['real_aborts']:>4d}  "
              f"sim={m['sim_aborts']:>4d}")
        print()
        for key in ['fidelity', 'commit_mae', 'abort_delta']:
            print(format_metric(key, m[key]))
        rat = rating(m['fidelity'])
        print(f"  {'Rating':15s}  {rat}")

        # ── CSV output ──────────────────────────────────────────
        if args.csv:
            trace_name = os.path.basename(trace_path)
            header = ('trace,simulator,backend,'
                      'total_tx,real_commits,sim_commits,'
                      'real_aborts,sim_aborts,'
                      'fidelity,commit_mae,abort_delta,'
                      'total_events,clean,conflicts')
            # Extract backend name from label "Sim (norec)" or use raw label
            backend_name = label.split('(')[-1].rstrip(')') if '(' in label else label
            row = (f'{trace_name},{label},{backend_name},'
                   f'{m["total_tx"]},{m["real_commits"]},{m["sim_commits"]},'
                   f'{m["real_aborts"]},{m["sim_aborts"]},'
                   f'{m["fidelity"]:.2f},{m["commit_mae"]},{m["abort_delta"]},'
                   f'{m["total_events"]},{counts.get("clean",False)},'
                   f'{counts.get("conflicts",False)}')
            exists = os.path.exists(args.csv)
            with open(args.csv, 'a') as cf:
                if not exists:
                    cf.write(header + '\n')
                cf.write(row + '\n')
            log(f"\n  Appended to {args.csv}")

    # ── Cleanup ─────────────────────────────────────────────────
    if args.bench and trace_path and os.path.exists(trace_path):
        os.unlink(trace_path)
    if not trace_is_jsonl and os.path.exists(jsonl_path):
        os.unlink(jsonl_path)

    sys.exit(0)


if __name__ == '__main__':
    main()
