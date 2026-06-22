#!/usr/bin/env python3
"""
Compare real TSXSGL throughput vs simulator throughput (cost model + backend).

Approach:
  1. Run real C++ TSXSGL benchmarks (fuzz_counter, fuzz_bank) with
     /usr/bin/time to get wall-clock throughput (txns/sec).
  2. Collect real abort rates from profiling data (TSX_STATS from
     the profiling patch run in the previous session).
  3. Generate JSONL trace files matching the same benchmark patterns
     (read/write counts, contention, thread count).
  4. Run the simulator in two modes:
     a) tm-des --clock-mode cost (pure cost model, no real backend)
     b) tm-sim --clock-mode cost --backend norec (runs real backend
        + accumulates cycle costs from calibrated machine profile)
  5. Convert estimated cycles -> time via CPU frequency.
  6. Compare throughput and abort rates.

Usage:
  python3 tools/compare_tsxsgl.py

Dependencies: simulator built (cargo build --bin tm-des --bin tm-sim)
"""

import subprocess, json, os, sys, tempfile, re

REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TM_DES  = os.path.join(REPO_DIR, "simulator", "target", "debug", "tm-des")
TM_SIM  = os.path.join(REPO_DIR, "simulator", "target", "debug", "tm-sim")
MACHINE_PROFILE = os.path.join(REPO_DIR, "simulator", "machine_profiles",
                               "broadwell_ep_v4.json")
RAW_DATA_DIR = os.path.join(REPO_DIR, "benchmarks", "profiling_data", "raw")
BENCHMARK_DIR = os.path.join(REPO_DIR, "benchmarks", "cpp")

FREQ_GHZ = 1.8
BASE_ADDR = 0x7f00_0000_8000
STRIDE = 8
SIM_ITERS = 1000
REAL_ITERS = 1000000


def build_sim():
    for bin_name in ["tm-des", "tm-sim"]:
        p = os.path.join(REPO_DIR, "simulator", "target", "debug", bin_name)
        if not os.path.exists(p):
            print(f"Building {bin_name}...")
            subprocess.run(["cargo", "build", "--bin", bin_name],
                           cwd=os.path.join(REPO_DIR, "simulator"), check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run_real(bench, threads, iters):
    """Run real TSXSGL benchmark, return (total_txns, wall_sec)."""
    params = {
        "fuzz_counter": [str(threads), str(iters), "64", "42"],
        "fuzz_bank":    [str(threads), str(iters), "64", "42"],
    }[bench]
    binary = os.path.join(BENCHMARK_DIR, "bin", bench)
    result = subprocess.run(
        ["/usr/bin/time", "-f", "real_sec=%e", binary] + params,
        cwd=BENCHMARK_DIR, capture_output=True, text=True, timeout=300)
    wall_sec = 0.0
    for line in result.stderr.split("\n"):
        if "real_sec=" in line:
            wall_sec = float(line.strip().split("=")[1])
    return iters * threads, wall_sec


def load_real_abort_rate(bench, threads):
    """Load TSX_STATS abort rate from profiling data."""
    # Files are like: fuzz_counter_tsxsgl_1t_20260621_125020.txt
    for fname in os.listdir(RAW_DATA_DIR):
        if fname.startswith(f"{bench}_tsxsgl_{threads}t_"):
            with open(os.path.join(RAW_DATA_DIR, fname)) as f:
                content = f.read()
            m = re.search(r'TSX_STATS: (.+)', content)
            if not m: continue
            parts = m.group(1).split()
            stats = {}
            for p in parts:
                if '=' not in p or '(' not in p: continue
                k, v = p.split('=', 1)
                cnt_str, cyc_str = v.split('(')
                cyc_str = cyc_str.rstrip(')')
                stats[k] = int(cnt_str)
            xbegin_ok = stats.get('xbegin_ok', 0)
            xbegin_abort = stats.get('xbegin_abort', 0)
            sgl_begin = stats.get('sgl_begin', 0)
            # "Aborts" = xbegin_abort (failed _xbegin) that went to SGL
            # But some of these are capacity aborts that retry, not real "aborts"
            # For TSXSGL, total tm_begin calls = xbegin_ok + sgl_begin
            # Aborts are xbegin_abort - sgl_begin (retried within TSX)
            # Actually in our terminology: sgl_begin = number of fallbacks;
            # xbegin_abort = total failed _xbegin() calls
            total = xbegin_ok + sgl_begin
            if total == 0: return 0.0, 0, 0
            # Real abort rate from perspective of TM transactions:
            # each sgl_begin means a TSX transaction aborted and fell back
            abort_rate = 100.0 * sgl_begin / total
            return abort_rate, sgl_begin, total
    return None, None, None


def generate_trace(bench, threads, iters, hot_ratio=0.5, num_counters=8):
    """Generate JSONL trace matching benchmark pattern.
    
    Events within each transaction iteration are kept sequential, but all
    threads' iterations are interleaved at the phase level so multiple
    transactions overlap in time, enabling realistic conflict detection.
    
    Parameters:
    - hot_ratio: fraction of iterations where all threads hit the same counter
      (hot spot contention). Higher = more aborts, matching TSXSGL RTM behavior.
    - num_counters: total distinct counters available. Lower = more conflicts.
    """
    import random
    rng = random.Random(42)  # deterministic seed
    events = []
    seq = [0] * max(threads, 1)
    ts = 1
    for t in range(threads):
        events.append({"timestamp": ts, "thread_id": 0, "seq": seq[0],
                       "kind": {"ThreadSpawn": t}})
        seq[0] += 1; ts += 1
    for i in range(iters):
        # Phase 1: all threads begin (randomized order to trigger LOCK_BUSY in TSX sim)
        begin_order = list(range(threads))
        rng.shuffle(begin_order)
        for t in begin_order:
            events.append({"timestamp": ts, "thread_id": t, "seq": seq[t], "kind": "TxBegin"})
            seq[t] += 1
        ts += 1
        # Determine access pattern for this iteration (shared across read+write)
        hot = rng.random() < hot_ratio
        if hot:
            # All threads access same address(es) → guaranteed conflict
            if bench == "fuzz_counter":
                h_addr = BASE_ADDR + 0 * STRIDE
            else:
                h_addr1 = BASE_ADDR + 0 * STRIDE
                h_addr2 = BASE_ADDR + 1 * STRIDE
        # Phase 2: all threads read
        for t in range(threads):
            if bench == "fuzz_counter":
                addr = h_addr if hot else BASE_ADDR + rng.randint(0, num_counters - 1) * STRIDE
                events.append({"timestamp": ts, "thread_id": t, "seq": seq[t],
                               "kind": {"Read": {"addr": addr, "width": 8}}})
                seq[t] += 1
            elif bench == "fuzz_bank":
                if hot:
                    adds = [h_addr1, h_addr2]
                else:
                    i1 = rng.randint(0, num_counters - 1)
                    i2 = rng.randint(0, num_counters - 1)
                    if i2 == i1: i2 = (i1 + 1) % num_counters
                    adds = [BASE_ADDR + i1 * STRIDE, BASE_ADDR + i2 * STRIDE]
                for a in adds:
                    events.append({"timestamp": ts, "thread_id": t, "seq": seq[t],
                                   "kind": {"Read": {"addr": a, "width": 8}}})
                    seq[t] += 1
        ts += 1
        # Phase 3: all threads write (same addresses as reads)
        # Use unique values per thread to trigger NOrec value-based validation
        for t in range(threads):
            if bench == "fuzz_counter":
                addr = h_addr if hot else BASE_ADDR + rng.randint(0, num_counters - 1) * STRIDE
                val = i * threads + t + 1  # unique value => conflict detected by NOrec
                events.append({"timestamp": ts, "thread_id": t, "seq": seq[t],
                               "kind": {"Write": {"addr": addr, "width": 8, "val": val}}})
                seq[t] += 1
            elif bench == "fuzz_bank":
                if hot:
                    adds = [h_addr1, h_addr2]
                else:
                    i1 = rng.randint(0, num_counters - 1)
                    i2 = rng.randint(0, num_counters - 1)
                    if i2 == i1: i2 = (i1 + 1) % num_counters
                    adds = [BASE_ADDR + i1 * STRIDE, BASE_ADDR + i2 * STRIDE]
                val = i * threads + t + 1  # unique value
                for a in adds:
                    events.append({"timestamp": ts, "thread_id": t, "seq": seq[t],
                                   "kind": {"Write": {"addr": a, "width": 8, "val": val}}})
                    seq[t] += 1
        ts += 1
        # Phase 4: all threads commit (randomized order independent of begin)
        end_order = list(range(threads))
        rng.shuffle(end_order)
        for t in end_order:
            events.append({"timestamp": ts, "thread_id": t, "seq": seq[t], "kind": "TxEnd"})
            seq[t] += 1
        ts += 1
    return events


def run_tm_des(events):
    """Run tm-des (pure cost model, no real backend)."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        p = f.name
        for ev in events:
            f.write(json.dumps(ev, separators=(",", ":")) + "\n")
    res = subprocess.run([TM_DES, "--trace", p, "--machine-profile", MACHINE_PROFILE,
                          "--backend", "tsxsgl", "--clock-mode", "cost"],
                         capture_output=True, text=True, timeout=60)
    os.unlink(p)
    cycles = commits = aborts = 0
    for line in res.stderr.split("\n"):
        if "Estimated cycles:" in line:  cycles = int(line.split(":")[1].strip())
        if "TX commits:" in line:        commits = int(line.split(":")[1].strip())
        if "TX aborts:" in line:         aborts = int(line.split(":")[1].strip())
    return cycles, commits, aborts


def run_tm_sim(events, backend="norec"):
    """Run tm-sim (real backend + cost mode)."""
    with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False) as f:
        p = f.name
        for ev in events:
            f.write(json.dumps(ev, separators=(",", ":")) + "\n")
    res = subprocess.run([TM_SIM, "--trace", p, "--machine-profile", MACHINE_PROFILE,
                          "--backend", backend, "--clock-mode", "cost",
                          "--freq-ghz", str(FREQ_GHZ)],
                         capture_output=True, text=True, timeout=60)
    os.unlink(p)
    commits = aborts = cycles = 0
    for line in res.stderr.split("\n"):
        low = line.lower()
        # Both commits and aborts are on the same "═══ tm-sim report ═══" line:
        #   Commits: N  Aborts: M  Abort rate: X%
        m = re.search(r'commits:\s*(\d+)\s+aborts:\s*(\d+)', low)
        if m:
            commits = int(m.group(1))
            aborts = int(m.group(2))
        if "cost mode:" in low:
            m = re.search(r'(\d+)\s*cycles', low)
            if m: cycles = int(m.group(1))
    return cycles, commits, aborts


def fmt_tps(txns, sec):
    return f"{txns/sec:,.0f}" if sec > 0 else "N/A"


def main():
    build_sim()

    print("=" * 90)
    print("TSXSGL Real vs Simulated — Throughput & Abort Rates")
    print("=" * 90)
    print(f"CPU: Intel Xeon E5-2648L v4 @ {FREQ_GHZ} GHz nominal")
    print(f"Profiling data: {RAW_DATA_DIR}")
    print(f"Real bench: TSXSGL native C++")
    print(f"Sim (tm-des): TSXSGL cost model (no backend)")
    print(f"Sim (tm-sim): NOrec real backend + cost mode")
    print()

    all_rows = []

    for bench in ["fuzz_counter", "fuzz_bank"]:
        for threads in [1, 2, 4]:
            print(f"\n── {bench} {threads}t {'─'*50}")

            # 1. Real TSXSGL
            txns_real, sec_real = run_real(bench, threads, REAL_ITERS)
            tps_real = txns_real / sec_real
            real_abort_pct, real_aborts, real_total = load_real_abort_rate(bench, threads)

            print(f"  [REAL TSXSGL]      {txns_real:,} txns in {sec_real:.3f}s = {tps_real:>10,.0f} tps", end="")
            if real_abort_pct is not None:
                print(f"  abort={real_abort_pct:.1f}%", end="")
            print()

            # 2. tm-des (pure cost model, no backend)
            evts = generate_trace(bench, threads, SIM_ITERS)
            c1, cm1, ab1 = run_tm_des(evts)
            sec_sim_des = c1 / (FREQ_GHZ * 1e9)
            tps_sim_des = cm1 / sec_sim_des if sec_sim_des > 0 else 0
            print(f"  [SIM tm-des TSXSGL] {cm1} commits, {c1} cycles = {sec_sim_des:.3e}s = {tps_sim_des:>10,.0f} tps", end="")
            if cm1 + ab1 > 0:
                print(f"  abort={100*ab1/(cm1+ab1):.1f}%", end="")
            print(f"  error={((tps_sim_des/tps_real)-1)*100:+.1f}%")

            # 3. tm-sim NOrec (real backend + cost mode)
            c2, cm2, ab2 = run_tm_sim(evts, "norec")
            sec_sim_sim = c2 / (FREQ_GHZ * 1e9)
            tps_sim_sim = cm2 / sec_sim_sim if sec_sim_sim > 0 else 0
            print(f"  [SIM tm-sim NOrec]  {cm2} commits, {c2} cycles = {sec_sim_sim:.3e}s = {tps_sim_sim:>10,.0f} tps", end="")
            if cm2 + ab2 > 0:
                print(f"  abort={100*ab2/(cm2+ab2):.1f}%", end="")
            print(f"  error={((tps_sim_sim/tps_real)-1)*100:+.1f}%")

            all_rows.append({
                "bench": bench, "threads": threads,
                "real_tps": tps_real, "real_abort_pct": real_abort_pct,
                "des_tps": tps_sim_des, "des_abort_pct": 100*ab1/(cm1+ab1) if cm1+ab1 > 0 else 0,
                "sim_tps": tps_sim_sim, "sim_abort_pct": 100*ab2/(cm2+ab2) if cm2+ab2 > 0 else 0,
            })

    # Summary
    print("\n" + "=" * 90)
    print("Summary")
    print("=" * 90)
    print(f"{'Benchmark':<16} {'T':<4} {'Real TPS':<14} {'Real ab%':<10}"
          f" {'tm-des TPS':<14} {'des ab%':<10} {'des err':<8}"
          f" {'tm-sim TPS':<14} {'sim ab%':<10} {'sim err':<8}")
    print("-" * 90)
    for r in all_rows:
        des_err = ((r["des_tps"] / r["real_tps"]) - 1) * 100 if r["real_tps"] > 0 else 0
        sim_err = ((r["sim_tps"] / r["real_tps"]) - 1) * 100 if r["real_tps"] > 0 else 0
        ra = f"{r['real_abort_pct']:.1f}%" if r['real_abort_pct'] is not None else "N/A"
        print(f"{r['bench']:<16} {r['threads']:<4}"
              f" {r['real_tps']:<14,.0f} {ra:<10}"
              f" {r['des_tps']:<14,.0f} {r['des_abort_pct']:<10.1f} {des_err:<+8.1f}"
              f" {r['sim_tps']:<14,.0f} {r['sim_abort_pct']:<10.1f} {sim_err:<+8.1f}")

    print()
    print("Notes:")
    print("  - Real: TSXSGL C++ backend, /usr/bin/time")
    print("  - tm-des: --clock-mode cost, TSXSGL cost model (no real backend)")
    print("  - tm-sim: --clock-mode cost, NOrec Rust backend + cost accumulation")
    print("  - Abort rates differ by backend: TSXSGL aborts = RTM failures,")
    print("    NOrec aborts = value-based validation failures")
    print("  - Simulated abort rate captures actual backend conflicts,")
    print("    unlike pure cost model which sees 0% aborts")

if __name__ == "__main__":
    main()
