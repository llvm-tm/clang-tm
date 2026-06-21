#!/bin/bash
# ── TSX Profiling Workflow ──────────────────────────────────
# 1. Apply profiling patch to TSXSGL
# 2. Build TSXSGL with instrumentation
# 3. Run benchmarks and collect TSX_STATS
# 4. Generate calibration data for the cost model
#
# Usage: bash run_workflow.sh [tsxsgl|spht]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PATCH_DIR="$(dirname "$0")"
BACKEND="${1:-tsxsgl}"

echo "=== TSX Profiling Workflow ==="
echo "Backend: $BACKEND"
echo "Root: $REPO_ROOT"
echo "Patch dir: $PATCH_DIR"
echo ""

# Step 1: Apply profiling patch
echo "--- Step 1: Apply profiling patch ---"
PATCH_FILE="$PATCH_DIR/0001-tsxsgl-tsx-timing-instrumentation.patch"
if [ -f "$PATCH_FILE" ]; then
    echo "Applying $PATCH_FILE ..."
    cd "$REPO_ROOT"
    # Check if already applied
    if git diff --quiet -- backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp 2>/dev/null; then
        echo "Patch not yet applied. Applying..."
        git apply "$PATCH_FILE" || {
            echo "ERROR: Failed to apply patch. Check for conflicts."
            exit 1
        }
        echo "Patch applied."
    else
        echo "Patched file has local changes. Backing up and re-applying..."
        git stash -- backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp
        git apply "$PATCH_FILE"
        echo "Patch applied (after stash)."
    fi
else
    echo "WARNING: Patch file $PATCH_FILE not found. Running without instrumentation."
fi

# Step 2: Build
echo ""
echo "--- Step 2: Build $BACKEND ---"
BUILD_DIR="$REPO_ROOT/build/$BACKEND"
if [ ! -d "$BUILD_DIR" ]; then
    echo "ERROR: $BUILD_DIR does not exist."
    echo "Create it and run cmake first."
    echo "  mkdir -p $BUILD_DIR && cd $BUILD_DIR && cmake ../.."
    exit 1
fi

echo "Building in $BUILD_DIR ..."
cd "$BUILD_DIR"
make -j$(nproc) 2>&1 | tail -5
echo "Build complete."

# Step 3: Run experiments
echo ""
echo "--- Step 3: Run profiling experiments ---"
cd "$PATCH_DIR"
python3 "$PATCH_DIR/run_tsx_profiling.py" 2>&1

# Step 4: Cleanup patch
echo ""
echo "--- Step 4: Revert profiling patch ---"
cd "$REPO_ROOT"
git checkout -- backends/tm_impl/tsx_sgl/TSXSGL_runtime.cpp 2>/dev/null || true
echo "Patch reverted."

echo ""
echo "=== Done ==="
echo "Raw results:     $REPO_ROOT/profiling_data/raw/"
ls -la "$REPO_ROOT/profiling_data/raw/" 2>/dev/null || echo "(empty)"
echo "Calibration:     $REPO_ROOT/profiling_data/calibration/"
ls -la "$REPO_ROOT/profiling_data/calibration/" 2>/dev/null || echo "(empty)"
echo "Machine configs: $REPO_ROOT/machine_profiles/"
ls -la "$REPO_ROOT/machine_profiles/" 2>/dev/null || echo "(empty)"
