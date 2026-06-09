#!/usr/bin/env python3
"""
Compare collected TM metrics against Table VI characterization data.

Usage:
  python3 compare_table_vi.py <results.csv>
"""

import csv
import re
import sys
from pathlib import Path
from typing import Dict, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
TABLE_VI = REPO_ROOT / "benchmarks" / "stamp_characterization.csv"

def load_table_vi() -> Dict[str, dict]:
    data = {}
    if not TABLE_VI.exists():
        print(f"Table VI file not found: {TABLE_VI}")
        return data
    with open(TABLE_VI) as f:
        reader = csv.DictReader(f)
        for row in reader:
            key = row.get("Application", "").strip().lower().replace(" ", "")
            if key:
                data[key] = row
    return data

def parse_int(s: str) -> Optional[int]:
    if not s or s == "?":
        return None
    s = s.replace(",", "").replace("~", "").strip()
    try:
        return int(s)
    except ValueError:
        return None

def parse_float(s: str) -> Optional[float]:
    if not s or s == "?":
        return None
    try:
        return float(s)
    except ValueError:
        return None

def load_results(csv_path: str) -> Dict[str, dict]:
    results = {}
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            bench = row.get("benchmark", "").strip().lower()
            results[bench] = row
    return results

def main():
    if len(sys.argv) < 2:
        print("Usage: compare_table_vi.py <results.csv>")
        sys.exit(1)

    vi = load_table_vi()
    results = load_results(sys.argv[1])

    print(f"{'Benchmark':<20s} {'Metric':<12s} {'Measured':>12s} {'Table VI':>12s} {'Match':>8s}")
    print(f"{'-'*64}")

    for bench_key in sorted(results.keys()):
        r = results[bench_key]
        vi_key = bench_key.replace("high", "").replace("low", "").replace("-", "").strip()
        row = vi.get(vi_key) or vi.get(bench_key)
        if not row:
            continue

        print(f"\n{bench_key:<20s} {'':12s} {'':>12s} {'':>12s} {'':>8s}")

        pairs = [
            ("Commits",    parse_int(r.get("commits", "")),    parse_int(row.get("Transactions (%)", row.get("Transactions", "")))),
            ("AvgReads",   parse_float(r.get("avg_reads", "")), parse_float(row.get("Read Set (90 pctile)", row.get("Read Set", "")))),
            ("AvgWrites",  parse_float(r.get("avg_writes", "")),parse_float(row.get("Write Set (90 pctile)", row.get("Write Set", "")))),
            ("TotReads",   parse_int(r.get("total_reads", "")),parse_int(row.get("Read Barrier (90 pctile)", row.get("Read Barriers", "")))),
            ("TotWrites",  parse_int(r.get("total_writes", "")),parse_int(row.get("Write Barrier (90 pctile)", row.get("Write Barriers", "")))),
            ("Aborts",     parse_int(r.get("aborts", "")),      None),
        ]

        for name, meas, tab in pairs:
            if meas is None and tab is None:
                continue
            meas_s = f"{meas:>12d}" if isinstance(meas, int) else f"{meas:>12.1f}" if meas else f"{'N/A':>12s}"
            tab_s = f"{tab:>12d}" if isinstance(tab, int) else f"{tab:>12.1f}" if tab else f"{'N/A':>12s}"
            if meas and tab and tab > 0:
                ratio = meas / tab
                match = f"{'OK' if 0.5 <= ratio <= 2.0 else 'DIFF':>8s}"
            else:
                match = f"{'':>8s}"
            print(f"{'':20s} {name:<12s} {meas_s} {tab_s} {match}")

if __name__ == "__main__":
    main()
