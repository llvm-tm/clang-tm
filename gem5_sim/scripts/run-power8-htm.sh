#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GEM5_ROOT="${GEM5_ROOT:-$PROJECT_ROOT/gem5}"
GEM5_BINARY="${GEM5_BINARY:-$GEM5_ROOT/build/ALL/gem5.opt}"
CONFIG="$PROJECT_ROOT/configs/power8-htm.py"
OUTDIR="${OUTDIR:-$PROJECT_ROOT/m5out/power8-htm}"
BENCHMARK="${BENCHMARK:-array_sum_bench}"

if [ ! -f "$GEM5_BINARY" ]; then
    echo "gem5 binary not found at $GEM5_BINARY"
    echo "Build it first:"
    echo "  GEM5_ROOT=$GEM5_ROOT ./scripts/build.sh all"
    exit 1
fi

BINARY_PATH="$PROJECT_ROOT/workloads/power8/$BENCHMARK"
if [ ! -f "$BINARY_PATH" ]; then
    echo "Benchmark binary not found: $BINARY_PATH"
    echo "Build it: make -C $PROJECT_ROOT/workloads/power8 $BENCHMARK"
    exit 1
fi

mkdir -p "$OUTDIR"

echo "=========================================="
echo " POWER8 HTM Simulation"
echo " Binary:  $GEM5_BINARY"
echo " Config:  $CONFIG"
echo " Outdir:  $OUTDIR"
echo " Workload: $BENCHMARK"
echo "=========================================="
echo ""

exec "$GEM5_BINARY" --outdir="$OUTDIR" "$CONFIG"
