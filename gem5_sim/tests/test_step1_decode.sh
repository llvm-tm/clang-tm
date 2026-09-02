#!/usr/bin/env bash
set -euo pipefail
# Step 1 gate: decoder + CPUID
GEM5=${1:-gem5_sim/gem5/build/X86_TSX/gem5.opt}
[ -x "$GEM5" ] || { echo "SKIP: $GEM5 not built"; exit 0; }
grep -q "XBegin" src/arch/x86/isa/decoder/one_byte_opcodes.isa && echo "PASS decode XBegin"
"$GEM5" --help | grep -q Ruby && echo "PASS Ruby"
echo "STEP 1 OK"
