#!/usr/bin/env bash
# Legacy wrapper — delegates to the unified build.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "INFO: Using unified build.sh. Run './scripts/build.sh --help' for options." >&2
exec "$SCRIPT_DIR/build.sh" all "$@"
