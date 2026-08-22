#!/usr/bin/env bash
# Apply the x86 TSX patch to gem5.
#
# The patch adds XBEGIN, XABORT, XEND (RTM) and HLE prefix support to
# the x86 ISA in gem5. It was originally submitted as review #2308:
#   https://reviews.gem5.org/r/2308/
#
# NOTE: This patch was created for gem5 ~2014 and will likely need
# rebasing for gem5 v25.1+. Use with caution.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GEM5_ROOT="${GEM5_ROOT:-$PROJECT_ROOT/gem5}"

if [ ! -d "$GEM5_ROOT" ]; then
    echo "ERROR: gem5 not found at $GEM5_ROOT"
    exit 1
fi

cd "$GEM5_ROOT"

# Default patch locations (searched in order)
PATCH_PATHS=(
    "$PROJECT_ROOT/patches/tsx.patch"
    "$PROJECT_ROOT/patches/x86-tsx.patch"
    "$PROJECT_ROOT/tsx.patch"
)
PATCH_URL="https://reviews.gem5.org/r/2308/raw/"

apply_patch() {
    local patch="$1"
    echo "Applying patch: $patch"
    if git apply --stat "$patch" >/dev/null 2>&1; then
        git apply "$patch"
        echo "Patch applied successfully."
    else
        echo "ERROR: Patch does not apply cleanly."
        echo "The TSX patch was created for an older gem5 version (~2014)"
        echo "and likely needs manual rebasing for gem5 v25.1."
        echo ""
        echo "Try: git apply --reject \"$patch\""
        echo "Then manually fix the rejected hunks."
        exit 1
    fi
}

# Check if patch is already applied
if grep -q "XBEGIN\|RTM\|TSX" src/arch/x86/insts/microcode/rtm.uca 2>/dev/null; then
    echo "TSX patch appears to already be applied (XBEGIN/RTM found in source)."
    exit 0
fi

# Try local patch files first
for p in "${PATCH_PATHS[@]}"; do
    if [ -f "$p" ]; then
        apply_patch "$p"
        exit 0
    fi
done

# Try command line argument
if [ $# -ge 1 ] && [ -f "$1" ]; then
    apply_patch "$1"
    exit 0
fi

# Try to download
echo "No local patch file found."
echo "Attempting to download from: $PATCH_URL"
echo ""

if command -v curl &>/dev/null; then
    PATCH_FILE=$(mktemp)
    if curl -sL "$PATCH_URL" -o "$PATCH_FILE" && [ -s "$PATCH_FILE" ]; then
        apply_patch "$PATCH_FILE"
        rm -f "$PATCH_FILE"
        exit 0
    fi
    rm -f "$PATCH_FILE"
fi

echo ""
echo "Could not obtain the TSX patch."
echo "See docs/x86-tsx-patch.md for alternatives."
exit 1
