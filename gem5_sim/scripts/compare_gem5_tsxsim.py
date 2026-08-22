#!/usr/bin/env python3
"""Compare gem5 TSX simulation results with tsx_sim cost model predictions."""

import re
import subprocess
import sys
import os
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
GEM5_DIR = SCRIPT_DIR.parent / "gem5"
GEM5_BIN = GEM5_DIR / "build" / "X86_TSX" / "gem5.opt"
GEM5_CONFIG = Path("/tmp/tsx_test_config.py")
C_BINARY = Path("/tmp/bank")
TSX_SIM_DIR = SCRIPT_DIR.parent.parent / "simulator"
TSX_SIM_BIN = TSX_SIM_DIR / "target" / "release" / "tsx-sim-bank"

# Calibrated costs from Broadwell-EP v4 (tsx_sim lib.rs constants)
COST_XBEGIN = 60
COST_XEND = 178
COST_READ_L1 = 5
COST_WRITE_L1 = 6
COST_BLOOM_CHECK = 2

def run_gem5(binary, m5out="/tmp/m5out"):
    """Run gem5 and extract stats."""
    subprocess.run(
        ["rm", "-rf", m5out],
        check=True, capture_output=True
    )
    env = os.environ.copy()
    env["PATH"] = f"/opt/homebrew/opt/python@3.12/bin:{env.get('PATH', '')}"
    env["PYTHON_CONFIG"] = "/opt/homebrew/opt/python@3.12/bin/python3.12-config"
    result = subprocess.run(
        [str(GEM5_BIN), "-d", m5out, str(GEM5_CONFIG), str(binary)],
        capture_output=True, text=True, timeout=300, env=env
    )

    # Parse output
    output = result.stdout + result.stderr
    tick_match = re.search(r'Exiting @ tick (\d+)', output)
    total_ticks = int(tick_match.group(1)) if tick_match else 0

    # Parse stats.txt
    stats = {}
    with open(f"{m5out}/stats.txt") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2 and not line.startswith('---'):
                key = parts[0]
                val = parts[1]
                stats[key] = val

    # Extract key metrics
    sim_ticks = int(stats.get('simTicks', 0))
    sim_insts = int(stats.get('simInsts', 0))
    host_seconds = float(stats.get('hostSeconds', 0))

    # HTM stats
    htm = {}
    for k, v in stats.items():
        if 'htm_transaction_cycles' in k:
            htm[k] = v
        elif 'htm_transaction_instructions' in k:
            htm[k] = v
        elif 'ReadSet' in k or 'WriteSet' in k:
            htm[k] = v

    # Parse HTM transaction cycles
    tx_cycles_mean = float(stats.get(
        'system.ruby.l0_cntrl.sequencer.m_htm_transaction_cycles::mean', 0))
    tx_insts_mean = float(stats.get(
        'system.ruby.l0_cntrl.sequencer.m_htm_transaction_instructions::mean', 0))

    # Parse CPU cycles
    cpu_cycles = int(stats.get('system.cpu.numCycles', 0))

    # Parse write set histogram
    ws_total = int(stats.get(
        'system.ruby.l0_cntrl.Dcache.htmTransCommitWriteSet::total', 0))
    ws_1 = int(stats.get(
        'system.ruby.l0_cntrl.Dcache.htmTransCommitWriteSet::1', 0))
    ws_2 = int(stats.get(
        'system.ruby.l0_cntrl.Dcache.htmTransCommitWriteSet::2', 0))

    # Gem5 clock period is 556 ticks/cycle (from stats)
    # Tick = 1 ps, clock = ~1.8 GHz
    clock_period = 556  # ticks per cycle
    total_cycles = sim_ticks / clock_period

    # Parse number of transactions from output
    commits = int(re.search(r'Commits: (\d+)', output).group(1))

    return {
        'total_ticks': sim_ticks,
        'total_cycles': total_cycles,
        'cpu_cycles': cpu_cycles,
        'total_instructions': sim_insts,
        'host_seconds': host_seconds,
        'commits': commits,
        'tx_cycles_mean': tx_cycles_mean,
        'tx_insts_mean': tx_insts_mean,
        'ws_size_1': ws_1,
        'ws_size_2': ws_2,
        'ws_total': ws_total,
    }


def run_tsx_sim():
    """Run tsx_sim bank benchmark and extract stats."""
    result = subprocess.run(
        [str(TSX_SIM_BIN)],
        capture_output=True, text=True, timeout=60
    )
    output = result.stdout + result.stderr

    commits = int(re.search(r'Commits: (\d+)', output).group(1))
    aborts = int(re.search(r'Aborts: (\d+)', output).group(1))

    # Expected cycles from cost model
    expected_per_tx = (COST_XBEGIN
        + 2 * (COST_READ_L1 + COST_BLOOM_CHECK)
        + 2 * (COST_WRITE_L1 + COST_BLOOM_CHECK)
        + COST_XEND)

    # Parse breakdown from STATS line
    conflict_match = re.search(r'conflict=(\d+)', output)
    capacity_match = re.search(r'capacity=(\d+)', output)
    explicit_match = re.search(r'explicit=(\d+)', output)
    self_match = re.search(r'self=(\d+)', output)
    other_match = re.search(r'other=(\d+)', output)
    fallback_match = re.search(r'fallback=(\d+)', output)

    return {
        'commits': commits,
        'aborts': aborts,
        'abort_conflict': int(conflict_match.group(1)) if conflict_match else 0,
        'abort_capacity': int(capacity_match.group(1)) if capacity_match else 0,
        'abort_explicit': int(explicit_match.group(1)) if explicit_match else 0,
        'abort_self': int(self_match.group(1)) if self_match else 0,
        'abort_other': int(other_match.group(1)) if other_match else 0,
        'fallback': int(fallback_match.group(1)) if fallback_match else 0,
        'expected_per_tx_cycles': expected_per_tx,
        'expected_total_cycles': expected_per_tx * commits,
    }


def main():
    print("=" * 65)
    print("  gem5 vs tsx_sim TSX Benchmark Comparison")
    print("  Target hardware: Broadwell-EP v4 (Xeon E5-2648L v4 @ 1.80 GHz)")
    print("=" * 65)

    print("\n--- Running gem5 simulation ---")
    gem5 = run_gem5(C_BINARY)
    print(f"  Total ticks:      {gem5['total_ticks']}")
    print(f"  Total cycles:     {gem5['total_cycles']:.0f} (@ 1.8 GHz)")
    print(f"  Total insts:      {gem5['total_instructions']}")
    print(f"  Host time:        {gem5['host_seconds']:.2f}s")

    print("\n--- Running tsx_sim (cost model) simulation ---")
    tsx = run_tsx_sim()
    print(f"  Expected per-TX:  {tsx['expected_per_tx_cycles']} cycles")
    print(f"  Expected total:   {tsx['expected_total_cycles']} cycles")

    print("\n" + "=" * 65)
    print("  Comparison")
    print("=" * 65)

    n_tx = gem5['commits']
    assert n_tx == tsx['commits'], "Transaction count mismatch!"

    # Key metric: sequencer cycles per TX + XEND commit latency
    # The sequencer measures time from HTM_START to HTM_COMMIT request.
    # This includes XBEGIN overhead (60) + body, but NOT XEND delay (178).
    gem5_tx_total = gem5['tx_cycles_mean'] + COST_XEND
    # Additional per-TX cost: CPU cycles not tracked by sequencer
    # (instruction fetch/decode, pipeline, etc.)
    cpu_total = gem5['cpu_cycles']
    non_tx_insts_est = gem5['total_instructions'] - n_tx * gem5['tx_insts_mean']
    # Estimate non-TX cycles: all instructions have some fetch/decode overhead
    # plus memory operation latency. Use average CPI - TX body CPI.
    tx_body_cpi_est = gem5['tx_cycles_mean'] / gem5['tx_insts_mean']  # ~9.1
    non_tx_cycles_est = non_tx_insts_est * tx_body_cpi_est
    tx_cpu_cycles_est = cpu_total - non_tx_cycles_est
    gem5_per_tx_cpu_est = tx_cpu_cycles_est / n_tx if n_tx > 0 else 0

    tsx_per_tx = tsx['expected_per_tx_cycles']

    print(f"  Transactions:                     {n_tx}")
    print(f"")
    print(f"  Per-transaction cycle cost:")
    print(f"    Gem5 sequencer cycles (A):      {gem5['tx_cycles_mean']:.1f}")
    print(f"    Gem5 + XEND (A + {COST_XEND}):           {gem5_tx_total:.1f}")
    print(f"    tsx_sim cost model:              {tsx_per_tx}")
    print(f"")
    print(f"  CPU-level:")
    print(f"    Gem5 total CPU cycles:           {gem5['cpu_cycles']}")
    print(f"    Gem5 per-TX (sequencer adj):     {gem5_tx_total:.1f}")
    print(f"    tsx_sim per-TX:                  {tsx_per_tx}")
    print(f"")
    print(f"  HTM write set:")
    print(f"    Size 1:                          {gem5['ws_size_1']} ({gem5['ws_size_1']/n_tx*100:.1f}%)")
    print(f"    Size 2:                          {gem5['ws_size_2']} ({gem5['ws_size_2']/n_tx*100:.1f}%)")
    print(f"")

    # Primary error metric: sequencer cycles + XEND vs tsx_sim model
    err_cycles = gem5_tx_total - tsx_per_tx
    err_pct = err_cycles / tsx_per_tx * 100

    print(f"  Error (gem5 sequencer adj - tsx_sim):")
    print(f"    Absolute: {err_cycles:+.1f} cycles/TX")
    print(f"    Relative: {err_pct:+.1f}%")

    # Sim speed
    sim_rate = gem5['total_instructions'] / gem5['host_seconds']
    print(f"\n  Simulation speed:                 {sim_rate:.0f} insts/s")

    print(f"\n  Verdict: ", end="")
    if abs(err_pct) < 10:
        print("EXCELLENT (<10% error)")
    elif abs(err_pct) < 25:
        print("GOOD (10-25% error)")
    elif abs(err_pct) < 50:
        print("FAIR (25-50% error)")
    else:
        print("POOR (>50% error)")


if __name__ == "__main__":
    main()
