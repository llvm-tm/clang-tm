#!/usr/bin/env python3
"""Parallel gem5 sweep for the X86_TSX validation.

Runs a bounded matrix of (benchmark x backend x threads) under gem5 SE mode
with the ROI-bracketed guest binaries, collects ROI + HTM stats per run, and
writes results.csv + results.json.

Usage:
  run_gem5_sweep.py [--jobs N] [--timeout SECS] [--tag FILTER] [--out DIR]
"""
import argparse
import concurrent.futures as cf
import csv
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TSXC = os.path.dirname(HERE)
REPO = os.path.dirname(TSXC)
GEM5 = os.path.join(TSXC, "gem5", "build", "X86_TSX", "gem5.opt")
CFG = os.path.join(TSXC, "configs", "x86-se-bank.py")
BIN = os.path.join(REPO, "benchmarks", "cpp", "bin")
PARSE = os.path.join(HERE, "parse_gem5_stats.py")

CLK = "1.8GHz"

# (tag, binary, threads, raw_args|None, bank_txns|None, extra_envs, max_ticks)
#
# NOTE: Ruby's MESI_Three_Level(_HTM) coherence livelocks on any gem5 run with
# 2+ cores (see gem5_sim/docs/x86-tsx-validation.md, "Known limitations"). A
# bounded tick budget is applied to multithreaded runs so the sweep terminates
# with stats instead of hanging until the wall timeout. Pass --mt-ticks 0 to
# disable the guard (at your own risk of a hang).
def matrix(mt_ticks=0):
    M = []
    # bank: -a accounts -t threads -r readpct -n total-txns (config default synth)
    for be in ("norec", "tsxsgl"):
        for t in (1, 2, 4):
            M.append(dict(
                tag=f"bank-{be}-t{t}", binary=f"bank_gem5_{be}", threads=t,
                raw=None, txns=1000, env=[], ticks=0))
    # fuzz_counter: positional "threads iters counters seed"
    for be in ("norec", "tsxsgl"):
        for t in (1, 2, 4):
            M.append(dict(
                tag=f"fuzz-{be}-t{t}", binary=f"fuzz_counter_gem5_{be}",
                threads=t, raw=f"{t} 300 8 42", txns=None, env=[], ticks=0))
    # intruder: -p threads -a pct -l maxlen -n flows -s seed
    for be in ("norec", "tsxsgl"):
        for t in (1, 2):
            M.append(dict(
                tag=f"intruder-{be}-t{t}", binary=f"intruder_gem5_{be}",
                threads=t, raw=f"-p {t} -a 30 -l 4 -n 300 -s 42",
                txns=None, env=[], ticks=0))
    for m in M:
        if m["binary"].endswith("tsxsgl"):
            m["env"].append("TM_RTM_DEBUG=1")
    if mt_ticks > 0:
        for m in M:
            if m["threads"] >= 2 and m["ticks"] == 0:
                m["ticks"] = mt_ticks
    return M


def run_one(m, outdir, timeout):
    rdir = os.path.join(outdir, m["tag"])
    os.makedirs(rdir, exist_ok=True)
    cmd = [GEM5, "-d", rdir, CFG,
           "--binary", os.path.join(BIN, m["binary"]),
           "--threads", str(m["threads"]), "--clk", CLK,
           "--cpu-type", "timing"]
    if m["raw"] is not None:
        cmd += ["--raw-args", m["raw"]]
    elif m["txns"] is not None:
        cmd += ["--accounts", "64", "--txns", str(m["txns"])]
    for e in m["env"]:
        cmd += ["--env", e]
    if m["ticks"]:
        cmd += ["--max-ticks", str(m["ticks"])]
    t0 = time.time()
    capped = False
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, cwd=REPO)
        rc = p.returncode
        log = (p.stdout or "") + (p.stderr or "")
        if "max-ticks" in log or "max ticks" in log.lower():
            capped = True
    except subprocess.TimeoutExpired:
        rc = "TIMEOUT"
    wall = time.time() - t0
    try:
        stats = subprocess.run(
            [sys.executable, PARSE, rdir, "tag=" + m["tag"],
             "wall_secs=%.1f" % wall, "rc=" + str(rc)],
            capture_output=True, text=True, timeout=30).stdout.strip()
        row = json.loads(stats)
    except Exception as e:
        row = {"tag": m["tag"], "error": str(e)}
    row["wall_secs"] = round(wall, 1)
    row["rc"] = rc
    if rc == "TIMEOUT":
        row["status"] = "hang/timeout"
    elif capped:
        row["status"] = "tick-capped (likely livelock; see docs)"
    elif rc == 0:
        row["status"] = "ok"
    else:
        row["status"] = "error"
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--timeout", type=int, default=900,
                    help="wall timeout per gem5 run (s)")
    ap.add_argument("--mt-ticks", type=int, default=2_000_000_000,
                    help="tick guard applied to runs with >=2 threads to dodge the "
                         "Ruby MESI_Three_Level livelock (0 disables it)")
    ap.add_argument("--tag", default=None, help="substring filter on tags")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    outdir = args.out or os.path.join(
        TSXC, "m5out", "sweep-" + time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(outdir, exist_ok=True)

    M = [m for m in matrix(args.mt_ticks) if not args.tag or args.tag in m["tag"]]
    print(f">>> {len(M)} runs, jobs={args.jobs}, timeout={args.timeout}s, "
          f"mt-ticks={args.mt_ticks}, out={outdir}", flush=True)

    rows = []
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, m, outdir, args.timeout): m for m in M}
        for fut in cf.as_completed(futs):
            row = fut.result()
            rows.append(row)
            print(json.dumps(row, default=str), flush=True)

    # stable order
    order = {m["tag"]: i for i, m in enumerate(M)}
    rows.sort(key=lambda r: order.get(r.get("tag", ""), 999))

    with open(os.path.join(outdir, "results.json"), "w") as f:
        json.dump(rows, f, indent=1, default=str)
    if rows:
        keys = []
        for r in rows:
            for k in r:
                if k not in keys:
                    keys.append(k)
        with open(os.path.join(outdir, "results.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow(r)
    print(f">>> done: {os.path.join(outdir, 'results.csv')}", flush=True)


if __name__ == "__main__":
    main()
