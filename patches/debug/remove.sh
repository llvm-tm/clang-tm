#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES="$ROOT/debug_patches/patches"

cd "$ROOT"

shopt -s nullglob
removed=0
errors=0
for p in "$PATCHES"/*.patch; do
    echo "Removing: $(basename "$p")"
    if git apply -R "$p" 2>/dev/null; then
        removed=$((removed + 1))
    else
        echo "  WARNING: Could not reverse-apply.  Patch may conflict or not be applied."
        echo "  Try:  git checkout -- ."
        errors=$((errors + 1))
    fi
done

echo "Removed $removed patch(es)."
if [ "$errors" -gt 0 ]; then
    echo "$errors patch(es) could not be cleanly removed."
    exit 1
fi
