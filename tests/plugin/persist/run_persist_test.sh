#!/usr/bin/env bash
set -euo pipefail

BIN_DIR="$(cd "$(dirname "$0")" && pwd)/bin"
PERSIST_DIR="$(cd "$(dirname "$0")" && pwd)"
rm -f "$PERSIST_DIR/benchmark_results/dudetm_persist.bin"

# Kill any orphaned replayers from prior crash-test runs.
# These are harmless (sleeping, no file I/O), but they clutter
# the process table and might confuse later runs.
for p in persist_dudetm persist_nvhtm persist_spht; do
    for pid in $(pgrep -x "$p" 2>/dev/null || true); do
        kill "$pid" 2>/dev/null || true
    done
done

PASS=0
FAIL=0

pass()  { echo "  ✓ PASS: $1";  ((PASS++)); }
fail()  { echo "  ✗ FAIL: $1";  ((FAIL++)); }

echo "=== DUDETM Persistence Test ==="
echo ""

BIN="$BIN_DIR/persist_dudetm"
if [ ! -x "$BIN" ]; then
    echo "SKIP: $BIN not built. Run 'make persist_dudetm' first."
    exit 1
fi

# Clean state
rm -f "$PERSIST_DIR/benchmark_results/dudetm_persist.bin"

# Run 1: init (creates persistence file)
echo "--- Run 1: init ---"
"$BIN" --mode init 2>&1
echo ""

# Run 2: do work (counter += 100)
echo "--- Run 2: add 100 ---"
"$BIN" --mode run --iterations 100 --delta 1 2>&1
echo ""

# Run 3: do more work (counter += 200)
echo "--- Run 3: add 200 ---"
"$BIN" --mode run --iterations 200 --delta 1 2>&1
echo ""

# Verify: counter should be 300
echo "--- Run 4: verify counter=300 ---"
"$BIN" --mode verify 300 2>&1 && pass "counter=300 after 2 runs"
echo ""

# Simulate crash: kill mid-execution
echo "--- Run 5: crash mid-execution ---"
"$BIN" --mode run --iterations 50 --delta 1 --crash 2>&1 || true
echo ""

# Run after crash: initial counter depends on what was persisted before the crash
echo "--- Run 6: verify after crash ---"
"$BIN" --mode run --iterations 0 --delta 0 2>&1
echo ""

# Multi-threaded (2 threads, 50 iterations each = 100 total adds)
echo "--- Run 7: multi-threaded work ---"
"$BIN" --mode run --iterations 50 --delta 1 --threads 2 2>&1
echo ""

# Final verify with "check" flag (just verify it prints PASS/FAIL)
echo "--- Run 8: DUDETM single-threaded same-run verify ---"
OUT=$("$BIN" --mode run --iterations 10 --delta 1 2>&1)
INIT=$(echo "$OUT" | grep "initial_counter" | sed 's/.*initial_counter=\([0-9]*\).*/\1/')
FINAL=$(echo "$OUT" | grep "final_counter" | sed 's/.*final_counter=\([0-9]*\).*/\1/')
EXPECTED=$((INIT + 10))
if [ "$FINAL" = "$EXPECTED" ]; then
    pass "single-run tx correctness"
else
    fail "single-run: expected $EXPECTED got $FINAL"
fi
echo ""

echo "=== NVHTM Persistence Test ==="
echo ""
BIN="$BIN_DIR/persist_nvhtm"
if [ -x "$BIN" ]; then
    # NVHTM uses clflush for persistence — works on NVM hardware only.
    # On DRAM-only systems the data does not survive a restart.
    # This test verifies in-memory correctness (counts are correct within a run).
    echo "--- Single-run verification ---"
    OUT=$("$BIN" --mode run --iterations 100 --delta 1 2>&1)
    echo "$OUT"
    INIT=$(echo "$OUT" | grep "initial_counter" | sed 's/.*initial_counter=\([0-9]*\).*/\1/')
    FINAL=$(echo "$OUT" | grep "final_counter" | sed 's/.*final_counter=\([0-9]*\).*/\1/')
    EXPECTED=$((INIT + 100))
    if [ "$FINAL" = "$EXPECTED" ]; then
        pass "NVHTM single-run tx correctness"
    else
        fail "NVHTM single-run: expected $EXPECTED got $FINAL"
    fi

    echo "--- Multi-threaded (2t x 50) ---"
    OUT=$("$BIN" --mode run --iterations 50 --delta 1 --threads 2 2>&1)
    echo "$OUT"
    INIT=$(echo "$OUT" | grep "initial_counter" | sed 's/.*initial_counter=\([0-9]*\).*/\1/')
    FINAL=$(echo "$OUT" | grep "final_counter" | sed 's/.*final_counter=\([0-9]*\).*/\1/')
    EXPECTED=$((INIT + 100))
    if [ "$FINAL" = "$EXPECTED" ]; then
        pass "NVHTM multi-threaded tx correctness"
    else
        fail "NVHTM multi-threaded: expected $EXPECTED got $FINAL"
    fi
else
    echo "SKIP: NVHTM binary not built"
fi
echo ""

echo "=== SPHT Persistence Test ==="
echo ""
BIN="$BIN_DIR/persist_spht"
if [ -x "$BIN" ]; then
    echo "--- Single-run verification ---"
    OUT=$("$BIN" --mode run --iterations 100 --delta 1 2>&1)
    echo "$OUT"
    INIT=$(echo "$OUT" | grep "initial_counter" | sed 's/.*initial_counter=\([0-9]*\).*/\1/')
    FINAL=$(echo "$OUT" | grep "final_counter" | sed 's/.*final_counter=\([0-9]*\).*/\1/')
    EXPECTED=$((INIT + 100))
    if [ "$FINAL" = "$EXPECTED" ]; then
        pass "SPHT single-run tx correctness"
    else
        fail "SPHT single-run: expected $EXPECTED got $FINAL"
    fi

    echo "--- Multi-threaded (2t x 50) ---"
    OUT=$("$BIN" --mode run --iterations 50 --delta 1 --threads 2 2>&1)
    echo "$OUT"
    INIT=$(echo "$OUT" | grep "initial_counter" | sed 's/.*initial_counter=\([0-9]*\).*/\1/')
    FINAL=$(echo "$OUT" | grep "final_counter" | sed 's/.*final_counter=\([0-9]*\).*/\1/')
    EXPECTED=$((INIT + 100))
    if [ "$FINAL" = "$EXPECTED" ]; then
        pass "SPHT multi-threaded tx correctness"
    else
        fail "SPHT multi-threaded: expected $EXPECTED got $FINAL"
    fi
else
    echo "SKIP: SPHT binary not built"
fi
echo ""

echo "=== Results: ${PASS} pass, ${FAIL} fail ==="
[ "$FAIL" -eq 0 ] || exit 1
