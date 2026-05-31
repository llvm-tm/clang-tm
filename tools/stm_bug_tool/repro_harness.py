#!/usr/bin/env python3
"""Reproduction harness for TM bug detection.

Given a backend, benchmark, and random seed, rebuilds the benchmark with
TM_EVENT_LOG, runs it with the given parameters, captures the event log,
and prints the full event sequence leading to the first invariant violation.

Usage:
    python3 repro_harness.py --backend tl2 --seed 42 [--threads 4 --iters 5000]
    python3 repro_harness.py --backend tinystm --benchmark bank --seed 12345

Output:
    - Build status
    - Run command + exit code
    - Invariant violations (if any)
    - Full event log (grouped by thread, with TX ranges)
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from event_parser import parse_event_log, group_by_thread, extract_tx_ranges
from invariant_checker import check_all, ALL_INVARIANTS
from fuzz_runner import ROOT, BACKENDS, build_fuzz_benchmark, run_fuzz


def print_event_log(parsed: dict, max_events: int = 200):
    """Print event log grouped by thread with TX range annotations."""
    events = parsed["events"]
    if not events:
        print("  (no events)")
        return

    tx_ranges = extract_tx_ranges(events)
    tx_by_idx = {}
    for start, end, etype in tx_ranges:
        for i in range(start, end + 1):
            tx_by_idx[i] = (start, etype)

    # Print header
    print(f"\n  Event log: {len(events)} events (ring dump)")
    if parsed.get("dump_start", 0):
        print(f"  Ring buffer: {parsed.get('total_entries', '?')} total, "
              f"dump from #{parsed['dump_start']}")

    # Trim to max_events
    trimmed = events[-max_events:] if len(events) > max_events else events
    offset = len(events) - len(trimmed)

    for i, ev in enumerate(trimmed):
        actual_idx = offset + i
        prefix = ""
        tx_info = tx_by_idx.get(actual_idx)
        if tx_info:
            if actual_idx == tx_info[0]:
                prefix = f" TX#{tx_info[0]} [{tx_info[1]}]"
            elif ev["type"] in ("COMMIT_SUCCESS", "TX_ABORT"):
                prefix = f" END#{tx_info[0]}"

        print(f"  #{actual_idx:5d}{prefix} | "
              f"thr={ev.get('thread_id','?')[-4:]} "
              f"{ev['type']:22s} "
              f"addr1=0x{ev.get('addr1',0):014x} "
              f"addr2=0x{ev.get('addr2',0):014x} "
              f"data={ev.get('data',0)}")


def main():
    parser = argparse.ArgumentParser(
        description="Reproduction harness for TM bugs",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--backend", required=True,
                        help="TM backend (tl2, tinystm, wt, wbetl, norec, swisstm)")
    parser.add_argument("--benchmark", default="counter",
                        help="Benchmark name (fuzz_<name>.cpp, default: counter)")
    parser.add_argument("--seed", type=int, default=0,
                        help="Random seed for parameters")
    parser.add_argument("--threads", type=int, default=None,
                        help="Thread count (overrides seed-based generation)")
    parser.add_argument("--iters", type=int, default=None,
                        help="Iterations per thread (overrides seed-based generation)")
    parser.add_argument("--counters", type=int, default=None,
                        help="Number of counters/accounts (overrides seed-based generation)")
    parser.add_argument("--timeout", type=int, default=60,
                        help="Per-run timeout in seconds")
    parser.add_argument("--events", type=int, default=100,
                        help="Max events to print (default: 100, 0=all)")
    parser.add_argument("--no-rebuild", action="store_true",
                        help="Skip rebuild, use existing binary")
    args = parser.parse_args()

    backend = args.backend
    benchmark = args.benchmark

    if backend not in BACKENDS:
        print(f"Unknown backend: {backend}", file=sys.stderr)
        print(f"Available: {', '.join(BACKENDS.keys())}", file=sys.stderr)
        return 1

    # Build
    if args.no_rebuild:
        bin_dir = ROOT / "tools" / "stm_bug_tool" / "bin"
        binary = str(bin_dir / f"fuzz_{benchmark}_{backend}")
        if not os.path.exists(binary):
            print(f"Binary not found: {binary}. Use --no-rebuild only with existing binaries.",
                  file=sys.stderr)
            return 1
    else:
        binary = build_fuzz_benchmark(backend, benchmark)
        if binary is None:
            print("BUILD FAILED", file=sys.stderr)
            return 1

    # Determine parameters
    if args.threads is not None:
        threads = args.threads
    else:
        import random
        rng = random.Random(args.seed)
        threads = rng.choice([1, 2, 3, 4, 6, 8])

    if args.iters is not None:
        iters = args.iters
    else:
        import random
        rng = random.Random(args.seed + 1)
        iters = rng.choice([100, 500, 1000, 2000, 5000])

    if args.counters is not None:
        counters = args.counters
    else:
        import random
        rng = random.Random(args.seed + 2)
        counters = rng.choice([1, 2, 4, 8, 16])

    # Run
    print(f"\n{'=' * 60}")
    print(f"Reproduction run")
    print(f"{'=' * 60}")
    print(f"  Backend:    {backend}")
    print(f"  Benchmark:  {benchmark}")
    print(f"  Seed:       {args.seed}")
    print(f"  Threads:    {threads}")
    print(f"  Iters:      {iters}")
    print(f"  Counters:   {counters}")
    print(f"  Binary:     {binary}")
    print()

    output = run_fuzz(binary, threads, iters, counters, args.seed, timeout=args.timeout)

    print(f"  Exit code:  {output['returncode']}")
    print(f"  Elapsed:    {output['elapsed']:.2f}s")
    print(f"  Crash:      {output.get('crash', False)}")
    print(f"  Invariant:  {output.get('invariant_result', 'N/A')}")
    print()

    # Parse event log
    stderr_text = output.get("stderr", "")
    if "SIGSEGV" in stderr_text or "Event log" in stderr_text:
        parsed = parse_event_log(stderr_text)
        print(f"  SIGSEGV:    {parsed.get('sigsegv_addr', 'N/A')}")
        print(f"  Events:     {len(parsed['events'])}")
        print()

        # Run invariant checks
        result = check_all(parsed)
        violations = result["violations"]

        if violations:
            print(f"  Invariant violations: {result['total']} "
                  f"(critical={result['critical']}, warnings={result['warnings']})")
            for v in violations:
                print(f"    [{v['severity']}] {v['invariant']}: {v['description'][:150]}")
        else:
            print(f"  Invariant checks: ALL PASS")

        # Print event log
        max_events = args.events if args.events > 0 else len(parsed["events"])
        print_event_log(parsed, max_events)

        # Check if stdout has invariant output
        if "INVARIANT:" in output.get("stdout", ""):
            print(f"\n  Application invariants:")
            for line in output["stdout"].splitlines():
                if "INVARIANT:" in line:
                    print(f"    {line.strip()}")
    else:
        print(f"  No event log found in stderr")
        print(f"  Stdout (last 10 lines):")
        for line in output.get("stdout", "").splitlines()[-10:]:
            print(f"    {line}")

    return 0 if not violations else 1


if __name__ == "__main__":
    sys.exit(main())
