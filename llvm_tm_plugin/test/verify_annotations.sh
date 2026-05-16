#!/usr/bin/env bash
# verify_annotations.sh
# Verifies that the TM plugin detected TM/TX annotations in LLVM IR
# by checking the instrumented output for TM runtime calls.

set -euo pipefail

BC_FILE="$1"
LOG_FILE="$2"

if [ -z "$BC_FILE" ] || [ -z "$LOG_FILE" ]; then
    echo "Usage: $0 <bitcode_file> <log_file>" >&2
    exit 1
fi

if [ ! -f "$BC_FILE" ]; then
    echo "ERROR: Bitcode file not found: $BC_FILE" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/../llvm-tool-helper.sh"
PLUGIN_LIB="$SCRIPT_DIR/../bin/libTMInstrument.so"

if [ ! -f "$PLUGIN_LIB" ]; then
    echo "ERROR: Plugin not found: $PLUGIN_LIB" >&2
    exit 1
fi

# Instrument to a temp file (don't use /dev/null so we can inspect the output)
INSTR_BC="$(dirname "$BC_FILE")/$(basename "$BC_FILE" .bc)_verify.bc"
$LLVM_OPT -load-pass-plugin="$PLUGIN_LIB" \
    -passes="tm-instrument" \
    "$BC_FILE" -o "$INSTR_BC" 2>"$LOG_FILE"

# Disassemble the instrumented IR and check for TM calls
$LLVM_DIS "$INSTR_BC" -o /dev/stdout 2>/dev/null > "${INSTR_BC}.ll" || true

if grep -qE "call.*@tm_begin|call.*@tm_init|call.*@tm_read_|call.*@tm_write_" "${INSTR_BC}.ll" 2>/dev/null; then
    echo "TM instrumentation verified for $BC_FILE" >&2
    rm -f "$INSTR_BC" "${INSTR_BC}.ll"
    exit 0
fi

echo "ERROR: No TM instrumentation found in $BC_FILE!" >&2
echo "The plugin failed to instrument the code." >&2
rm -f "$INSTR_BC" "${INSTR_BC}.ll"
exit 1
