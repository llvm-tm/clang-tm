#!/usr/bin/env bash
# Legacy wrapper — delegates to run-x86-tsx.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "INFO: Using run-x86-tsx.sh" >&2
exec "$SCRIPT_DIR/run-x86-tsx.sh" "$@"
