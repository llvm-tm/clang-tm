#!/bin/bash
# Comprehensive plugin test: detect all plugin issues
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$SCRIPT_DIR/../../plugin"
BIN_DIR="$PLUGIN_DIR/bin"
export PATH="$PLUGIN_DIR:$PATH"

pass=0
fail=0
TIMEOUT=10

run_test() {
    local name="$1"
    local binary="$2"
    shift 2

    echo -n "  $name ... "
    if timeout $TIMEOUT "$binary" "$@" 2>/dev/null >/dev/null; then
        echo "PASS"
        pass=$((pass + 1))
    else
        local rc=$?
        echo "FAIL (exit=$rc)"
        fail=$((fail + 1))
    fi
}

echo "=== Comprehensive Plugin Tests ==="
echo

# 1. Build core tests
echo "--- Building tests ---"
make -C "$PLUGIN_DIR" test 2>&1 | tail -1

echo
echo "--- Running tests ---"

# 2. Run each test
for test_bin in "$BIN_DIR"/test_*; do
    name="$(basename "$test_bin")"
    run_test "$name" "$test_bin"
done

echo
echo "=== Summary: $pass passed, $fail failed ==="
exit $fail
