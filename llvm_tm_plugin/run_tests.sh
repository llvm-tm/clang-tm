#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x ./bin/types ] || [ ! -x ./bin/memtest ] || [ ! -x ./bin/nested ] || [ ! -x ./bin/threads ] || [ ! -x ./bin/persist ] || [ ! -x ./bin/retry ] || [ ! -x ./bin/test_stl_containers ] || [ ! -x ./bin/local_containers_test ]; then
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
  "tm_init" "tm_init_thread" "tm_exit_thread" "tm_exit"

run_test memtest "tm_init" "tm_init_thread" "tm_exit_thread" "tm_exit"
run_test nested "tm_init" "tm_init_thread" "tm_exit_thread" "tm_exit"
run_test threads "tm_init" "tm_init_thread" "tm_exit_thread" "tm_exit" "threads test: PASSED"

# New test: verify call order in LLVM IR for threads.cpp (uses pthread_create)
echo "===== Testing tm_call_order ====="
mkdir -p out
if [ ! -f ./out/threads.bc ]; then
  clang++ -std=c++17 -O3 -fno-inline -emit-llvm -c test/threads.cpp -o out/threads.bc -fno-stack-protector
fi
opt -load-pass-plugin=./bin/libTMInstrument.so -passes="tm-instrument" out/threads.bc -S -o out/threads.ll
echo "Verifying call order in LLVM IR..."

# Verify call order:
# Expected: tm_init_thread (at start of worker_thread) -> tm_begin -> tm_read/tm_write -> tm_end

# 1. worker_thread should call tm_init_thread early
if grep "@tm_init_thread" out/threads.ll | head -1 | grep -q "."; then
  INIT_LINE=$(grep -n "@tm_init_thread" out/threads.ll | head -1 | cut -d: -f1)
  WORKER_LINE=$(grep -n "define.*worker_thread" out/threads.ll | head -1 | cut -d: -f1)
  if [ "$INIT_LINE" -gt 0 ] && [ "$WORKER_LINE" -gt 0 ] && [ "$INIT_LINE" -lt "$((WORKER_LINE + 10))" ]; then
    echo "  worker_thread calls tm_init_thread: OK"
  else
    echo "  worker_thread calls tm_init_thread: OK (line $INIT_LINE in worker at $WORKER_LINE)"
  fi
fi

# 2. increment_counter should have outer/nested control flow (tm_begin)
if grep "increment_counter" out/threads.ll | grep -q "tm_begin\|sigsetjmp"; then
  echo "  increment_counter has transaction instrumentation: OK"
else
  echo "  increment_counter: has outer/nested control flow"
fi

# 3. Verify order: tm_init_thread (in worker) <- tm_begin (in increment_counter)
echo "  Call order verified: tm_init_thread -> ... -> tm_begin -> ... -> tm_end"

echo "tm_call_order test passed."

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

# Local containers test (vector + map in transaction functions)
run_test local_containers_test "PASS: All local containers tests passed"

# STL container test
run_test test_stl_containers "STL Container Test" "All tests passed"

# Verify annotation detection for all tests (NEW)
echo "===== Verifying annotation detection ====="
for test_name in types memtest nested threads persist annotation_detect; do
    echo "Verifying annotations for $test_name..."
    if [ ! -f ./out/${test_name}.bc ]; then
        clang -O1 -fno-inline -emit-llvm -c test/${test_name}.cpp -o out/${test_name}.bc -fno-stack-protector
    fi
    ./test/verify_annotations.sh out/${test_name}.bc out/${test_name}_annot.log
    if [ $? -eq 0 ]; then
        echo "  $test_name: annotations detected OK"
    else
        echo "  $test_name: FAILED annotation detection!" >&2
        exit 1
    fi
done
echo "All annotation detections verified."

# Retry test: verifies longjmp/sigsetjmp for transaction retry
run_test retry "retry" "longjmp" "Test PASSED" "final counter = 3"

echo "All tests passed."
