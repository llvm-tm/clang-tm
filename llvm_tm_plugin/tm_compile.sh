#!/usr/bin/env bash
# ============================================================
# tm_compile.sh — TM Plugin Compilation Pipeline
#
# Compiles a C++ source file through the 4-step TM pipeline:
#   1. clang++ -O3 -fno-inline -emit-llvm       → source.bc
#   2. opt -load-pass-plugin (instrumentation)   → source.instr.bc
#   3. opt -O3 (optimize instrumented IR)        → source.opt.bc
#   4. clang++ (link with STM runtime)           → binary
#
# Usage:
#   tm_compile.sh [options] source.cpp
#
# Options:
#   -o, --output FILE    Output binary path (default: ./a.out)
#   -r, --runtime TYPE   Runtime backend: tinystm (default),
#                        tl2, singlelock, swisstm, norec
#   -p, --plugin FILE    Path to libTMInstrument.so
#                        (default: auto-located relative to this script)
#   -k, --keep-temps     Keep intermediate .bc files
#   -I, --include DIR    Extra include directories
#   -D, --define DEF     Extra preprocessor defines
#   -v, --verbose        Print each step
#   -h, --help           Show this help
#
# Examples:
#   tm_compile.sh mybench.cpp -o mybench
#   tm_compile.sh mybench.cpp -o mybench --runtime tl2
#   tm_compile.sh mybench.cpp -o mybench --keep-temps --verbose
# ============================================================

set -euo pipefail

# ---- Helper functions ----

die() {
    echo "Error: $*" >&2
    exit 1
}

usage() {
    sed -n 's/^# \?//p' "$0"
    exit 0
}

log() {
    if [ "$VERBOSE" = 1 ]; then
        echo "$*"
    fi
}

# ---- Locate plugin ----

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- Defaults ----

OUTPUT=""
RUNTIME="tinystm"
PLUGIN=""
KEEP_TEMPS=0
VERBOSE=0
EXTRA_INCLUDES=()
EXTRA_DEFINES=()
SOURCE=""

# ---- Parse arguments ----

while [ $# -gt 0 ]; do
    case "$1" in
        -o|--output)
            OUTPUT="$2"; shift 2 ;;
        -r|--runtime)
            RUNTIME="$2"; shift 2 ;;
        -p|--plugin)
            PLUGIN="$2"; shift 2 ;;
        -k|--keep-temps)
            KEEP_TEMPS=1; shift ;;
        -v|--verbose)
            VERBOSE=1; shift ;;
        -I)
            EXTRA_INCLUDES+=("-I$2"); shift 2 ;;
        -D)
            EXTRA_DEFINES+=("-D$2"); shift 2 ;;
        -h|--help)
            usage ;;
        -*)
            die "Unknown option: $1 (use --help for usage)" ;;
        *)
            if [ -z "$SOURCE" ]; then
                SOURCE="$1"
            else
                die "Multiple source files not supported: $1"
            fi
            shift ;;
    esac
done

[ -z "$SOURCE" ] && die "No source file specified. Use --help for usage."

# Resolve source path
SOURCE="$(realpath "$SOURCE")"
[ ! -f "$SOURCE" ] && die "Source file not found: $SOURCE"

# Default plugin path
if [ -z "$PLUGIN" ]; then
    PLUGIN="$SCRIPT_DIR/bin/libTMInstrument.so"
fi
[ ! -f "$PLUGIN" ] && die "Plugin not found: $PLUGIN (use --plugin to specify)"

# Default output
BASENAME="$(basename "$SOURCE" .cpp)"
if [ -z "$OUTPUT" ]; then
    OUTPUT="./$BASENAME"
fi

# ---- Locate runtime files ----

RUNTIMES_DIR="$PROJECT_ROOT/backends/runtimes"
BACKENDS_DIR="$PROJECT_ROOT/backends"
TINYSTM_DIR="$BACKENDS_DIR/TinySTM"

case "$RUNTIME" in
    tinystm)
        RUNTIME_CPP="$RUNTIMES_DIR/TinySTM_runtime.cpp"
        RUNTIME_DEFINES="-DDESIGN_WBCTL"
        RUNTIME_INCLUDES="-I$TINYSTM_DIR -I$BACKENDS_DIR"
        ;;
    tl2)
        RUNTIME_CPP="$RUNTIMES_DIR/tl2_runtime.cpp"
        RUNTIME_DEFINES=""
        RUNTIME_INCLUDES=""
        ;;
    singlelock)
        RUNTIME_CPP="$RUNTIMES_DIR/SingleGlobalLock_runtime.cpp"
        RUNTIME_DEFINES=""
        RUNTIME_INCLUDES=""
        ;;
    swisstm)
        RUNTIME_CPP="$RUNTIMES_DIR/SwissTM_runtime.cpp"
        RUNTIME_DEFINES=""
        RUNTIME_INCLUDES="-I$BACKENDS_DIR/SwissTM"
        ;;
    norec)
        RUNTIME_CPP="$RUNTIMES_DIR/NOrec_runtime.cpp"
        RUNTIME_DEFINES=""
        RUNTIME_INCLUDES="-I$BACKENDS_DIR/NOrec -I$BACKENDS_DIR"
        ;;
    *)
        die "Unknown runtime: $RUNTIME (use: tinystm, tl2, singlelock, swisstm, norec)"
        ;;
esac

[ ! -f "$RUNTIME_CPP" ] && die "Runtime file not found: $RUNTIME_CPP"

# ---- Build directory for intermediates ----

OUT_DIR="out"
mkdir -p "$OUT_DIR"

STEP1_BC="$OUT_DIR/$BASENAME.bc"
STEP2_BC="$OUT_DIR/$BASENAME.instr.bc"
STEP3_BC="$OUT_DIR/$BASENAME.opt.bc"

# ---- Step 1: Generate LLVM IR ----

log "Step 1: Generating LLVM IR ($STEP1_BC)..."
clang++ -std=c++20 -O3 -fno-inline -emit-llvm -c "$SOURCE" -o "$STEP1_BC" \
    -fno-stack-protector -pthread \
    "${EXTRA_INCLUDES[@]}" "${EXTRA_DEFINES[@]}"

# ---- Step 2: Run plugin ----

log "Step 2: Running TM instrumentation plugin..."
opt -load-pass-plugin="$PLUGIN" -passes="tm-instrument" "$STEP1_BC" -o "$STEP2_BC"

# ---- Step 3: Optimize instrumented IR ----

log "Step 3: Optimizing instrumented IR..."
opt -O3 "$STEP2_BC" -o "$STEP3_BC"

# ---- Step 4: Link with runtime ----

log "Step 4: Linking with $RUNTIME runtime..."
clang++ -std=c++20 -O1 -pthread $RUNTIME_DEFINES "$STEP3_BC" "$RUNTIME_CPP" \
    -o "$OUTPUT" $RUNTIME_INCLUDES \
    "${EXTRA_INCLUDES[@]}" "${EXTRA_DEFINES[@]}"

# ---- Clean temps ----

if [ "$KEEP_TEMPS" != 1 ]; then
    log "Cleaning intermediate files..."
    rm -f "$STEP1_BC" "$STEP2_BC" "$STEP3_BC"
fi

echo "Built: $OUTPUT"
