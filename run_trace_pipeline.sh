#!/usr/bin/env bash
# ============================================================================
# run_trace_pipeline.sh — End-to-end TM trace pipeline
#
# Builds a benchmark (explicit-API or plugin), runs it with trace emission,
# converts the raw trace to JSONL, and replays it through the
# TM-model simulator for validation and statistics.
#
# Usage:
#   ./run_trace_pipeline.sh                        # run with defaults
#   ./run_trace_pipeline.sh --backend NOREC        # pick a backend
#   ./run_trace_pipeline.sh --benchmark bank       # pick a benchmark
#   ./run_trace_pipeline.sh --duration 500         # ms per run
#   ./run_trace_pipeline.sh --threads 4
#   ./run_trace_pipeline.sh --plugin              # use LLVM plugin pipeline
#   ./run_trace_pipeline.sh --sim-only trace.jsonl # skip build/run
#
# All flags:
#   -b, --backend   Backend (NOREC TINYSTM WBETL WT SWISSTM TL2 SGL)
#   -m, --benchmark Benchmark name
#   -d, --duration  Test duration in ms (default: 200)
#   -t, --threads   Number of threads (default: 2)
#   -p, --plugin    Use LLVM plugin pipeline (default: explicit-API)
#   -o, --outdir    Output directory (default: /tmp/tm_pipeline)
#   -s, --sim-only  Skip build/run, just sim an existing JSONL file
#   -h, --help      Show this message
# ============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIM_DIR="$SCRIPT_DIR/simulator"

# ---- Defaults ----
BACKEND="NOREC"
BENCHMARK="bank"
DURATION_MS=200
THREADS=2
OUTDIR="/tmp/tm_pipeline"
SIM_ONLY=""
PLUGIN=0
CARGO="${CARGO_HOME:-$HOME/.cargo}/bin/cargo"

# ---- Backend name mapping ----
expli_backend() {
  case "$1" in
    TINYSTM|WBCTL|tinystm|wbctl) echo "TINYSTM" ;;
    NOREC|norec)                  echo "NOREC" ;;
    TL2|tl2)                      echo "TL2" ;;
    SWISSTM|swisstm)              echo "SWISSTM" ;;
    SGL|singlelock)               echo "SGL" ;;
    WT|wt)                        echo "WT" ;;
    *)                            echo "$1" ;;
  esac
}

plugin_backend() {
  case "$1" in
    TINYSTM|WBCTL|tinystm|wbctl) echo "tinystm" ;;
    NOREC|norec)                  echo "norec" ;;
    SGL|singlelock)               echo "singlelock" ;;
    TL2|tl2)                      echo "tl2" ;;
    SWISSTM|swisstm)              echo "swisstm" ;;
    *)                            echo "$1" ;;
  esac
}

# ---- Parse ----
while [ $# -gt 0 ]; do
  case "$1" in
    -b|--backend)   BACKEND="$2"; shift 2 ;;
    -m|--benchmark) BENCHMARK="$2"; shift 2 ;;
    -d|--duration)  DURATION_MS="$2"; shift 2 ;;
    -t|--threads)   THREADS="$2"; shift 2 ;;
    -p|--plugin)    PLUGIN=1; shift ;;
    -o|--outdir)    OUTDIR="$2"; shift 2 ;;
    -s|--sim-only)  SIM_ONLY="$2"; shift 2 ;;
    -h|--help)      sed -n '/^#/s/^# \?//p' "$0"; exit 0 ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

mkdir -p "$OUTDIR"
RAW_TRACE="$OUTDIR/${BENCHMARK}_${BACKEND}_raw.txt"
JSONL_TRACE="$OUTDIR/${BENCHMARK}_${BACKEND}.jsonl"

# ── Step 1: Build ────────────────────────────────────────────
if [ -z "$SIM_ONLY" ]; then
  echo "═══ Step 1: Building $BENCHMARK with $BACKEND ═══"

  if [ "$PLUGIN" -eq 1 ]; then
    # Plugin pipeline: use benchmarks/plugin/Makefile with tm_pipeline.mk
    PBACKEND=$(plugin_backend "$BACKEND")
    DS_DIR="$SCRIPT_DIR/benchmarks/plugin/datastructures"
    STAMP_DIR="$SCRIPT_DIR/benchmarks/plugin/STAMP"
    BIN=""

    if [ -f "$DS_DIR/$BENCHMARK.cpp" ]; then
      echo "  (plugin datastructures benchmark)"
      make -C "$DS_DIR" "${BENCHMARK}_${PBACKEND}" > /dev/null 2>&1
      BIN="$DS_DIR/bin/${BENCHMARK}_${PBACKEND}"
    elif [ -f "$STAMP_DIR/STAMP.cpp" ]; then
      echo "  (plugin STAMP benchmark — use make stamp_<name>_<backend>)"
      make -C "$STAMP_DIR" "stamp_${BENCHMARK}_${PBACKEND}" > /dev/null 2>&1
      BIN="$STAMP_DIR/bin/stamp_${BENCHMARK}_${PBACKEND}"
    else
      echo "FAIL: unknown plugin benchmark '$BENCHMARK'" >&2
      exit 1
    fi

    if [ ! -x "$BIN" ]; then
      echo "FAIL: $BIN not built" >&2
      exit 1
    fi
  else
    # Explicit-API pipeline: use benchmarks/cpp/Makefile
    EBACKEND=$(expli_backend "$BACKEND")
    (cd "$SCRIPT_DIR/benchmarks/cpp" && make -j4 BACKEND="$EBACKEND" bin/"$BENCHMARK" > /dev/null 2>&1)
    BIN="$SCRIPT_DIR/benchmarks/cpp/bin/$BENCHMARK"
    if [ ! -x "$BIN" ]; then
      echo "FAIL: $BIN not built" >&2
      exit 1
    fi
  fi

  # ── Step 2: Run with trace ────────────────────────────────
  echo "═══ Step 2: Running ($DURATION_MS ms, $THREADS threads) ═══"
  # Build benchmark args
  ARGS=""
  case "$BENCHMARK" in
    bank)       ARGS="-d $DURATION_MS -a 64 -t $THREADS --test" ;;
    eigenbench) ARGS="-d $DURATION_MS -t $THREADS --test" ;;
    vacation)   ARGS="-p $THREADS -r 16384 -n 2 -u 98 -t 1024 --test" ;;
    labyrinth)  ARGS="-p $THREADS -x 4 -y 4 -z 4 -n 32 --test" ;;
    kmeans)     ARGS="-p $THREADS -k 8 -d 2 -n 512 --test" ;;
    genome)     ARGS="-p $THREADS -g 4096 -s 32 -n 262144 --test" ;;
    intruder)   ARGS="-p $THREADS -a 10 -l 64 -n 262144 -s 1 --test" ;;
    ssca2)      ARGS="-p $THREADS -s 10 -u 1.0 -l 3 -m 3 -i 3 --test" ;;
    bayes)      ARGS="-t $THREADS -v 16 -r 128 -n 2 -p 2 -e 2 --test" ;;
    yada)       ARGS="-t $THREADS -a 10 --test" ;;
    tpcc)       ARGS="-t $THREADS -d $DURATION_MS --test" ;;
    ycsb)       ARGS="-t $THREADS -d $DURATION_MS --test" ;;
    rbtree|avltree|avltree_recursive|hashmap|list|set|heap)
      ARGS="$THREADS 1000 $DURATION_MS 80 10" ;;
    test_ds|test_tx) ARGS="" ;;
  esac

  BACKEND="$BACKEND" TM_TRACE_PATH="$RAW_TRACE" "$BIN" $ARGS > /dev/null 2>&1 || true
  LINES=$(wc -l < "$RAW_TRACE" 2>/dev/null || echo 0)
  echo "  Raw trace: $LINES lines → $RAW_TRACE"
fi

# ── Step 3: Convert to JSONL ─────────────────────────────────
echo "═══ Step 3: Converting raw trace → JSONL ═══"
if [ -n "$SIM_ONLY" ]; then
  JSONL_TRACE="$SIM_ONLY"
else
  "$CARGO" run --manifest-path "$SIM_DIR/Cargo.toml" --bin tm-trace2jsonl -- \
    -i "$RAW_TRACE" -o "$JSONL_TRACE" 2>&1
fi
EVENTS=$(grep -c . "$JSONL_TRACE" 2>/dev/null || echo 0)
echo "  JSONL events: $EVENTS → $JSONL_TRACE"

# ── Step 4: Replay through TM model ──────────────────────────
echo "═══ Step 4: Simulating (WBCTL model) ═══"
"$CARGO" run --manifest-path "$SIM_DIR/Cargo.toml" --bin tm-check -- "$JSONL_TRACE" 2>&1 |
  grep -v "CONFLICTS\|⚡"

echo ""
echo "═══ Pipeline complete ═══"
echo "  Raw:    $RAW_TRACE"
echo "  JSONL:  $JSONL_TRACE"
