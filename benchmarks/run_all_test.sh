#!/bin/bash
# Run all benchmarks with TinySTM WBCTL at 16 threads.
# Uses timeout per benchmark to detect hangs.

TIMEOUT=120  # seconds per benchmark
THREADS=16
STAMP_BIN=STAMP/bin/stamp_tinystm_wbctl
STM7_BIN=STMbench7/bin/stmbench_tinystm_wbctl
TPCC_BIN=TPCC/bin/tpcc_tinystm_wbctl
RESULTS_DIR=test_results
FAILED=0
PASSED=0
HUNG=0

mkdir -p $RESULTS_DIR
rm -f $RESULTS_DIR/*

run_bench() {
    local name="$1"
    shift
    echo -n "Testing $name ... "
    local outfile="$RESULTS_DIR/$name.out"
    if timeout $TIMEOUT "$@" > "$outfile" 2>&1; then
        local rc=$?
        local elapsed=$(grep -oP 'Elapsed:\s+\K[0-9]+' "$outfile" | head -1)
        local ops=$(grep -oP 'Total ops:\s+\K[0-9]+' "$outfile" | head -1)
        local aborts=$(grep -oP 'Aborts:\s+\K[0-9]+' "$outfile" | head -1)
        echo -e "\033[32mPASSED\033[0m (${elapsed:-?}ms, ${ops:-?} ops, ${aborts:-?} aborts)"
        PASSED=$((PASSED+1))
    else
        local rc=$?
        if [ $rc -eq 124 ]; then
            echo -e "\033[31mHUNG\033[0m (timeout after ${TIMEOUT}s)"
            HUNG=$((HUNG+1))
        else
            echo -e "\033[31mFAILED\033[0m (exit code $rc)"
            tail -5 "$outfile"
            FAILED=$((FAILED+1))
        fi
    fi
}

BASE_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "============================================"
echo "  TM Benchmark Test Suite (WBCTL, $THREADS threads)"
echo "  Timeout: ${TIMEOUT}s per benchmark"
echo "============================================"
echo ""

# --- STAMP benchmarks ---
echo "=== STAMP ==="
STAMP="$BASE_DIR/STAMP/bin/stamp_tinystm_wbctl"
run_bench "stamp_bayes"    "$STAMP" -b bayes -t $THREADS
run_bench "stamp_genome"   "$STAMP" -b genome -t $THREADS
run_bench "stamp_intruder" "$STAMP" -b intruder -t $THREADS
run_bench "stamp_kmeans"   "$STAMP" -b kmeans -t $THREADS
run_bench "stamp_labyrinth" "$STAMP" -b labyrinth -t $THREADS
run_bench "stamp_ssca2"    "$STAMP" -b ssca2 -t $THREADS
run_bench "stamp_vacation" "$STAMP" -b vacation -t $THREADS -n 64 -q 90 -r 16384 -u 98 -t 4096
run_bench "stamp_yada"     "$STAMP" -b yada -t $THREADS

# --- STMbench7 ---
echo ""
echo "=== STMbench7 ==="
STM7="$BASE_DIR/STMbench7/bin/stmbench_tinystm_wbctl"
run_bench "stmbench7_rd90"    "$STM7" -t $THREADS -d 10000 -w 1
run_bench "stmbench7_rw60"    "$STM7" -t $THREADS -d 10000 -w 2
run_bench "stmbench7_wr10"    "$STM7" -t $THREADS -d 10000 -w 3

# --- TPC-C ---
echo ""
echo "=== TPC-C ==="
TPCC="$BASE_DIR/TPCC/bin/tpcc_tinystm_wbctl"
run_bench "tpcc_default"   "$TPCC" -t $THREADS -d 10000

# --- Summary ---
echo ""
echo "============================================"
echo "  Test Summary"
echo "============================================"
echo "  PASSED: $PASSED"
echo "  FAILED: $FAILED"
echo "  HUNG:   $HUNG"
echo "  TOTAL:  $((PASSED + FAILED + HUNG))"
echo "============================================"
exit $((FAILED + HUNG))
