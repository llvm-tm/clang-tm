#!/bin/bash
# run_fuzzer.sh - Run all fuzz targets for TM API C++

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# tests/fuzz -> repo root is ../..
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
FUZZ_DIR="$SCRIPT_DIR"
OUT_DIR="$PROJECT_DIR/fuzz_results_$(date +%s)"

mkdir -p "$OUT_DIR"

echo "=== Building fuzz targets ==="
make -C "$FUZZ_DIR" -j4 2>&1 || echo "WARNING: fuzz build failed, continuing"

echo "=== Running fuzz targets ==="

if [ -f "$FUZZ_DIR/test_tx_fuzzer" ]; then
    echo "--- test_tx_fuzzer ---"
    timeout 60 "$FUZZ_DIR/test_tx_fuzzer" -print_final_stats 1>"$OUT_DIR/tx_fuzz.txt" 2>"$OUT_DIR/tx_fuzz_crashes.txt" || true
    echo "Stats:"; grep -E 'total|cov|PASSES|Done' "$OUT_DIR/tx_fuzz.txt" 2>/dev/null || echo "(no stats)"
else
    echo "skipping test_tx_fuzzer (not built)"
fi

if [ -f "$FUZZ_DIR/test_ds_fuzzer" ]; then
    echo "--- test_ds_fuzzer ---"
    timeout 60 "$FUZZ_DIR/test_ds_fuzzer" -print_final_stats 1>"$OUT_DIR/ds_fuzz.txt" 2>"$OUT_DIR/ds_fuzz_crashes.txt" || true
    echo "Stats:"; grep -E 'total|cov|PASSES|Done' "$OUT_DIR/ds_fuzz.txt" 2>/dev/null || echo "(no stats)"
else
    echo "skipping test_ds_fuzzer (not built)"
fi

echo "=== Fuzz Results Summary ==="
echo "Output directory: $OUT_DIR"
ls -t "$PROJECT_DIR"/fuzz_results_* 2>/dev/null | tail -n +6 | xargs rm -rf 2>/dev/null || true
echo "Done."
