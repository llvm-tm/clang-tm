#!/usr/bin/env bash
# verify_annotations.sh
# Verifies that the TM plugin detected TM/TX annotations in LLVM IR
# Exits with non-zero code if annotations were NOT detected

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

# Find the plugin relative to the test directory
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_LIB="$SCRIPT_DIR/../bin/libTMInstrument.so"

if [ ! -f "$PLUGIN_LIB" ]; then
    echo "ERROR: Plugin not found: $PLUGIN_LIB" >&2
    exit 1
fi

# Run plugin and capture output
opt -load-pass-plugin="$PLUGIN_LIB" \
    -passes="tm-instrument" \
    "$BC_FILE" -o /dev/null 2>&1 | tee "$LOG_FILE"

# Check for TM-annotated symbols detection
if ! grep -qE "Found [1-9][0-9]* TM-annotated symbols" "$LOG_FILE"; then
    echo "ERROR: No TM-annotated symbols detected in $BC_FILE!" >&2
    echo "The plugin failed to detect TM annotations!" >&2
    exit 1
fi

# Check for transaction functions detection
if ! grep -qE "Instrumenting transaction function:" "$LOG_FILE"; then
    echo "ERROR: No transaction functions detected in $BC_FILE!" >&2
    echo "The plugin failed to detect TX annotations!" >&2
    exit 1
fi

echo "Annotation detection verified for $BC_FILE"
exit 0
