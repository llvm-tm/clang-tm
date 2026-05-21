#!/usr/bin/env python3
"""
tm-resolve-opaque — Resolve opaque function symbols for TM instrumentation.

Reads a list of unresolved symbol names (one per line) from a file,
searches system shared libraries and LLVM bitcode archives for their
definitions, and produces a bitcode archive with external declarations
that can be linked into the TM pipeline.

Usage:
  tm-resolve-opaque --symbols <file> [--output <dir>] [--verbose]

The tool searches in order:
  1. Known-safe opaque table (opaque_safe_table.hpp) — immediate resolution
  2. Targeted system libraries via nm -D (libm, libc, libpthread, etc.)
  3. Broader search of library directories if symbols remain

For each symbol it produces an LLVM IR declaration stub, assembles them
into a single bitcode archive, and reports unresolved symbols.
"""

import argparse
import os
import subprocess
import sys
import shutil
import tempfile

TARGETED_LIBS = [
    "libm.so*",
    "libc.so*",
    "libpthread.so*",
    "librt.so*",
    "libdl.so*",
    "libstdc++.so*",
    "libgcc_s.so*",
]

STD_LIB_PATHS = [
    "/usr/lib/x86_64-linux-gnu",
    "/lib/x86_64-linux-gnu",
    "/usr/lib/gcc/x86_64-linux-gnu/15",
    "/usr/lib/llvm-22/lib",
    "/usr/lib",
    "/lib",
]

IR_STUB_TEMPLATE = """\
; ModuleID = 'tm-opaque-resolved'
source_filename = "tm-opaque-resolved"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

{declarations}
"""

# Known function signatures for LLVM IR stub generation.
# These cover common math, libc, and system call functions.
KNOWN_FUNCS = {
    "sqrt":     "declare double @sqrt(double) #0",
    "sqrtf":    "declare float @sqrtf(float) #0",
    "sqrtl":    "declare x86_fp80 @sqrtl(x86_fp80) #0",
    "sin":      "declare double @sin(double) #0",
    "cos":      "declare double @cos(double) #0",
    "tan":      "declare double @tan(double) #0",
    "asin":     "declare double @asin(double) #0",
    "acos":     "declare double @acos(double) #0",
    "atan":     "declare double @atan(double) #0",
    "atan2":    "declare double @atan2(double, double) #0",
    "sinh":     "declare double @sinh(double) #0",
    "cosh":     "declare double @cosh(double) #0",
    "tanh":     "declare double @tanh(double) #0",
    "asinh":    "declare double @asinh(double) #0",
    "acosh":    "declare double @acosh(double) #0",
    "atanh":    "declare double @atanh(double) #0",
    "exp":      "declare double @exp(double) #0",
    "exp2":     "declare double @exp2(double) #0",
    "expm1":    "declare double @expm1(double) #0",
    "log":      "declare double @log(double) #0",
    "log2":     "declare double @log2(double) #0",
    "log10":    "declare double @log10(double) #0",
    "log1p":    "declare double @log1p(double) #0",
    "pow":      "declare double @pow(double, double) #0",
    "cbrt":     "declare double @cbrt(double) #0",
    "hypot":    "declare double @hypot(double, double) #0",
    "ceil":     "declare double @ceil(double) #0",
    "floor":    "declare double @floor(double) #0",
    "trunc":    "declare double @trunc(double) #0",
    "round":    "declare double @round(double) #0",
    "nearbyint":"declare double @nearbyint(double) #0",
    "rint":     "declare double @rint(double) #0",
    "fabs":     "declare double @fabs(double) #0",
    "fmod":     "declare double @fmod(double, double) #0",
    "remainder":"declare double @remainder(double, double) #0",
    "copysign": "declare double @copysign(double, double) #0",
    "fdim":     "declare double @fdim(double, double) #0",
    "fmax":     "declare double @fmax(double, double) #0",
    "fmin":     "declare double @fmin(double, double) #0",
    "erf":      "declare double @erf(double) #0",
    "erfc":     "declare double @erfc(double) #0",
    "tgamma":   "declare double @tgamma(double) #0",
    "lgamma":   "declare double @lgamma(double) #0",
    "abs":      "declare i32 @abs(i32) #0",
    "labs":     "declare i64 @labs(i64) #0",
    "llabs":    "declare i64 @llabs(i64) #0",
    "atoi":     "declare i32 @atoi(ptr) #0",
    "atol":     "declare i64 @atol(ptr) #0",
    "atoll":    "declare i64 @atoll(ptr) #0",
    "atof":     "declare double @atof(ptr) #0",
    "strtol":   "declare i64 @strtol(ptr, ptr, i32) #0",
    "strtoul":  "declare i64 @strtoul(ptr, ptr, i32) #0",
    "strtoll":  "declare i64 @strtoll(ptr, ptr, i32) #0",
    "strtoull": "declare i64 @strtoull(ptr, ptr, i32) #0",
    "strtof":   "declare float @strtof(ptr, ptr) #0",
    "strtod":   "declare double @strtod(ptr, ptr) #0",
    "strtold":  "declare x86_fp80 @strtold(ptr, ptr) #0",
    "drand48":  "declare double @drand48() #0",
    "srand48":  "declare void @srand48(i64) #0",
    "bzero":    "declare void @bzero(ptr, i64) #0",
    "posix_memalign": "declare i32 @posix_memalign(ptr, i64, i64) #0",
    "rand":     "declare i32 @rand() #0",
    "srand":    "declare void @srand(i32) #0",
    "time":     "declare i64 @time(ptr) #0",
    "gettimeofday": "declare i32 @gettimeofday(ptr, ptr) #0",
    "nanosleep":    "declare i32 @nanosleep(ptr, ptr) #0",
    "usleep":   "declare i32 @usleep(i32) #0",
    "clock_gettime": "declare i32 @clock_gettime(i32, ptr) #0",
    "mmap":     "declare ptr @mmap(ptr, i64, i32, i32, i32, i64) #0",
    "munmap":   "declare i32 @munmap(ptr, i64) #0",
    "brk":      "declare i32 @brk(ptr) #0",
    "sbrk":     "declare ptr @sbrk(i64) #0",
    "printf":   "declare i32 @printf(ptr, ...) #0",
    "fprintf":  "declare i32 @fprintf(ptr, ptr, ...) #0",
    "sprintf":  "declare i32 @sprintf(ptr, ptr, ...) #0",
    "snprintf": "declare i32 @snprintf(ptr, i64, ptr, ...) #0",
    "puts":     "declare i32 @puts(ptr) #0",
    "putchar":  "declare i32 @putchar(i32) #0",
    "fflush":   "declare i32 @fflush(ptr) #0",
    "exit":     "declare void @exit(i32) #0",
    "abort":    "declare void @abort() #0",
    "strlen":   "declare i64 @strlen(ptr) #0",
    "strcmp":   "declare i32 @strcmp(ptr, ptr) #0",
    "strncmp":  "declare i32 @strncmp(ptr, ptr, i64) #0",
    "memcmp":   "declare i32 @memcmp(ptr, ptr, i64) #0",
    "memcpy":   "declare ptr @memcpy(ptr, ptr, i64) #0",
    "memset":   "declare ptr @memset(ptr, i32, i64) #0",
    "memmove":  "declare ptr @memmove(ptr, ptr, i64) #0",
    "malloc":   "declare ptr @malloc(i64) #0",
    "calloc":   "declare ptr @calloc(i64, i64) #0",
    "realloc":  "declare ptr @realloc(ptr, i64) #0",
    "free":     "declare void @free(ptr) #0",
    "aligned_alloc": "declare ptr @aligned_alloc(i64, i64) #0",
    "pthread_create":  "declare i32 @pthread_create(ptr, ptr, ptr, ptr) #0",
    "pthread_join":    "declare i32 @pthread_join(ptr, ptr) #0",
    "pthread_mutex_lock":   "declare i32 @pthread_mutex_lock(ptr) #0",
    "pthread_mutex_unlock": "declare i32 @pthread_mutex_unlock(ptr) #0",
    "pthread_mutex_init":   "declare i32 @pthread_mutex_init(ptr, ptr) #0",
    "pthread_mutex_destroy": "declare i32 @pthread_mutex_destroy(ptr) #0",
}


def guess_declaration_ir(sym_name):
    """Generate an LLVM IR external declaration stub for a symbol.

    Returns IR text or None if we cannot generate a stub.
    """
    if sym_name.startswith("_Z"):
        return f"declare extern_weak void @\"{sym_name}\"(...) #0\n"
    if sym_name in KNOWN_FUNCS:
        return KNOWN_FUNCS[sym_name] + "\n"
    return f"declare extern_weak void @\"{sym_name}\"(...) #0\n"


def get_dynamic_symbols(lib_path, timeout=3):
    """Return set of defined dynamic symbols in a shared library."""
    try:
        result = subprocess.run(
            ["nm", "-D", "--defined-only", lib_path],
            capture_output=True, text=True, timeout=timeout
        )
        symbols = set()
        for line in result.stdout.splitlines():
            parts = line.strip().split()
            if len(parts) >= 3:
                sym_type = parts[1]
                sym_name = parts[2]
                if sym_type in ("T", "W"):
                    symbols.add(sym_name)
        return symbols
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return set()


def find_targeted_libs(patterns, search_dirs):
    """Find library files matching glob patterns in search directories."""
    import glob as glob_mod
    results = []
    for d in search_dirs:
        if not os.path.isdir(d):
            continue
        for p in patterns:
            matched = sorted(glob_mod.glob(os.path.join(d, p)))
            results.extend(matched)
    return results


def resolve_symbols(symbols, verbose=False):
    """Resolve symbol names against system libraries.

    Returns:
        found: dict of symbol -> library_path
        unresolved: set of symbol names not found
    """
    found = {}
    unresolved = set(symbols)
    nm_timeout = 3

    if not unresolved:
        return found, unresolved

    # Step 1: Resolve known symbols (anything in KNOWN_FUNCS is already resolved)
    # These are known to exist in system libm/libc; we just need to find them.
    # But we skip verification since they are well-known.
    known_in_table = set(sym for sym in unresolved if sym in KNOWN_FUNCS)
    for sym in known_in_table:
        found[sym] = "(known-safe function, resolution skipped)"
        if verbose:
            print(f"  {sym} -> known function signature table")
    unresolved -= known_in_table
    if not unresolved:
        return found, unresolved

    # Step 2: Search targeted libraries first (fast path)
    targeted = find_targeted_libs(TARGETED_LIBS, STD_LIB_PATHS)
    for lib in targeted:
        if not unresolved:
            break
        lib_symbols = get_dynamic_symbols(lib, timeout=nm_timeout)
        match = unresolved & lib_symbols
        for sym in match:
            found[sym] = lib
            if verbose:
                print(f"  {sym} -> {lib}")
        unresolved -= match

    # Step 3: Broader search of remaining shared libs
    if unresolved:
        if verbose:
            print("  Searching additional libraries...")
        for path in STD_LIB_PATHS:
            if not os.path.isdir(path):
                continue
            try:
                entries = os.listdir(path)
            except PermissionError:
                continue
            for f in entries:
                if not unresolved:
                    break
                if not (f.endswith(".so") or ".so." in f):
                    continue
                lib = os.path.join(path, f)
                if lib in targeted:
                    continue
                lib_symbols = get_dynamic_symbols(lib, timeout=nm_timeout)
                match = unresolved & lib_symbols
                for sym in match:
                    found[sym] = lib
                    if verbose:
                        print(f"  {sym} -> {lib}")
                unresolved -= match

    return found, unresolved


def generate_ir_stubs(symbols, output_dir, verbose=False):
    """Generate LLVM IR stub declarations and assemble to bitcode.

    Returns list of generated file paths.
    """
    os.makedirs(output_dir, exist_ok=True)
    declarations = []
    for sym in sorted(symbols):
        ir = guess_declaration_ir(sym)
        if ir:
            declarations.append(ir.rstrip())
            if verbose:
                print(f"  Generated stub: {sym}")

    if not declarations:
        return []

    ir_content = IR_STUB_TEMPLATE.format(declarations="\n".join(declarations))
    ir_path = os.path.join(output_dir, "tm-opaque-resolved.ll")
    with open(ir_path, "w") as f:
        f.write(ir_content)

    bc_path = os.path.join(output_dir, "tm-opaque-resolved.bc")
    llvm_as = shutil.which("llvm-as-22") or shutil.which("llvm-as") or "llvm-as"
    result = subprocess.run(
        [llvm_as, ir_path, "-o", bc_path],
        capture_output=True, text=True, timeout=30
    )
    if result.returncode != 0:
        print(f"Error: llvm-as failed: {result.stderr}", file=sys.stderr)
        return []
    return [bc_path]


def main():
    parser = argparse.ArgumentParser(
        description="Resolve opaque function symbols for TM instrumentation"
    )
    parser.add_argument("--symbols", "-s", required=True,
                        help="Path to symbols file (one per line)")
    parser.add_argument("--output", "-o", default="/tmp/tm-opaque-resolved",
                        help="Output directory for generated bitcode")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Verbose output")
    args = parser.parse_args()

    if not os.path.exists(args.symbols):
        print(f"Error: symbols file not found: {args.symbols}", file=sys.stderr)
        sys.exit(1)

    with open(args.symbols) as f:
        symbols = set(line.strip() for line in f if line.strip())

    if not symbols:
        print("No symbols to resolve.")
        sys.exit(0)

    print(f"Resolving {len(symbols)} opaque symbols...")
    if args.verbose:
        for s in sorted(symbols):
            print(f"  {s}")

    found, unresolved = resolve_symbols(symbols, args.verbose)

    print(f"\nResults:")
    print(f"  Resolved:   {len(found)}")
    print(f"  Unresolved: {len(unresolved)}")

    if found:
        print(f"\nResolved symbols:")
        for sym, lib in sorted(found.items()):
            print(f"  {sym:40s} -> {lib}")

    if unresolved:
        print(f"\nUnresolved symbols (add to KnownSafeOpaqueTable or provide bitcode):")
        for sym in sorted(unresolved):
            print(f"  {sym}")
        print("\nTo register known-safe opaque functions, add entries to")
        print("opaque_safe_table.hpp or use -tm-allow-opaque.")
        print("To provide LLVM IR stubs, add them to the pipeline link step.")

    if found:
        stubs = generate_ir_stubs(found, args.output, args.verbose)
        if stubs:
            print(f"\nGenerated bitcode stubs: {', '.join(stubs)}")
        else:
            print("\nWarning: could not generate bitcode stubs.")

    if unresolved:
        sys.exit(1)


if __name__ == "__main__":
    main()
