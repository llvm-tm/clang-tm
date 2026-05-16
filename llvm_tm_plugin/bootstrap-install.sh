#!/usr/bin/env bash
# =============================================================================
# clang-tm: one-line installer
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/llvm_tm_plugin/bootstrap-install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/llvm-tm/clang-tm/main/llvm_tm_plugin/bootstrap-install.sh | bash -s -- --prefix ~/.local
#
# All arguments are forwarded to install.sh.
# =============================================================================
set -euo pipefail

REPO="https://github.com/llvm-tm/clang-tm.git"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "==> Downloading clang-tm..."
git clone --depth 1 "$REPO" "$TMPDIR"

echo "==> Running install.sh..."
exec "$TMPDIR/llvm_tm_plugin/install.sh" "$@"
