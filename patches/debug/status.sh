#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES="$ROOT/debug_patches/patches"

cd "$ROOT"

echo "=== git status ==="
git status --short
echo ""

echo "=== Debug patches ==="
shopt -s nullglob
for p in "$PATCHES"/*.patch; do
    name="$(basename "$p")"
    # Try reverse-apply to check if patch is applied
    if git apply --check -R "$p" 2>/dev/null; then
        echo "  [APPLIED]  $name"
    else
        echo "  [NOT APPLIED] $name"
    fi
done
echo ""

echo "=== TM_EVENT_LOG ==="
# Check if any compile commands use -DTM_EVENT_LOG
if grep -r "TM_EVENT_LOG" "$ROOT/backends" --include="*.mk" --include="Makefile*" 2>/dev/null; then
    echo "  TM_EVENT_LOG is referenced in Makefiles."
else
    echo "  No Makefile references (activate manually: CXXFLAGS=\"-DTM_EVENT_LOG\")."
fi
