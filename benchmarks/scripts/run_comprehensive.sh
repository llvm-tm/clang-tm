#!/usr/bin/env bash
# Comprehensive TM Backend Benchmark Runner
# Tests 5 backends × 4 suites across 12 thread levels × 3 samples
set -euo pipefail
cd "$(dirname "$0")"

THREAD_LIST="${THREADS:-"1 2 4 7 10 14 21 28 35 42 49 56"}"
SAMPLES=3
TIMEOUT=300

# Portable timeout: use gtimeout (macOS coreutils) if available
TIMEOUT_CMD="timeout"
if ! command -v timeout &>/dev/null && command -v gtimeout &>/dev/null; then
    TIMEOUT_CMD="gtimeout"
fi

RESULTS_DIR="benchmark_results/comprehensive_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
SUMMARY="$RESULTS_DIR/SUMMARY.txt"

echo "Comprehensive TM Benchmark Runner"
echo "================================="
echo "Samples: $SAMPLES  Threads: $THREAD_LIST  Timeout: ${TIMEOUT}s"

# Count runs
nthreads=$(echo $THREAD_LIST | wc -w)
nruns_stamp=$((4 * nthreads * SAMPLES))
nruns_tpcc=$((4 * nthreads * SAMPLES))
nruns_ycsb=$((4 * nthreads * SAMPLES))
nruns_stm7=$((3 * nthreads * SAMPLES))
total=$((nruns_stamp + nruns_tpcc + nruns_ycsb + nruns_stm7))
echo "Runs: stamp=$nruns_stamp tpcc=$nruns_tpcc ycsb=$nruns_ycsb stmbench7=$nruns_stm7 total=$total"
echo ""

run_one() {
    local label="$1"; shift
    local binary="$1"; shift
    local out="$RESULTS_DIR/${label//\//_}.txt"
    printf "  %-50s " "$label"
    set +e
    $TIMEOUT_CMD $TIMEOUT "$binary" "$@" > "$out" 2>/dev/null
    local rc=$?
    set -e
    if [ "$rc" = 124 ]; then
        echo "TIMEOUT (>${TIMEOUT}s)"
    elif [ "$rc" -ge 128 ]; then
        sig=$((rc - 128))
        echo "CRASH (signal $sig)"
    else
        # Check for various success indicators
        if grep -qE 'Ops/sec|Total ops' "$out" 2>/dev/null; then
            echo "✓"
        elif grep -qE 'Read-only:|Read/Update split' "$out" 2>/dev/null; then
            echo "✓"
        elif grep -qE 'Time\s*[=:]\s*[0-9]' "$out" 2>/dev/null; then
            echo "✓"
        elif grep -qE 'Total edges|Unique segments' "$out" 2>/dev/null; then
            echo "✓"
        elif [ "$rc" = 0 ]; then
            echo "✓ (exit=0)"
        else
            echo "FAIL (exit=$rc)"
        fi
    fi
}

# ─── STAMP (fixed-work, time-to-complete) ───
# Using genome workload (works with all non-hanging backends)
echo "=== STAMP Benchmark (genome workload) ==="
STAMP_BASE=benchmarks/STAMP/bin
for backend in tinystm singlelock tsxsgl spht; do
    for t in $THREAD_LIST; do
        for s in $(seq 1 $SAMPLES); do
            run_one "stamp_${backend}_genome_${t}t_s${s}" \
                "$STAMP_BASE/stamp_$backend" -t "$t" -b genome
        done
    done
done

# ─── TPCC (duration-based, Ops/sec) ───
echo ""
echo "=== TPCC Benchmark ==="
TPCC_BASE=benchmarks/TPCC/bin
for backend in tinystm singlelock tsxsgl spht; do
    for t in $THREAD_LIST; do
        for s in $(seq 1 $SAMPLES); do
            run_one "tpcc_${backend}_${t}t_s${s}" \
                "$TPCC_BASE/tpcc_$backend" -t "$t" -d 5000
        done
    done
done

# ─── YCSB (duration-based, Ops/sec) ───
echo ""
echo "=== YCSB Benchmark ==="
YCSB_BASE=benchmarks/YCSB/bin
for backend in tinystm singlelock tsxsgl spht; do
    for t in $THREAD_LIST; do
        for s in $(seq 1 $SAMPLES); do
            run_one "ycsb_${backend}_${t}t_s${s}" \
                "$YCSB_BASE/ycsb_$backend" -t "$t" -d 5000 -w A -k 10000 -i 1000
        done
    done
done

# ─── STMbench7 (duration-based) ───
echo ""
echo "=== STMbench7 Benchmark ==="
STM7_BASE=benchmarks/STMbench7/bin
for backend in singlelock tsxsgl spht; do
    for t in $THREAD_LIST; do
        for s in $(seq 1 $SAMPLES); do
            run_one "stmbench7_${backend}_${t}t_s${s}" \
                "$STM7_BASE/stmbench_$backend" -t "$t" -d 5000 -w 1
        done
    done
done

# ─── Generate Summary ───
echo ""
echo "========================================="
echo "Generating summary..."
echo "========================================="

{
    echo "Comprehensive TM Benchmark Results"
    echo "================================="
    echo "Date: $(date)"
    echo "Samples: $SAMPLES, Threads:$THREAD_LIST, Timeout: ${TIMEOUT}s"
    echo ""

    for category in stamp tpcc ycsb stmbench7; do
        echo "[$category]"
        for f in "$RESULTS_DIR"/${category}_*.txt; do
            [ -f "$f" ] || continue
            base=$(basename "$f" .txt)

            # Extract throughput metrics
            ops=$(grep -E 'Ops/sec' "$f" 2>/dev/null | head -1 | sed 's/.*:[[:space:]]*//')
            time_val=$(grep -oE 'Time\s*[=:]\s*[0-9.]+' "$f" 2>/dev/null | head -1 | sed 's/.*[=:]\s*//')
            ro=$(grep -oE 'Read-only:\s*[0-9]+' "$f" 2>/dev/null | head -1 | sed 's/.*:\s*//')
            upd=$(grep -oE 'Update:\s*[0-9]+' "$f" 2>/dev/null | head -1 | sed 's/.*:\s*//')

            if [ -n "$ops" ]; then
                echo "  $base  Ops/sec=$ops"
            elif [ -n "$time_val" ]; then
                echo "  $base  Time=${time_val}s"
            elif [ -n "$ro" ]; then
                echo "  $base  RO=$ro UPD=$upd"
            else
                echo "  $base  (no throughput data)"
            fi
        done
        echo ""
    done
} | tee "$SUMMARY"

echo "Results: $RESULTS_DIR"
echo "Summary: $SUMMARY"
echo "done."
