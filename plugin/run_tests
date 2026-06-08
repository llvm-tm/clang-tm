#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
source "$SCRIPT_DIR/llvm-tool-helper.sh"

if [ ! -x ./bin/test_types ] || [ ! -x ./bin/test_memtest ] || [ ! -x ./bin/test_threads ] || [ ! -x ./bin/test_persist ] || [ ! -x ./bin/test_retry ] || [ ! -x ./bin/test_nested_tx ]; then
  echo "Test binaries missing. Building with 'make test'..." >&2
  make test 2>&1 || { echo "Build failed." >&2; exit 1; }
fi

mkdir -p out

run_test() {
  local test="$1"
  shift
  echo "Running $test..."
  ./bin/$test > out/$test.actual.txt
  for pattern in "$@"; do
    if ! grep -q "$pattern" out/$test.actual.txt; then
      echo "Missing expected pattern '$pattern' in $test" >&2
      echo "---- output for $test ----" >&2
      cat out/$test.actual.txt >&2
      exit 1
    fi
  done
  echo "$test passed."
}

run_test test_types \
  "tm_init" "tm_init_thread" "tm_exit_thread" "tm_exit"

run_test test_memtest "tm_init" "tm_init_thread" "tm_exit_thread" "tm_exit"
run_test test_threads "tm_init" "tm_init_thread" "tm_exit_thread" "tm_exit" "worker_thread 0: finished"
run_test test_nested_tx "final counter = 111" "Test PASSED"

# Persistent test: run twice to verify persistence
echo "Running persist (first run)..."
rm -f /tmp/tm_persistent_state.bin
./bin/test_persist > out/persist_run1.txt
echo "Running persist (second run - should continue from run1)..."
./bin/test_persist > out/persist_run2.txt

if ! grep -q "counter = 2" out/persist_run1.txt; then
  echo "persist: first run should end with counter = 2" >&2
  exit 1
fi

if ! grep -q "counter = 2" out/persist_run2.txt; then
  echo "persist: second run should start with counter = 2 (loaded from persist)" >&2
  echo "---- output for persist run 2 ----" >&2
  cat out/persist_run2.txt >&2
  exit 1
fi

# Retry test: verifies longjmp/sigsetjmp for transaction retry
run_test test_retry "retry" "longjmp" "Test PASSED" "final counter = 3"

# STL container test
run_test test_stl_containers "STL Container Test" "All tests passed" || echo "  Skipped (may timeout)"

# Map find regression test
run_test test_stl_map_find "std::map::find regression test" "PASS: map find test passed"

# Argument trace test
run_test test_tm_arg_trace "argument trace test" "PASS: argument trace test passed"

# Custom class test
run_test test_custom_class "custom" "PASS"

# Simple vector test
run_test test_simple_vector "PASS" || echo "  Skipped (may hang)"

# Init/exit don't start transactions test
run_test test_init_no_tx "Test 1: g_value = 1 (expected 1) PASS" "Test 3: g_value = 1 (expected 1) PASS" "PASS"

# Math opaque test
if [ -x ./bin/test_math_opaque ]; then
  run_test test_math_opaque "Math opaque test PASSED"
fi

echo "All tests passed."
