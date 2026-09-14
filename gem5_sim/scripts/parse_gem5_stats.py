#!/usr/bin/env python3
"""Parse one gem5 run dir (stats.txt last section = ROI) into a flat dict.

Usage: parse_gem5_stats.py <run_dir> [key=value ...]
Prints a JSON line: the parsed stats merged with the given key=value extras.
"""
import json
import os
import re
import sys


def last_section(path):
    # gem5 appends one "Begin/End Simulation Statistics" block per
    # dump_stats call. The ROI run ends with ROI_RESET_STATS (zeroing
    # counters) then ROI_DUMP_STATS, so the LAST occurrence of every
    # stat name in the file is the ROI value. Return the whole file and
    # let parse() keep last-wins per name.
    with open(path) as f:
        return f.read()


def parse(section):
    # Two line shapes:
    #   name  value  # comment                (plain)
    #   name::sub  value  pct min max | ...   (distribution; value still 2nd)
    # Last occurrence wins (ROI section is last in the file).
    out = {}
    for line in section.splitlines():
        toks = line.split()
        if len(toks) < 2:
            continue
        try:
            out[toks[0]] = float(toks[1])
        except ValueError:
            continue
    return out


def f(d, name):
    v = d.get(name)
    if v is None or v != v:  # nan
        return None
    return v


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: parse_gem5_stats.py <run_dir> [k=v ...]")
    rdir = sys.argv[1]
    extras = {}
    for kv in sys.argv[2:]:
        k, _, v = kv.partition("=")
        extras[k] = v

    stats_path = os.path.join(rdir, "stats.txt")
    out = dict(extras)
    if not os.path.exists(stats_path):
        out["error"] = "no stats.txt"
        print(json.dumps(out))
        return

    d = parse(last_section(stats_path))

    # Guest verdict (simout.txt)
    simout = os.path.join(rdir, "simout.txt")
    verdict = ""
    guest_txns = None
    if os.path.exists(simout):
        with open(simout) as fh:
            text = fh.read()
        m = re.search(r"Total txns:\s*(\d+)", text)
        if m:
            guest_txns = int(m.group(1))
        if "PASS: Money conserved" in text:
            verdict = "PASS (money conserved)"
        elif "FAIL" in text:
            verdict = "FAIL"
        elif re.search(r"INVARIANT: counter sum: PASS", text):
            verdict = "PASS (counter sum)"
        elif re.search(r"INVARIANT: counter sum: FAIL", text):
            verdict = "FAIL (counter sum)"
        else:
            # intruder: success == all flows found
            mflow = re.search(r"Num flow\s*=\s*(\d+)", text)
            mfound = re.search(r"Num found\s*=\s*(\d+)", text)
            if mflow and mfound:
                ok = int(mfound.group(1)) == int(mflow.group(1))
                verdict = (f"{'PASS' if ok else 'FAIL'} "
                           f"(found {mfound.group(1)}/{mflow.group(1)})")
    out["guest"] = verdict
    out["guest_txns"] = guest_txns

    # Core aggregates (sum idle cores = 0)
    cycles = 0
    for k, v in d.items():
        if re.fullmatch(r"board\.processor\.cores\d+\.core\.numCycles", k):
            cycles += int(v)
    out["simTicks"] = f(d, "simTicks")
    out["simSeconds"] = f(d, "simSeconds")
    out["simInsts"] = f(d, "simInsts")
    out["numCycles_allcores"] = cycles or None

    # HTM aggregates. Start/Commit/Abort counters live on the shared
    # L0Cache_Controller (one per system); per-transaction set sizes and
    # cycle stats live on each l1_controllersN (one per core).
    def htm_total(suffix):
        tot = 0
        found = False
        for k, v in d.items():
            if re.fullmatch(
                r"board\.cache_hierarchy\.ruby_system\.L0Cache_Controller\."
                rf"{suffix}::total", k
            ):
                tot = int(v)
                found = True
        return tot if found else None

    def htm_weighted_mean(suffix):
        samples = 0
        acc = 0.0
        for k, v in d.items():
            m = re.fullmatch(
                r"board\.cache_hierarchy\.ruby_system\.l1_controllers\d+\."
                rf"Dcache\.{suffix}::(mean|total|samples)", k
            )
            if not m:
                continue
            if m.group(1) == "samples":
                samples += int(v)
            elif m.group(1) == "mean":
                acc += v * (d.get(k.replace("::mean", "::samples"), 0) or 0)
        return acc / samples if samples else None

    out["htm_start"] = htm_total("HTM_Start")
    out["htm_commit"] = htm_total("HTM_Commit")
    out["htm_abort"] = htm_total("HTM_Abort")
    out["commit_readset_mean"] = htm_weighted_mean("htmTransCommitReadSet")
    out["commit_writeset_mean"] = htm_weighted_mean("htmTransCommitWriteSet")
    out["abort_readset_mean"] = htm_weighted_mean("htmTransAbortReadSet")

    # sequencer transaction-cycle stats (per l1 controller)
    cyc_samples = 0
    cyc_acc = 0.0
    instr_samples = 0
    instr_acc = 0.0
    abort_causes = {}
    for k, v in d.items():
        m = re.fullmatch(
            r"board\.cache_hierarchy\.ruby_system\.l1_controllers\d+\."
            r"sequencer\.(m_htm_transaction_cycles|m_htm_transaction_instructions)"
            r"::(mean|total|samples)", k
        )
        if m:
            if m.group(2) == "samples":
                if m.group(1) == "m_htm_transaction_cycles":
                    cyc_samples += int(v)
                else:
                    instr_samples += int(v)
            elif m.group(2) == "mean":
                s = d.get(
                    k.replace("::mean", "::samples"), 0
                )
                if m.group(1) == "m_htm_transaction_cycles":
                    cyc_acc += v * s
                else:
                    instr_acc += v * s
        m = re.fullmatch(
            r"board\.cache_hierarchy\.ruby_system\.l1_controllers\d+\."
            r"sequencer\.m_htm_transaction_abort_cause::(\w+)", k
        )
        if m:
            abort_causes[m.group(1)] = abort_causes.get(m.group(1), 0) + int(v)
    out["tx_cycles_mean"] = (cyc_acc / cyc_samples) if cyc_samples else None
    out["tx_insts_mean"] = (instr_acc / instr_samples) if instr_samples else None
    out["abort_causes"] = abort_causes

    # Derived: committed transactions per second of simulated time
    if out.get("htm_commit") and out.get("simSeconds"):
        out["txns_per_sec"] = out["htm_commit"] / out["simSeconds"]

    print(json.dumps(out, default=str))


if __name__ == "__main__":
    main()
