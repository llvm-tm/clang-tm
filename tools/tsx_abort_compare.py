#!/usr/bin/env python3
"""
Compare TSX abort breakdowns: Real (TSXSGL with TSX_PROFILE) vs Sim (tsx-sim backend).

Real profiling data from: benchmarks/profiling_data/raw/
Sim: runs tm-sim --backend tsx-sim on generated traces.
"""

import json, subprocess, tempfile, os, re, sys
from pathlib import Path

HERE = Path(__file__).parent
PROJ = HERE.parent
SIM = PROJ / "simulator"
MP = SIM / "machine_profiles" / "broadwell_ep_v4.json"
RAW = PROJ / "benchmarks" / "profiling_data" / "raw"

sys.path.insert(0, str(PROJ))
from tools.compare_tsxsgl import generate_trace, BASE_ADDR, STRIDE, FREQ_GHZ

TM_SIM = ["cargo", "run", "--bin", "tm-sim", "--"]

# ── Real TSX stats parser ──────────────────────────────────

TSX_STATS_RE = re.compile(
    r"TSX_STATS:"
    r" xbegin_ok=(\d+)\((\d+)\)"
    r" xbegin_abort=(\d+)\((\d+)\)"
    r" xend=(\d+)\((\d+)\)"
    r" xabort=(\d+)\((\d+)\)"
    r" sgl_begin=(\d+)\((\d+)\)"
    r" sgl_end=(\d+)\((\d+)\)"
    r" sgl_spin=(\d+)\((\d+)\)"
    r" read=(\d+)\((\d+)\)"
    r" write=(\d+)\((\d+)\)"
    r" depth=(\d+)\((\d+)\)"
    r" conflict=(\d+)"
    r" capacity=(\d+)"
    r" explicit=(\d+)"
    r" other=(\d+)"
)

def parse_real_tsx(path):
    """Parse TSX_STATS line from profiling output file."""
    with open(path) as f:
        for line in f:
            m = TSX_STATS_RE.search(line)
            if m:
                g = m.groups()
                return {
                    "xbegin_ok": int(g[0]),
                    "xbegin_abort": int(g[2]),
                    "xend": int(g[4]),
                    "sgl_begin": int(g[8]),
                    "sgl_end": int(g[10]),
                    "sgl_spin": int(g[12]),
                    "read": int(g[14]),
                    "write": int(g[16]),
                    "depth": int(g[18]),
                    "conflict": int(g[20]),
                    "capacity": int(g[21]),
                    "explicit": int(g[22]),
                    "other": int(g[23]),
                }
    return None

# ── Sim TSX stats parser ──────────────────────────────────

def parse_sim_output(stderr):
    """Parse tm-sim --backend tsx-sim output for commits/aborts + breakdown."""
    result = {"sim_commits": 0, "sim_aborts": 0, "tsx_commits": 0, "tsx_aborts": 0,
              "conflict": 0, "capacity": 0, "explicit": 0, "self": 0, "other": 0, "fallback": 0,
              "cycles": 0}
    for line in stderr.split("\n"):
        low = line.lower()
        m = re.search(r'commits:\s*(\d+)\s+aborts:\s*(\d+)', low)
        if m:
            result["sim_commits"] = int(m.group(1))
            result["sim_aborts"] = int(m.group(2))
        if "cost mode:" in low:
            m = re.search(r'(\d+)\s*cycles', low)
            if m: result["cycles"] = int(m.group(1))
    # Parse STATS (TSX SIM) block — multiline
    if "STATS (TSX SIM):" in stderr:
        m = re.search(r'Commits=(\d+)\s+Aborts=(\d+)', stderr)
        if m:
            result["tsx_commits"] = int(m.group(1))
            result["tsx_aborts"] = int(m.group(2))
        m = re.search(r'conflict=(\d+)\s+capacity=(\d+)\s+explicit=(\d+)\s+self=(\d+)\s+other=(\d+)\s+fallback=(\d+)', stderr)
        if m:
            result["conflict"] = int(m.group(1))
            result["capacity"] = int(m.group(2))
            result["explicit"] = int(m.group(3))
            result["self"] = int(m.group(4))
            result["other"] = int(m.group(5))
            result["fallback"] = int(m.group(6))
    return result

# ── Run sim ────────────────────────────────────────────────

def run_sim(bench, threads, iters=1000):
    """Run TSX sim backend and return stats."""
    events = generate_trace(bench, threads, iters, hot_ratio=0.9, num_counters=4)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        p = f.name
        for ev in events:
            f.write(json.dumps(ev, separators=(",", ":")) + "\n")
    res = subprocess.run(
        [*TM_SIM, "--trace", p, "--backend", "tsx-sim",
         "--clock-mode", "cost", "--machine-profile", str(MP),
         "--freq-ghz", str(FREQ_GHZ)],
        capture_output=True, text=True, timeout=120,
        cwd=str(SIM))
    os.unlink(p)
    return parse_sim_output(res.stderr)

# ── Main ───────────────────────────────────────────────────

BENCHMARKS = [
    ("fuzz_counter", ["1t", "2t", "4t", "8t"]),
    ("fuzz_bank",    ["1t", "2t", "4t", "8t"]),
]

def main():
    build_done = False

    print("=" * 98)
    print("  TSX Abort Type Comparison: Real (TSXSGL TSX_PROFILE) vs Sim (tsx-sim backend)")
    print("=" * 98)
    print(f"  CPU: Intel Xeon E5-2648L v4 @ 1.8 GHz")
    print()

    for bench, threads_list in BENCHMARKS:
        for tlabel in threads_list:
            nthreads = int(tlabel[0])
            # Real data
            real_file = list(RAW.glob(f"{bench}_tsxsgl_{tlabel}_*.txt"))
            real_stats = parse_real_tsx(real_file[0]) if real_file else None

            # Run sim
            if not build_done:
                # Let first run trigger build
                pass
            sim_stats = run_sim(bench, nthreads, iters=5000)
            build_done = True  # subsequent runs don't need to rebuild

            # Print comparison
            print(f"── {bench} {tlabel} ─{'─' * (70)}")
            if real_stats:
                total_rxns = real_stats["sgl_begin"] + real_stats["xend"]
                rt = (real_stats["conflict"] + real_stats["capacity"] +
                      real_stats["explicit"] + real_stats["other"])
                print(f"  REAL TSXSGL:")
                print(f"    RTM: xbegin_ok={real_stats['xend']} "
                      f"xbegin_abort={real_stats['xbegin_abort']} "
                      f"RTM abort rate={100*real_stats['xbegin_abort']/(real_stats['xbegin_ok']+real_stats['xbegin_abort']):.1f}%")
                print(f"    Total txns: {total_rxns} (RTM={real_stats['xend']} SGL={real_stats['sgl_begin']})")
                print(f"    Abort breakdown: conflict={real_stats['conflict']} "
                      f"capacity={real_stats['capacity']} "
                      f"explicit={real_stats['explicit']} "
                      f"other={real_stats['other']}")
                print(f"    SGL transactions: {real_stats['sgl_begin']} "
                      f"SGL spins: {real_stats['sgl_spin']}")
            else:
                print(f"  REAL TSXSGL: No data")

            print(f"  SIM tsx-sim (MAX_RETRIES=5, begin-time retry loop):")
            print(f"    SimEngine: {sim_stats['sim_commits']} commits, "
                  f"{sim_stats['sim_aborts']} aborts "
                  f"(rate={100*sim_stats['sim_aborts']/(sim_stats['sim_commits']+sim_stats['sim_aborts']+1):.1f}%)")
            print(f"    TSX SIM backend: {sim_stats['tsx_commits']} commits, "
                  f"{sim_stats['tsx_aborts']} aborts "
                  f"(rate={100*sim_stats['tsx_aborts']/(sim_stats['tsx_commits']+sim_stats['tsx_aborts']+1):.1f}%)")
            total_sim = sim_stats['conflict'] + sim_stats['capacity'] + sim_stats['explicit'] + sim_stats['self'] + sim_stats['other'] + sim_stats['fallback']
            print(f"    Abort breakdown: conflict={sim_stats['conflict']} "
                  f"capacity={sim_stats['capacity']} "
                  f"explicit={sim_stats['explicit']} "
                  f"self={sim_stats['self']} "
                  f"other={sim_stats['other']} "
                  f"fallback={sim_stats['fallback']}")
            # Backend already counts every _xbegin() failure in abort_count
            # (no extra normalization needed).  Total _xbegin() attempts =
            # aborts + commits.  Split commits into TSX vs SGL.
            sgl_txns = sim_stats['fallback']
            tsx_commits = sim_stats['tsx_commits'] - sgl_txns
            norm_attempts = sim_stats['tsx_aborts'] + sim_stats['tsx_commits']
            norm_aborts = sim_stats['tsx_aborts']
            norm_commits = sim_stats['tsx_commits']
            print(f"    Per-_xbegin_: {norm_attempts} attempts, "
                  f"{norm_aborts} aborts, {norm_commits} commits "
                  f"(TSX={tsx_commits} SGL={sgl_txns})")
            if norm_attempts > 0:
                print(f"    Per-_xbegin_ abort rate: {100*norm_aborts/norm_attempts:.1f}%")
            print(f"    Cycles: {sim_stats['cycles']}")

            # Compare
            print(f"  COMPARISON:")
            if real_stats:
                r_xbegin = real_stats["xbegin_ok"] + real_stats["xbegin_abort"]
                r_abort_rate = 100 * real_stats["xbegin_abort"] / r_xbegin if r_xbegin > 0 else 0
                r_tsx_commits = real_stats["xend"]
                r_sgl_commits = real_stats["sgl_begin"]
                print(f"    Real xbegin abort rate: {r_abort_rate:.1f}%")
                print(f"    Sim abort rate (per-_xbegin_): {100*norm_aborts/norm_attempts:.1f}%" if norm_attempts > 0 else "    Sim abort rate: N/A")
                print(f"    Real: {r_xbegin} xbegin ({r_tsx_commits} ok, {real_stats['xbegin_abort']} abort)  "
                      f"SGL={r_sgl_commits}")
                print(f"    Sim:  {norm_attempts} xbegin ({tsx_commits} ok, {norm_aborts} abort)  "
                      f"SGL={sgl_txns}")

                def ratio(r, s, name):
                    if r > 0 or s > 0:
                        match = "✓" if abs(r - s) < 0.1 * max(r, s, 1) else "✗"
                        print(f"    {name}: real={r} sim={s} {match}")

                ratio(real_stats["conflict"], sim_stats["conflict"], "conflict aborts")
                ratio(real_stats["explicit"], sim_stats["explicit"], "explicit aborts (LOCK_BUSY)")
                ratio(real_stats["capacity"], sim_stats["capacity"], "capacity aborts")
                ratio(real_stats["other"], sim_stats["self"] + sim_stats["other"], "other+self aborts")
                ratio(r_sgl_commits, sgl_txns, "SGL fallback txns")
                # Throughput comparison
                sim_time = sim_stats['cycles'] / (FREQ_GHZ * 1e9)
                print(f"    Cycles: {sim_stats['cycles']} → est. time {sim_time*1e6:.2f}us")
            print()

        print()

if __name__ == "__main__":
    main()
