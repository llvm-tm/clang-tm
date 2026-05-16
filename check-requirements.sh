#!/usr/bin/env bash
# ============================================================
# check-requirements.sh — Verify all tools needed by TM API C++
# ============================================================

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass() { echo -e "${GREEN}PASS${NC}: $1"; }
fail() { echo -e "${RED}FAIL${NC}: $1"; exit 1; }

echo "=== Checking software requirements ==="
echo ""

# LLVM / opt
if command -v opt &>/dev/null; then
    ver=$(opt --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || echo "unknown")
    pass "opt found (version $ver)"
else
    fail "opt not found (install llvm)"
fi

# clang++
if command -v clang++ &>/dev/null; then
    ver=$(clang++ --version 2>&1 | head -1)
    pass "clang++ found ($ver)"
else
    fail "clang++ not found"
fi

# llvm-link
if command -v llvm-link &>/dev/null; then
    pass "llvm-link found"
else
    fail "llvm-link not found"
fi

# make
if command -v make &>/dev/null; then
    pass "make found"
else
    fail "make not found"
fi

# bash
if command -v bash &>/dev/null; then
    pass "bash found"
else
    fail "bash not found"
fi

# gtimeout / timeout
if command -v gtimeout &>/dev/null; then
    pass "gtimeout found (macOS coreutils)"
elif command -v timeout &>/dev/null; then
    pass "timeout found (Linux)"
else
    echo "  WARN: neither gtimeout nor timeout found (optional, for test harness)"
fi

# LLVM plugin .so
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="$SCRIPT_DIR/llvm_tm_plugin/bin/libTMInstrument.so"
if [ -f "$PLUGIN" ]; then
    pass "TM plugin found at $PLUGIN"
else
    echo "  WARN: TM plugin not built yet (run 'make' in llvm_tm_plugin/)"
fi

# backends
BACKENDS="$SCRIPT_DIR/backends"
if [ -d "$BACKENDS/runtimes" ]; then
    pass "Backend runtimes directory found"
else
    fail "Backend runtimes directory not found"
fi

# C++20 support test
echo "#include <atomic>" > /tmp/tm_test_cpp20.cpp
echo "thread_local bool test_var = false;" >> /tmp/tm_test_cpp20.cpp
echo "int main() { std::atomic<int> x{0}; return x.load(); }" >> /tmp/tm_test_cpp20.cpp
if clang++ -std=c++20 /tmp/tm_test_cpp20.cpp -o /tmp/tm_test_cpp20 2>/dev/null; then
    pass "C++20 compilation works"
    rm -f /tmp/tm_test_cpp20 /tmp/tm_test_cpp20.cpp
else
    rm -f /tmp/tm_test_cpp20.cpp
    fail "C++20 compilation failed"
fi

echo ""
echo "=== All checks passed ==="
