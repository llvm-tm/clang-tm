#!/usr/bin/env bash
# ============================================================================
# TM Benchmarks Runner
# ============================================================================
# Usage:
#   ./run_benchmarks.sh                          — standard mode, 4 threads
#   ./run_benchmarks.sh fast                     — quick smoke test, 2 threads
#   ./run_benchmarks.sh -t 8                     — standard mode, 8 threads
#   ./run_benchmarks.sh -t "1 2 4 8 16" fast    — fast mode, thread sweep
#   ./run_benchmarks.sh -t "1 2 4 8 16"         — standard mode, thread sweep
# ============================================================================

set -euo pipefail
cd "$(dirname "$0")"

# Parse -t flag (thread count(s)) before positional mode argument
THREADS=""
while [ $# -gt 0 ]; do
    case "$1" in
        -t) THREADS="$2"; shift 2 ;;
        -t=*) THREADS="${1#*=}"; shift ;;
        *) break ;;
    esac
done

MODE="${1:-standard}"
SAMPLES=""
DEF_THREADS=""
BANK_ARGS=""
DS_ARGS=""
STM7_ARGS=""

case "$MODE" in
  fast)
    SAMPLES=3
    DEF_THREADS=2
    BANK_ARGS="-a 64 -d 500 -r 10 -w 0"
    DS_ARGS="1000 500 80 10 10"      # no leading thread count
    STM7_ARGS="-d 500 -w 1"
    STAMP_ARGS="-d 500 -b genome"
    STAMP_BENCHES="-b genome"
    YCSB_ARGS="-d 500 -w A -k 100 -i 10"
    EIGEN_ARGS="-d 500"
    ;;
  standard)
    SAMPLES=10
    DEF_THREADS=4
    BANK_ARGS="-a 256 -d 3000 -r 10 -w 0"
    DS_ARGS="10000 3000 80 10 10"
    STM7_ARGS="-d 3000 -w 1"
    STAMP_ARGS="-d 2000 -b genome"
    STAMP_BENCHES="genome"
    YCSB_ARGS="-d 3000 -w A -k 10000 -i 1000"
    EIGEN_ARGS="-d 2000"
    ;;
  *)
    echo "Usage: $0 [-t threads] {fast|standard}"
    exit 1
    ;;
esac

# Allow -t flag to override default thread count(s).  A space-separated
# list produces a sweep (one run per thread count).
THREAD_LIST="${THREADS:-$DEF_THREADS}"

RESULTS_DIR="benchmark_results/${MODE}_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
SUMMARY="$RESULTS_DIR/SUMMARY.txt"

echo "TM Benchmark Runner — Mode: $MODE"
echo "========================================="
echo "Samples:       $SAMPLES"
echo "Threads:       $THREAD_LIST"
echo "Bank args:     -t <thread> $BANK_ARGS"
echo "DataStruct:    <thread> $DS_ARGS"
echo "STMbench7:     -t <thread> $STM7_ARGS"
echo "Results dir:   $RESULTS_DIR"
echo ""

# ────────────────────────────────────────────────────────────────────────────
# Helpers
# ────────────────────────────────────────────────────────────────────────────

run_one() {
    # run_one <label> <binary> [args...]
    local label="$1"; shift
    local binary="$1"; shift
    local out="$RESULTS_DIR/${label//\//_}.txt"
    echo -n "  $label ... "
    set +e
    gtimeout 30 "$binary" "$@" > "$out" 2>/dev/null
    local rc=$?
    set -e
    if [ "$rc" = 124 ]; then
        echo "TIMEOUT (>30s)"
    elif [ "$rc" = 138 ] || [ "$rc" = 139 ]; then
        echo "CRASH (SIGBUS/SIGSEGV)"
    else
        # Check for PASS marker or Results section (TL2 outputs neither
        # but exits 0 with correct balance; AVL/hashmap print "Results")
        local passed=0
        grep -q 'PASS\|Results\|✓\|All tests passed' "$out" 2>/dev/null && passed=1
        if [ "$passed" = 1 ]; then
            echo "✓ PASS"
        elif [ "$rc" = 0 ]; then
            echo "✓ (exit=0, no crashes)"
        else
            echo "FAIL (exit=$rc)"
        fi
    fi
    grep -E 'Txns/sec|Ops/sec|ops/sec' "$out" 2>/dev/null | head -1 | sed 's/^/    /' || true
}

build_if_needed() {
    # build_if_needed <make_target> <build_dir>
    local target="$1"; shift
    local dir="$1"; shift
    if [ ! -x "$dir/bin/$target" ]; then
        echo "  (building $target ...)"
        make -C "$dir" "$target" 2>/dev/null | tail -2
    fi
}

# ────────────────────────────────────────────────────────────────────────────
# Build all backends
# ────────────────────────────────────────────────────────────────────────────

echo "=== Building benchmarks ==="
build_if_needed bank_norec      benchmarks/test/bank
build_if_needed bank_singlelock benchmarks/test/bank
build_if_needed bank_tl2        benchmarks/test/bank
if [ -x "$(command -v clang-tm 2>/dev/null)" ] && grep -q rtm /proc/cpuinfo 2>/dev/null; then
    build_if_needed bank_tsxsgl   benchmarks/test/bank
fi
build_if_needed hashmap_NOrec   benchmarks/datastructures
build_if_needed set_NOrec       benchmarks/datastructures
build_if_needed avltree_NOrec   benchmarks/datastructures
build_if_needed avltree_SingleGlobalLock benchmarks/datastructures
build_if_needed stmbench_singlelock  benchmarks/STMbench7
# STAMP build skipped (TinySTM too slow; see section below)
build_if_needed ycsb_singlelock benchmarks/YCSB
build_if_needed eigen_singlelock benchmarks/EigenBench
echo ""

# ────────────────────────────────────────────────────────────────────────────
# Bank benchmark — all backends
# ────────────────────────────────────────────────────────────────────────────

echo "=== Bank Benchmark ==="
for t in $THREAD_LIST; do
    for backend in norec singlelock tl2; do
        for s in $(seq 1 $SAMPLES); do
            run_one "bank_${backend}_${t}t_sample${s}" \
                "benchmarks/test/bank/bin/bank_${backend}" \
                -t "$t" $BANK_ARGS
        done
    done
    if [ -x "benchmarks/test/bank/bin/bank_tsxsgl" ]; then
        for s in $(seq 1 $SAMPLES); do
            run_one "bank_tsxsgl_${t}t_sample${s}" \
                "benchmarks/test/bank/bin/bank_tsxsgl" \
                -t "$t" $BANK_ARGS
        done
    fi
done

# ────────────────────────────────────────────────────────────────────────────
# Data Structure Benchmarks (NOrec, which is the fastest correct backend)
# ────────────────────────────────────────────────────────────────────────────

echo "=== Data Structure Benchmarks ==="
for t in $THREAD_LIST; do
    for ds in hashmap set; do
        for s in $(seq 1 $SAMPLES); do
            run_one "ds_${ds}_${t}t_sample${s}" \
                "benchmarks/datastructures/bin/${ds}_NOrec" \
                "$t" $DS_ARGS
        done
    done
    if [ -x "benchmarks/datastructures/bin/avltree_SingleGlobalLock" ]; then
        for s in $(seq 1 $SAMPLES); do
            run_one "ds_avltree_${t}t_sample${s}" \
                "benchmarks/datastructures/bin/avltree_SingleGlobalLock" \
                "$t" $DS_ARGS
        done
    else
        echo "  avltree_SingleGlobalLock not built, skipping"
    fi
done

# ────────────────────────────────────────────────────────────────────────────
# STMbench7 (SingleGlobalLock — only backend fast enough)
# ────────────────────────────────────────────────────────────────────────────

echo "=== STMbench7 (SingleGlobalLock) ==="
for t in $THREAD_LIST; do
    for s in $(seq 1 $SAMPLES); do
        run_one "stmbench7_sgl_${t}t_sample${s}" \
            "benchmarks/STMbench7/bin/stmbench_singlelock" \
            -t "$t" $STM7_ARGS
    done
done

# ────────────────────────────────────────────────────────────────────────────
# STAMP benchmark — NOTE: STAMP only has TinySTM backend.
# TinySTM is too slow with instrumented code (>30s), so STAMP is skipped.
# Run manually: make -C benchmarks/STAMP stamp_tinystm && ./bin/stamp_tinystm -t 2 -d 5000 -b genome
# ────────────────────────────────────────────────────────────────────────────
echo "=== STAMP (skipped — TinySTM too slow with instrumentation) ==="

# ────────────────────────────────────────────────────────────────────────────
# YCSB (SingleGlobalLock — fastest correct backend)
# ────────────────────────────────────────────────────────────────────────────

echo "=== YCSB (SingleGlobalLock) ==="
for t in $THREAD_LIST; do
    for s in $(seq 1 $SAMPLES); do
        run_one "ycsb_sgl_${t}t_sample${s}" \
            "benchmarks/YCSB/bin/ycsb_singlelock" \
            -t "$t" $YCSB_ARGS
    done
done

# ────────────────────────────────────────────────────────────────────────────
# EigenBench (SingleGlobalLock)
# ────────────────────────────────────────────────────────────────────────────

echo "=== EigenBench (SingleGlobalLock) ==="
for t in $THREAD_LIST; do
    for s in $(seq 1 $SAMPLES); do
        run_one "eigen_sgl_${t}t_sample${s}" \
            "benchmarks/EigenBench/bin/eigen_singlelock" \
            -t "$t" $EIGEN_ARGS
    done
done

# ────────────────────────────────────────────────────────────────────────────
# Summary
# ────────────────────────────────────────────────────────────────────────────

echo ""
echo "========================================="
echo "Results saved to: $RESULTS_DIR"
echo "========================================="

# Generate summary table
{
    echo "TM Benchmark Results — Mode: $MODE"
    echo "========================================="
    echo ""
    echo "Bank:"
    for f in "$RESULTS_DIR"/bank_*.txt; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .txt)
        speed=$(grep -E 'Txns/sec|Ops/sec' "$f" 2>/dev/null | head -1 | sed 's/.*:[[:space:]]*//')
        grep -q 'PASS' "$f" 2>/dev/null && status="OK" || status="(no PASS marker)"
        echo "  $base  $status  $speed"
    done
    echo ""
    echo "Data Structures (NOrec):"
    for f in "$RESULTS_DIR"/ds_*.txt; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .txt)
        speed=$(grep -E 'Txns/sec|Ops/sec' "$f" 2>/dev/null | head -1 | sed 's/.*:[[:space:]]*//')
        echo "  $base  $speed"
    done
    echo ""
    echo "STMbench7:"
    for f in "$RESULTS_DIR"/stmbench7_*.txt; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .txt)
        speed=$(grep -E 'Txns/sec|Ops/sec' "$f" 2>/dev/null | head -1 | sed 's/.*:[[:space:]]*//')
        echo "  $base  $speed"
    done
    echo ""
    echo "YCSB:"
    for f in "$RESULTS_DIR"/ycsb_*.txt; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .txt)
        speed=$(grep -E 'Ops/sec' "$f" 2>/dev/null | head -1 | sed 's/.*:[[:space:]]*//')
        echo "  $base  $speed"
    done
    echo ""
    echo "EigenBench:"
    for f in "$RESULTS_DIR"/eigen_*.txt; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .txt)
        speed=$(grep -E 'Ops/sec' "$f" 2>/dev/null | head -1 | sed 's/.*:[[:space:]]*//')
        echo "  $base  $speed"
    done
} | tee "$SUMMARY"

echo ""
echo "Summary: $SUMMARY"
