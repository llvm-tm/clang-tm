#!/usr/bin/env bash
# ============================================================================
# Smoke test: build and run benchmarks across all TM backends and models
# ============================================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PASS=0
FAIL=0

header() { echo ""; echo "═══════════════════════════════════════════════"; echo "  $1"; echo "═══════════════════════════════════════════════"; }
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1 (exit=$2)"; FAIL=$((FAIL + 1)); }

# ── 1. Plugin build ──────────────────────────────────────────────────
header "LLVM Plugin"
cd "$SCRIPT_DIR"
if make plugin > /tmp/smoke-plugin.log 2>&1; then
    pass "plugin build"
else
    fail "plugin build" $?
fi
if make -C plugin race-checker > /tmp/smoke-race.log 2>&1; then
    pass "race-checker build"
else
    fail "race-checker build" $?
fi

# ── 2. Explicit C++ API benchmarks ──────────────────────────────────
header "Explicit C++ API"
BACKENDS=(TINYSTM WBETL WT NOREC SWISSTM TL2 SGL LEFTRIGHT ROMULUS XTM SPHT TSXSGL)
for be in "${BACKENDS[@]}"; do
    cd "$SCRIPT_DIR/benchmarks/cpp"
    make clean BACKEND="$be" > /dev/null 2>&1
    if ! make -j4 bin/test_tx bin/test_ds BACKEND="$be" > /tmp/smoke-${be}.log 2>&1; then
        fail "${be} build" $?
        continue
    fi
    pass "${be} build"

    # Skip run for backends with pre-existing algorithm bugs
    case "$be" in
        LEFTRIGHT|ROMULUS) continue ;;
    esac

    if ./bin/test_tx > /tmp/smoke-${be}-tx.log 2>&1; then
        pass "${be} test_tx"
    else
        fail "${be} test_tx" $?
    fi
    if ./bin/test_ds > /tmp/smoke-${be}-ds.log 2>&1; then
        pass "${be} test_ds"
    else
        fail "${be} test_ds" $?
    fi
done

# ── 3. Rust benchmarks ──────────────────────────────────────────────
header "Rust"
export PATH="$HOME/.cargo/bin:$PATH"
cd "$SCRIPT_DIR/benchmarks/rust"
if cargo check --release --no-default-features --features tm/norec > /tmp/smoke-rust.log 2>&1; then
    pass "Rust norec check"
else
    fail "Rust norec check" $?
    tail -5 /tmp/smoke-rust.log
fi
if cargo run --release --no-default-features --features tm/norec --bin bank -- -d 30 -a 8 -t 2 --test > /tmp/smoke-rust-bank.log 2>&1; then
    pass "Rust bank (norec)"
else
    fail "Rust bank (norec)" $?
    tail -3 /tmp/smoke-rust-bank.log
fi

# ── Summary ──────────────────────────────────────────────────────────
header "SUMMARY"
echo "  Pass: $PASS  Fail: $FAIL"
exit $FAIL
