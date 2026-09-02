#!/usr/bin/env bash
set -euo pipefail
# Step 4 gate: RR 0%, RW reader ~60% vs ground_truth
GEM5=${1:-gem5_sim/gem5/build/X86_TSX/gem5.opt}
OUT=/tmp/m5out; mkdir -p $OUT
for variant in RR RW WR WW; do
  echo "=== $variant ==="
  # freestanding probe compiled for gem5 SE (see benchmarks/tsx/)
  [ -x /tmp/tsx_${variant}_gem5 ] || echo "SKIP /tmp/tsx_${variant}_gem5 not built"
done
grep -E "htmTx|htmAbort" $OUT/stats.txt 2>/dev/null | head -20 || echo "no stats yet"
echo "STEP 4: compare vs benchmarks/tsx/ground_truth_intel14v2.txt"
