#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x ./bin/types ] || [ ! -x ./bin/memtest ] || [ ! -x ./bin/nested ] || [ ! -x ./bin/threads ] || [ ! -x ./bin/persist ] || [ ! -x ./bin/retry ] || [ ! -x ./bin/test_stl_containers ]; then
  echo "Error: test binaries are missing. Run 'make test' first." >&2
  exit 1
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

run_test types \
  "tm_write_i1" "tm_read_i1" "tm_write_i2" "tm_read_i2" \
  "tm_write_i4" "tm_read_i4" "tm_write_i8" "tm_read_i8" \
  "tm_write_f4" "tm_read_f4" "tm_write_f8" "tm_read_f8" \
  "tm_write_ptr" "tm_read_ptr"

run_test memtest "tm_memset" "tm_write_z" "tm_read_z"
run_test nested "tm_begin outer" "tm_end outer"
run_test threads "tm_init_thread" "tm_begin outer" "tm_read_i4" "tm_write_i4" "tm_end outer" "tm_threads test: PASSED"

# Persistent test: run twice to verify persistence
echo "Running persist (first run)..."
rm -f /tmp/tm_persistent_state.bin
./bin/persist > out/persist_run1.txt
echo "Running persist (second run - should continue from run1)..."
./bin/persist > out/persist_run2.txt

# Verify second run loaded the persisted value
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

# STL container test
run_test test_stl_containers "STL Container Test" "All tests passed"

# Retry test: verifies longjmp/sigsetjmp for transaction retry
run_test retry "retry" "longjmp" "retry detected" "Test PASSED"

echo "All tests passed."
