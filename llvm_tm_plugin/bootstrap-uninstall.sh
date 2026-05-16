#!/usr/bin/env bash
# =============================================================================
# clang-tm: one-line uninstaller
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/llvm_tm_plugin/bootstrap-uninstall.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/llvm_tm_plugin/bootstrap-uninstall.sh | bash -s -- --prefix ~/.local
#
# All arguments are forwarded to uninstall.sh.
# =============================================================================
set -euo pipefail

REPO="https://github.com/llvm-tm/clang-tm.git"

# First try: use installed uninstall.sh if present
PREFIX="${1:-/usr/local}"
if [ -f "$PREFIX/lib/clang-tm/uninstall.sh" ]; then
    echo "==> Using installed uninstall.sh at $PREFIX/lib/clang-tm/uninstall.sh"
    exec "$PREFIX/lib/clang-tm/uninstall.sh" "$@"
fi

# Fallback: download and run
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "==> Downloading clang-tm uninstall script..."
git clone --depth 1 "$REPO" "$TMPDIR"

echo "==> Running uninstall.sh..."
exec "$TMPDIR/llvm_tm_plugin/uninstall.sh" "$@"
