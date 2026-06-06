#!/usr/bin/env bash
# =============================================================================
# uninstall.sh — Remove clang-tm and all installed files
#
# Usage:
#   ./uninstall.sh [OPTIONS]
#
# Options:
#   --prefix DIR     Prefix that was passed to install.sh   (env: PREFIX)
#   --destdir DIR    Staging directory for packaging         (env: DESTDIR)
#   -y, --yes        Non-interactive
#   --dry-run        Print what would be done, do nothing
#   -h, --help       Show this message
# =============================================================================

set -euo pipefail

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; NC=''
fi

info()  { printf "${GREEN}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
err()   { printf "${RED}==>${NC} ${BOLD}%s${NC}\n" "$*" >&2; }
die()   { err "$@"; exit 1; }

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"
YES=0
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            PREFIX="$2"; shift 2 ;;
        --prefix=*)
            PREFIX="${1#*=}"; shift ;;
        --destdir)
            DESTDIR="$2"; shift 2 ;;
        --destdir=*)
            DESTDIR="${1#*=}"; shift ;;
        -y|--yes)
            YES=1; shift ;;
        --dry-run)
            DRY_RUN=1; shift ;;
        -h|--help)
            sed -n 's/^# \?//p' "$0" | sed '1,/^$/d' | head -n -1
            exit 0 ;;
        *)
            die "Unknown option: $1. Try --help." ;;
    esac
done

UNINSTALL_DIR="${DESTDIR}${PREFIX}"
INSTALL_BIN="${UNINSTALL_DIR}/bin/clang-tm"
INSTALL_LIB="${UNINSTALL_DIR}/lib/clang-tm"

REMOVED=0

# Check for anything to remove
if [ -f "$INSTALL_BIN" ]; then REMOVED=1; fi
if [ -d "$INSTALL_LIB" ]; then REMOVED=1; fi

if [ "$REMOVED" -eq 0 ]; then
    info "Nothing to remove — clang-tm not installed at ${BOLD}${UNINSTALL_DIR}${NC}"
    exit 0
fi

info "Removing from: ${BOLD}${UNINSTALL_DIR}${NC}"
printf "  ${INSTALL_BIN}\n"
printf "  ${INSTALL_LIB}/\n"

if [ "$YES" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    printf "Continue? [y/N] "
    read -r reply
    case "$reply" in
        y|Y|yes|YES) ;;
        *) die "Aborted." ;;
    esac
fi

if [ "$DRY_RUN" -eq 1 ]; then
    info "Dry-run — nothing removed."
    exit 0
fi

rm -f "$INSTALL_BIN"
rm -rf "$INSTALL_LIB"

info "Uninstallation complete."
