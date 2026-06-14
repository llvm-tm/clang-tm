#!/usr/bin/env bash
set -euo pipefail

# ──────────────────────────────────────────────────────────────────────────
# add_debug_patch.sh  —  Create a debug patch from current working changes
#
# Usage:
#   1. HACK source files in the working tree
#   2. Run:  ./debug_patches/add_debug_patch.sh [name.patch]
#      (default: patches/$(date -u +%Y%m%dT%H%M%S)-unnamed.patch)
#   3. Working tree is reverted to clean.  The patch file is created.
#   4. To re-apply later:  ./debug_patches/apply.sh
# ──────────────────────────────────────────────────────────────────────────

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PATCHES="$(cd "$(dirname "$0")" && pwd)/patches"

NAME="${1:-$(date -u +%Y%m%dT%H%M%S)-unnamed.patch}"
OUT="$PATCHES/$NAME"

cd "$ROOT"

# Check that there ARE changes to capture
if git diff --quiet HEAD; then
    echo "ERROR: No uncommitted changes to capture as a debug patch."
    echo "       Make edits to source files first, then run this script."
    exit 1
fi

# Show what will be captured
echo "Capturing uncommitted changes as debug patch:"
git diff --stat HEAD
echo ""

# Write the patch
git diff HEAD > "$OUT"
echo "Created: $OUT"
echo ""

# Warn about untracked files
if git ls-files --others --exclude-standard | grep -q .; then
    echo "NOTE: Untracked files are NOT included in the patch."
    echo "      They remain in the working tree:"
    git ls-files --others --exclude-standard
fi

# Offer to revert
read -p "Revert working tree to clean? [Y/n] " ans
case "$ans" in
    n|N|no) echo "Kept working tree as-is." ;;
    *) git checkout -- . 2>/dev/null || true
       echo "Working tree reverted to HEAD." ;;
esac
