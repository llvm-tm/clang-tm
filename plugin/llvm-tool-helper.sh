# llvm-tool-helper.sh — Discover LLVM tools (handles versioned installs)
#
# Source this in shell scripts to get $LLVM_CONFIG, $LLVM_CXX, $LLVM_OPT, etc.
#
# LLVM_VERSION selects the desired LLVM major version (default: 22). Search
# order is llvm-config-<major>, llvm-config-<major>.1, then the legacy
# llvm-config-22/22.1/generic fallbacks. If LLVM_VERSION is explicitly set,
# the discovered LLVM major must match it. This helper remains non-fatal when
# no LLVM toolchain is present; callers may check $LLVM_CONFIG.
#
# Usage:
#   SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
#   source "$SCRIPT_DIR/llvm-tool-helper.sh"

LLVM_VERSION_REQUESTED="${LLVM_VERSION:-}"
LLVM_VERSION="${LLVM_VERSION_REQUESTED:-22}"
LLVM_CONFIG=""

for cfg in llvm-config-"$LLVM_VERSION" llvm-config-"${LLVM_VERSION}.1" llvm-config-22 llvm-config-22.1 llvm-config; do
    if command -v "$cfg" &>/dev/null; then
        LLVM_CONFIG="$cfg"
        break
    fi
done

if [ -z "$LLVM_CONFIG" ]; then
    for dir in "/usr/lib/llvm-${LLVM_VERSION}/bin" "/usr/lib/llvm-22/bin" "/usr/lib/llvm-23/bin" "/usr/lib/llvm-20/bin" "/usr/local/opt/llvm/bin"; do
        if [ -x "$dir/llvm-config" ]; then
            LLVM_CONFIG="$dir/llvm-config"
            break
        fi
    done
fi

LLVM_BINDIR=""
if [ -n "$LLVM_CONFIG" ]; then
    LLVM_VER="$("$LLVM_CONFIG" --version 2>/dev/null || true)"
    LLVM_MAJOR="${LLVM_VER%%.*}"

    if [ -n "$LLVM_VERSION_REQUESTED" ]; then
        LLVM_VERSION_MAJOR_REQ="${LLVM_VERSION_REQUESTED%%.*}"
        if [ "$LLVM_VERSION_MAJOR_REQ" != "$LLVM_MAJOR" ]; then
            echo "llvm-tool-helper: LLVM $LLVM_VERSION_REQUESTED requested explicitly, but $LLVM_CONFIG reports $LLVM_VER" >&2
            return 1
        fi
    fi

    case "$LLVM_MAJOR" in
        ''|*[!0-9]*) ;;
        *)
            if [ "$LLVM_MAJOR" -lt 22 ]; then
                echo "llvm-tool-helper: LLVM 22+ required, found $LLVM_VER" >&2
                return 1
            fi
            ;;
    esac

    LLVM_BINDIR="$("$LLVM_CONFIG" --bindir 2>/dev/null || true)"
    if [ -z "$LLVM_BINDIR" ]; then
        LLVM_BINDIR="$(dirname "$LLVM_CONFIG")"
    fi
fi

LLVM_CXX="${LLVM_BINDIR:+$LLVM_BINDIR/}clang++"
LLVM_OPT="${LLVM_BINDIR:+$LLVM_BINDIR/}opt"
LLVM_LINK="${LLVM_BINDIR:+$LLVM_BINDIR/}llvm-link"
LLVM_DIS="${LLVM_BINDIR:+$LLVM_BINDIR/}llvm-dis"
LLVM_CC="${LLVM_BINDIR:+$LLVM_BINDIR/}clang"
