#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GEM5_ROOT="${GEM5_ROOT:-$PROJECT_ROOT/gem5}"
GEM5_BINARY="${GEM5_BINARY:-$GEM5_ROOT/build/ALL/gem5.opt}"
CONFIG="$PROJECT_ROOT/configs/arm-tme-kvm.py"
OUTDIR="${OUTDIR:-$PROJECT_ROOT/m5out/arm-tme}"

if [ ! -f "$GEM5_BINARY" ]; then
    echo "gem5 binary not found at $GEM5_BINARY"
    echo "Build it first:"
    echo "  GEM5_ROOT=$GEM5_ROOT ./scripts/build.sh all"
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "Config not found: $CONFIG"
    echo "Create an ARM TME configuration script."
    exit 1
fi

mkdir -p "$OUTDIR"

echo "=========================================="
echo " ARM TME Simulation"
echo " Binary:  $GEM5_BINARY"
echo " Config:  $CONFIG"
echo " Outdir:  $OUTDIR"
echo ""
echo " NOTE: ARM TME requires KVM for fast-forward"
echo " on Linux, or Atomic CPU as fallback."
echo "=========================================="
echo ""

exec "$GEM5_BINARY" --outdir="$OUTDIR" "$CONFIG"
