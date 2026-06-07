#!/usr/bin/env bash
# ============================================================================
# Comprehensive Backend Comparison Runner
#
# Compares TSXSGL, TinySTM WBCTL, NOrec, and SingleGlobalLock across:
#   - 8 STAMP benchmarks (plugin/expli/rust)
#   - TPC-C (plugin/expli)
#   - STMbench7 (plugin/expli)
# With uninstrumented baseline at 1 thread.
# Threads: 1 2 4 7 10 14 21 28 35 42 49 56
# Samples: 3
# ============================================================================

# pipefail requires bash 4+ (not available on macOS's default bash 3.x)
if set -o pipefail 2>/dev/null; then
    set -euo pipefail
else
    set -eu
fi
SELF="$(cd "$(dirname "$0")/../.." && pwd -P)/benchmarks/scripts/$(basename "$0")"
cd "$(dirname "$0")/../.."

# ── Configuration ─────────────────────────────────────────────────────────
THREAD_LIST="${THREADS:-"1 2 4 7 10 14 21 28 35 42 49 56"}"
SAMPLES=3
TIMEOUT=600
STAMP_WORKLOAD="fixed"     # STAMP is time-to-complete (fixed work)

# Plugin STAMP params (STAMP.cpp unified binary, -b dispatch)
# These use benchmark-specific flags; note that -t is consumed globally
# for thread count, so vacation's -t (tasks) and kmeans's -t (threshold)
# use defaults (4096 tasks / 0.00001 threshold).
declare -A STAMP_PLUGIN_PARAMS
STAMP_PLUGIN_PARAMS[vacation]="-n 2 -q 90 -r 16384 -u 98"
STAMP_PLUGIN_PARAMS[kmeans]=""           # uses defaults: -m 40 -n 40
STAMP_PLUGIN_PARAMS[labyrinth]="-x 8 -y 8 -z 8 -n 64"
STAMP_PLUGIN_PARAMS[genome]="-g 16384 -s 64 -n 1000000"
STAMP_PLUGIN_PARAMS[intruder]="-a 10 -l 128 -n 5120 -s 1"
STAMP_PLUGIN_PARAMS[ssca2]="-s 14 -u 1.0 -l 3 -p 3 -i 3"
STAMP_PLUGIN_PARAMS[bayes]="-v 16 -r 32 -n 2 -p 2 -e 4"
STAMP_PLUGIN_PARAMS[yada]="-a 20 -j 0.5"

# Expli C++ STAMP params (individual binaries)
declare -A STAMP_EXPLI_PARAMS
STAMP_EXPLI_PARAMS[vacation]="-r 16384 -n 2 -u 98 -t 4096"
STAMP_EXPLI_PARAMS[kmeans]="-k 8 -d 2 -n 200"
STAMP_EXPLI_PARAMS[labyrinth]="-x 8 -y 8 -z 8 -n 64"
STAMP_EXPLI_PARAMS[genome]="-g 16384 -s 64 -n 1000000"
STAMP_EXPLI_PARAMS[intruder]="-a 10 -l 128 -n 5120 -s 1"
STAMP_EXPLI_PARAMS[ssca2]="-s 14 -u 1.0 -l 3 -m 3 -i 3"
STAMP_EXPLI_PARAMS[bayes]="-v 16 -r 32 -n 2 -p 2 -e 4"
STAMP_EXPLI_PARAMS[yada]="-a 20 -j 0.5"

# Rust STAMP params (individual binaries)
declare -A STAMP_RUST_PARAMS
STAMP_RUST_PARAMS[vacation]="-r 16384 -n 2 -u 98 -t 4096"
STAMP_RUST_PARAMS[kmeans]="-k 8 -d 2 -n 200"
STAMP_RUST_PARAMS[labyrinth]="-x 8 -y 8 -z 8 -n 64"
STAMP_RUST_PARAMS[genome]="-g 16384 -s 64 -n 1000000"
STAMP_RUST_PARAMS[intruder]="-a 10 -l 128 -n 5120 -s 1"
STAMP_RUST_PARAMS[ssca2]="-s 14 -u 1.0 -l 3 -m 3 -i 3"
STAMP_RUST_PARAMS[bayes]="-v 16 -r 32 -n 2 -e 4 -i 2"
STAMP_RUST_PARAMS[yada]="-a 20 -j 0.5"

# Which benchmarks exist per path
STAMP_BENCHES_PLUGIN=(vacation kmeans labyrinth genome intruder ssca2 bayes yada)
STAMP_BENCHES_EXPLI=(vacation kmeans labyrinth genome intruder ssca2 bayes yada)
STAMP_BENCHES_RUST=(vacation kmeans labyrinth genome intruder ssca2 bayes yada)

# ── Results directory ─────────────────────────────────────────────────────
RESULTS_DIR="benchmark_results/compare_all_$(date +%Y%m%d_%H%M%S)"
RAW_DIR="$RESULTS_DIR/raw"
SUMMARY="$RESULTS_DIR/SUMMARY.txt"
CSV="$RESULTS_DIR/results.csv"
PROGRESS="$RESULTS_DIR/progress.txt"
SKIP_FILE="$RESULTS_DIR/skip_combos.txt"
SAVEPOINT="$RESULTS_DIR/savepoint.txt"
mkdir -p "$RAW_DIR"/{plugin,expli,rust}/{tsxsgl,tinystm_wbctl,norec,sgl,uninstrumented}
touch "$SKIP_FILE" "$SAVEPOINT"
# Known-broken combos: skip all stmbench7 with plugin TinySTM WBCTL
# (crashes during worker init at 2+ threads due to std::vector reallocation issue)
echo "plugin tinystm_wbctl stmbench7" >> "$SKIP_FILE"

# Early validation
if [ ! -d "llvm_tm_plugin/bin" ]; then
    echo "ERROR: llvm_tm_plugin/bin/ not found. Build the plugin first."
    exit 1
fi
if [ ! -f "llvm_tm_plugin/bin/libTMInstrument.so" ]; then
    echo "ERROR: libTMInstrument.so not found. Run 'make -C llvm_tm_plugin' first."
    exit 1
fi

# ── Stats counters ────────────────────────────────────────────────────────
RUN_COMPLETED=0
RUN_TIMEOUT=0
RUN_CRASH=0
RUN_FAIL=0
RUN_SKIPPED=0
RUN_TOTAL=0

log_progress() {
    {
        echo "=== Progress at $(date) ==="
        echo "Total run attempts: $RUN_TOTAL"
        echo "  Completed: $RUN_COMPLETED"
        echo "  Timeout:   $RUN_TIMEOUT"
        echo "  Crash:     $RUN_CRASH"
        echo "  Fail:      $RUN_FAIL"
        echo "  Skipped:   $RUN_SKIPPED"
        echo "Last: $*"
        echo ""
    } > "$PROGRESS"
}

# ── Skip-list: mark consistently-crashing (impl, backend, bench) combos ──
is_skipped() {
    grep -qx "$1 $2 $3" "$SKIP_FILE" 2>/dev/null
}
mark_broken() {
    local label="${1}_${2}_${3}"
    if ! grep -qx "$1 $2 $3" "$SKIP_FILE" 2>/dev/null; then
        echo "$1 $2 $3" >> "$SKIP_FILE"
        printf "  *** %-60s ALL REMAINING SKIPPED (consistent crash) ***\n" "$label" >&2
    fi
}

# ── Helper: run one benchmark ─────────────────────────────────────────────
# run_one <impl> <backend> <bench_name> <threads> <sample> -- <binary> [args...]
run_one() {
    local impl="$1"; shift
    local backend="$1"; shift
    local bench="$1"; shift
    local threads="$1"; shift
    local sample="$1"; shift
    local sep="$1"; shift  # should be "--"
    local binary="$1"; shift

    local combo="${impl}_${backend}_${bench}"
    local label="${combo}_${threads}t_s${sample}"
    local outfile="$RAW_DIR/$impl/$backend/${bench}_${threads}t_s${sample}.txt"

    RUN_TOTAL=$((RUN_TOTAL + 1))

    # Skip if this (impl, backend, bench) is known to crash consistently
    if is_skipped "$impl" "$backend" "$bench"; then
        RUN_SKIPPED=$((RUN_SKIPPED + 1))
        printf "  %-60s %s\n" "$label" "SKIP (known-broken: $combo)"
        log_progress "SKIP $label (known-broken: $combo)"
        return 0
    fi

    # Record savepoint before launching (enables resume if interrupted)
    echo "$label" >> "$SAVEPOINT"

    # Skip if already exists and looks successful (resume support)
    if [ -f "$outfile" ] && [ -s "$outfile" ]; then
        if grep -qE 'PASS|Verification passed|done\.|Ops/sec|Results|Time\s*[=:]' "$outfile" 2>/dev/null; then
            RUN_SKIPPED=$((RUN_SKIPPED + 1))
            printf "  %-60s %s\n" "$label" "SKIP (exists)"
            log_progress "SKIP $label (exists)"
            return 0
        fi
        rm -f "$outfile"
    fi

    set +e
    timeout "$TIMEOUT" "$binary" "$@" > "$outfile" 2>&1
    local rc=$?
    set -e

    local status=""
    if [ "$rc" = 124 ]; then
        status="TIMEOUT"
        RUN_TIMEOUT=$((RUN_TIMEOUT + 1))
    elif [ "$rc" -ge 128 ]; then
        local sig=$((rc - 128))
        status="CRASH(sig=$sig)"
        RUN_CRASH=$((RUN_CRASH + 1))
    elif [ "$rc" = 0 ]; then
        status="OK"
        RUN_COMPLETED=$((RUN_COMPLETED + 1))
    else
        status="FAIL(exit=$rc)"
        RUN_FAIL=$((RUN_FAIL + 1))
    fi

    # Quick check for known success indicators
    if [ "$rc" != 124 ]; then
        if grep -qE 'PASS|Verification passed|done\.|Results|Time\s*[=:]' "$outfile" 2>/dev/null; then
            if [ "$status" != "OK" ]; then
                status="OK+"
                RUN_COMPLETED=$((RUN_COMPLETED + 1))
                RUN_FAIL=$((RUN_FAIL - 1))
            fi
        fi
    fi

    # Auto-detect consistently-broken combos: truncated output + no success indicators
    local fsize=$(wc -c < "$outfile" 2>/dev/null || echo 0)
    if [ "$rc" != 0 ] && [ "$fsize" -gt 0 ] && [ "$fsize" -lt 600 ]; then
        if ! grep -qE 'PASS|Results|Time\s*[=:]|Ops/sec|Verification passed|done\.' "$outfile" 2>/dev/null; then
            mark_broken "$impl" "$backend" "$bench"
        fi
    fi

    printf "  %-60s %s\n" "$label" "$status"
    log_progress "$label $status (rc=$rc)"
}

# ── Helper: build plugin target ───────────────────────────────────────────
build_plugin() {
    local dir="$1"; shift
    local target="$1"; shift
    local bin_path="$1"; shift

    if [ -x "$bin_path" ]; then
        echo "  [build] $target already exists, skipping"
        return 0
    fi
    echo "  [build] $target ..."
    set +e
    make -C "$dir" "$target" \
        TM_LINK_OPT="-O3" TM_OPT_LEVEL="-O3" \
        > /tmp/build_${target}.log 2>&1
    local rc=$?
    set -e
    if [ "$rc" != 0 ]; then
        echo "  [build] $target FAILED (see /tmp/build_${target}.log)"
        tail -5 /tmp/build_${target}.log
        return 1
    fi
    echo "  [build] $target OK"
}

# ═══════════════════════════════════════════════════════════════════════════
# PHASE 1: BUILD ALL BINARIES
# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  PHASE 1: BUILDING ALL BINARIES                              ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# ── 1a. Plugin STAMP ─────────────────────────────────────────────────────
echo "=== Building plugin STAMP benchmarks ==="
PLUGIN_STAMP_DIR="benchmarks/plugin/STAMP"
PLUGIN_STAMP_BIN="$PLUGIN_STAMP_DIR/bin"

mkdir -p "$PLUGIN_STAMP_BIN"

build_plugin "$PLUGIN_STAMP_DIR" stamp_tsxsgl         "$PLUGIN_STAMP_BIN/stamp_tsxsgl"         || true
build_plugin "$PLUGIN_STAMP_DIR" stamp_tinystm_wbctl   "$PLUGIN_STAMP_BIN/stamp_tinystm_wbctl"   || true
build_plugin "$PLUGIN_STAMP_DIR" stamp_norec           "$PLUGIN_STAMP_BIN/stamp_norec"           || true
build_plugin "$PLUGIN_STAMP_DIR" stamp_singlelock      "$PLUGIN_STAMP_BIN/stamp_singlelock"      || true
# Build uninstrumented with -O3 for fair baseline
if [ ! -x "$PLUGIN_STAMP_BIN/stamp_uninstrumented" ]; then
    echo "  [build] stamp_uninstrumented ..."
    set +e
    make -C "$PLUGIN_STAMP_DIR" stamp_uninstrumented \
        CXXFLAGS="-std=c++20 -O3 -pthread" \
        > /tmp/build_stamp_uninstrumented.log 2>&1
    rc=$?; set -e
    if [ "$rc" != 0 ]; then echo "  [build] stamp_uninstrumented FAILED"; else echo "  [build] stamp_uninstrumented OK"; fi
fi

# ── 1b. Plugin TPCC ──────────────────────────────────────────────────────
echo "=== Building plugin TPCC benchmarks ==="
PLUGIN_TPCC_DIR="benchmarks/plugin/tpcc"
PLUGIN_TPCC_BIN="$PLUGIN_TPCC_DIR/bin"

mkdir -p "$PLUGIN_TPCC_BIN"

build_plugin "$PLUGIN_TPCC_DIR" tpcc_tsxsgl         "$PLUGIN_TPCC_BIN/tpcc_tsxsgl"         || true
build_plugin "$PLUGIN_TPCC_DIR" tpcc_tinystm_wbctl   "$PLUGIN_TPCC_BIN/tpcc_tinystm_wbctl"   || true
build_plugin "$PLUGIN_TPCC_DIR" tpcc_singlelock      "$PLUGIN_TPCC_BIN/tpcc_singlelock"      || true
if [ ! -x "$PLUGIN_TPCC_BIN/tpcc_uninstrumented" ]; then
    echo "  [build] tpcc_uninstrumented ..."
    set +e
    make -C "$PLUGIN_TPCC_DIR" tpcc_uninstrumented \
        CXXFLAGS="-std=c++20 -O3 -pthread" \
        > /tmp/build_tpcc_uninstrumented.log 2>&1
    rc=$?; set -e
    if [ "$rc" != 0 ]; then echo "  [build] tpcc_uninstrumented FAILED"; else echo "  [build] tpcc_uninstrumented OK"; fi
fi
# Note: NOrec is NOT available for plugin TPCC (no Makefile target)

# ── 1c. Plugin STMbench7 ─────────────────────────────────────────────────
echo "=== Building plugin STMbench7 benchmarks ==="
PLUGIN_STM7_DIR="benchmarks/plugin/stmbench7"
PLUGIN_STM7_BIN="$PLUGIN_STM7_DIR/bin"

mkdir -p "$PLUGIN_STM7_BIN"

build_plugin "$PLUGIN_STM7_DIR" stmbench_tsxsgl         "$PLUGIN_STM7_BIN/stmbench_tsxsgl"         || true
build_plugin "$PLUGIN_STM7_DIR" stmbench_tinystm_wbctl   "$PLUGIN_STM7_BIN/stmbench_tinystm_wbctl"   || true
build_plugin "$PLUGIN_STM7_DIR" stmbench_norec           "$PLUGIN_STM7_BIN/stmbench_norec"           || true
build_plugin "$PLUGIN_STM7_DIR" stmbench_singlelock      "$PLUGIN_STM7_BIN/stmbench_singlelock"      || true
if [ ! -x "$PLUGIN_STM7_BIN/stmbench_uninstrumented" ]; then
    echo "  [build] stmbench_uninstrumented ..."
    set +e
    make -C "$PLUGIN_STM7_DIR" stmbench_uninstrumented \
        CXXFLAGS="-std=c++20 -O3 -pthread" \
        > /tmp/build_stmbench_uninstrumented.log 2>&1
    rc=$?; set -e
    if [ "$rc" != 0 ]; then echo "  [build] stmbench_uninstrumented FAILED"; else echo "  [build] stmbench_uninstrumented OK"; fi
fi

# ── 1d. Expli C++ benchmarks ─────────────────────────────────────────────
echo "=== Building expli C++ benchmarks ==="
EXPLI_DIR="benchmarks/cpp"

for backend_name in TINYSTM NOREC SGL; do
    echo "  [build] expli benchmarks with BACKEND=$backend_name ..."
    set +e
    make -C "$EXPLI_DIR" all BACKEND="$backend_name" > /tmp/build_expli_${backend_name}.log 2>&1
    rc=$?; set -e
    if [ "$rc" != 0 ]; then
        echo "  [build] expli BACKEND=$backend_name FAILED (see /tmp/build_expli_${backend_name}.log)"
        tail -5 /tmp/build_expli_${backend_name}.log
    else
        echo "  [build] expli BACKEND=$backend_name OK"
    fi
done

# ── 1e. Rust benchmarks ─────────────────────────────────────────────────
echo "=== Building Rust STAMP benchmarks ==="
RUST_DIR="benchmarks/rust"
if command -v cargo &>/dev/null; then
    for feature in wbctl norec tsxsgl; do
        echo "  [build] rust --features $feature ..."
        set +e
        cargo build --release --manifest-path "$RUST_DIR/Cargo.toml" \
            --no-default-features --features "$feature" \
            > /tmp/build_rust_${feature}.log 2>&1
        rc=$?; set -e
        if [ "$rc" != 0 ]; then
            echo "  [build] rust --features $feature FAILED (see /tmp/build_rust_${feature}.log)"
            tail -10 /tmp/build_rust_${feature}.log
        else
            echo "  [build] rust --features $feature OK"
        fi
    done
else
    echo "  [SKIP] cargo not found — Rust benchmarks not built"
fi

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  BUILD PHASE COMPLETE                                        ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# ═══════════════════════════════════════════════════════════════════════════
# PHASE 2: RUN ALL BENCHMARKS
# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  PHASE 2: RUNNING BENCHMARKS                                 ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

nthreads=$(echo "$THREAD_LIST" | wc -w)
echo "Thread levels: $THREAD_LIST ($nthreads levels)"
echo "Samples: $SAMPLES"
echo "Timeout: ${TIMEOUT}s"
echo "Results: $RESULTS_DIR"
echo ""

# Count total expected runs
echo "Estimating total runs..."
total_est=0
for impl in plugin expli rust; do
    for backend in tsxsgl tinystm_wbctl norec sgl; do
        case "$impl-$backend" in
            plugin-tsxsgl|plugin-tinystm_wbctl|plugin-sgl)
                # STAMP (8 benches) + TPCC + STM7 = 10
                total_est=$((total_est + (8 + 1 + 1) * nthreads * SAMPLES))
                ;;
            plugin-norec)
                # STAMP (8) + STM7 (1) = 9 (TPCC has NOrec)
                total_est=$((total_est + (8 + 1) * nthreads * SAMPLES))
                ;;
            expli-tinystm_wbctl|expli-norec|expli-sgl)
                # STAMP (8) + TPCC + STM7 = 10
                total_est=$((total_est + (8 + 1 + 1) * nthreads * SAMPLES))
                ;;
            rust-tsxsgl|rust-tinystm_wbctl|rust-norec)
                # STAMP (8) only
                total_est=$((total_est + 8 * nthreads * SAMPLES))
                ;;
            rust-sgl|expli-tsxsgl)
                # Not available
                ;;
        esac
    done
done
# Uninstrumented: 1 thread only, 1 sample
total_est=$((total_est + (8 + 1 + 1) * 1 * 1))  # stamp + tpcc + stm7 at 1t
total_est=$((total_est + nthreads * SAMPLES * 1)) # extra slack
echo "Estimated total runs: ~$total_est"
echo ""

log_progress "Starting runs"

# ── 2a. Uninstrumented baseline (plugin, 1 thread only) ──────────────────
echo "=== Uninstrumented Baseline (1 thread) ==="
for bench in "${STAMP_BENCHES_PLUGIN[@]}"; do
    params="${STAMP_PLUGIN_PARAMS[$bench]}"
    if [ -x "$PLUGIN_STAMP_BIN/stamp_uninstrumented" ]; then
        run_one plugin uninstrumented "$bench" 1 1 -- \
            "$PLUGIN_STAMP_BIN/stamp_uninstrumented" -b "$bench" -t 1 $params
    fi
done

for bench in tpcc stmbench7; do
    case "$bench" in
        tpcc)
            if [ -x "$PLUGIN_TPCC_BIN/tpcc_uninstrumented" ]; then
                run_one plugin uninstrumented tpcc 1 1 -- \
                    "$PLUGIN_TPCC_BIN/tpcc_uninstrumented" -t 1 -d 5000
            fi
            ;;
        stmbench7)
            if [ -x "$PLUGIN_STM7_BIN/stmbench_uninstrumented" ]; then
                run_one plugin uninstrumented stmbench7 1 1 -- \
                    "$PLUGIN_STM7_BIN/stmbench_uninstrumented" -t 1 -d 5000 -w 1
            fi
            ;;
    esac
done

# ── 2b. Plugin path ──────────────────────────────────────────────────────
echo ""
echo "=== Plugin Path ==="

run_plugin_stamp() {
    local backend="$1"
    local binary="$PLUGIN_STAMP_BIN/stamp_$backend"
    [ -x "$binary" ] || return 0
    for bench in "${STAMP_BENCHES_PLUGIN[@]}"; do
        params="${STAMP_PLUGIN_PARAMS[$bench]}"
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one plugin "$backend" "$bench" "$t" "$s" -- \
                    "$binary" -b "$bench" -t "$t" $params
            done
        done
    done
}

run_plugin_tpcc() {
    local backend="$1"
    local binary="$PLUGIN_TPCC_BIN/tpcc_$backend"
    [ -x "$binary" ] || return 0
    for t in $THREAD_LIST; do
        for s in $(seq 1 $SAMPLES); do
            run_one plugin "$backend" tpcc "$t" "$s" -- \
                "$binary" -t "$t" -d 5000
        done
    done
}

run_plugin_stm7() {
    local backend="$1"
    local binary="$PLUGIN_STM7_BIN/stmbench_$backend"
    [ -x "$binary" ] || return 0
    for t in $THREAD_LIST; do
        for s in $(seq 1 $SAMPLES); do
            run_one plugin "$backend" stmbench7 "$t" "$s" -- \
                "$binary" -t "$t" -d 5000 -w 1
        done
    done
}

for backend in tsxsgl tinystm_wbctl norec sgl; do
    run_plugin_stamp "$backend"
    run_plugin_tpcc "$backend"
    run_plugin_stm7 "$backend"
done

# ── 2c. Expli C++ path ───────────────────────────────────────────────────
echo ""
echo "=== Expli C++ Path ==="

run_expli() {
    local backend_label="$1"  # tinystm_wbctl|norec|sgl
    local backend_var="$2"    # TINYSTM|NOREC|SGL
    local bin_dir="$EXPLI_DIR/bin"

    # STAMP benchmarks
    for bench in "${STAMP_BENCHES_EXPLI[@]}"; do
        local binary="$bin_dir/$bench"
        [ -x "$binary" ] || continue
        params="${STAMP_EXPLI_PARAMS[$bench]}"
        local tflag="-p"
        # bayes and yada use -t instead of -p
        [ "$bench" = "bayes" ] || [ "$bench" = "yada" ] && tflag="-t"
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one expli "$backend_label" "$bench" "$t" "$s" -- \
                    "$binary" "$tflag" "$t" $params
            done
        done
    done

    # TPCC
    local tpcc_bin="$bin_dir/tpcc"
    if [ -x "$tpcc_bin" ]; then
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one expli "$backend_label" tpcc "$t" "$s" -- \
                    "$tpcc_bin" -t "$t" -d 5000
            done
        done
    fi

    # STMbench7
    local stm7_bin="$bin_dir/stmbench7"
    if [ -x "$stm7_bin" ]; then
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one expli "$backend_label" stmbench7 "$t" "$s" -- \
                    "$stm7_bin" "$t" 5000
            done
        done
    fi
}

# Map backend labels to Makefile BACKEND values
run_expli tinystm_wbctl TINYSTM
run_expli norec NOREC
run_expli sgl SGL

# ── 2d. Rust path ─────────────────────────────────────────────────────────
echo ""
echo "=== Rust Path ==="

run_rust() {
    local backend_label="$1"
    local feature="$2"
    local target_dir="$RUST_DIR/target/release"

    for bench in "${STAMP_BENCHES_RUST[@]}"; do
        local binary="$target_dir/stamp_${bench}"
        [ -x "$binary" ] || continue
        params="${STAMP_RUST_PARAMS[$bench]}"
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one rust "$backend_label" "$bench" "$t" "$s" -- \
                    "$binary" -p "$t" $params
            done
        done
    done
}

run_rust tinystm_wbctl wbctl
run_rust norec norec
run_rust tsxsgl tsxsgl

# ── Final progress ────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  RUN PHASE COMPLETE                                         ║"
echo "╚══════════════════════════════════════════════════════════════╝"
log_progress "Run phase complete"

# ═══════════════════════════════════════════════════════════════════════════
# PHASE 3: ANALYZE RESULTS
# ═══════════════════════════════════════════════════════════════════════════
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  PHASE 3: ANALYZING RESULTS                                  ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Write embedded Python analysis script
cat > "$RESULTS_DIR/analyze.py" << 'PYEOF'
#!/usr/bin/env python3
"""Parse benchmark results, compute mean/stddev, output CSV and summary."""
import os, sys, re, csv, math
from collections import defaultdict

RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else "."
RAW_DIR = os.path.join(RESULTS_DIR, "raw")

# ── Parsing patterns ──────────────────────────────────────────────

# Elapsed time in seconds (STAMP fixed-work benchmarks)
# Various output formats across plugin/expli/rust
TIME_PATTERNS = [
    re.compile(r'Time\s*[=:]\s*([\d.]+)'),                   # "Time = 0.123456" | "Time: 0.123456"
    re.compile(r'Elapsed time\s*[=:]\s*([\d.]+)\s*seconds'),  # "Elapsed time = 1.234567 seconds"
    re.compile(r'Time taken for all is\s*([\d.]+)\s*sec'),    # "Time taken for all is 1.234567 sec."
    re.compile(r'Elapsed:\s+(\d+)\s+ms'),                      # "Elapsed: 1234 ms" (plugin yada)
    re.compile(r'Time\s*=\s*(\d+)\s*ms'),                      # "Time = 1234 ms" (rust)
    re.compile(r'Results\s*\((\d+)\s*ms\)'),                   # "Results (1234 ms):" (expli bayes/yada)
]

# Throughput in ops/sec (TPC-C, STMbench7, Rust)
OPS_PATTERNS = [
    re.compile(r'Ops/sec\s*:\s*([\d.]+)'),          # "Ops/sec: 12345.67"
    re.compile(r'([\d.]+)\s*ops/sec'),               # "12345.67 ops/sec" (rust)
    re.compile(r'(\d+)\s*ops/sec'),                   # "12345 ops/sec"
    re.compile(r'([\d.]+)\s*Ops/sec'),                # "12345.67 Ops/sec"
    re.compile(r'Rate:\s*([\d.]+)\s*ops/sec'),        # "Rate: 12345 ops/sec" (expli bayes/yada)
    re.compile(r'Business ops:\s+\d+\s+\(([\d.]+)\s+ops/sec\)'),  # "Business ops: 1000 (1234 ops/sec)" (expli stm7)
]

# ── Parse a single result file ────────────────────────────────────
def parse_file(filepath):
    """Return (time_seconds, ops_per_sec) or (None, None) if unparseable."""
    try:
        with open(filepath) as f:
            text = f.read()
    except Exception:
        return None, None

    time_sec = None
    ops_sec = None

    # Try to extract time in seconds
    for pat in TIME_PATTERNS:
        m = pat.search(text)
        if m:
            val = float(m.group(1))
            # Check if it's in ms vs seconds
            if 'ms' in pat.pattern:
                time_sec = val / 1000.0
            else:
                time_sec = val
            break

    # Try to extract ops/sec
    for pat in OPS_PATTERNS:
        m = pat.search(text)
        if m:
            ops_sec = float(m.group(1))
            break

    # Fallback: compute ops/sec from total_ops + elapsed
    if ops_sec is None and time_sec is not None and time_sec > 0:
        total_m = re.search(r'Total ops\s*[=:]\s*(\d+)', text)
        if total_m:
            ops_sec = float(total_m.group(1)) / time_sec

    return time_sec, ops_sec

# ── Walk results ──────────────────────────────────────────────────
data = []  # list of dicts

file_pattern = re.compile(
    r'(?P<impl>plugin|expli|rust)_'
    r'(?P<backend>[^_]+)_'
    r'(?P<bench>[^_]+)_'
    r'(?P<threads>\d+)t_s(?P<sample>\d+)\.txt'
)

for root, dirs, files in os.walk(RAW_DIR):
    for fname in files:
        if not fname.endswith('.txt'):
            continue
        fpath = os.path.join(root, fname)

        # Extract metadata from directory path + filename
        rel = os.path.relpath(fpath, RAW_DIR)
        parts = rel.replace('.txt', '').split('/')
        if len(parts) >= 3:
            impl = parts[0]      # plugin, expli, rust
            backend = parts[1]   # tsxsgl, tinystm_wbctl, norec, sgl, uninstrumented
            rest = parts[2]      # <bench>_<threads>t_s<sample>
        else:
            continue

        # Parse filename
        m = re.match(r'(.+)_(\d+)t_s(\d+)', rest)
        if not m:
            continue
        bench = m.group(1)
        threads = int(m.group(2))
        sample = int(m.group(3))

        time_sec, ops_sec = parse_file(fpath)

        # Skip files that produced no data (crashed/timeout/empty)
        if time_sec is None and ops_sec is None:
            continue

        data.append({
            'impl': impl,
            'backend': backend,
            'bench': bench,
            'threads': threads,
            'sample': sample,
            'time_sec': time_sec,
            'ops_sec': ops_sec,
            'path': rel,
        })

# ── Aggregate: mean & stddev per (impl, backend, bench, threads) ──
agg = defaultdict(list)
for d in data:
    # Use time_sec for STAMP benchmarks, ops_sec for tpcc/stmbench7
    key = (d['impl'], d['backend'], d['bench'], d['threads'])
    # Determine metric: prefer ops_sec for duration-based, time_sec for fixed-work
    if d['bench'] in ('tpcc', 'stmbench7') and d['ops_sec'] is not None:
        metric = d['ops_sec']
    elif d['time_sec'] is not None:
        metric = d['time_sec']
    elif d['ops_sec'] is not None:
        metric = d['ops_sec']
    else:
        continue
    agg[key].append(metric)

# Compute stats
results = []
for key, values in agg.items():
    impl, backend, bench, threads = key
    n = len(values)
    mean = sum(values) / n
    if n > 1:
        variance = sum((v - mean) ** 2 for v in values) / n
        stddev = variance ** 0.5
    else:
        stddev = 0.0

    # Determine metric type
    if bench in ('tpcc', 'stmbench7'):
        metric_type = 'ops/sec'
    else:
        metric_type = 'time_sec'

    results.append((impl, backend, bench, threads, mean, stddev, n, metric_type))

# Sort
results.sort(key=lambda r: (r[0], r[1], r[2], r[3]))

# ── Write CSV ─────────────────────────────────────────────────────
csv_path = os.path.join(RESULTS_DIR, 'results.csv')
with open(csv_path, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['impl', 'backend', 'benchmark', 'threads', 'mean', 'stddev', 'n', 'metric'])
    for r in results:
        w.writerow(r)

print(f"CSV: {csv_path} ({len(results)} aggregated entries from {len(data)} data points)")

# ── Write Summary ─────────────────────────────────────────────────
summary_path = os.path.join(RESULTS_DIR, 'SUMMARY.txt')
lines = []
lines.append("=" * 72)
lines.append("  Comprehensive Backend Comparison Results")
lines.append("=" * 72)
lines.append(f"  Data points: {len(data)}")
lines.append(f"  Aggregated:  {len(results)} entries")
lines.append("")

# Group by benchmark
current_bench = None
for r in results:
    impl, backend, bench, threads, mean, stddev, n, mtype = r
    label = f"{bench}"
    if label != current_bench:
        lines.append(f"\n{'─' * 72}")
        lines.append(f"  {label.upper()}")
        lines.append(f"{'─' * 72}")
        lines.append(f"  {'Impl':8s} {'Backend':14s} {'Threads':>8s} {'Mean':>14s} {'StdDev':>10s} {'N':>4s} {'Metric':>10s}")
        lines.append(f"  {'─'*8} {'─'*14} {'─'*8} {'─'*14} {'─'*10} {'─'*4} {'─'*10}")
        current_bench = label

    # Format mean/stddev nicely
    if mtype == 'time_sec':
        mean_s = f"{mean:.6f}"
        std_s = f"{stddev:.6f}"
    else:
        mean_s = f"{mean:.1f}"
        std_s = f"{stddev:.1f}"

    lines.append(f"  {impl:8s} {backend:14s} {threads:8d} {mean_s:>14s} {std_s:>10s} {n:4d} {mtype:>10s}")

# Speedup table: uninstrumented vs backends
lines.append(f"\n\n{'=' * 72}")
lines.append(f"  SPEEDUP vs UNINSTRUMENTED (1 thread, plugin path)")
lines.append(f"{'=' * 72}")
lines.append(f"  {'Benchmark':14s} {'Uninstr':>10s} {'TSXSGL':>10s} {'Ratio':>8s} {'WBCTL':>10s} {'Ratio':>8s} {'NOrec':>10s} {'Ratio':>8s} {'SGL':>10s} {'Ratio':>8s}")
lines.append(f"  {'─'*14} {'─'*10} {'─'*10} {'─'*8} {'─'*10} {'─'*8} {'─'*10} {'─'*8} {'─'*10} {'─'*8}")

# Find uninstrumented baseline
baselines = {}
for r in results:
    impl, backend, bench, threads, mean, stddev, n, mtype = r
    if impl == 'plugin' and backend == 'uninstrumented' and threads == 1:
        baselines[bench] = (mean, mtype)

# For each benchmark, show 1-thread backend vs uninstrumented
uni_benches = sorted(set(r[2] for r in results if r[0] == 'plugin'))
for bench in uni_benches:
    if bench not in baselines:
        continue
    base_val, base_type = baselines[bench]
    base_s = f"{base_val:.1f}" if base_type == 'ops/sec' else f"{base_val:.6f}"
    line = f"  {bench:14s} {base_s:>10s}"

    for backend in ('tsxsgl', 'tinystm_wbctl', 'norec', 'sgl'):
        val = None
        for r in results:
            if r[0] == 'plugin' and r[1] == backend and r[2] == bench and r[3] == 1:
                val = r[4]
                break
        if val is not None:
            vs = f"{val:.1f}" if base_type == 'ops/sec' else f"{val:.6f}"
            if base_type == 'ops/sec':
                ratio = val / base_val if base_val > 0 else 0
            else:
                ratio = base_val / val if val > 0 else 0  # lower time is better
            line += f" {vs:>10s} {ratio:>7.2f}x"
        else:
            line += f" {'N/A':>10s} {'N/A':>8s}"
    lines.append(line)

# Write summary
with open(summary_path, 'w') as f:
    f.write('\n'.join(lines) + '\n')

print(f"Summary: {summary_path}")
print("Done.")
PYEOF

cd "$RESULTS_DIR"
python3 analyze.py "$RESULTS_DIR" 2>&1 | tee -a "$SUMMARY"

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  ALL DONE                                                    ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "Results: $RESULTS_DIR"
echo "Summary: $SUMMARY"
echo "CSV:     $CSV"
echo "Progress:$PROGRESS"
echo ""
echo "Final stats: completed=$RUN_COMPLETED timeout=$RUN_TIMEOUT crash=$RUN_CRASH fail=$RUN_FAIL skipped=$RUN_SKIPPED total=$RUN_TOTAL"
