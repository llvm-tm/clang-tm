#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PATCHES="$(cd "$(dirname "$0")" && pwd)/patches"

# Must be in a git repo
cd "$ROOT"

# Refuse if working tree is dirty — debug patches apply to clean source
if ! git diff --quiet HEAD; then
    echo "ERROR: Working tree has uncommitted changes.  Commit/stash first."
    echo "       Running patches over dirty source may produce wrong results."
    git status --short
    exit 1
fi

shopt -s nullglob
applied=0
for p in "$PATCHES"/*.patch; do
    echo "Applying: $(basename "$p")"
    git apply "$p"
    applied=$((applied + 1))
done

echo "Applied $applied patch(es)."
