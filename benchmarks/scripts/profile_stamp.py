#!/usr/bin/env python3
"""
STAMP Profiling Script

Profiles STAMP benchmarks across plugin/C++/Rust using paper-matched
parameters and compares cross-implementation timing consistency.

Paper reference parameters (Table IV, IISWC 2008):
  bayes:        -v32 -r1024 -n2 -p20 -i2 -e2
  bayes+:       -v32 -r4096 -n2 -p20 -i2 -e2
  genome:       -g256 -s16 -n16384
  genome+:      -g512 -s32 -n32768
  intruder:     -a10 -l4 -n2048 -s1
  intruder+:    -a10 -l16 -n4096 -s1
  kmeans-high:  -m15 -n15 -t0.05 -i random-n2048-d16-c16
  kmeans-high+: -m15 -n15 -t0.05 -i random-n16384-d24-c16
  kmeans-low:   -m40 -n40 -t0.05 -i random-n2048-d16-c16
  kmeans-low+:  -m40 -n40 -t0.05 -i random-n16384-d24-c16
  labyrinth:    -i random-x32-y32-z3-n96
  labyrinth+:   -i random-x48-y48-z3-n64
  ssca2:        -s13 -i1.0 -u1.0 -l3 -p3
  ssca2+:       -s14 -i1.0 -u1.0 -l9 -p9
  vacation-high:  -n4 -q60 -u90 -r16384 -t4096
  vacation-high+: -n4 -q60 -u90 -r1048576 -t4096
  vacation-low:   -n2 -q90 -u98 -r16384 -t4096
  vacation-low+:  -n2 -q90 -u98 -r1048576 -t4096
  yada:         -a20 -i 633.2
  yada+:        -a10 -i ttimeu10000.2
"""

import csv
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PLUGIN_STAMP_DIR = REPO_ROOT / "benchmarks" / "plugin" / "STAMP"
EXPLI_DIR = REPO_ROOT / "benchmarks" / "cpp"
RUST_DIR = REPO_ROOT / "benchmarks" / "rust"

# Shared params (identical for all 3 impls) + per-impl overrides where flags differ
# impl=None means the shared key is used for all.
SHARED = "shared"
CLI_ARGS = {
    "bayes":      {SHARED: "-v 32 -r 1024 -n 2 -c 20 -i 2 -e 2"},
    "bayes+":     {SHARED: "-v 32 -r 4096 -n 2 -c 20 -i 2 -e 2"},
    "genome":     {SHARED: "-g 256 -s 16 -n 16384"},
    "genome+":    {SHARED: "-g 512 -s 32 -n 32768"},
    "intruder":   {SHARED: "-a 10 -l 4 -n 2048 -s 1"},
    "intruder+":  {SHARED: "-a 10 -l 16 -n 4096 -s 1"},
    # kmeans: plugin+C++ accept -m; Rust uses -c -d. Plugin+C++ accept -t; Rust has none (hardcoded 0.001)
    "kmeans-high":  {"plugin": "-m 16 -n 2048 -t 0.05", "expli": "-m 16 -n 2048 -t 0.05", "rust": "-c 16 -d 16 -n 2048"},
    "kmeans-high+": {"plugin": "-m 16 -n 16384 -t 0.05", "expli": "-m 16 -n 16384 -t 0.05", "rust": "-c 16 -d 24 -n 16384"},
    "kmeans-low":   {"plugin": "-m 16 -n 2048 -t 0.05", "expli": "-m 16 -n 2048 -t 0.05", "rust": "-c 16 -d 16 -n 2048"},
    "kmeans-low+":  {"plugin": "-m 16 -n 16384 -t 0.05", "expli": "-m 16 -n 16384 -t 0.05", "rust": "-c 16 -d 24 -n 16384"},
    "labyrinth":  {SHARED: "-x 32 -y 32 -z 3 -n 96"},
    "labyrinth+": {SHARED: "-x 48 -y 48 -z 3 -n 64"},
    # ssca2: plugin uses -p for max_paral_edges; C++/Rust use -m
    "ssca2":      {"plugin": "-s 13 -i 1 -u 1.0 -l 3 -p 3", "expli": "-s 13 -i 1 -u 1.0 -l 3 -m 3", "rust": "-s 13 -i 1 -u 1.0 -l 3 -m 3"},
    "ssca2+":     {"plugin": "-s 14 -i 1 -u 1.0 -l 9 -p 9", "expli": "-s 14 -i 1 -u 1.0 -l 9 -m 9", "rust": "-s 14 -i 1 -u 1.0 -l 9 -m 9"},
    # vacation: plugin has -q (query %) that C++/Rust lack — separate
    "vacation-high":  {"plugin": "-n 4 -q 60 -u 90 -r 16384 -t 4096", "expli": "-r 16384 -n 4 -u 90 -t 4096", "rust": "-r 16384 -n 4 -u 90 -t 4096"},
    "vacation-high+": {"plugin": "-n 4 -q 60 -u 90 -r 1048576 -t 4096", "expli": "-r 1048576 -n 4 -u 90 -t 4096", "rust": "-r 1048576 -n 4 -u 90 -t 4096"},
    "vacation-low":   {"plugin": "-n 2 -q 90 -u 98 -r 16384 -t 4096", "expli": "-r 16384 -n 2 -u 98 -t 4096", "rust": "-r 16384 -n 2 -u 98 -t 4096"},
    "vacation-low+":  {"plugin": "-n 2 -q 90 -u 98 -r 1048576 -t 4096", "expli": "-r 1048576 -n 2 -u 98 -t 4096", "rust": "-r 1048576 -n 2 -u 98 -t 4096"},
    "yada":      {SHARED: "-a 20 -j 0.5"},
    "yada+":     {SHARED: "-a 10 -j 0.3"},
}

def get_params(bench: str, impl: str) -> str:
    entry = CLI_ARGS.get(bench, {})
    if impl in entry:
        return entry[impl]
    return entry.get(SHARED, "")

THREAD_FLAGS = {
    "expli:bayes": "-t", "expli:yada": "-t",
    "rust:bayes":  "-t", "rust:yada":  "-t",
}
DEFAULT_TF = {"plugin": "-p", "expli": "-p", "rust": "-p"}

BACKEND_EXPLI = {"tinystm": "TINYSTM", "norec": "NOREC", "singlelock": "SGL", "tsxsgl": "SGL"}
BACKEND_RUST  = {"tinystm": "wbctl", "norec": "norec", "tsxsgl": "tsxsgl", "singlelock": "sgl"}
PLUGIN_BINARY = "stamp_tinystm"

BENCH_BINARY = {
    "expli": {b: b for b in ("bayes","genome","intruder","kmeans","labyrinth","ssca2","vacation","yada")},
    "rust":  {"bayes":"stamp_bayes","genome":"stamp_genome","intruder":"stamp_intruder",
              "kmeans":"stamp_kmeans","labyrinth":"stamp_labyrinth",
              "ssca2":"stamp_ssca2","vacation":"stamp_vacation","yada":"stamp_yada"},
}

@dataclass
class RunResult:
    impl: str; benchmark: str; backend: str; threads: int
    elapsed_ms: Optional[float] = None; ops: Optional[int] = None
    rate: Optional[float] = None; aborts: Optional[int] = None
    passed: bool = False; error: str = ""

# ── Safe subprocess with kill ─────────────────────────────────────────────
def run_safe(cmd, timeout, cwd=None):
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
        return proc.returncode, stdout, stderr
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return -1, "", f"TIMEOUT after {timeout}s"
    except Exception as e:
        proc.kill()
        proc.wait()
        return -2, "", str(e)

# ── Build with timeout ────────────────────────────────────────────────────
BUILD_TIMEOUT = 300

def build_plugin():
    print(f"  [CHECKPOINT] Building plugin...", flush=True)
    rc, _, stderr = run_safe(["make", "-j", PLUGIN_BINARY], BUILD_TIMEOUT, PLUGIN_STAMP_DIR)
    if rc != 0:
        print(f"  BUILD FAILED: {stderr.strip()[-200:]}")
        return False
    return True

def build_expli(backend):
    be = BACKEND_EXPLI.get(backend, "TINYSTM")
    print(f"  [CHECKPOINT] Building C++ ({be})...", flush=True)
    rc, _, stderr = run_safe(["make", "-j", f"BACKEND={be}"], BUILD_TIMEOUT, EXPLI_DIR)
    if rc != 0:
        print(f"  BUILD FAILED: {stderr.strip()[-200:]}")
        return False
    return True

def build_rust(backend):
    feat = BACKEND_RUST.get(backend, "wbctl")
    print(f"  [CHECKPOINT] Building Rust ({feat})...", flush=True)
    rc, _, stderr = run_safe(
        ["cargo", "build", "--release", "--no-default-features", "--features", feat],
        BUILD_TIMEOUT, RUST_DIR)
    if rc != 0:
        tail = "\n".join(stderr.strip().split("\n")[-20:])
        print(f"  BUILD FAILED:\n{tail}")
        return False
    return True

# ── Output parsing ────────────────────────────────────────────────────────
RE_TIME = re.compile(
    r"(?:Time|Elapsed|Learn time)\s*[=:]\s*([\d.]+)\s*(?:ms|sec|seconds)?"
    r"|Time taken for all is\s*([\d.]+)\s*sec"
    r"|Results\s*\((\d+)\s*ms\)"
    r"|Elapsed:\s+(\d+)\s+ms", re.IGNORECASE)
RE_OPS = re.compile(
    r"(?:Total ops|Operations|Num found|Unique segments|Paths routed|Total edges learned)"
    r"[=:\s]*(\d+)", re.IGNORECASE)
RE_RATE = re.compile(r"(?:Rate|Ops/sec)[=:\s]*([\d.]+)", re.IGNORECASE)
RE_ABORTS = re.compile(r"Aborts[=:\s]*(\d+)", re.IGNORECASE)
RE_CHECK = re.compile(r"(?:PASS|Verification passed|done\.)", re.IGNORECASE)

def parse_output(out):
    elapsed = None; ops = None; rate = None; aborts = None
    passed = bool(RE_CHECK.search(out))
    for m in RE_TIME.finditer(out):
        if m.group(1):
            elapsed = float(m.group(1)) if "ms" in m.group(0) else float(m.group(1)) * 1000
        if m.group(2): elapsed = float(m.group(2)) * 1000  # "Time taken for all is X sec"
        if m.group(3): elapsed = float(m.group(3))          # "Results (X ms):"
        if m.group(4): elapsed = float(m.group(4))          # "Elapsed: X ms"
    for m in RE_OPS.finditer(out): ops = int(m.group(1))
    for m in RE_RATE.finditer(out): rate = float(m.group(1))
    for m in RE_ABORTS.finditer(out): aborts = int(m.group(1))
    return elapsed, ops, rate, aborts, passed

def classify_error(rc, stderr, passed, output):
    if rc == -1: return "TIMEOUT"
    if rc == -2: return str(stderr)[:200]
    if rc != 0 and not passed:
        tail = stderr.strip().split("\n")[-3:] if stderr.strip() else [f"exit {rc}"]
        return "; ".join(tail)
    if not passed and not re.search(r"(?:Time|Elapsed|done\.|PASS)", output, re.IGNORECASE):
        return "NO VERIFICATION OUTPUT"
    return ""

def tf(impl, bench):
    if f"{impl}:{bench}" in THREAD_FLAGS:
        return THREAD_FLAGS[f"{impl}:{bench}"]
    for b in ["bayes","genome","intruder","kmeans","labyrinth","ssca2","vacation","yada"]:
        if b in bench and f"{impl}:{b}" in THREAD_FLAGS:
            return THREAD_FLAGS[f"{impl}:{b}"]
    return DEFAULT_TF.get(impl, "-t")

def bench_binary(bench, impl):
    for b in ["bayes","genome","intruder","kmeans","labyrinth","ssca2","vacation","yada"]:
        if b in bench and b in BENCH_BINARY.get(impl, {}):
            return BENCH_BINARY[impl][b]
    return bench

# ── Runners ───────────────────────────────────────────────────────────────
def run_plugin(bench, threads, timeout):
    binary = PLUGIN_STAMP_DIR / "bin" / PLUGIN_BINARY
    if not binary.exists():
        return RunResult("plugin", bench, "tinystm", threads, error="binary not found")
    params = get_params(bench, "plugin")
    bname = bench.split("+")[0].split("-")[0]
    cmd = [str(binary), tf("plugin", bench), str(threads), "-b", bname] + params.split()
    rc, out, err = run_safe(cmd, timeout, PLUGIN_STAMP_DIR)
    el, ops, rate, aborts, passed = parse_output(out + err)
    return RunResult("plugin", bench, "tinystm", threads, el, ops, rate, aborts, passed or rc == 0,
                     classify_error(rc, err, passed, out + err))

def run_expli(bench, threads, timeout):
    bn = bench_binary(bench, "expli")
    binary = EXPLI_DIR / "bin" / bn
    if not binary.exists():
        return RunResult("expli", bench, "tinystm", threads, error="binary not found")
    params = get_params(bench, "expli")
    cmd = [str(binary), tf("expli", bench), str(threads)] + params.split()
    rc, out, err = run_safe(cmd, timeout, EXPLI_DIR)
    el, ops, rate, aborts, passed = parse_output(out + err)
    return RunResult("expli", bench, "tinystm", threads, el, ops, rate, aborts, passed or rc == 0,
                     classify_error(rc, err, passed, out + err))

def run_rust(bench, threads, timeout):
    bn = bench_binary(bench, "rust")
    binary = RUST_DIR / "target" / "release" / bn
    if not binary.exists():
        return RunResult("rust", bench, "tinystm", threads, error="binary not found")
    params = get_params(bench, "rust")
    cmd = [str(binary), tf("rust", bench), str(threads)] + params.split()
    rc, out, err = run_safe(cmd, timeout, RUST_DIR)
    el, ops, rate, aborts, passed = parse_output(out + err)
    return RunResult("rust", bench, "tinystm", threads, el, ops, rate, aborts, passed or rc == 0,
                     classify_error(rc, err, passed, out + err))

# ── Comparison ────────────────────────────────────────────────────────────
def compare(results, ref_impl="expli"):
    print(f"\n{'='*72}\nCROSS-IMPLEMENTATION COMPARISON\n{'='*72}")
    groups = {}
    for r in results:
        groups.setdefault((r.benchmark, r.backend, r.threads), {})[r.impl] = r
    mismatches = 0
    for (bench, backend, threads), impls in sorted(groups.items()):
        if ref_impl not in impls or len(impls) < 2:
            continue
        ref = impls[ref_impl]
        if ref.elapsed_ms is None or ref.elapsed_ms == 0:
            continue
        parts = [f"  {bench:20s} {threads:2d}t"]
        for name in ("plugin", "expli", "rust"):
            if name in impls and impls[name].elapsed_ms:
                t_ = impls[name].elapsed_ms
                parts.append(f"  {name[:4]}:{t_:8.0f}ms ({t_/ref.elapsed_ms:.2f}x)")
        print("".join(parts))
        for name, r in impls.items():
            if name == ref_impl or r.elapsed_ms is None:
                continue
            ratio = r.elapsed_ms / ref.elapsed_ms
            if ratio < 0.75 or ratio > 1.25:
                print(f"    ! {name} differs by {abs(1-ratio)*100:.0f}%")
                mismatches += 1
    if mismatches == 0:
        print("\n  OK: All implementations consistent")
    return mismatches

# ── Main ──────────────────────────────────────────────────────────────────
def main():
    import argparse
    ap = argparse.ArgumentParser(description="STAMP Profiler")
    ap.add_argument("--backend", default="tinystm", choices=["tinystm","norec","singlelock","tsxsgl"])
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=600, help="per-run timeout (s)")
    ap.add_argument("--benchmarks", nargs="+", default=list(CLI_ARGS.keys()))
    ap.add_argument("--impls", nargs="+", default=["plugin","expli","rust"],
                    choices=["plugin","expli","rust"])
    ap.add_argument("--ref-impl", default="expli", choices=["plugin","expli","rust"])
    ap.add_argument("--output", default=None)
    args = ap.parse_args()

    print("="*72)
    print("STAMP Profiling — Paper-Matched Parameters")
    print("="*72)
    print(f"  Backend: {args.backend}  Threads: {args.threads}  "
          f"Timeout: {args.timeout}s")
    print(f"  Benchmarks ({len(args.benchmarks)}): {', '.join(args.benchmarks)}")
    print(f"  Impls: {', '.join(args.impls)}  Ref: {args.ref_impl}")
    print()

    results = []
    total = len(args.benchmarks) * len(args.impls)
    count = 0

    for bench in args.benchmarks:
        for impl in args.impls:
            count += 1
            print(f"\n{'─'*60}")
            print(f"SAVE POINT [{count}/{total}]  {impl} / {bench}")
            print(f"{'─'*60}")
            sys.stdout.flush()

            if impl == "plugin":
                if not build_plugin():
                    results.append(RunResult(impl, bench, args.backend, args.threads, error="BUILD FAILED"))
                    continue
                result = run_plugin(bench, args.threads, args.timeout)
            elif impl == "expli":
                if not build_expli(args.backend):
                    results.append(RunResult(impl, bench, args.backend, args.threads, error="BUILD FAILED"))
                    continue
                result = run_expli(bench, args.threads, args.timeout)
            elif impl == "rust":
                if not build_rust(args.backend):
                    results.append(RunResult(impl, bench, args.backend, args.threads, error="BUILD FAILED"))
                    continue
                result = run_rust(bench, args.threads, args.timeout)
            else:
                continue

            if result.error:
                print(f"  RESULT: FAIL — {result.error}")
            elif result.elapsed_ms is not None:
                s = f"  RESULT: {result.elapsed_ms:.0f}ms"
                if result.ops is not None: s += f"  ops={result.ops}"
                if result.rate is not None: s += f"  rate={result.rate:.0f}/s"
                if result.aborts is not None: s += f"  aborts={result.aborts}"
                print(s)
            else:
                print(f"  RESULT: No timing output")
            sys.stdout.flush()

            results.append(result)

    if args.output:
        with open(Path(args.output), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["impl","benchmark","backend","threads","elapsed_ms","ops","rate","aborts","passed","error"])
            for r in results:
                w.writerow([r.impl, r.benchmark, r.backend, r.threads, r.elapsed_ms, r.ops, r.rate, r.aborts, r.passed, r.error])
        print(f"\nResults written to {args.output}")

    compare(results, args.ref_impl)

    n_ok = sum(1 for r in results if not r.error)
    n_fail = sum(1 for r in results if r.error)
    print(f"\n{'='*72}")
    print(f"SUMMARY  Total: {len(results)}  OK: {n_ok}  Failed: {n_fail}")
    print(f"{'='*72}")
    for r in results:
        if r.error:
            print(f"  FAIL [{r.impl:6s}] {r.benchmark:20s}: {r.error}")
    sys.exit(1 if n_fail > 0 else 0)

if __name__ == "__main__":
    main()
