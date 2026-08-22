#!/usr/bin/env bash
# Unified gem5 build script for TM comparison project.
#
# Usage:
#   ./scripts/build.sh <target> [options]
#
# Targets:
#   all           Build gem5 for ALL ISAs (includes all three TM implementations)
#   power         Build gem5 for POWER ISA only (POWER8 HTM)
#   arm           Build gem5 for ARM ISA only (ARM TME)
#   x86           Build gem5 for X86 ISA only
#   x86-tsx       Build gem5 for X86 with TSX patch applied
#
# Options:
#   -j N          Parallel build jobs (default: host CPU count)
#   --clean       Clean build (scons -c first)
#   --no-ruby     Build without Ruby support (faster, no HTM)
#   --python X    Python config to use (default: auto-detect)
#   --debug       Build debug binary (gem5.debug) instead of opt
#   --patch FILE  Path to TSX patch file (for x86-tsx target)
#   --patch-url URL URL to download TSX patch from
#
# Environment:
#   GEM5_ROOT     Path to gem5 source (default: ../gem5 relative to script)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GEM5_ROOT="${GEM5_ROOT:-$PROJECT_ROOT/gem5}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
TARGET=""
BUILD_TYPE="opt"
CLEAN=""
WITH_RUBY="--with-ruby"
PYTHON_CONFIG=""
TSX_PATCH=""
TSX_PATCH_URL=""

# ---- Detect Python 3.12 on macOS ----
detect_python_config() {
    if command -v python3.12-config &>/dev/null; then
        echo "python3.12-config"
    elif python3 -c "import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)" 2>/dev/null; then
        echo "python3-config"
    else
        echo "WARNING: No suitable Python >= 3.10 found. gem5 25.1 requires Python >= 3.10."
        echo "On macOS 12, install: brew install python@3.12"
        echo ""
        # Try to find python3.12-config anyway
        if [ -f /opt/homebrew/bin/python3.12-config ]; then
            echo "/opt/homebrew/bin/python3.12-config"
        elif command -v python3-config &>/dev/null; then
            echo "python3-config"
        else
            echo ""
        fi
    fi
}

usage() {
    sed -n '2,/^$/{ /^$/!p; }' "$0"
    exit 1
}

# ---- Parse arguments ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        all|power|arm|x86|x86-tsx)
            TARGET="$1"
            shift
            ;;
        -j)
            JOBS="$2"; shift 2
            ;;
        --clean)
            CLEAN="yes"; shift
            ;;
        --no-ruby)
            WITH_RUBY=""; shift
            ;;
        --python)
            PYTHON_CONFIG="$2"; shift 2
            ;;
        --debug)
            BUILD_TYPE="debug"; shift
            ;;
        --patch)
            TSX_PATCH="$2"; shift 2
            ;;
        --patch-url)
            TSX_PATCH_URL="$2"; shift 2
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "ERROR: Unknown argument: $1"
            usage
            ;;
    esac
done

if [ -z "$TARGET" ]; then
    echo "ERROR: No build target specified."
    usage
fi

# ---- Auto-detect Python config ----
if [ -z "$PYTHON_CONFIG" ]; then
    PYTHON_CONFIG="$(detect_python_config)"
fi
if [ -z "$PYTHON_CONFIG" ]; then
    echo "ERROR: Cannot find a suitable Python configuration."
    echo "Install Python 3.10+ and try again, or use --python to specify."
    exit 1
fi
echo "Using Python config: $PYTHON_CONFIG"

# ---- Verify gem5 source ----
if [ ! -d "$GEM5_ROOT" ]; then
    echo "ERROR: gem5 not found at $GEM5_ROOT"
    echo "Clone it: git clone https://github.com/gem5/gem5.git \"$GEM5_ROOT\""
    echo "Or set GEM5_ROOT to the correct path."
    exit 1
fi

cd "$GEM5_ROOT"

# ---- Determine ISA target and build directory ----
case "$TARGET" in
    all)
        ISATAG="ALL"
        ;;
    power)
        ISATAG="POWER"
        ;;
    arm)
        ISATAG="ARM"
        ;;
    x86)
        ISATAG="X86"
        ;;
    x86-tsx)
        ISATAG="X86_TSX"
        if [ -n "$TSX_PATCH" ]; then
            if [ -f "$TSX_PATCH" ]; then
                echo "Applying TSX patch from: $TSX_PATCH"
                git apply "$TSX_PATCH" || {
                    echo "ERROR: Failed to apply TSX patch. It may need rebasing for gem5 $(git describe --tags 2>/dev/null || echo 'current')."
                    echo "See docs/x86-tsx-patch.md for details."
                    exit 1
                }
            else
                echo "ERROR: Patch file not found: $TSX_PATCH"
                exit 1
            fi
        elif [ -n "$TSX_PATCH_URL" ]; then
            echo "Downloading TSX patch from: $TSX_PATCH_URL"
            PATCH_FILE=$(mktemp)
            if curl -sL "$TSX_PATCH_URL" -o "$PATCH_FILE"; then
                git apply "$PATCH_FILE" || {
                    echo "ERROR: Failed to apply TSX patch."
                    rm -f "$PATCH_FILE"
                    exit 1
                }
                rm -f "$PATCH_FILE"
            else
                echo "ERROR: Failed to download TSX patch from $TSX_PATCH_URL"
                rm -f "$PATCH_FILE"
                exit 1
            fi
        else
            # Check if patch is already applied
            if ! grep -q "XBEGIN\|RTM\|TSX" src/arch/x86/insts/microcode/rtm.uca 2>/dev/null; then
                echo "WARNING: No TSX patch provided and no TSX patch detected."
                echo "The x86-tsx target requires the external TSX patch."
                echo "See docs/x86-tsx-patch.md for details on obtaining and applying it."
                echo ""
                echo "To build a plain x86 gem5 (without TSX), use: ./scripts/build.sh x86"
                echo ""
                read -rp "Continue without the TSX patch? [y/N] " reply
                if [[ ! "$reply" =~ ^[Yy]$ ]]; then
                    exit 1
                fi
            fi
        fi
        ;;
esac

BUILD_DIR="build/$ISATAG"

# ---- Clean build ----
if [ -n "$CLEAN" ] && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning build directory: $BUILD_DIR"
    scons -c "$BUILD_DIR/gem5.$BUILD_TYPE" 2>/dev/null || true
fi

# ---- Build ----
echo ""
echo "=========================================="
echo " Building gem5 for $TARGET"
echo " Target:      $ISATAG"
echo " Type:        $BUILD_TYPE"
echo " Jobs:        $JOBS"
echo " With Ruby:   $([[ -n "$WITH_RUBY" ]] && echo yes || echo no)"
echo " Python:      $PYTHON_CONFIG"
echo " Output:      $BUILD_DIR/gem5.$BUILD_TYPE"
echo "=========================================="
echo ""

PYTHON_CONFIG="$PYTHON_CONFIG" scons "$BUILD_DIR/gem5.$BUILD_TYPE" -j"$JOBS" $WITH_RUBY

echo ""
echo "=========================================="
echo " Build complete!"
echo " Binary: $PWD/$BUILD_DIR/gem5.$BUILD_TYPE"
echo "=========================================="
