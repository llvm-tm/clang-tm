#!/usr/bin/env bash
# Clone gem5, verify/apply x86 TSX support, and build X86_TSX target.
#
# Usage:
#   ./gem5_sim/setup.sh              # clone + verify + build
#   ./gem5_sim/setup.sh --clone      # clone only
#   ./gem5_sim/setup.sh --build      # build only (gem5 dir must exist)
#   ./gem5_sim/setup.sh --clean      # clean build artifacts
#   ./gem5_sim/setup.sh --build-all  # build ALL ISAs (all, power, arm, x86)
#
# Env overrides:
#   GEM5_VERSION   gem5 tag/branch   (default: read from .gem5-version)
#   GEM5_JOBS      parallel jobs     (default: nproc)
#   GEM5_TARGET    ISA build target  (default: X86_TSX)
#   GEM5_BUILD_TYPE  opt/debug       (default: opt)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GEM5_DIR="$SCRIPT_DIR/gem5"
VERSION_FILE="$SCRIPT_DIR/.gem5-version"

# ---- Defaults ----
GEM5_URL="https://github.com/gem5/gem5.git"
GEM5_VERSION="${GEM5_VERSION:-$(cat "$VERSION_FILE" 2>/dev/null || echo v25.1.0.1)}"
GEM5_JOBS="${GEM5_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
GEM5_TARGET="${GEM5_TARGET:-X86_TSX}"
GEM5_BUILD_TYPE="${GEM5_BUILD_TYPE:-opt}"
MODE="${1:---all}"

# ---- Helpers ----
info()  { echo ">>> $*"; }
error() { echo "ERROR: $*" >&2; exit 1; }

detect_python_config() {
    if command -v python3.12-config &>/dev/null; then
        echo "python3.12-config"
    elif command -v python3-config &>/dev/null; then
        echo "python3-config"
    else
        echo ""
    fi
}

# ---- Parse arguments ----
DO_CLONE=0
DO_BUILD=0
DO_CLEAN=0
BUILD_ALL=0

case "$MODE" in
    --clone)      DO_CLONE=1 ;;
    --build)      DO_BUILD=1 ;;
    --clean)      DO_CLEAN=1 ;;
    --build-all)  DO_BUILD=1; BUILD_ALL=1 ;;
    --all)        DO_CLONE=1; DO_BUILD=1 ;;
    --help|-h)
        sed -n '2,/^$/{ /^$/!p; }' "$0"
        exit 0
        ;;
    *)
        error "Unknown argument: $MODE (use --clone, --build, --clean, --build-all, or --all)"
        ;;
esac

# ---- Step 0: Clean ----
if [ "$DO_CLEAN" -eq 1 ]; then
    info "Cleaning gem5 build artifacts..."
    if [ -d "$GEM5_DIR/build" ]; then
        rm -rf "$GEM5_DIR/build"
        info "Build directory removed."
    else
        info "Nothing to clean."
    fi
    [ "$DO_CLONE" -eq 1 ] || [ "$DO_BUILD" -eq 1 ] || exit 0
fi

# ---- Step 1: Clone ----
if [ "$DO_CLONE" -eq 1 ]; then
    if [ -d "$GEM5_DIR/.git" ]; then
        info "gem5 already cloned at $GEM5_DIR"
        info "Checking out $GEM5_VERSION..."
        cd "$GEM5_DIR"
        git fetch --all --tags --quiet 2>/dev/null || true
        git checkout "$GEM5_VERSION" --quiet 2>/dev/null || {
            info "Tag $GEM5_VERSION not found, trying as branch..."
            git checkout "$GEM5_VERSION" 2>/dev/null || error "Cannot checkout $GEM5_VERSION"
        }
        info "Checked out $(git describe --tags --always 2>/dev/null || echo "$GEM5_VERSION")"
    else
        info "Cloning gem5 ($GEM5_VERSION) into $GEM5_DIR..."
        git clone --depth 1 --branch "$GEM5_VERSION" "$GEM5_URL" "$GEM5_DIR" 2>/dev/null || {
            info "Shallow clone failed; doing full clone..."
            git clone "$GEM5_URL" "$GEM5_DIR"
            cd "$GEM5_DIR"
            git checkout "$GEM5_VERSION"
        }
        info "Clone complete."
    fi

    # Verify x86 TSX in-tree support and apply local fixes
    TSX_MARKER="$GEM5_DIR/src/arch/x86/insts/microcode/rtm.uca"
    if [ -f "$TSX_MARKER" ]; then
        info "x86 TSX in-tree support detected (rtm.uca exists)."
    else
        info "x86 TSX not found in gem5 tree (old version)."
    fi
    # Always apply local HTM fixes (idempotent): 003,004,030 correct
    # in-tree bugs (InvalidOpcode on abort, missing WW detection,
    # 32-line capacity, CLONE_VM/RAX status). Each patch is skipped if
    # already applied.
    PATCH_DIR="$SCRIPT_DIR/patches"
    if ls "$PATCH_DIR"/*.patch &>/dev/null; then
        info "Applying local HTM patches from $PATCH_DIR..."
        cd "$GEM5_DIR"
        for p in "$PATCH_DIR"/*.patch; do
            info "  Checking $(basename "$p")..."
            if git apply --check "$p" 2>/dev/null; then
                info "  Applying $(basename "$p")..."
                git apply "$p" || info "  Patch $(basename "$p") failed to apply."
            else
                # Check if already applied by reverse check
                if git apply --check --reverse "$p" 2>/dev/null; then
                    info "  $(basename "$p") already applied, skipping."
                else
                    info "  Patch $(basename "$p") needs rebase or manual fix."
                fi
            fi
        done
        info "Patch application complete."
    else
        [ -f "$TSX_MARKER" ] || error "No TSX support and no patches found. Cannot build X86_TSX."
    fi
fi

# ---- Step 2: Build ----
if [ "$DO_BUILD" -eq 1 ]; then
    [ -d "$GEM5_DIR" ] || error "gem5 not found at $GEM5_DIR. Run with --clone first."

    PYTHON_CONFIG="$(detect_python_config)"
    if [ -z "$PYTHON_CONFIG" ]; then
        error "Cannot find Python >= 3.10 config. Install python@3.12 (brew) or python3-dev (apt)."
    fi
    info "Using Python config: $PYTHON_CONFIG"

    cd "$GEM5_DIR"

    if [ "$BUILD_ALL" -eq 1 ]; then
        info "Building gem5 for ALL ISAs..."
        PYTHON_CONFIG="$PYTHON_CONFIG" scons build/ALL/gem5.opt -j"$GEM5_JOBS"
        info "Build complete: build/ALL/gem5.opt"
    else
        BUILD_DIR="build/$GEM5_TARGET"
        info "Building gem5 for $GEM5_TARGET..."
        info "  Jobs: $GEM5_JOBS  Type: $GEM5_BUILD_TYPE  Ruby: yes"
        # --with-ruby removed in gem5 v25.1 (RUBY=y via build_opts/X86_TSX)
        PYTHON_CONFIG="$PYTHON_CONFIG" scons "$BUILD_DIR/gem5.$GEM5_BUILD_TYPE" -j"$GEM5_JOBS"
        info "Build complete: $GEM5_DIR/$BUILD_DIR/gem5.$GEM5_BUILD_TYPE"
    fi
fi

info "Done."
