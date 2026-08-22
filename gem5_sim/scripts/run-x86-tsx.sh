#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GEM5_ROOT="${GEM5_ROOT:-$PROJECT_ROOT/gem5}"
GEM5_BINARY="${GEM5_BINARY:-$GEM5_ROOT/build/X86_TSX/gem5.opt}"
CONFIG="$PROJECT_ROOT/configs/x86-tsx-kvm.py"
OUTDIR="${OUTDIR:-$PROJECT_ROOT/m5out/x86-tsx}"

if [ ! -f "$GEM5_BINARY" ]; then
    echo "gem5 binary not found at $GEM5_BINARY"
    echo "Build it first:"
    echo "  GEM5_ROOT=$GEM5_ROOT ./scripts/build.sh x86-tsx --patch /path/to/tsx.patch"
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "Config not found: $CONFIG"
    echo "Create an x86 TSX configuration script."
    exit 1
fi

mkdir -p "$OUTDIR"

echo "=========================================="
echo " x86 TSX Simulation"
echo " Binary:  $GEM5_BINARY"
echo " Config:  $CONFIG"
echo " Outdir:  $OUTDIR"
echo ""
echo " NOTE: The x86 TSX patch only supports the"
echo " O3 CPU model. KVM fast-forward requires"
echo " a Linux host with /dev/kvm."
echo "=========================================="
echo ""

exec "$GEM5_BINARY" --outdir="$OUTDIR" "$CONFIG"
