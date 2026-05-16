# llvm-tool-helper.sh — Discover LLVM tools (handles versioned installs)
#
# Source this in shell scripts to get $LLVM_CXX, $LLVM_OPT, etc.
# Looks for llvm-config-22, llvm-config-22.1, llvm-config in order,
# then derives tool paths from --bindir output. Falls back to bare names.
#
# Usage:
#   SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
#   source "$SCRIPT_DIR/llvm-tool-helper.sh"

LLVM_BINDIR=""
for cfg in llvm-config-22 llvm-config-22.1 llvm-config; do
    if command -v "$cfg" &>/dev/null; then
        LLVM_BINDIR="$("$cfg" --bindir 2>/dev/null || true)"
        [ -n "$LLVM_BINDIR" ] && break
    fi
done

if [ -z "$LLVM_BINDIR" ]; then
    for dir in /usr/lib/llvm-22/bin /usr/lib/llvm-20/bin /usr/local/opt/llvm/bin; do
        if [ -x "$dir/llvm-config" ]; then
            LLVM_BINDIR="$dir"
            break
        fi
    done
fi

LLVM_CXX="${LLVM_BINDIR:+$LLVM_BINDIR/}clang++"
LLVM_OPT="${LLVM_BINDIR:+$LLVM_BINDIR/}opt"
LLVM_LINK="${LLVM_BINDIR:+$LLVM_BINDIR/}llvm-link"
LLVM_DIS="${LLVM_BINDIR:+$LLVM_BINDIR/}llvm-dis"
LLVM_CC="${LLVM_BINDIR:+$LLVM_BINDIR/}clang"
LLVM_CONFIG="${LLVM_BINDIR:+$LLVM_BINDIR/}llvm-config"
