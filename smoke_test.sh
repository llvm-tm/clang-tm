#!/usr/bin/env bash
# ============================================================================
# Smoke test: build and run all benchmarks across all TM backends
# ============================================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/benchmarks/cpp"

BACKENDS=(TINYSTM WBETL WT NOREC SWISSTM TL2 SGL)
ALL_PASS=0
ALL_FAIL=0
ALL_SKIP=0

# Per-benchmark run args (override via --test flag)
args_for() {
  case $1 in
    bank)       echo "-d 100 -a 64 -t 2 --test" ;;
    eigenbench) echo "-d 100 -t 2 --test" ;;
    stmbench7)  echo "--test 2 500" ;;
    vacation)   echo "-p 2 -r 16384 -n 2 -u 98 -t 1024 --test" ;;
    labyrinth)  echo "-p 2 -x 4 -y 4 -z 4 -n 32 --test" ;;
    kmeans)     echo "-p 2 -k 8 -d 2 -n 512 --test" ;;
    genome)     echo "-p 2 -g 4096 -s 32 -n 262144 --test" ;;
    intruder)   echo "-p 2 -a 10 -l 64 -n 262144 -s 1 --test" ;;
    ssca2)      echo "-p 2 -s 10 -u 1.0 -l 3 -m 3 -i 3 --test" ;;
    bayes)      echo "-t 2 -v 16 -r 128 -n 2 -p 2 -e 2 --test" ;;
    yada)       echo "-t 2 -a 10 --test" ;;
    tpcc)       echo "-t 2 -d 100 --test" ;;
    ycsb)       echo "-t 2 -d 100 --test" ;;
    test_ds)    echo "" ;;
    test_tx)    echo "" ;;
    *)          echo "" ;;
  esac
}

BENCHMARKS=(bank eigenbench stmbench7 vacation labyrinth kmeans \
            genome intruder ssca2 bayes yada tpcc ycsb test_ds test_tx)

for be in "${BACKENDS[@]}"; do
  echo ""
  echo "═══════════════════════════════════════════════"
  echo "  Backend: $be"
  echo "═══════════════════════════════════════════════"

  # Build
  if ! make -j4 BACKEND="$be" > /tmp/build.log 2>&1; then
    echo "  BUILD FAIL"
    tail -5 /tmp/build.log
    ALL_FAIL=$((ALL_FAIL + ${#BENCHMARKS[@]}))
    continue
  fi

  BE_PASS=0
  BE_FAIL=0

  for bm in "${BENCHMARKS[@]}"; do
    args=$(args_for "$bm")
    if ./bin/"$bm" $args > /tmp/run.log 2>&1; then
      echo "  PASS: $bm"
      BE_PASS=$((BE_PASS + 1))
    else
      echo "  FAIL: $bm (exit=$?)"
      # Print first/last lines of output on failure
      head -3 /tmp/run.log 2>/dev/null
      tail -3 /tmp/run.log 2>/dev/null
      BE_FAIL=$((BE_FAIL + 1))
    fi
  done

  echo "  ── $be: $BE_PASS pass, $BE_FAIL fail ──"
  ALL_PASS=$((ALL_PASS + BE_PASS))
  ALL_FAIL=$((ALL_FAIL + BE_FAIL))
done

echo ""
echo "═══════════════════════════════════════════════"
echo "  TOTAL: $ALL_PASS pass, $ALL_FAIL fail"
echo "═══════════════════════════════════════════════"

# Also run backend unit tests
echo ""
echo "───────────────────────────────────────────────"
echo "  Backend unit tests"
echo "───────────────────────────────────────────────"
cd "$SCRIPT_DIR/tests/backends/tm_impl"
if make -j4 run > /tmp/unit.log 2>&1; then
  UNIT_PASS=$(grep -c "PASS" /tmp/unit.log 2>/dev/null || echo 0)
  UNIT_FAIL=$(grep -c "FAIL" /tmp/unit.log 2>/dev/null || echo 0)
  echo "  Unit tests: $(grep -c 'PASS\|FAIL' /tmp/unit.log) results"
  ALL_PASS=$((ALL_PASS + UNIT_PASS))
  ALL_FAIL=$((ALL_FAIL + UNIT_FAIL))
else
  echo "  Unit tests: BUILD FAIL"
  tail -10 /tmp/unit.log
fi

exit $ALL_FAIL
