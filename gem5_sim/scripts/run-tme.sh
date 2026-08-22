#!/usr/bin/env bash
# Legacy wrapper — delegates to run-arm-tme.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "INFO: Using run-arm-tme.sh" >&2
exec "$SCRIPT_DIR/run-arm-tme.sh" "$@"
