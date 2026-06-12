#!/bin/bash
# Test that TinySTM backends exit cleanly with multiple threads
set -euo pipefail

BENCH_DIR="../../benchmarks/plugin/stmbench7"
BIN="$BENCH_DIR/bin"
TIMEOUT=30
DURATION=3000
# Write-heavy workloads need more time due to lock contention
TIMEOUT_WRITE=45

pass=0
fail=0

run_test() {
    local name="$1"
    local binary="$2"
    shift 2

    # Use longer timeout for write-heavy workloads (w=2, w=3)
    local to=$TIMEOUT
    local prev=""
    for arg in "$@"; do
        if [[ "$prev" == "-w" && ("$arg" == "2" || "$arg" == "3") ]]; then
            to=$TIMEOUT_WRITE
        fi
        if [[ "$prev" == "-t" && "$arg" == "4" ]]; then
            to=$TIMEOUT_WRITE
        fi
        prev="$arg"
    done

    echo -n "  $name ... "
    if timeout $to "$binary" -d $DURATION "$@" 2>/dev/null | grep -q "^Results"; then
        echo "PASS"
        pass=$((pass + 1))
    else
        echo "FAIL (no Results output)"
        fail=$((fail + 1))
    fi
}

echo "=== TinySTM Plugin Hang Tests ==="
echo

for backend in wbctl wbetl wt; do
    binary="$BIN/stmbench_tinystm_$backend"
    echo "--- Backend: tinystm_$backend ---"

    # Single thread (baseline)
    run_test "1 thread, w=1" "$binary" -t 1

    # Multi-thread (should not hang)
    run_test "2 threads, w=1" "$binary" -t 2
    run_test "4 threads, w=1" "$binary" -t 4

    # All workloads
    run_test "2 threads, w=2" "$binary" -t 2 -w 2
    run_test "2 threads, w=3" "$binary" -t 2 -w 3
    echo
done

echo "=== Summary: $pass passed, $fail failed ==="
exit $fail
