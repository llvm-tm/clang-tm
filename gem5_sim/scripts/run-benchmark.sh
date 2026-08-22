#!/usr/bin/env bash
# Build and run a benchmark workload inside the gem5 simulation.
# This script is intended to be copied into the gem5 disk image
# or used as an after_boot.sh script.
set -euo pipefail

WORKLOAD_DIR="/home/gem5/benchmark"
WORKLOAD="${1:-array_sum}"

cd "$WORKLOAD_DIR"

echo "Building $WORKLOAD..."
make "$WORKLOAD"

echo "Running $WORKLOAD..."
m5 resetstats
./"$WORKLOAD"
m5 dumpstats

echo "Benchmark complete."
m5 exit
