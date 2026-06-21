#!/usr/bin/env python3
"""
TSX Profiling Experiment Runner

1. Builds TSXSGL/SPHT backends with TSX_PROFILE instrumentation
2. Runs fuzz_counter, bank, and STAMP benchmarks
3. Collects TSX_STATS timing lines and benchmark output
4. Stores results in patches/profile/tsx/results/
5. Generates calibration data for the simulator cost model
"""

import csv
import json
import os
import platform
import re
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# Git-ignored raw profiling data (large CSVs, calibration JSONs)
PROFILING_RAW = REPO_ROOT / "profiling_data" / "raw"
PROFILING_CALIB = REPO_ROOT / "profiling_data" / "calibration"

# Tracked machine profiles (small curated JSON files, one per CPU model)
MACHINE_PROFILES = REPO_ROOT / "machine_profiles"

# Backend + benchmark matrix
BENCHMARKS = {
    "fuzz_counter": {
        "variants": ["1t", "2t", "4t", "8t"],
        "cmd": "./fuzz_counter -d 5000 -a 64 -t {threads}",
    },
    "bank": {
        "variants": ["1t", "2t", "4t", "8t"],
        "cmd": "./bank -d 3000 -a 128 -t {threads}",
    },
    "fuzz_counter_high": {
        "variants": ["4t"],
        "cmd": "./fuzz_counter -d 5000 -a 8 -t 4",
        "desc": "high contention (8 accounts)",
    },
    "fuzz_counter_low": {
        "variants": ["4t"],
        "cmd": "./fuzz_counter -d 5000 -a 1024 -t 4",
        "desc": "low contention (1024 accounts)",
    },
}

# TSX_STATS regex
TSX_STATS_RE = re.compile(
    r"TSX_STATS:"
    r"\s+xbegin_ok=(\d+)\((\d+)\)"
    r"\s+xbegin_abort=(\d+)\((\d+)\)"
    r"\s+xend=(\d+)\((\d+)\)"
    r"\s+xabort=(\d+)\((\d+)\)"
    r"\s+sgl_begin=(\d+)\((\d+)\)"
    r"\s+sgl_end=(\d+)\((\d+)\)"
    r"\s+sgl_spin=(\d+)\((\d+)\)"
    r"\s+read=(\d+)\((\d+)\)"
    r"\s+write=(\d+)\((\d+)\)"
    r"\s+depth=(\d+)\((\d+)\)"
    r"\s+conflict=(\d+)\s+capacity=(\d+)\s+explicit=(\d+)\s+other=(\d+)"
)

# Benchmark timing regex
TIME_RE = re.compile(r"(?:Time|time|Duration|wall)\s*[=:]\s*([\d.]+)\s*(s|ms|us|ns|sec)", re.I)


@dataclass
class TsxStats:
    xbegin_ok: Tuple[int, int] = (0, 0)
    xbegin_abort: Tuple[int, int] = (0, 0)
    xend: Tuple[int, int] = (0, 0)
    xabort: Tuple[int, int] = (0, 0)
    sgl_begin: Tuple[int, int] = (0, 0)
    sgl_end: Tuple[int, int] = (0, 0)
    sgl_spin: Tuple[int, int] = (0, 0)
    read: Tuple[int, int] = (0, 0)
    write: Tuple[int, int] = (0, 0)
    depth: Tuple[int, int] = (0, 0)
    conflict: int = 0
    capacity: int = 0
    explicit: int = 0
    other: int = 0

    @staticmethod
    def from_match(m) -> "TsxStats":
        def pair(cnt_idx, acc_idx):
            return (int(m.group(cnt_idx)), int(m.group(acc_idx)))
        return TsxStats(
            xbegin_ok=pair(1, 2),
            xbegin_abort=pair(3, 4),
            xend=pair(5, 6),
            xabort=pair(7, 8),
            sgl_begin=pair(9, 10),
            sgl_end=pair(11, 12),
            sgl_spin=pair(13, 14),
            read=pair(15, 16),
            write=pair(17, 18),
            depth=pair(19, 20),
            conflict=int(m.group(21)),
            capacity=int(m.group(22)),
            explicit=int(m.group(23)),
            other=int(m.group(24)),
        )

    def avg(self, cnt, acc) -> float:
        return acc / cnt if cnt > 0 else 0.0

    def to_calibration_dict(self) -> dict:
        return {
            "xbegin_ok_cycles": self.avg(*self.xbegin_ok),
            "xbegin_abort_cycles": self.avg(*self.xbegin_abort),
            "xend_cycles": self.avg(*self.xend),
            "xabort_cycles": self.avg(*self.xabort),
            "sgl_begin_cycles": self.avg(*self.sgl_begin),
            "sgl_end_cycles": self.avg(*self.sgl_end),
            "sgl_spin_cycles": self.avg(*self.sgl_spin),
            "read_cycles": self.avg(*self.read),
            "write_cycles": self.avg(*self.write),
            "depth_cycles": self.avg(*self.depth),
            "conflict_aborts": self.conflict,
            "capacity_aborts": self.capacity,
            "explicit_aborts": self.explicit,
            "other_aborts": self.other,
            "txn_commits": self.xend[0],
            "txn_aborts": self.xbegin_abort[0],
            "total_reads": self.read[0],
            "total_writes": self.write[0],
        }

    def __str__(self) -> str:
        d = self.to_calibration_dict()
        parts = [f"commits={d['txn_commits']} aborts={d['txn_aborts']}"]
        for k in ["xbegin_ok_cycles", "xend_cycles", "read_cycles", "write_cycles", "xabort_cycles"]:
            parts.append(f"{k}={d[k]:.1f}")
        parts.append(f"conflict={d['conflict_aborts']} capacity={d['capacity_aborts']} explicit={d['explicit_aborts']}")
        return " | ".join(parts)


def parse_tsx_stats(output: str) -> Optional[TsxStats]:
    for line in output.splitlines():
        m = TSX_STATS_RE.search(line)
        if m:
            return TsxStats.from_match(m)
    return None


def parse_time(output: str) -> Optional[float]:
    for line in output.splitlines():
        m = TIME_RE.search(line)
        if m:
            val = float(m.group(1))
            unit = m.group(2).lower()
            if unit in ("ms",):
                return val / 1000.0
            elif unit in ("us",):
                return val / 1_000_000.0
            elif unit in ("ns",):
                return val / 1_000_000_000.0
            return val
    return None


def run_benchmark(backend_dir: Path, bench_cmd: str, env: dict) -> Tuple[str, float, Optional[TsxStats]]:
    """Run a benchmark and capture output, wall time, and TSX stats."""
    start = time.time()
    try:
        result = subprocess.run(
            bench_cmd.split(),
            cwd=str(backend_dir),
            capture_output=True,
            text=True,
            timeout=300,
            env={**os.environ, **env},
        )
        elapsed = time.time() - start
        output = result.stdout + result.stderr
        tsx_stats = parse_tsx_stats(output)
        bench_time = parse_time(output) or elapsed
        return output, bench_time, tsx_stats
    except subprocess.TimeoutExpired:
        return "", 0.0, None
    except Exception as e:
        print(f"  ERROR: {e}", file=sys.stderr)
        return "", 0.0, None


def run_experiments(backend: str, backend_dir: Path, env_add: dict):
    """Run all benchmarks for a single backend."""
    print(f"\n{'='*60}")
    print(f"Backend: {backend} ({backend_dir})")
    print(f"{'='*60}")

    results = []

    for bench_name, bench_info in BENCHMARKS.items():
        for variant in bench_info["variants"]:
            threads = variant.replace("t", "")
            cmd = bench_info["cmd"].format(threads=threads)
            desc = bench_info.get("desc", "")

            print(f"\n  [{bench_name} {variant}] {desc}")
            print(f"    $ {cmd}")

            output, wall_time, tsx_stats = run_benchmark(
                backend_dir, cmd, env_add
            )

            if tsx_stats:
                print(f"    OK: wall={wall_time:.3f}s stats=[{tsx_stats}]")
                results.append({
                    "backend": backend,
                    "benchmark": bench_name,
                    "variant": variant,
                    "threads": int(threads),
                    "wall_time_s": wall_time,
                    "description": desc,
                    **tsx_stats.to_calibration_dict(),
                })
            else:
                print(f"    NO TSX_STATS (wall={wall_time:.3f}s)")
                # Print last few lines of output for debugging
                for line in output.splitlines()[-5:]:
                    print(f"      {line.strip()}")

    return results


def write_results_csv(results: List[dict], path: Path):
    """Write results to CSV."""
    if not results:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(results[0].keys())
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)
    print(f"\nResults written to {path}")


def generate_calibration(results: List[dict], path: Path):
    """Generate calibration JSON for the cost model from aggregated results."""
    if not results:
        return
    path.parent.mkdir(parents=True, exist_ok=True)

    # Aggregate by benchmark, weighted by transaction count
    by_bench = defaultdict(list)
    for r in results:
        by_bench[r["benchmark"]].append(r)

    calibration = {}
    for bench, items in by_bench.items():
        total_commits = sum(it["txn_commits"] for it in items)
        total_reads = sum(it["total_reads"] for it in items)
        total_writes = sum(it["total_writes"] for it in items)

        if total_commits == 0:
            continue

        # Weighted averages
        weights = [it["txn_commits"] for it in items]
        total_w = sum(weights)
        if total_w == 0:
            continue

        avg = lambda key: sum(it[key] * w for it, w in zip(items, weights)) / total_w

        calibration[bench] = {
            "xbegin_ok_cycles": avg("xbegin_ok_cycles"),
            "xend_cycles": avg("xend_cycles"),
            "xabort_cycles": avg("xabort_cycles"),
            "read_cycles": avg("read_cycles"),
            "write_cycles": avg("write_cycles"),
            "sgl_begin_cycles": avg("sgl_begin_cycles"),
            "sgl_end_cycles": avg("sgl_end_cycles"),
            "abort_rate_pct": 100.0 * avg("txn_aborts") / (total_commits + sum(it["txn_aborts"] for it in items)),
            "avg_reads_per_tx": total_reads / total_commits if total_commits > 0 else 0,
            "avg_writes_per_tx": total_writes / total_commits if total_commits > 0 else 0,
            "capacity_abort_pct": 100.0 * avg("capacity_aborts") / avg("txn_aborts") if avg("txn_aborts") > 0 else 0,
            "conflict_abort_pct": 100.0 * avg("conflict_aborts") / avg("txn_aborts") if avg("txn_aborts") > 0 else 0,
            "samples": len(items),
        }

    import json
    with open(path, "w") as f:
        json.dump(calibration, f, indent=2)
    print(f"Calibration written to {path}")


def generate_machine_profile(results: List[dict], path: Path):
    """Generate a portable machine profile JSON from profiling results.

    This file can be consumed by the simulator on any machine — it
    contains only hardware characteristics, not workload-specific data.
    """
    if not results:
        return
    path.parent.mkdir(parents=True, exist_ok=True)

    # Aggregate cycle costs across all benchmarks
    total_commits = sum(r["txn_commits"] for r in results)
    if total_commits == 0:
        return

    def weighted_avg(key):
        total = sum(r[key] * r["txn_commits"] for r in results)
        return total / total_commits

    # Detect CPU info
    cpu = platform.processor() or "unknown"
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu = line.split(":")[1].strip()
                    break
    except (IOError, FileNotFoundError):
        pass

    # Estimate frequency from /proc/cpuinfo
    freq_ghz = 3.0
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("cpu MHz"):
                    mhz = float(line.split(":")[1].strip())
                    freq_ghz = mhz / 1000.0
                    break
    except (IOError, FileNotFoundError, ValueError):
        pass

    profile = {
        "cpu": cpu,
        "freq_ghz": round(freq_ghz, 2),
        "tsx": {
            "xbegin_cycles": weighted_avg("xbegin_ok_cycles"),
            "xend_cycles": weighted_avg("xend_cycles"),
            "xabort_cycles": weighted_avg("xabort_cycles"),
            "read_l1_cycles": weighted_avg("read_cycles"),
            "write_l1_cycles": weighted_avg("write_cycles"),
            "bloom_check_cycles": 2.0,
            "mutex_lock_cycles": weighted_avg("sgl_begin_cycles"),
            "mutex_unlock_cycles": weighted_avg("sgl_end_cycles"),
            "conflict_abort_penalty": 2000.0,
            "cache_line_size": 64,
            "max_read_lines": 512,
            "max_write_lines": 128,
        },
        "memory": {
            "l1_hit_cycles": 4.0,
            "l2_hit_cycles": 12.0,
            "l3_hit_cycles": 40.0,
            "ram_cycles": 200.0,
        },
        "backends": [
            {
                "backend": "default",
                "begin_overhead": weighted_avg("xbegin_ok_cycles"),
                "commit_overhead": weighted_avg("xend_cycles"),
                "abort_overhead": weighted_avg("xabort_cycles"),
                "read_overhead": 2.0,
                "write_overhead": 2.0,
                "validation_entry_cost": 3.0,
                "lock_acquire_cost": weighted_avg("sgl_begin_cycles"),
            },
            {
                "backend": "tsxsgl",
                "begin_overhead": weighted_avg("xbegin_ok_cycles"),
                "commit_overhead": weighted_avg("xend_cycles"),
                "abort_overhead": weighted_avg("xabort_cycles"),
                "read_overhead": weighted_avg("read_cycles") - weighted_avg("xbegin_ok_cycles"),
                "write_overhead": weighted_avg("write_cycles") - weighted_avg("xbegin_ok_cycles"),
                "validation_entry_cost": 0.0,
                "lock_acquire_cost": weighted_avg("sgl_begin_cycles"),
            },
            {
                "backend": "spht",
                "begin_overhead": weighted_avg("xbegin_ok_cycles"),
                "commit_overhead": weighted_avg("xend_cycles"),
                "abort_overhead": weighted_avg("xabort_cycles"),
                "read_overhead": weighted_avg("read_cycles") - weighted_avg("xbegin_ok_cycles"),
                "write_overhead": weighted_avg("write_cycles") - weighted_avg("xbegin_ok_cycles"),
                "validation_entry_cost": 0.0,
                "lock_acquire_cost": weighted_avg("sgl_begin_cycles"),
            },
        ],
        "collected": datetime.now().isoformat(),
        "description": f"Auto-generated from {len(results)} profiling runs on {cpu}",
    }

    with open(path, "w") as f:
        json.dump(profile, f, indent=2)
    print(f"Machine profile written to {path} (CPU: {cpu}, freq: {freq_ghz:.2f} GHz)")


def main():
    PROFILING_RAW.mkdir(parents=True, exist_ok=True)
    PROFILING_CALIB.mkdir(parents=True, exist_ok=True)
    MACHINE_PROFILES.mkdir(parents=True, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    # Backend build dirs
    # These are the C++ explicit-API builds in build/
    backends = {
        "tsxsgl": {
            "dir": REPO_ROOT / "build" / "tsxsgl",
            "env": {"TSX_PROFILE": "1"},
        },
        "spht": {
            "dir": REPO_ROOT / "build" / "spht",
            "env": {"TSX_PROFILE": "1"},
        },
    }

    all_results = []

    for backend_name, info in backends.items():
        if not info["dir"].exists():
            print(f"WARNING: {info['dir']} does not exist — skipping {backend_name}", file=sys.stderr)
            print(f"  Build first: cd {REPO_ROOT} && make tsxsgl && make spht")
            continue

        results = run_experiments(backend_name, info["dir"], info["env"])
        all_results.extend(results)

    # Write combined results
    if all_results:
        csv_path = PROFILING_RAW / f"tsx_profile_{timestamp}.csv"
        calib_path = PROFILING_CALIB / f"calibration_{timestamp}.json"
        machine_path = MACHINE_PROFILES / f"machine_profile_{timestamp}.json"
        write_results_csv(all_results, csv_path)
        generate_calibration(all_results, calib_path)
        generate_machine_profile(all_results, machine_path)
    else:
        print("\nNo results collected. Build the TSXSGL/SPHT backends first:")
        print("  cd build/tsxsgl && make -j")
        print("  cd build/spht && make -j")

    print("\n=== Output ===")
    print(f"  Raw results:    {PROFILING_RAW}/")
    print(f"  Calibration:    {PROFILING_CALIB}/")
    print(f"  Machine config: {MACHINE_PROFILES}/")
    print("\nDone.")


if __name__ == "__main__":
    main()
