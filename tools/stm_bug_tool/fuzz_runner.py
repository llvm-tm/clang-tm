#!/usr/bin/env python3
"""
fuzz_runner.py — Fuzzing orchestration for STM backends.

Builds fuzz benchmarks with -DTM_EVENT_LOG for each backend, runs them
with random parameters, captures event logs, and runs invariant checks.

Usage:
    python3 fuzz_runner.py --backends all --runs 10
    python3 fuzz_runner.py --backends tl2,tinystm --runs 100 --verbose
    python3 fuzz_runner.py --benchmark counter --backends tl2 --runs 5
"""

import argparse
import os
import random
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from event_parser import parse_event_log
from invariant_checker import check_all

# ── Backend definitions ──────────────────────────────────────────────

ROOT = Path(__file__).resolve().parent.parent.parent  # /home/.../clang-tm-dev

# ── STMbench7 definitions ──────────────────────────────────────────
# These use the LLVM plugin pipeline (not direct-compiled).
STM7_BACKENDS = {
    "tinystm": {
        "make_target": "stmbench_tinystm_wbctl",
        "defines_var": "TM_DEFINES_tinystm_wbctl",
        "defines": "-DDESIGN_WBCTL -DTM_EVENT_LOG",
    },
    "wt": {
        "make_target": "stmbench_tinystm_wt",
        "defines_var": "TM_DEFINES_tinystm_wt",
        "defines": "-DDESIGN_WT -DTM_EVENT_LOG",
    },
    "wbetl": {
        "make_target": "stmbench_tinystm_wbetl",
        "defines_var": "TM_DEFINES_tinystm_wbetl",
        "defines": "-DDESIGN_WBETL -DTM_EVENT_LOG",
    },
}

BACKENDS = {
    "tl2": {
        "runtime": str(ROOT / "backends" / "runtimes" / "tl2_runtime.cpp"),
        "include": str(ROOT / "backends" / "TL2"),
        "define": "-DTM_BACKEND_TL2",
    },
    "tinystm": {
        "runtime": str(ROOT / "backends" / "runtimes" / "TinySTM_runtime.cpp"),
        "include": str(ROOT / "backends" / "TinySTM"),
        "define": "-DDESIGN_WBCTL -DTM_BACKEND_TINYSTM",
    },
    "wt": {
        "runtime": str(ROOT / "backends" / "runtimes" / "TinySTM_runtime.cpp"),
        "include": str(ROOT / "backends" / "TinySTM"),
        "define": "-DDESIGN_WT -DTM_BACKEND_TINYSTM",
    },
    "wbetl": {
        "runtime": str(ROOT / "backends" / "runtimes" / "TinySTM_runtime.cpp"),
        "include": str(ROOT / "backends" / "TinySTM"),
        "define": "-DDESIGN_WBETL -DTM_BACKEND_TINYSTM",
    },
    "norec": {
        "runtime": str(ROOT / "backends" / "runtimes" / "NOrec_runtime.cpp"),
        "include": str(ROOT / "backends" / "NOrec"),
        "define": "-DTM_BACKEND_NOREC",
    },
    "swisstm": {
        "runtime": str(ROOT / "backends" / "runtimes" / "SwissTM_runtime.cpp"),
        "include": str(ROOT / "backends" / "SwissTM"),
        "define": "-DTM_BACKEND_SWISSTM",
    },
}


def build_fuzz_benchmark(backend: str, bench_name: str) -> str:
    """Build a fuzz benchmark binary. Returns path to binary."""
    info = BACKENDS[backend]

    bin_dir = ROOT / "tools" / "stm_bug_tool" / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)

    binary = bin_dir / f"fuzz_{bench_name}_{backend}"

    src = ROOT / "tools" / "stm_bug_tool" / "benchmarks" / f"fuzz_{bench_name}.cpp"
    runtime = info["runtime"]
    inc = f"-I{ROOT} -I{ROOT / 'backends'}"
    backend_inc = f"-I{info['include']}"
    event_define = "-DTM_EVENT_LOG"
    cxxflags = f"-std=c++20 -O0 -pthread -g {inc} {backend_inc} {info['define']} {event_define}"
    cmd = f"clang++ {cxxflags} -o {binary} {src} {runtime} -lpthread -ldl"

    print(f"  BUILD: {cmd}", file=sys.stderr)
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"  BUILD FAILED: {result.stderr}", file=sys.stderr)
        return None

    return str(binary)


# ── STMbench7 helpers ──────────────────────────────────────────────

STM7_DIR = ROOT / "benchmarks" / "STMbench7"
STM7_BIN_DIR = STM7_DIR / "bin"


def build_stmbench7(backend: str) -> str:
    """Build STMbench7 binary via make with event logging enabled."""
    info = STM7_BACKENDS.get(backend)
    if not info:
        print(f"  No STMbench7 build config for backend '{backend}'", file=sys.stderr)
        return None

    target = info["make_target"]
    defines_var = info["defines_var"]
    defines = info["defines"]

    binary = STM7_BIN_DIR / target

    cmd = (f"make -C {STM7_DIR} {target} "
           f"{defines_var}=\"{defines}\" "
           f"2>&1")

    print(f"  BUILD: make {target} {defines_var}=\"{defines}\"", file=sys.stderr)
    result = subprocess.run(
        f"make -C {STM7_DIR} {target} "
        f"{defines_var}=\"{defines}\"",
        shell=True, capture_output=True, text=True, timeout=300
    )

    if result.returncode != 0:
        # Print last 30 lines of build output
        lines = result.stdout.splitlines() + result.stderr.splitlines()
        print(f"  BUILD FAILED (exit={result.returncode}):", file=sys.stderr)
        for line in lines[-30:]:
            print(f"    {line}", file=sys.stderr)
        return None

    if not binary.exists():
        print(f"  BUILD: binary not found at {binary}", file=sys.stderr)
        return None

    return str(binary)


def run_stmbench7(binary: str, threads: int, duration_ms: int,
                  workload: int = 1, timeout: int = 300) -> dict:
    """Run STMbench7 and capture output/events."""
    cmd = f"{binary} -t {threads} -d {duration_ms} -w {workload}"

    start = time.time()
    result = subprocess.run(
        cmd, shell=True, capture_output=True, text=True, timeout=timeout
    )
    elapsed = time.time() - start

    output = {
        "stdout": result.stdout,
        "stderr": result.stderr if result.stderr else "",
        "returncode": result.returncode,
        "elapsed": elapsed,
    }

    if "SIGSEGV" in result.stderr:
        output["crash"] = True
    elif result.returncode != 0:
        output["crash"] = False
        output["invariant_result"] = f"exit code {result.returncode}"
    else:
        output["crash"] = False
        # Check for expected output pattern
        if "Total ops" in result.stdout:
            output["invariant_result"] = "PASS (ops reported)"
        else:
            output["invariant_result"] = "FAIL (no ops in output)"

    return output


def generate_stm7_params(runs: int, seed: int = 0) -> list:
    """Generate random STMbench7 parameter sets."""
    rng = random.Random(seed)
    param_sets = []
    for _ in range(runs):
        threads = rng.choice([1, 2, 4])
        duration = rng.choice([2000, 5000])
        workload = rng.choice([1, 2, 3])
        param_sets.append({
            "threads": threads,
            "duration_ms": duration,
            "workload": workload,
        })
    return param_sets


def run_fuzz(binary: str, threads: int, iters: int,
             counters: int, seed: int, timeout: int = 60) -> dict:
    """Run a fuzz binary and return {stdout, stderr, returncode, elapsed}."""
    cmd = f"{binary} {threads} {iters} {counters} {seed}"

    start = time.time()
    result = subprocess.run(
        cmd, shell=True, capture_output=True, text=True, timeout=timeout
    )
    elapsed = time.time() - start

    output = {
        "stdout": result.stdout,
        "stderr": result.stderr if result.stderr else "",
        "returncode": result.returncode,
        "elapsed": elapsed,
    }

    if "SIGSEGV" in result.stderr:
        output["crash"] = True
    else:
        output["crash"] = False

    if "INVARIANT:" in result.stdout:
        for line in result.stdout.splitlines():
            if "INVARIANT:" in line:
                output["invariant_result"] = line.strip()

    return output


def generate_params(runs: int, seed: int = 0, benchmark: str = "counter") -> list:
    """Generate random fuzz parameter sets.

    For 'alloc' benchmark: uses larger key_range (counters parameter)
    to ensure the linked list exercises multiple nodes.
    """
    rng = random.Random(seed)
    param_sets = []

    is_alloc = (benchmark == "alloc")

    for _ in range(runs):
        threads = rng.choice([1, 2, 3, 4, 6, 8])
        iters = rng.choice([100, 500, 1000, 2000, 5000])
        if is_alloc:
            counters = rng.choice([16, 32, 64, 128])
        else:
            counters = rng.choice([1, 2, 4, 8, 16])
        param_seed = rng.randint(0, 1000000)
        param_sets.append({
            "threads": threads,
            "iters": iters,
            "counters": counters,
            "seed": param_seed,
        })

    return param_sets


def run_fuzz_loop(backend: str, binary: str, params: list,
                  verbose: bool = False) -> dict:
    """Run fuzz loop for a single backend with multiple param sets."""
    results = {
        "pass": 0,
        "fail_invariant": 0,
        "fail_crash": 0,
        "fail_timeout": 0,
        "total": 0,
        "violations": [],
        "crashes": [],
    }

    for p in params:
        results["total"] += 1

        try:
            output = run_fuzz(binary, p["threads"], p["iters"],
                              p["counters"], p["seed"])
        except subprocess.TimeoutExpired:
            results["fail_timeout"] += 1
            if verbose:
                print(f"  TIMEOUT: threads={p['threads']} iters={p['iters']} "
                      f"counters={p['counters']}", file=sys.stderr)
            continue

        if output["crash"]:
            results["fail_crash"] += 1
            parsed = parse_event_log(output["stderr"])
            inv_result = check_all(parsed)
            results["crashes"].append({
                "params": p,
                "stderr": output["stderr"][-2000:],
                "invariant_result": inv_result,
            })
            if verbose:
                print(f"  CRASH: threads={p['threads']} iters={p['iters']} "
                      f"counters={p['counters']}", file=sys.stderr)
                if inv_result["violations"]:
                    for v in inv_result["violations"]:
                        print(f"    [{v['severity']}] {v['invariant']}: "
                              f"{v['description'][:120]}",
                              file=sys.stderr)
            continue

        if output["returncode"] != 0:
            results["fail_invariant"] += 1
            parsed = parse_event_log(output["stderr"])
            inv_result = check_all(parsed)

            results["violations"].append({
                "params": p,
                "invariant_result": inv_result,
                "stdout": output["stdout"],
            })
            if verbose:
                print(f"  INVARIANT FAIL: threads={p['threads']} iters={p['iters']} "
                      f"counters={p['counters']}: {output.get('invariant_result', '?')}",
                      file=sys.stderr)
                if inv_result["violations"]:
                    for v in inv_result["violations"][:3]:
                        print(f"    [{v['severity']}] {v['invariant']}: "
                              f"{v['description'][:120]}",
                              file=sys.stderr)
            continue

        results["pass"] += 1

    return results


def main():
    parser = argparse.ArgumentParser(
        description="STM Bug Detection via Fuzzing + Invariant Checking"
    )
    parser.add_argument(
        "--backends", default="all",
        help="Comma-separated list, or 'all' (default: all)"
    )
    parser.add_argument(
        "--benchmark", default="counter",
        help="Benchmark name (fuzz_<name>.cpp, default: counter)"
    )
    parser.add_argument("--runs", type=int, default=20,
                        help="Number of fuzz parameter sets per backend")
    parser.add_argument("--seed", type=int, default=0,
                        help="Random seed for fuzz parameter generation")
    parser.add_argument("--verbose", action="store_true",
                        help="Print per-run details")
    parser.add_argument("--timeout", type=int, default=120,
                        help="Per-run timeout in seconds")
    args = parser.parse_args()

    is_stm7 = args.benchmark == "stmbench7"

    if is_stm7:
        stm7_backends = list(STM7_BACKENDS.keys())
        if args.backends == "all":
            backends = stm7_backends
        else:
            backends = [b.strip() for b in args.backends.split(",")]
            for b in backends:
                if b not in stm7_backends and b not in BACKENDS:
                    print(f"Unknown backend: {b}", file=sys.stderr)
                    sys.exit(1)
                if b not in stm7_backends:
                    print(f"Backend '{b}' not supported for stmbench7 (no LLVM pipeline config)", file=sys.stderr)
                    sys.exit(1)
    else:
        if args.backends == "all":
            backends = list(BACKENDS.keys())
        else:
            backends = [b.strip() for b in args.backends.split(",")]
            for b in backends:
                if b not in BACKENDS:
                    print(f"Unknown backend: {b}", file=sys.stderr)
                    sys.exit(1)

    if is_stm7:
        params = generate_stm7_params(args.runs, args.seed)
    else:
        params = generate_params(args.runs, args.seed, args.benchmark)

    print(f"Fuzz plan: {len(params)} runs x {len(backends)} backends = "
          f"{len(params) * len(backends)} total executions", file=sys.stderr)

    overall = {
        "pass": 0,
        "fail_invariant": 0,
        "fail_crash": 0,
        "fail_timeout": 0,
        "total": 0,
    }

    for backend in backends:
        print(f"\n{'=' * 60}", file=sys.stderr)
        print(f"Backend: {backend}", file=sys.stderr)
        print(f"{'=' * 60}", file=sys.stderr)

        if is_stm7:
            binary = build_stmbench7(backend)
        else:
            binary = build_fuzz_benchmark(backend, args.benchmark)

        if binary is None:
            print(f"  SKIP (build failed)", file=sys.stderr)
            continue

        if is_stm7:
            results = {"pass": 0, "fail_invariant": 0, "fail_crash": 0,
                       "fail_timeout": 0, "total": 0,
                       "violations": [], "crashes": []}
            for p in params:
                results["total"] += 1
                try:
                    output = run_stmbench7(
                        binary, p["threads"], p["duration_ms"],
                        p["workload"], timeout=args.timeout
                    )
                except subprocess.TimeoutExpired:
                    results["fail_timeout"] += 1
                    continue

                if output["crash"]:
                    results["fail_crash"] += 1
                    parsed = parse_event_log(output["stderr"])
                    inv_result = check_all(parsed)
                    results["crashes"].append({
                        "params": p,
                        "stderr": output["stderr"][-2000:],
                        "invariant_result": inv_result,
                    })
                    if args.verbose:
                        print(f"  CRASH: threads={p['threads']} "
                              f"duration={p['duration_ms']}ms",
                              file=sys.stderr)
                        if inv_result["violations"]:
                            for v in inv_result["violations"]:
                                print(f"    [{v['severity']}] {v['invariant']}: "
                                      f"{v['description'][:120]}",
                                      file=sys.stderr)
                    continue

                if output["returncode"] != 0:
                    results["fail_invariant"] += 1
                    parsed = parse_event_log(output["stderr"])
                    inv_result = check_all(parsed)
                    results["violations"].append({
                        "params": p,
                        "invariant_result": inv_result,
                        "stdout": output["stdout"],
                    })
                    continue

                results["pass"] += 1
        else:
            results = run_fuzz_loop(backend, binary, params, args.verbose)

        for k in ("pass", "fail_invariant", "fail_crash", "fail_timeout", "total"):
            overall[k] += results[k]

        print(f"\n  Results for {backend}:", file=sys.stderr)
        print(f"    PASS:               {results['pass']}", file=sys.stderr)
        print(f"    FAIL (invariant):   {results['fail_invariant']}", file=sys.stderr)
        print(f"    FAIL (crash):       {results['fail_crash']}", file=sys.stderr)
        print(f"    FAIL (timeout):     {results['fail_timeout']}", file=sys.stderr)
        print(f"    Total:              {results['total']}", file=sys.stderr)

        if results["crashes"] and args.verbose:
            c = results["crashes"][0]
            label = f"threads={c['params']['threads']}"
            if "iters" in c["params"]:
                label += f" iters={c['params']['iters']}"
            if "duration_ms" in c["params"]:
                label += f" duration={c['params']['duration_ms']}ms"
            print(f"\n  === First crash details ===", file=sys.stderr)
            print(f"  params: {label}", file=sys.stderr)
            inv = c.get("invariant_result", {})
            if inv.get("violations"):
                print(f"  Invariant violations:", file=sys.stderr)
                for v in inv["violations"][:5]:
                    print(f"    [{v['severity']}] {v['invariant']}: "
                          f"{v['description'][:150]}", file=sys.stderr)
            print(f"  stderr (last 20 lines):", file=sys.stderr)
            for line in c["stderr"].splitlines()[-20:]:
                print(f"    {line}", file=sys.stderr)

    print(f"\n{'=' * 60}", file=sys.stderr)
    print(f"OVERALL SUMMARY", file=sys.stderr)
    print(f"{'=' * 60}", file=sys.stderr)
    print(f"  PASS:               {overall['pass']}", file=sys.stderr)
    print(f"  FAIL (invariant):   {overall['fail_invariant']}", file=sys.stderr)
    print(f"  FAIL (crash):       {overall['fail_crash']}", file=sys.stderr)
    print(f"  FAIL (timeout):     {overall['fail_timeout']}", file=sys.stderr)
    print(f"  Total:              {overall['total']}", file=sys.stderr)

    if overall["fail_crash"] > 0:
        print(f"\n⚠  {overall['fail_crash']} crashes detected. "
              f"Run with --verbose for event log details.", file=sys.stderr)

    if overall["fail_invariant"] > 0:
        print(f"\n⚠  {overall['fail_invariant']} invariant violations detected.",
              file=sys.stderr)

    return 0 if overall["fail_crash"] == 0 and overall["fail_invariant"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
