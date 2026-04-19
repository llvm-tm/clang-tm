#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x ./bin/types ] || [ ! -x ./bin/memtest ] || [ ! -x ./bin/nested ]; then
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
run_test nested "tm_begin outer" "tm_begin nested" "tm_end nested" "tm_end outer"

echo "All tests passed."
