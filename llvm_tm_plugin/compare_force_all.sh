#!/usr/bin/env bash
# Compare FORCE_ALL_TM_READ_WRITE vs normal instrumentation.
# Usage: compare_force_all.sh <test_name>
#   e.g., compare_force_all.sh test_tm_simple
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <test_name> [test_args...]"
    echo "Compares normal vs FORCE_ALL_TM_READ_WRITE instrumentation for a test."
    exit 1
fi

TEST_NAME="$1"
shift || true
SRC="test/${TEST_NAME}.cpp"
if [ ! -f "$SRC" ]; then
    echo "Error: source not found: $SRC"
    exit 1
fi

mkdir -p out_release out_force_all

# Save current TM_PLUGIN override
OLD_PLUGIN="${TM_PLUGIN:-}"

compile_and_compare() {
    local variant="$1"
    local plugin="$2"
    local outdir="out_${variant}"
    local bc1="${outdir}/${TEST_NAME}.bc"
    local bc2="${outdir}/${TEST_NAME}.instr.bc"
    local bc3="${outdir}/${TEST_NAME}.opt.bc"
    local ir="${outdir}/${TEST_NAME}.ll"
    local bin="${outdir}/${TEST_NAME}"

    clang++ -std=c++20 -O1 -fno-inline -fno-vectorize -fno-slp-vectorize \
        -fno-unroll-loops -fno-stack-protector -pthread -emit-llvm -c "$SRC" -o "$bc1"
    TM_PLUGIN="$plugin" \
    opt -load-pass-plugin="$plugin" -passes="tm-instrument-inline" "$bc1" -S -o "$ir"
    opt -O3 "$ir" -o "$bc3"
    clang++ -std=c++20 -O1 -pthread "$bc3" runtime/tm_runtime.cpp -o "$bin"
}

# Build both variants
echo "Building release variant..."
compile_and_compare "release" "bin/libTMInstrument.so"
echo "Building force_all variant..."
compile_and_compare "force_all" "bin/libTMInstrument_force_all.so"

# Compare IR
RELEASE_IR="out_release/${TEST_NAME}.ll"
FORCE_ALL_IR="out_force_all/${TEST_NAME}.ll"

echo ""
echo "======================================"
echo "Instrumentation comparison for $TEST_NAME"
echo "======================================"
echo ""

RELEASE_COUNT=$(grep -c "@tm_read_\|@tm_write_" "$RELEASE_IR" || echo 0)
FORCE_ALL_COUNT=$(grep -c "@tm_read_\|@tm_write_" "$FORCE_ALL_IR" || echo 0)
DIFF=$((FORCE_ALL_COUNT - RELEASE_COUNT))
PCT=$(awk "BEGIN {printf \"%.1f\", ($RELEASE_COUNT > 0) ? ($FORCE_ALL_COUNT / $RELEASE_COUNT - 1) * 100 : 0}")
echo "TM call sites:"
echo "  release:   $RELEASE_COUNT"
echo "  force_all: $FORCE_ALL_COUNT"
echo "  delta:     +$DIFF (+${PCT}%)"
echo ""

# Identify extra loads/stores
echo "Extra instrumented loads (present only in force_all):"
if [ "$RELEASE_COUNT" -gt 0 ]; then
    grep -o "@tm_read_[a-z0-9]*\|@tm_write_[a-z0-9]*" "$RELEASE_IR" | sort > /tmp/tm_release_$$.txt
    grep -o "@tm_read_[a-z0-9]*\|@tm_write_[a-z0-9]*" "$FORCE_ALL_IR" | sort > /tmp/tm_force_$$.txt
    diff /tmp/tm_release_$$.txt /tmp/tm_force_$$.txt 2>/dev/null | grep '^>' || echo "  (none)"
    rm -f /tmp/tm_release_$$.txt /tmp/tm_force_$$.txt
fi
echo ""

# Run both
echo "Running release variant..."
out_release/${TEST_NAME} "$@" 2>&1 | grep -E '(PASS|FAIL|passed|failed)' || echo "  exit code: $?"
echo ""
echo "Running force_all variant..."
out_force_all/${TEST_NAME} "$@" 2>&1 | grep -E '(PASS|FAIL|passed|failed)' || echo "  exit code: $?"
echo ""

# Cleanup
rm -rf out_release out_force_all
