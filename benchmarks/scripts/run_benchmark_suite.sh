#!/usr/bin/env bash
# ============================================================================
# Clean benchmark runner — 3 impls × 4 backends × 4 thread levels × 3 samples
# + uninstrumented baseline at 1 thread
#
# Backends:     norec, tinystm_wbctl, tinystm_wt, singlelock
#              (SingleGlobalLock for expli, TSXSGL for plugin, tsxsgl for Rust)
# Implementations: plugin, expli, rust
# Threads:     1, 2, 4, 8
# Samples:     3
# ============================================================================
set -euo pipefail
SELF="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
cd "$(dirname "$0")"

# ── Configuration ─────────────────────────────────────────────────────────
THREADS="${THREADS:-"1 2 4 8"}"
SAMPLES=3
TIMEOUT=120
RESULTS_BASE="benchmark_results/benchmark_suite_$(date +%H%M%S)"
STAMP_WORKLOAD="fixed"

# ── STAMP parameters (matching run_compare_all.sh) ───────────────────────
declare -A STAMP_PLUGIN_PARAMS
STAMP_PLUGIN_PARAMS[vacation]="-n 2 -q 90 -r 16384 -u 98"
STAMP_PLUGIN_PARAMS[kmeans]=""
STAMP_PLUGIN_PARAMS[labyrinth]="-x 8 -y 8 -z 8 -n 64"
STAMP_PLUGIN_PARAMS[genome]="-g 16384 -s 64 -n 1000000"
STAMP_PLUGIN_PARAMS[intruder]="-a 10 -l 128 -n 5120 -s 1"
STAMP_PLUGIN_PARAMS[ssca2]="-s 14 -u 1.0 -l 3 -p 3 -i 3"
STAMP_PLUGIN_PARAMS[bayes]="-v 16 -r 32 -n 2 -p 2 -e 4"
STAMP_PLUGIN_PARAMS[yada]="-a 20 -j 0.5"

declare -A STAMP_EXPLI_PARAMS
STAMP_EXPLI_PARAMS[vacation]="-r 16384 -n 2 -u 98 -t 4096"
STAMP_EXPLI_PARAMS[kmeans]="-k 8 -d 2 -n 200"
STAMP_EXPLI_PARAMS[labyrinth]="-x 8 -y 8 -z 8 -n 64"
STAMP_EXPLI_PARAMS[genome]="-g 16384 -s 64 -n 1000000"
STAMP_EXPLI_PARAMS[intruder]="-a 10 -l 128 -n 5120 -s 1"
STAMP_EXPLI_PARAMS[ssca2]="-s 14 -u 1.0 -l 3 -m 3 -i 3"
STAMP_EXPLI_PARAMS[bayes]="-v 16 -r 32 -n 2 -p 2 -e 4"
STAMP_EXPLI_PARAMS[yada]="-a 20 -j 0.5"

declare -A STAMP_RUST_PARAMS
STAMP_RUST_PARAMS[vacation]="-r 16384 -n 2 -u 98 -t 4096"
STAMP_RUST_PARAMS[kmeans]="-c 8 -d 2 -n 200"
STAMP_RUST_PARAMS[labyrinth]="-x 8 -y 8 -z 8 -n 64"
STAMP_RUST_PARAMS[genome]="-g 16384 -s 64 -n 1000000"
STAMP_RUST_PARAMS[intruder]="-a 10 -l 128 -n 5120 -s 1"
STAMP_RUST_PARAMS[ssca2]="-s 14 -u 1.0 -l 3 -m 3 -i 3"
STAMP_RUST_PARAMS[bayes]="-v 16 -r 32 -n 2 -p 2 -e 4"
STAMP_RUST_PARAMS[yada]="-a 20 -j 0.5"

# ── Non-STAMP params ──────────────────────────────────────────────────────
TPCC_PARAMS="-t \$t -d 5000 -w 1"
YCSB_PARAMS="-t \$t -d 5000 -k 1000 -i 500 -w a"
STM7_PLUGIN_PARAMS="-t \$t -d 5000 -w 1"
STM7_EXPLI_PARAMS="\$t 5000"  # positional args

# ── Helpers ───────────────────────────────────────────────────────────────
TIMEOUT_CMD="timeout"
if ! command -v timeout &>/dev/null && command -v gtimeout &>/dev/null; then
    TIMEOUT_CMD="gtimeout"
fi

red()    { printf '\033[1;31m%s\033[0m\n' "$*"; }
green()  { printf '\033[1;32m%s\033[0m\n' "$*"; }
blue()   { printf '\033[1;34m%s\033[0m\n' "$*"; }
log()    { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# ── Build with NDEBUG ────────────────────────────────────────────────────
build_all() {
    log "Building all implementations with NDEBUG..."

    # Expli C++ — build each backend
    cd benchmarks/cpp
    for backend in norec tinystm_wbctl tinystm_wt singlelock; do
        log "  Building expli $backend..."
        case "$backend" in
            norec)          make clean 2>/dev/null; CXXFLAGS="-DNDEBUG -DTM_BACKEND_NOREC" make -j$(nproc) 2>&1 | tail -1 ;;
            tinystm_wbctl)  make clean 2>/dev/null; CXXFLAGS="-DNDEBUG -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL" make -j$(nproc) 2>&1 | tail -1 ;;
            tinystm_wt)     make clean 2>/dev/null; CXXFLAGS="-DNDEBUG -DTM_BACKEND_TINYSTM -DDESIGN_WT" make -j$(nproc) 2>&1 | tail -1 ;;
            singlelock)     make clean 2>/dev/null; CXXFLAGS="-DNDEBUG -DTM_BACKEND_SINGLELOCK" make -j$(nproc) 2>&1 | tail -1 ;;
        esac
    done
    # Also build without TM (for bench_stubs)
    CXXFLAGS="-DNDEBUG -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL" make clean 2>/dev/null; CXXFLAGS="-DNDEBUG -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL" make -j$(nproc) 2>&1 | tail -1
    cd ..

    # Rust — build each feature
    cd explicit_api/rust/workspace
    for backend in norec wbctl wt tsxsgl; do
        log "  Building Rust $backend..."
        RUSTFLAGS="-C target-cpu=native" cargo build --release -p benchmarks --features "$backend" 2>&1 | tail -1
    done
    cd ..

    # Plugin — build each backend
    cd benchmarks/plugin/STAMP
    for backend in tinystm_wbctl tinystm_wt norec singlelock; do
        log "  Building plugin $backend..."
        make -j$(nproc) BACKEND="$backend" CXXFLAGS_EXTRA="-DNDEBUG" 2>&1 | tail -1
    done
    cd ../..

    log "Build complete."
}

# ── Run function ─────────────────────────────────────────────────────────
run_bench() {
    local impl="$1" backend="$2" bench="$3" threads="$4" sample="$5" cmd="$6"
    local outdir="$RESULTS_BASE/raw/$impl/$backend"
    local outfile="$outdir/${impl}_${backend}_${bench}_${threads}t_s${sample}.txt"
    mkdir -p "$outdir"

    log "  [$impl/$backend] $bench ${threads}t sample ${sample}..."
    local start=$EPOCHREALTIME
    if $TIMEOUT_CMD $TIMEOUT bash -c "$cmd" > "$outfile" 2>&1; then
        local end=$EPOCHREALTIME
        local elapsed=$(awk "BEGIN { printf \"%.1f\", $end - $start }")
        echo "OK" >> "$outfile"
        echo "ELAPSED=$elapsed" >> "$outfile"
        green "    OK (${elapsed}s)"
    else
        local ec=$?
        echo "FAIL (exit=$ec)" >> "$outfile"
        red "    FAIL (exit=$ec)"
    fi
}

# ── Run plugin implementation ───────────────────────────────────────────
run_plugin() {
    local backend="$1" threads="$2" sample="$3"
    local bin_dir="benchmarks/plugin/STAMP/bin"

    case "$backend" in
        norec)          binary="$bin_dir/stamp_norec" ;;
        tinystm_wbctl)  binary="$bin_dir/stamp_tinystm_wbctl" ;;
        tinystm_wt)     binary="$bin_dir/stamp_tinystm_wt" ;;
        singlelock)     binary="$bin_dir/stamp_singlelock" ;;
    esac

    if [ ! -x "$binary" ]; then
        log "  [SKIP] plugin $backend — binary not found at $binary"
        return
    fi

    # STAMP benchmarks
    for bench in vacation kmeans labyrinth genome intruder ssca2 bayes yada; do
        local params="${STAMP_PLUGIN_PARAMS[$bench]}"
        local cmd="$binary -t $threads -b ${bench:0:1} $params"
        run_bench "plugin" "$backend" "$bench" "$threads" "$sample" "$cmd"
    done

    # Non-STAMP
    local tpcc_cmd=$(eval "echo \"$TPCC_PARAMS\"")
    local tpcc_bin="benchmarks/plugin/tpcc/bin/tpcc_${backend}"
    [ -x "$tpcc_bin" ] && run_bench "plugin" "$backend" "tpcc" "$threads" "$sample" "$tpcc_bin $tpcc_cmd"

    local ycsb_cmd=$(eval "echo \"$YCSB_PARAMS\"")
    local ycsb_bin="benchmarks/plugin/ycsb/bin/ycsb_${backend}"
    [ -x "$ycsb_bin" ] && run_bench "plugin" "$backend" "ycsb" "$threads" "$sample" "$ycsb_bin $ycsb_cmd"

    local stm7_cmd=$(eval "echo \"$STM7_PLUGIN_PARAMS\"")
    local stm7_bin="benchmarks/plugin/stmbench7/bin/stmbench7_${backend}"
    [ -x "$stm7_bin" ] && run_bench "plugin" "$backend" "stmbench7" "$threads" "$sample" "$stm7_bin $stm7_cmd"
}

# ── Run expli C++ implementation ────────────────────────────────────────
run_expli() {
    local backend="$1" threads="$2" sample="$3"
    local bin_dir="benchmarks/cpp/bin"

    for bench in vacation kmeans labyrinth genome intruder ssca2 bayes yada; do
        local binary="$bin_dir/$bench"
        [ ! -x "$binary" ] && continue
        local params="${STAMP_EXPLI_PARAMS[$bench]}"
        local cmd="$binary -p $threads $params"
        run_bench "expli" "$backend" "$bench" "$threads" "$sample" "$cmd"
    done

    # Non-STAMP
    local tpcc_cmd=$(eval "echo \"$TPCC_PARAMS\"")
    [ -x "$bin_dir/tpcc" ] && run_bench "expli" "$backend" "tpcc" "$threads" "$sample" "$bin_dir/tpcc $tpcc_cmd"

    local ycsb_cmd=$(eval "echo \"$YCSB_PARAMS\"")
    [ -x "$bin_dir/ycsb" ] && run_bench "expli" "$backend" "ycsb" "$threads" "$sample" "$bin_dir/ycsb $ycsb_cmd"

    local stm7_cmd=$(eval "echo \"$STM7_EXPLI_PARAMS\"")
    [ -x "$bin_dir/stmbench7" ] && run_bench "expli" "$backend" "stmbench7" "$threads" "$sample" "$bin_dir/stmbench7 $stm7_cmd"
}

# ── Run Rust implementation ─────────────────────────────────────────────
run_rust() {
    local backend="$1" threads="$2" sample="$3"
    local rust_dir="explicit_api/rust/workspace"

    # Map backend to Rust feature/bin name
    local feature
    case "$backend" in
        norec)          feature="norec" ;;
        tinystm_wbctl)  feature="wbctl" ;;
        tinystm_wt)     feature="wt" ;;
        singlelock)     feature="tsxsgl" ;;
    esac

    for bench in vacation kmeans labyrinth genome intruder ssca2 bayes yada; do
        local params="${STAMP_RUST_PARAMS[$bench]}"
        local cmd="cargo run --release -p benchmarks --features $feature --bin stamp_${bench} -- -p $threads $params"
        run_bench "rust" "$backend" "$bench" "$threads" "$sample" "cd $rust_dir && $cmd"
    done
}

# ── Uninstrumented baseline ────────────────────────────────────────────
run_uninstrumented() {
    log "Running uninstrumented baseline (1 thread)..."

    # For expli, build with bench_stubs (no TM)
    local bin_dir="benchmarks/cpp/bin"
    local outdir="$RESULTS_BASE/raw/uninstrumented/none"
    mkdir -p "$outdir"

    for bench in vacation kmeans labyrinth genome intruder ssca2 bayes yada; do
        local binary="$bin_dir/${bench}_stubs"
        [ ! -x "$binary" ] && continue
        local params="${STAMP_EXPLI_PARAMS[$bench]}"
        local outfile="$outdir/uninstrumented_none_${bench}_1t_s1.txt"
        log "  ${bench}..."
        $TIMEOUT_CMD $TIMEOUT bash -c "$binary -p 1 $params" > "$outfile" 2>&1 || true
    done
}

# ── Main ──────────────────────────────────────────────────────────────────
main() {
    echo ""
    blue "================================================"
    blue "  Benchmark Suite: 3 impls × 4 backends × 4 threads"
    blue "  Results: $RESULTS_BASE"
    blue "================================================"
    echo ""

    build_all

    local total=$(( $(echo "$THREADS" | wc -w) * SAMPLES ))
    log "Total combos per backend: $total runs"

    for backend in norec tinystm_wbctl tinystm_wt singlelock; do
        echo ""
        blue "─── Backend: $backend ───"
        for impl in plugin expli rust; do
            log "  Implementation: $impl"
            for threads in $THREADS; do
                for sample in $(seq 1 $SAMPLES); do
                    case "$impl" in
                        plugin) run_plugin "$backend" "$threads" "$sample" ;;
                        expli)  run_expli  "$backend" "$threads" "$sample" ;;
                        rust)   run_rust  "$backend" "$threads" "$sample" ;;
                    esac
                done
            done
        done
    done

    echo ""
    blue "─── Uninstrumented baseline ───"
    run_uninstrumented

    echo ""
    blue "─── Analysis ───"

    # Use the existing analyze.py from run_compare_all.sh if available, or inline
    if [ -f "$RESULTS_BASE/analyze.py" ]; then
        python3 "$RESULTS_BASE/analyze.py" "$RESULTS_BASE" 2>&1
    else
        log "Analysis script not found — run_compare_all.sh embedded script not extracted."
        log "Run analysis manually: python3 analyze_results.py $RESULTS_BASE"
    fi

    echo ""
    green "================================================"
    green "  Complete: results in $RESULTS_BASE"
    green "================================================"
}

main "$@"
