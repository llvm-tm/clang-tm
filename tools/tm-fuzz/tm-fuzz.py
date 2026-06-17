#!/usr/bin/env python3
"""
tm-fuzz.py — Generic TM Fuzz Driver (Phase 5 of fuzz tool plan)

Pipeline:
  1. (Optional) tm-fuzz-strategy pass → annotated bitcode
  2. Instrument with TM backend (opt passes)
  3. Build binary
  4. Run with multiple thread counts, collect traces
  5. Check invariants (sequential baseline / user oracle)
  6. Report results

Usage:
  # Basic: fuzz a bitcode file with TINYSTM
  python3 tm-fuzz.py --app myapp.bc --backend TINYSTM

  # With strategy pass and sampling
  python3 tm-fuzz.py --app myapp.bc --strategy auto \\
      --sample-rate 10 --threads 2,4,8

  # With trace output and invariant checking
  python3 tm-fuzz.py --app myapp.bc --trace \\
      --baseline baseline.txt --duration 30

  # All backends
  python3 tm-fuzz.py --app myapp.bc --backend all

For more details, see docs/fuzz-tool-plan.md
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ── Paths ──────────────────────────────────────────────────────────
ROOT = Path(__file__).resolve().parent.parent.parent
PLUGIN_DIR = ROOT / "plugin"
BIN_DIR = PLUGIN_DIR / "bin"
RUNTIME_DIR = PLUGIN_DIR / "runtime"

# Available backends for fuzzing
BACKENDS = {
    "TINYSTM": {
        "runtime": str(ROOT / "backends" / "tm_impl" / "tiny_stm" / "TinySTM_runtime.cpp"),
        "include": str(ROOT / "backends" / "tm_impl" / "tiny_stm"),
        "define": "-DDESIGN_WBCTL -DLLVM_TM_PLUGIN",
    },
    "NOREC": {
        "runtime": str(ROOT / "backends" / "tm_impl" / "norec" / "NOrec_runtime.cpp"),
        "include": str(ROOT / "backends" / "tm_impl" / "norec"),
        "define": "-DTM_BACKEND_NOREC -DLLVM_TM_PLUGIN",
    },
    "TL2": {
        "runtime": str(ROOT / "backends" / "tm_impl" / "tl2" / "TL2_runtime.cpp"),
        "include": str(ROOT / "backends" / "tm_impl" / "tl2"),
        "define": "-DTM_BACKEND_TL2 -DLLVM_TM_PLUGIN",
    },
    "SWISSTM": {
        "runtime": str(ROOT / "backends" / "tm_impl" / "swisstm" / "SwissTM_runtime.cpp"),
        "include": str(ROOT / "backends" / "tm_impl" / "swisstm"),
        "define": "-DTM_BACKEND_SWISSTM -DLLVM_TM_PLUGIN",
    },
    "SGL": {
        "runtime": str(ROOT / "backends" / "tm_impl" / "single_global_lock" / "SingleGlobalLock_runtime.cpp"),
        "include": str(ROOT / "backends" / "tm_impl" / "single_global_lock"),
        "define": "-DLLVM_TM_PLUGIN",
    },
    "XTM": {
        "runtime": str(ROOT / "backends" / "tm_impl" / "xtm" / "XTM_runtime.cpp"),
        "include": str(ROOT / "backends" / "tm_impl" / "xtm"),
        "define": "-DLLVM_TM_PLUGIN",
    },
}

# Common includes (region allocator dir needed by most backends)
COMMON_INC = f"-I{ROOT}/backends/tm_impl/common -I{ROOT}/backends -I{ROOT}/backends/tm_impl/tm_region_allocator"


def find_opt() -> str:
    """Find opt-22 or opt."""
    for name in ["opt-22", "opt-22.1", "opt"]:
        path = subprocess.run(["which", name], capture_output=True, text=True).stdout.strip()
        if path:
            return path
    return "opt"


def find_clang() -> str:
    """Find clang++-22 or clang++."""
    for name in ["clang++-22", "clang++-22.1", "clang++"]:
        path = subprocess.run(["which", name], capture_output=True, text=True).stdout.strip()
        if path:
            return path
    return "clang++"


def step(msg: str):
    print(f"  [{msg}]", file=sys.stderr)


def run_or_die(cmd: str, desc: str, timeout: int = 300):
    step(f"{desc}: {cmd[:120]}...")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        print(f"  FAILED ({desc}):\n{result.stderr[:2000]}", file=sys.stderr)
        sys.exit(1)
    return result


def run_strategy_pass(bc_file: str, out_file: str) -> str:
    """Run tm-fuzz-strategy pass on a bitcode file."""
    opt = find_opt()
    strategy_so = str(BIN_DIR / "libTMFuzzStrategy.so")

    if not os.path.exists(strategy_so):
        step("Building fuzz strategy plugin...")
        run_or_die(
            f"make -C {PLUGIN_DIR} fuzz-strategy",
            "build strategy plugin"
        )

    step("Running tm-fuzz-strategy pass...")
    run_or_die(
        f"{opt} -load-pass-plugin={strategy_so} "
        f"-passes=\"tm-fuzz-strategy\" "
        f"-tm-strategy-dump "
        f"{bc_file} -o {out_file}",
        "strategy pass",
    )
    return out_file


def compile_to_bc(source: str, out_dir: str, extra_flags: str = "") -> str:
    """Compile a .cpp source file to LLVM bitcode."""
    clang_tm = str(PLUGIN_DIR / "clang-tm")
    stem = Path(source).stem
    bc_file = os.path.join(out_dir, f"{stem}.bc")
    step("Compiling to bitcode...")
    run_or_die(
        f"{clang_tm} --compile-only {extra_flags} "
        f"-std=c++20 -O1 -fno-inline "
        f"-I{ROOT}/backends/tm_impl/common -I{ROOT}/backends "
        f"{source} -o {bc_file}",
        "compile",
    )
    return bc_file


def build_binary(source: str, backend: str, out_dir: str,
                 run_strategy: bool = False,
                 extra_cxxflags: str = "",
                 extra_libs: str = "") -> str:
    """Full build pipeline: compile → (strategy) → instrument → optimize → link."""
    stem = Path(source).stem
    binary = os.path.join(out_dir, f"{stem}_{backend.lower()}")

    clang_tm = str(PLUGIN_DIR / "clang-tm")
    opt = find_opt()
    info = BACKENDS[backend]
    runtime = info["runtime"]
    inc = f"-I{info['include']} {COMMON_INC}"
    defines = info["define"]

    if not os.path.exists(str(BIN_DIR / "libTMInstrument.so")):
        step("Building TM instrument plugin...")
        run_or_die(f"make -C {PLUGIN_DIR} libTMInstrument.so", "build plugin")

    # Step 1: Compile source to optimized bitcode
    bc_file = os.path.join(out_dir, f"{stem}.bc")
    opt_bc = os.path.join(out_dir, f"{stem}.opt.bc")
    if not os.path.exists(bc_file):
        step("Compiling to bitcode...")
        run_or_die(
            f"{clang_tm} --compile-only {defines} {inc} {extra_cxxflags} "
            f"-std=c++20 -O1 -fno-inline "
            f"{source} -o {bc_file}",
            "compile",
        )

    # Step 2: Strategy pass (optional, on .bc)
    instr_bc = os.path.join(out_dir, f"{stem}.instr.bc")
    if run_strategy:
        strategy_bc = os.path.join(out_dir, f"{stem}.strategy.bc")
        run_strategy_pass(bc_file, strategy_bc)
        # Instrument strategy-annotated bitcode
        step("Instrumenting...")
        run_or_die(
            f"{opt} -load-pass-plugin={BIN_DIR}/libTMInstrument.so "
            f"-passes=\"tm-instrument\" "
            f"{strategy_bc} -o {instr_bc}",
            "instrument",
        )
    else:
        # Instrument directly
        step("Instrumenting...")
        run_or_die(
            f"{opt} -load-pass-plugin={BIN_DIR}/libTMInstrument.so "
            f"-passes=\"tm-instrument\" "
            f"{bc_file} -o {instr_bc}",
            "instrument",
        )

    # Step 3: Optimize instrumented IR
    step("Optimizing...")
    run_or_die(
        f"{opt} -O1 {instr_bc} -o {opt_bc}",
        "optimize",
    )

    # Step 4: Link
    step("Linking...")
    run_or_die(
        f"{clang_tm} --link-only --runtime={runtime} "
        f"{defines} {inc} {extra_cxxflags} "
        f"-std=c++20 -O1 -pthread "
        f"{opt_bc} -o {binary} "
        f"-lpthread -ldl {extra_libs}",
        "link",
        timeout=300,
    )
    return binary

    return binary


def run_binary(binary: str, args: list, trace_file: str = None,
               timeout: int = 60) -> dict:
    """Run a fuzz binary and return results."""
    env = os.environ.copy()
    if trace_file:
        env["TM_TRACE_PATH"] = trace_file

    cmd = [binary] + [str(a) for a in args]

    start = time.time()
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, env=env
        )
    except subprocess.TimeoutExpired:
        return {"crash": False, "timeout": True, "stdout": "", "stderr": ""}

    elapsed = time.time() - start

    output = {
        "stdout": result.stdout,
        "stderr": result.stderr,
        "returncode": result.returncode,
        "elapsed": elapsed,
        "crash": "SIGSEGV" in result.stderr,
        "timeout": False,
    }

    # Parse invariant result
    for line in result.stdout.splitlines():
        if "INVARIANT:" in line:
            output["invariant_result"] = line.strip()
        if "TM-INVARIANT: FAIL" in line:
            output["invariant_result"] = line.strip()

    return output


def run_fuzz_plan(binary: str, threads_list: list, duration: int,
                  trace_dir: str = None, sample_rate: int = 0) -> list:
    """Run binary with varying thread counts for a given duration.

    Returns list of result dicts.
    """
    results = []

    for nthreads in threads_list:
        step(f"Running with {nthreads} threads (duration={duration}s)")

        trace_file = None
        if trace_dir:
            os.makedirs(trace_dir, exist_ok=True)
            trace_file = os.path.join(
                trace_dir, f"trace_t{nthreads}_{int(time.time())}.log"
            )

        env = os.environ.copy()
        if sample_rate > 0:
            env["TM_SAMPLE_MODE"] = "rate"
            env["TM_SAMPLE_RATE"] = str(sample_rate)
        if trace_file:
            env["TM_TRACE_PATH"] = trace_file

        # The binary must accept: <threads> <duration> or similar
        # We try common argument patterns
        result = run_binary(binary, [nthreads, duration], trace_file)

        if result.get("timeout"):
            print(f"  TIMEOUT ({nthreads}t)", file=sys.stderr)
        elif result["crash"]:
            print(f"  CRASH ({nthreads}t)", file=sys.stderr)
        elif result.get("invariant_result"):
            print(f"  {result['invariant_result']} ({nthreads}t)",
                  file=sys.stderr)
        else:
            print(f"  OK ({nthreads}t, {result['elapsed']:.1f}s)",
                  file=sys.stderr)

        results.append(result)

    return results


def check_invariants(trace_files: list, baseline_file: str = None) -> list:
    """Run invariant checks on collected trace files."""
    sys.path.insert(0, str(ROOT / "tools" / "stm_bug_tool"))
    from event_parser import parse_trace_log
    from invariant_checker import check_all, load_baseline

    reports = []

    baseline = {}
    if baseline_file:
        baseline = load_baseline(baseline_file)

    for tf in trace_files:
        if not os.path.exists(tf):
            continue
        with open(tf) as f:
            text = f.read()
        parsed = parse_trace_log(text)
        parsed["baseline"] = baseline
        parsed["_raw_text"] = text

        violations = check_all(parsed)
        reports.append({
            "file": tf,
            "violations": violations,
        })

    return reports


def print_report(results: list, reports: list):
    """Print final report."""
    print("\n" + "=" * 60)
    print("TM-FUZZ REPORT")
    print("=" * 60)

    passed = sum(1 for r in results if not r.get("crash") and not r.get("timeout")
                 and "FAIL" not in r.get("invariant_result", ""))
    crashed = sum(1 for r in results if r.get("crash"))
    timeout = sum(1 for r in results if r.get("timeout"))
    failed = sum(1 for r in results if not r.get("crash") and not r.get("timeout")
                 and "FAIL" in r.get("invariant_result", ""))

    print(f"  Runs:     {len(results)}")
    print(f"  Passed:   {passed}")
    print(f"  Failed:   {failed}")
    print(f"  Crashed:  {crashed}")
    print(f"  Timeout:  {timeout}")

    if reports:
        total_violations = sum(
            len(r["violations"].get("violations", [])) for r in reports
        )
        print(f"  Invariant violations: {total_violations}")
        if total_violations > 0:
            print("\n  --- Violation details ---")
            for r in reports:
                for v in r["violations"].get("violations", [])[:5]:
                    print(f"    [{v['severity']}] {v['invariant']}: "
                          f"{v['description'][:120]}")

    status = "PASS" if crashed == 0 and failed == 0 else "FAIL"
    print(f"\n  Status: {status}")


def main():
    parser = argparse.ArgumentParser(
        description="Generic TM Fuzz Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --app counter.bc --backend TINYSTM
  %(prog)s --app bank.bc --strategy auto --threads 2,4,8 --duration 30
  %(prog)s --app app.bc --backend all --trace --baseline baseline.txt
        """
    )
    parser.add_argument("--app", required=True,
                        help="Application bitcode file (.bc)")
    parser.add_argument("--backend", default="TINYSTM",
                        help="TM backend(s): comma-separated or 'all'")
    parser.add_argument("--threads", default="1,2,4",
                        help="Comma-separated thread counts")
    parser.add_argument("--duration", type=int, default=10,
                        help="Run duration in seconds (passed to app)")
    parser.add_argument("--strategy", choices=["none", "auto"], default="none",
                        help="Run tm-fuzz-strategy pass first")
    parser.add_argument("--sample-rate", type=int, default=0,
                        help="Set TM sample rate (0 = no sampling)")
    parser.add_argument("--trace", action="store_true",
                        help="Collect TM_TRACE_PATH traces")
    parser.add_argument("--trace-dir", default="/tmp/tm-fuzz-traces",
                        help="Directory for trace output")
    parser.add_argument("--baseline",
                        help="Baseline file for sequential comparison")
    parser.add_argument("--output-dir", default="/tmp/tm-fuzz-build",
                        help="Build output directory")
    parser.add_argument("--extra-cxxflags", default="",
                        help="Additional C++ compilation flags")
    parser.add_argument("--extra-libs", default="",
                        help="Additional linker libraries")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print commands without executing")
    parser.add_argument("--verbose", "-V", action="store_true",
                        help="Verbose output")

    args = parser.parse_args()

    # Resolve backends
    if args.backend == "all":
        backends = list(BACKENDS.keys())
    else:
        backends = [b.strip().upper() for b in args.backend.split(",")]
        for b in backends:
            if b not in BACKENDS:
                print(f"Unknown backend: {b}", file=sys.stderr)
                sys.exit(1)

    threads = [int(t) for t in args.threads.split(",")]

    # Validate inputs
    if not os.path.exists(args.app):
        print(f"Application not found: {args.app}", file=sys.stderr)
        sys.exit(1)

    if args.baseline and not os.path.exists(args.baseline):
        print(f"Baseline file not found: {args.baseline}", file=sys.stderr)
        sys.exit(1)

    out_dir = args.output_dir
    os.makedirs(out_dir, exist_ok=True)

    run_strategy = (args.strategy == "auto")

    all_results = []
    all_trace_files = []

    for backend in backends:
        print(f"\n{'─' * 60}", file=sys.stderr)
        print(f"Backend: {backend}", file=sys.stderr)
        print(f"{'─' * 60}", file=sys.stderr)

        # Build
        if args.dry_run:
            print(f"  [dry-run] Would build {args.app} with {backend}",
                  file=sys.stderr)
            continue

        binary = build_binary(
            args.app, backend, out_dir,
            run_strategy=run_strategy,
            extra_cxxflags=args.extra_cxxflags,
            extra_libs=args.extra_libs,
        )

        if not os.path.exists(binary):
            print(f"  Build failed for {backend}", file=sys.stderr)
            continue

        # Run
        trace_dir = args.trace_dir if args.trace else None
        results = run_fuzz_plan(
            binary, threads, args.duration,
            trace_dir=trace_dir,
            sample_rate=args.sample_rate,
        )
        all_results.extend(results)

        if trace_dir:
            trace_files = sorted(Path(trace_dir).glob("*.log"))
            all_trace_files.extend(str(f) for f in trace_files)

        # Cleanup binary
        if not args.verbose:
            os.unlink(binary)

    # Check invariants
    reports = []
    if all_trace_files:
        reports = check_invariants(all_trace_files, args.baseline)

    # Report
    print_report(all_results, reports)

    return 0 if all(
        not r.get("crash") and not r.get("timeout")
        and "FAIL" not in r.get("invariant_result", "")
        for r in all_results
    ) else 1


if __name__ == "__main__":
    sys.exit(main())
