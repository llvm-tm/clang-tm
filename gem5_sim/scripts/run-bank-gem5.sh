#!/usr/bin/env bash
# Build and run the tm_api_cpp `bank` benchmark (software TM backend, no TM
# ISA required) under gem5 SE mode with ROI-delimited, cycle-accurate stats.
#
# Usage:
#   ./run-bank-gem5.sh [BACKEND] [THREADS] [ACCOUNTS] [TXNS]
#   ./run-bank-gem5.sh NOREC 2 64 2000
#   TRACE=1 ./run-bank-gem5.sh NOREC 1 64 500     # also record TM trace
#
# Env overrides:
#   TM_API   path to tm_api_cpp repo   (default ../..)
#   GEM5_DIR path to gem5 checkout     (default ../gem5)
#   GEM5_BIN gem5 binary               (default $GEM5_DIR/build/X86_TSX/gem5.opt)
#   CLK      core clock                (default 1.8GHz, Broadwell-EP target)
#   CPU_TYPE timing|atomic|o3          (default timing)
#   TRACE    1 = set TM_TRACE_PATH in guest and convert to JSONL
set -euo pipefail

BACKEND=${1:-NOREC}
THREADS=${2:-1}
ACCOUNTS=${3:-64}
TXNS=${4:-2000}

HERE=$(cd "$(dirname "$0")" && pwd)
TSXC=$(cd "$HERE/.." && pwd)
GEM5_DIR=${GEM5_DIR:-$(cd "$HERE/../gem5" && pwd)}
GEM5_BIN=${GEM5_BIN:-$GEM5_DIR/build/X86_TSX/gem5.opt}
TM_API=${TM_API:-$(cd "$HERE/../.." && pwd)}
CLK=${CLK:-1.8GHz}
CPU_TYPE=${CPU_TYPE:-timing}
TRACE=${TRACE:-0}

BACKEND_LC=$(echo "$BACKEND" | tr 'A-Z' 'a-z')
BIN=$TM_API/benchmarks/cpp/bin/bank_gem5_$BACKEND_LC
TAG=bank-${BACKEND_LC}-t${THREADS}-a${ACCOUNTS}-n${TXNS}
OUT=$TSXC/m5out/$TAG

# --- 1. Build (static x86-64, musl, via Docker linux/amd64) ---------------
if [ ! -x "$BIN" ] || [ "${REBUILD:-0}" = "1" ]; then
    echo ">>> building $(basename "$BIN") (BACKEND=$BACKEND) via docker linux/amd64"
    docker run --rm --platform linux/amd64 \
        -v "$TM_API":/w -w /w/benchmarks/cpp alpine:3.20 \
        sh -c "apk add --no-cache g++ make >/dev/null 2>&1 && \
               make BACKEND=$BACKEND GEM5=1 bin/$(basename "$BIN")"
fi
file "$BIN" | grep -q "ELF 64-bit.*x86-64.*static" || {
    echo "ERROR: $BIN is not a static x86-64 ELF"; exit 1; }

# --- 1b. HTM fast-path pre-check (TSXSGL/SPHT built with -mrtm) --------
if [[ "$BACKEND" == "TSXSGL" || "$BACKEND" == "tsxsgl" || "$BACKEND" == "SPHT" || "$BACKEND" == "spht" ]]; then
    if ! llvm-objdump -d "$BIN" 2>/dev/null | grep -q "xbegin"; then
        echo "ERROR: $BIN lacks xbegin/xend (built without -mrtm) — rebuild with BACKEND=$BACKEND GEM5=1"
        exit 1
    fi
fi

# --- 2. Run gem5 -----------------------------------------------------------
ENVS=()
# Make HTM probe visible even under GEM5_M5OPS (tm_rtm.hpp: TM_RTM_DEBUG)
if [[ "$BACKEND" == "TSXSGL" || "$BACKEND" == "tsxsgl" || "$BACKEND" == "SPHT" || "$BACKEND" == "spht" ]]; then
    ENVS+=(--env "TM_RTM_DEBUG=1")
fi
if [ "$TRACE" = "1" ]; then
    # SE mode forwards guest file I/O to the host FS: the trace lands in $OUT.
    mkdir -p "$OUT"
    ENVS+=(--env "TM_TRACE_PATH=$OUT/tm_trace.txt")
fi

echo ">>> gem5: $TAG  (clk=$CLK cpu=$CPU_TYPE)"
rm -rf "$OUT"
# bash 3.2 (macOS) chokes on empty "${ENVS[@]}" under set -u
"$GEM5_BIN" -d "$OUT" "$TSXC/configs/x86-se-bank.py" \
    --binary "$BIN" --threads "$THREADS" --accounts "$ACCOUNTS" \
    --txns "$TXNS" --clk "$CLK" --cpu-type "$CPU_TYPE" ${ENVS[@]+"${ENVS[@]}"}

# --- 3. Verify + extract cycle-accurate ROI stats --------------------------
echo "=== guest output ==="
cat "$OUT/simout.txt"
grep -q "PASS: Money conserved" "$OUT/simout.txt" \
    && echo "=== RESULT: PASS (money conserved) ===" \
    || { echo "=== RESULT: FAIL ==="; exit 1; }

echo "=== ROI stats (benchmark phase only, between m5 markers) ==="
grep -E "^(simSeconds|simTicks|simInsts|simOps|system\.cpu\.numCycles|system\.cpu\.cpi|system\.cpu\.ipc)" \
    "$OUT/stats.txt" | head -20

if [ "$TRACE" = "1" ] && [ -f "$OUT/tm_trace.txt" ]; then
    echo "=== TM trace: $OUT/tm_trace.txt ($(wc -l < "$OUT/tm_trace.txt") events) ==="
fi
