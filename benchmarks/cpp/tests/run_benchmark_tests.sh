#!/usr/bin/env bash
# Run all benchmark self-tests (--test mode).
# Usage: ./tests/run_benchmark_tests.sh [benchmark_name...]
#   Without args, runs all benchmarks.
#   With args, runs only the named benchmarks.

set -euo pipefail

BENCHMARKS=(
    yada kmeans vacation genome intruder labyrinth bayes ssca2
    stmbench7 tpcc ycsb
    fuzz_counter fuzz_bank
    bank eigenbench
)

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"

# Portable timeout: use gtimeout (macOS coreutils) if available
TIMEOUT_CMD="timeout"
if ! command -v timeout &>/dev/null && command -v gtimeout &>/dev/null; then
    TIMEOUT_CMD="gtimeout"
fi

FAILED=()
PASSED=()

if [ $# -gt 0 ]; then
    BENCHMARKS=("$@")
fi

echo "=========================================="
echo "  Benchmark Self-Test Suite"
echo "=========================================="
echo ""

for name in "${BENCHMARKS[@]}"; do
    binary="$BIN/$name"
    if [ ! -x "$binary" ]; then
        echo "  [SKIP] $name (binary not found)"
        echo ""
        continue
    fi

    echo "  Running: $name --test"
    if $TIMEOUT_CMD 30 "$binary" --test > /tmp/btest_${name}.out 2>&1; then
        echo "  [PASS] $name"
        PASSED+=("$name")
    else
        echo "  [FAIL] $name (exit code $?)"
        cat /tmp/btest_${name}.out
        FAILED+=("$name")
    fi
    echo ""
done

echo "=========================================="
echo "  Results: ${#PASSED[@]} passed, ${#FAILED[@]} failed"
echo "=========================================="

if [ ${#FAILED[@]} -gt 0 ]; then
    echo "  FAILED: ${FAILED[*]}"
    exit 1
fi
exit 0
