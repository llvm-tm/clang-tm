#!/usr/bin/env bash
# =============================================================================
# install.sh — Install clang-tm, runtimes, and TM API headers
#
# Installs:
#   PREFIX/bin/clang-tm              — compilation pipeline wrapper
#   PREFIX/lib/clang-tm/plugin/      — LLVM plugin .so (all variants)
#   PREFIX/lib/clang-tm/runtimes/    — TM runtime source files
#   PREFIX/lib/clang-tm/include/     — tm_api.hpp, tm_common.hpp, backend headers
#
# Usage:
#   ./install.sh [OPTIONS]
#
# Options:
#   --prefix DIR     Install under DIR instead of /usr/local  (env: PREFIX)
#   --destdir DIR    Staging directory for packaging           (env: DESTDIR)
#   -y, --yes        Non-interactive (answer yes to prompts)
#   --dry-run        Print what would be done, do nothing
#   -h, --help       Show this message
#
# Examples:
#   ./install.sh                              # /usr/local
#   PREFIX=~/.local ./install.sh              # user-local
#   ./install.sh --prefix /opt/clang-tm       # custom prefix
#   DESTDIR=/tmp/pkg ./install.sh             # packaging
# =============================================================================

set -euo pipefail

# ---- Colours (disabled if not a terminal) ----
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BOLD='\033[1m'
    NC='\033[0m' # No Colour
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; NC=''
fi

info()  { printf "${GREEN}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
warn()  { printf "${YELLOW}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
err()   { printf "${RED}==>${NC} ${BOLD}%s${NC}\n" "$*" >&2; }
die()   { err "$@"; exit 1; }

# ---- Helper: check a command exists ----
need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        die "Required command '$1' not found. See docs/REQUIREMENTS.md."
    fi
}

# ---- Resolve paths ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LLVM_TM_DIR="$SCRIPT_DIR"
BACKENDS_DIR="$PROJECT_ROOT/backends"

# ---- Parse arguments ----
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

INSTALL_DIR="${DESTDIR}${PREFIX}"

# ---- Prerequisites ----
need_cmd clang++
need_cmd opt

if [ ! -f "$LLVM_TM_DIR/bin/libTMInstrument.so" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        info "Would build plugin with 'make variants'"
    else
        info "Building clang-tm plugin..."
        make -C "$LLVM_TM_DIR" variants
    fi
fi

# ---- Confirm ----
INSTALL_BIN="${INSTALL_DIR}/bin"
INSTALL_LIB="${INSTALL_DIR}/lib/clang-tm"
INSTALL_PLUGIN="${INSTALL_LIB}/plugin"
INSTALL_RUNTIMES="${INSTALL_LIB}/runtimes"
INSTALL_INCLUDE="${INSTALL_LIB}/include"

info "Destination: ${BOLD}$INSTALL_DIR${NC}"
info "  clang-tm  → ${INSTALL_BIN}/clang-tm"
info "  plugin    → ${INSTALL_PLUGIN}/"
info "  runtimes  → ${INSTALL_RUNTIMES}/"
info "  headers   → ${INSTALL_INCLUDE}/"

if [ "$YES" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    printf "Continue? [Y/n] "
    read -r reply
    case "$reply" in
        n|N|no|NO) die "Aborted." ;;
    esac
fi

if [ "$DRY_RUN" -eq 1 ]; then
    info "Dry-run — nothing installed."
    exit 0
fi

# ---- Create directories ----
mkdir -p "$INSTALL_BIN"
mkdir -p "$INSTALL_PLUGIN"
mkdir -p "$INSTALL_RUNTIMES"
mkdir -p "$INSTALL_INCLUDE"

# ---- 1. Install clang-tm script ----
cp "$LLVM_TM_DIR/clang-tm" "$INSTALL_BIN/clang-tm"
chmod 755 "$INSTALL_BIN/clang-tm"
info "clang-tm → $INSTALL_BIN/clang-tm"

# Install install-benchmarks.sh to lib directory
cp "$LLVM_TM_DIR/install-benchmarks.sh" "$INSTALL_LIB/install-benchmarks.sh"
chmod 755 "$INSTALL_LIB/install-benchmarks.sh"

# Bootstrap scripts
cp "$LLVM_TM_DIR/bootstrap-install.sh" "$INSTALL_LIB/" 2>/dev/null || true
cp "$LLVM_TM_DIR/bootstrap-uninstall.sh" "$INSTALL_LIB/" 2>/dev/null || true

# ---- 2. Install plugin .so files ----
for variant in "$LLVM_TM_DIR/bin/libTMInstrument"*.so; do
    [ -f "$variant" ] || continue
    cp "$variant" "$INSTALL_PLUGIN/"
done
info "plugin → $INSTALL_PLUGIN/ ($(ls -1 "$INSTALL_PLUGIN" | wc -l | tr -d ' ') variants)"

# ---- 3. Install runtime sources ----
cp "$BACKENDS_DIR/runtimes/"*.cpp "$INSTALL_RUNTIMES/"
info "runtimes → $INSTALL_RUNTIMES/ ($(ls -1 "$INSTALL_RUNTIMES" | wc -l | tr -d ' ') files)"

# ---- 4. Install backend headers (preserving ../-relative layout) ----
#
# Runtime .cpp files use includes like:
#   #include "../tm_alloc_overrides.hpp"   → lib/clang-tm/tm_alloc_overrides.hpp
#   #include "../TL2/tl2.hpp"              → lib/clang-tm/TL2/tl2.hpp
#
# We place all headers at the lib/clang-tm/ level so ../ resolves correctly.
cp "$BACKENDS_DIR/tm_api.hpp" "$INSTALL_LIB/"
cp "$BACKENDS_DIR/tm_common.hpp" "$INSTALL_LIB/"
cp "$BACKENDS_DIR/rel_ptr.hpp" "$INSTALL_LIB/" 2>/dev/null || true
cp "$BACKENDS_DIR/tm_alloc_overrides.hpp" "$INSTALL_LIB/" 2>/dev/null || true

# Backend subdirectories (TL2/, NOrec/, SwissTM/, TinySTM/)
for subdir in TL2 NOrec SwissTM TinySTM; do
    sub="$BACKENDS_DIR/$subdir"
    if [ -d "$sub" ]; then
        mkdir -p "$INSTALL_LIB/$subdir"
        cp "$sub/"*.hpp "$INSTALL_LIB/$subdir/" 2>/dev/null || true
    fi
done

# Also copy the API header to include/ for convenience
mkdir -p "$INSTALL_INCLUDE"
cp "$BACKENDS_DIR/tm_api.hpp" "$INSTALL_INCLUDE/"
cp "$BACKENDS_DIR/tm_common.hpp" "$INSTALL_INCLUDE/"
info "headers → $INSTALL_LIB/ (backends + tm_api.hpp)"

# ---- 5. Summary ----
echo ""
info "Installation complete."
echo ""
printf "  ${BOLD}Usage:${NC}\n"
printf "    clang-tm --runtime SingleGlobalLock_runtime.cpp -o myapp app.cpp\n"
printf "    clang-tm --runtime tl2_runtime.cpp -o myapp app.cpp\n"
echo ""
printf "  ${BOLD}Quick test:${NC}\n"
printf "    clang-tm -I${INSTALL_LIB} --runtime ${INSTALL_RUNTIMES}/SingleGlobalLock_runtime.cpp -o /tmp/test /tmp/test.cpp\n"
echo ""
printf "  ${BOLD}To uninstall:${NC} ${LLVM_TM_DIR}/uninstall.sh\n"
