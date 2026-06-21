#!/usr/bin/env python3
"""
Compare real TSXSGL throughput vs simulator estimated throughput.

Approach:
  1. Run real C++ TSXSGL benchmarks with /usr/bin/time to get wall-clock
     throughput (txns/sec).
  2. Generate JSONL trace files matching the same transaction pattern
     (read/write counts, contention level, thread count).
  3. Run the simulator (tm-des) with --clock-mode cost and the calibrated
     Broadwell machine profile.
  4. Convert estimated cycles → time via CPU frequency (1.8 GHz on this
     Broadwell-EP v4).
  5. Compute error = (simulated - real) / real * 100.

Benchmark patterns:
  - fuzz_counter: N threads × K iterations, each iteration = 1 read + 1
    write on one of M TM<uint64_t> counters. Contention = random.
  - fuzz_bank: N threads × K iterations, each iteration = 2 reads + 2
    writes (transfer between two random accounts). Contention = random.

Timestamps in the trace control interleaving order. For multi-threaded,
thread events are interleaved at fine granularity (each txn event
alternates between threads) to approximate real concurrent execution.

Usage:
  python3 tools/compare_tsxsgl.py [--regen]

Dependencies: Python 3.8+, /usr/bin/time, simulator built at
  simulator/target/debug/tm-des

Output:
  Prints comparison table and error analysis.
"""

import subprocess
import json
import os
import sys
import tempfile
import math

# ── Configuration ──────────────────────────────────────────
REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SIMULATOR = os.path.join(REPO_DIR, "simulator", "target", "debug", "tm-des")
MACHINE_PROFILE = os.path.join(REPO_DIR, "simulator", "machine_profiles",
                               "broadwell_ep_v4.json")
BENCHMARK_DIR = os.path.join(REPO_DIR, "benchmarks", "cpp")
BENCHMARKS = {
    "fuzz_counter": {
        "binary": "bin/fuzz_counter",
        "params": ["%d", "1000000", "64", "42"],
        "read_per_tx": 1,
        "write_per_tx": 1,
        "num_counters": 64,
    },
    "fuzz_bank": {
        "binary": "bin/fuzz_bank",
        "params": ["%d", "1000000", "64", "42"],
        "read_per_tx": 2,
        "write_per_tx": 2,
        "num_counters": 64,
    },
}

BASE_ADDR = 0x7f00_0000_8000   # TM region base (matches C++ tm_region_allocator)
STRIDE = 8                      # uint64_t alignment
FREQ_GHZ = 1.8                  # Broadwell-EP v4 nominal frequency


def build_simulator():
    """Build simulator binary if needed."""
    sim_dir = os.path.join(REPO_DIR, "simulator")
    if not os.path.exists(SIMULATOR):
        print("Building simulator...")
        subprocess.run(["cargo", "build", "--bin", "tm-des"],
                       cwd=sim_dir, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_real_benchmark(bench_name, threads, iters):
    """Run real TSXSGL benchmark and return (txn_count, wall_sec)."""
    b = BENCHMARKS[bench_name]
    binary = os.path.join(BENCHMARK_DIR, b["binary"])
    params = [p % threads if "%d" in p else p for p in b["params"]]

    # Override iters
    params[1] = str(iters)

    result = subprocess.run(
        ["/usr/bin/time", "-f", "real_sec=%e", binary] + params,
        cwd=BENCHMARK_DIR,
        capture_output=True, text=True, timeout=300
    )

    txn_count = 0
    wall_sec = 0.0

    for line in result.stdout.split("\n"):
        if "INVARIANT:" in line and "PASS" in line:
            # Extract the initial sum (which equals total committed)
            # For fuzz_counter: "INVARIANT: counter sum: PASS (N == N)"
            # Total txns = N - initial_sum of all counters
            # Actually, each committed txn adds delta, and the sum
            # of all deltas = final_sum - initial_sum (which is
            # num_counters * 1000 = 64000)
            pass
        if "PASS" in line:
            txn_count = iters * threads  # each thread does `iters` txns

    for line in result.stderr.split("\n"):
        if "real_sec=" in line:
            wall_sec = float(line.strip().split("=")[1])

    return txn_count, wall_sec


def generate_trace(bench_name, threads, iters):
    """Generate a JSONL trace matching the benchmark's transaction pattern.

    Returns list of event dicts.
    """
    """Generate a JSONL trace matching the benchmark's transaction pattern.

    For single-threaded (1t): sequential transactions with no contention.
    For multi-threaded (2t, 4t): transactions interleaved at fine
    granularity. Each thread picks addresses deterministically from
    64 slots, creating realistic contention when multiple threads
    access the same slot.

    Returns list of event dicts matching the Rust Event JSON format.
    """
    b = BENCHMARKS[bench_name]
    num_counters = b["num_counters"]

    events = []
    seq = 0
    ts = 1

    # Spawn threads (required for multi-threaded simulation)
    for t in range(threads):
        events.append({
            "timestamp": ts, "thread_id": 0, "seq": seq,
            "kind": {"ThreadSpawn": t}
        })
        ts += 1
        seq += 1

    # Interleave transactions across threads at fine granularity
    # to model realistic concurrent execution.
    for i in range(iters):
        for t in range(threads):
            # Deterministic address selection (replaces rand_r)
            idx = (i * (t + 1) * 7 + t * 3) % num_counters

            if bench_name == "fuzz_counter":
                # 1 read + 1 write at the same address
                addr = BASE_ADDR + idx * STRIDE
                events.extend([
                    {"timestamp": ts, "thread_id": t, "seq": seq, "kind": "TxBegin"},
                ]); ts += 1; seq += 1
                events.extend([
                    {"timestamp": ts, "thread_id": t, "seq": seq,
                     "kind": {"Read": {"addr": addr, "width": 8}}},
                ]); ts += 1; seq += 1
                events.extend([
                    {"timestamp": ts, "thread_id": t, "seq": seq,
                     "kind": {"Write": {"addr": addr, "width": 8, "val": 0}}},
                ]); ts += 1; seq += 1
                events.extend([
                    {"timestamp": ts, "thread_id": t, "seq": seq, "kind": "TxEnd"},
                ]); ts += 1; seq += 1

            elif bench_name == "fuzz_bank":
                # 2 reads + 2 writes (transfer between two accounts)
                idx2 = (idx * 13 + 3) % num_counters
                if idx2 == idx:
                    idx2 = (idx + 1) % num_counters
                addr_a = BASE_ADDR + idx * STRIDE
                addr_b = BASE_ADDR + idx2 * STRIDE
                events.extend([
                    {"timestamp": ts, "thread_id": t, "seq": seq, "kind": "TxBegin"},
                ]); ts += 1; seq += 1
                for a in [addr_a, addr_b]:
                    events.extend([
                        {"timestamp": ts, "thread_id": t, "seq": seq,
                         "kind": {"Read": {"addr": a, "width": 8}}},
                    ]); ts += 1; seq += 1
                for a in [addr_a, addr_b]:
                    events.extend([
                        {"timestamp": ts, "thread_id": t, "seq": seq,
                         "kind": {"Write": {"addr": a, "width": 8, "val": 0}}},
                    ]); ts += 1; seq += 1
                events.extend([
                    {"timestamp": ts, "thread_id": t, "seq": seq, "kind": "TxEnd"},
                ]); ts += 1; seq += 1

    return events


def run_simulator(events):
    """Run simulator with given events, return estimated cycles."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl",
                                     delete=False) as f:
        trace_path = f.name
        for ev in events:
            f.write(json.dumps(ev, separators=(",", ":")) + "\n")

    result = subprocess.run(
        [SIMULATOR, "--trace", trace_path,
         "--machine-profile", MACHINE_PROFILE,
         "--backend", "tsxsgl",
         "--clock-mode", "cost"],
        capture_output=True, text=True, timeout=60
    )

    os.unlink(trace_path)

    cycles = 0
    commits = 0
    aborts = 0
    # Simulator prints everything to stderr
    output = result.stderr
    for line in output.split("\n"):
        if "Estimated cycles:" in line:
            try:
                cycles = int(line.split(":")[1].strip())
            except (IndexError, ValueError):
                pass
        if "TX commits:" in line:
            try:
                commits = int(line.split(":")[1].strip())
            except (IndexError, ValueError):
                pass
        if "TX aborts:" in line:
            try:
                aborts = int(line.split(":")[1].strip())
            except (IndexError, ValueError):
                pass

    return cycles, commits, aborts


def main():
    build_simulator()

    print("=" * 80)
    print("TSXSGL Real vs Simulated Throughput Comparison")
    print("=" * 80)
    print(f"CPU: Intel Xeon E5-2648L v4 @ {FREQ_GHZ} GHz")
    print(f"Machine profile: {MACHINE_PROFILE}")
    print()

    # Test sizes: 1000 txns for quick sim, 100K for reliable real timing
    SIM_ITERS = 1000
    REAL_ITERS = 1000000

    rows = []

    for bench_name in ["fuzz_counter", "fuzz_bank"]:
        for threads in [1, 2, 4]:
            print(f"\n--- {bench_name} {threads}t ---")

            # 1. Real throughput
            real_txns, real_sec = run_real_benchmark(
                bench_name, threads, REAL_ITERS)
            real_tps = real_txns / real_sec if real_sec > 0 else 0
            print(f"  Real: {real_txns} txns in {real_sec:.3f}s = "
                  f"{real_tps:,.0f} txns/sec")

            # 2. Generate trace and run simulator
            events = generate_trace(bench_name, threads, SIM_ITERS)
            est_cycles, commits, aborts = run_simulator(events)
            sim_txns = commits  # committed transactions from sim

            # 3. Convert cycles → time
            sim_sec = est_cycles / (FREQ_GHZ * 1e9)
            sim_tps = sim_txns / sim_sec if sim_sec > 0 else 0

            # 4. Normalize: scale simulated throughput to 1 thread base
            #    (since the trace has fewer txns than the real run)
            sim_tps_scaled = sim_tps

            print(f"  Sim:  {sim_txns} commits in {est_cycles} cycles = "
                  f"{sim_sec:.6f}s sim = {sim_tps_scaled:,.0f} txns/sec")
            print(f"  Sim abort rate: {aborts}/{commits+aborts} "
                  f"({100*aborts/(commits+aborts) if commits+aborts > 0 else 0:.1f}%)")

            # 5. Error
            if real_tps > 0:
                error = (sim_tps / real_tps - 1) * 100
            else:
                error = 0
            err_str = f"{error:+.1f}%"
            print(f"  Error: {err_str}")

            rows.append({
                "benchmark": bench_name,
                "threads": threads,
                "real_tps": real_tps,
                "sim_tps": sim_tps,
                "error_pct": error,
                "sim_commits": commits,
                "sim_aborts": aborts,
                "sim_cycles": est_cycles,
            })

    # Summary table
    print("\n" + "=" * 80)
    print("Summary")
    print("=" * 80)
    print(f"{'Benchmark':<16} {'Threads':<8} {'Real TPS':<16} "
          f"{'Sim TPS':<16} {'Error %':<10} {'Sim cyc/txn':<12}")
    print("-" * 80)
    for r in rows:
        cyc_per_txn = r["sim_cycles"] / r["sim_commits"] if r["sim_commits"] > 0 else 0
        print(f"{r['benchmark']:<16} {r['threads']:<8} "
              f"{r['real_tps']:<16,.0f} {r['sim_tps']:<16,.0f} "
              f"{r['error_pct']:<+10.1f} {cyc_per_txn:<12.1f}")

    print()
    print("Notes:")
    print("  - Real time from /usr/bin/time -f real_sec=%e")
    print("  - Sim time = estimated_cycles / CPU_frequency")
    print(f"  - CPU frequency: {FREQ_GHZ} GHz (nominal)")
    print("  - Sim trace has fewer txns (1000) than real run (1,000,000)")
    print("    to keep trace generation fast. Throughput is normalized.")
    print("  - Multi-threaded traces use deterministic address selection")
    print("    (not random) — contention is approximate.")


if __name__ == "__main__":
    main()
