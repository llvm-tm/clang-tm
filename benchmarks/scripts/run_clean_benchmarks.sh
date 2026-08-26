#!/usr/bin/env bash
# ============================================================================
# Clean benchmark runner
# 3 impls × 4 backends × 4 threads × 3 samples + uninstrumented baseline
#
# Backends (by impl):
#   Plugin:  tsxsgl, tinystm_wbctl, norec, sgl (SingleGlobalLock)
#   Expli:   tinystm_wbctl, norec, sgl (+ tinystm_wt via TINYSTM+WT)
#   Rust:    wbctl, norec, tsxsgl (+ wt)
#
# Usage: THREADS="1 2 4 8" SAMPLES=3 ./run_clean_benchmarks.sh
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

THREAD_LIST="${THREADS:-"1 2 4 8"}"
SAMPLES="${SAMPLES:-3}"
TIMEOUT="${TIMEOUT:-180}"
RESULTS_DIR="benchmark_results/clean_$(date +%Y%m%d_%H%M%S)"
RAW_DIR="$RESULTS_DIR/raw"
SUMMARY="$RESULTS_DIR/SUMMARY.txt"
CSV="$RESULTS_DIR/results.csv"
SKIP_FILE="$RESULTS_DIR/skip_combos.txt"

mkdir -p "$RAW_DIR"/{plugin,expli,rust}/{tsxsgl,tinystm_wbctl,tinystm_wt,norec,sgl,uninstrumented}
touch "$SKIP_FILE"

TIMEOUT_CMD="timeout"
if ! command -v timeout &>/dev/null && command -v gtimeout &>/dev/null; then
    TIMEOUT_CMD="gtimeout"
fi

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
red()  { printf '\033[1;31m%s\033[0m\n' "$*"; }
green(){ printf '\033[1;32m%s\033[0m\n' "$*"; }

log "Results: $RESULTS_DIR"

# ── STAMP parameters ─────────────────────────────────────────────────────
declare -A P
P[vacation]="-r 16384 -n 2 -u 98 -t 4096"
P[kmeans]="-k 8 -d 2 -n 200"
P[labyrinth]="-x 8 -y 8 -z 8 -n 64"
P[genome]="-g 16384 -s 64 -n 1000000"
P[intruder]="-a 10 -l 128 -n 5120 -s 1"
P[ssca2]="-s 14 -u 1.0 -l 3 -m 3 -i 3"
P[bayes]="-v 16 -r 32 -n 2 -p 2 -e 4"
P[yada]="-a 20 -j 0.5"

declare -A PPLUGIN
PPLUGIN[vacation]="-n 2 -q 90 -r 16384 -u 98"
PPLUGIN[kmeans]=""
PPLUGIN[labyrinth]="-x 8 -y 8 -z 8 -n 64"
PPLUGIN[genome]="-g 16384 -s 64 -n 1000000"
PPLUGIN[intruder]="-a 10 -l 128 -n 5120 -s 1"
PPLUGIN[ssca2]="-s 14 -u 1.0 -l 3 -p 3 -i 3"
PPLUGIN[bayes]="-v 16 -r 32 -n 2 -p 2 -e 4"
PPLUGIN[yada]="-a 20 -j 0.5"

declare -A PRUST
PRUST[vacation]="-r 16384 -n 2 -u 98 -t 4096"
PRUST[kmeans]="-c 8 -d 2 -n 200"
PRUST[labyrinth]="-x 8 -y 8 -z 8 -n 64"
PRUST[genome]="-g 16384 -s 64 -n 1000000"
PRUST[intruder]="-a 10 -l 128 -n 5120 -s 1"
PRUST[ssca2]="-s 14 -u 1.0 -l 3 -m 3 -i 3"
PRUST[bayes]="-v 16 -r 32 -n 2 -e 4 -i 2"
PRUST[yada]="-a 20 -j 0.5"

STAMP_BENCHES=(vacation kmeans labyrinth genome intruder ssca2 bayes yada)

# ── Build phase ──────────────────────────────────────────────────────────
build_all() {
    log "=== BUILD PHASE ==="

    # Plugin STAMP — build all backends (CXXFLAGS_EXTRA not used by plugin Makefile)
    cd benchmarks/plugin/STAMP
    for target in stamp_tinystm_wbctl stamp_tinystm_wt stamp_norec stamp_singlelock stamp_tsxsgl; do
        log "  Plugin STAMP: $target"
        make -j$(nproc) "$target" 2>&1 | tail -1 || log "  [WARN] Plugin STAMP $target build had issues"
    done
    cd ../..

    # Plugin TPCC, YCSB, STM7 — build individually
    for app_dir in tpcc ycsb stmbench7; do
        local app_bin="benchmarks/plugin/$app_dir"
        if [ -f "$app_bin/Makefile" ]; then
            for be in tinystm_wbctl tinystm_wt norec singlelock tsxsgl; do
                local target="${app_dir}_${be}"
                log "  Plugin $app_dir: $target"
                make -C "$app_bin" -j$(nproc) "$target" 2>&1 | tail -1 || true
            done
        fi
    done

    log "  Expli: will build-on-demand during run phase"

    # Rust
    cd explicit_api/rust/workspace
    for fe in wbctl norec tsxsgl wt; do
        log "  Rust: $fe"
        RUSTFLAGS="-C target-cpu=native" cargo build --release --no-default-features --features "$fe" -p benchmarks 2>&1 | tail -1 || true
    done
    cd ..
}

# ── Single run ───────────────────────────────────────────────────────────
run_one() {
    local impl="$1" backend="$2" bench="$3" threads="$4" sample="$5"
    shift 5
    local outdir="$RAW_DIR/$impl/$backend"
    local outfile="$outdir/${impl}_${backend}_${bench}_${threads}t_s${sample}.txt"
    mkdir -p "$outdir"

    local combo="${impl}_${backend}_${bench}"
    if grep -q "^$combo" "$SKIP_FILE" 2>/dev/null; then
        echo "SKIPPED ($combo on skip-list)" > "$outfile"
        return
    fi

    printf "  [%s/%s] %s %st #%d ... " "$impl" "$backend" "$bench" "$threads" "$sample"
    if $TIMEOUT_CMD $TIMEOUT bash -c "$*" > "$outfile" 2>&1; then
        if grep -qiE "FAIL|CRASH|SEGFAULT|SIGSEGV" "$outfile"; then
            echo "  CRASH"
            echo "CRASH" >> "$outfile"
        else
            echo "  OK"
            echo "OK" >> "$outfile"
        fi
    else
        local ec=$?
        echo "  FAIL(exit=$ec)"
        echo "FAIL(exit=$ec)" >> "$outfile"
    fi
}

# ── Run implementations ──────────────────────────────────────────────────
run_plugin() {
    local backend="$1" threads="$2" sample="$3"
    local bin="benchmarks/plugin/STAMP/bin/stamp_$backend"
    [ ! -x "$bin" ] && return

    for bench in "${STAMP_BENCHES[@]}"; do
        run_one "plugin" "$backend" "$bench" "$threads" "$sample" "$bin" -t "$threads" -b "$bench" ${PPLUGIN[$bench]}
    done

    # TPCC
    local tpcc_bin="benchmarks/plugin/tpcc/bin/tpcc_$backend"
    [ -x "$tpcc_bin" ] && run_one "plugin" "$backend" "tpcc" "$threads" "$sample" "$tpcc_bin" -t "$threads" -d 5000 -w 1

    # YCSB
    local ycsb_bin="benchmarks/plugin/ycsb/bin/ycsb_$backend"
    [ -x "$ycsb_bin" ] && run_one "plugin" "$backend" "ycsb" "$threads" "$sample" "$ycsb_bin" -t "$threads" -d 5000 -k 1000 -i 500 -w a

    # STMbench7
    local stm7_bin="benchmarks/plugin/stmbench7/bin/stmbench_$backend"
    [ -x "$stm7_bin" ] && run_one "plugin" "$backend" "stmbench7" "$threads" "$sample" "$stm7_bin" -t "$threads" -d 5000 -w 1
}

run_expli() {
    local backend="$1" threads="$2" sample="$3"
    local be_tag="$4"  # tinystm_wbctl|tinystm_wt|norec|sgl
    local bin_dir="benchmarks/cpp/bin"
    local make_dir="benchmarks/cpp"

    # Build on-demand for this backend
    local defs=""
    case "$backend" in
        TINYSTM) defs="-DTM_BACKEND_TINYSTM -DDESIGN_WBCTL";;
        WT)      defs="-DTM_BACKEND_TINYSTM -DDESIGN_WT";;
        NOREC)   defs="-DTM_BACKEND_NOREC";;
        SGL)     defs="-DTM_BACKEND_SGL";;
        *) log "  Unknown expli backend: $backend"; return;;
    esac

    log "  Building expli ($be_tag: $backend)..."
    make -C "$make_dir" clean 2>/dev/null
    make -C "$make_dir" -j$(nproc) BACKEND="$backend" CXXFLAGS_EXTRA="-DNDEBUG" 2>&1 | tail -1 || {
        log "  Build failed for expli $be_tag"
        return
    }

    for bench in "${STAMP_BENCHES[@]}"; do
        local binary="$bin_dir/$bench"
        [ ! -x "$binary" ] && continue
        run_one "expli" "$be_tag" "$bench" "$threads" "$sample" "$binary" -p "$threads" ${P[$bench]}
    done

    [ -x "$bin_dir/tpcc" ]      && run_one "expli" "$be_tag" "tpcc"      "$threads" "$sample" "$bin_dir/tpcc"      -t "$threads" -d 5000 -w 1
    [ -x "$bin_dir/ycsb" ]      && run_one "expli" "$be_tag" "ycsb"      "$threads" "$sample" "$bin_dir/ycsb"      -t "$threads" -d 5000 -k 1000 -i 500 -w a
    [ -x "$bin_dir/stmbench7" ] && run_one "expli" "$be_tag" "stmbench7" "$threads" "$sample" "$bin_dir/stmbench7"  "$threads" 5000
}

run_rust() {
    local backend="$1" threads="$2" sample="$3"
    local feature="$4"  # wbctl|norec|tsxsgl|wt
    local rust_dir="explicit_api/rust/workspace"

    for bench in "${STAMP_BENCHES[@]}"; do
        local params="${PRUST[$bench]}"
        run_one "rust" "$backend" "$bench" "$threads" "$sample" \
            "cd '$rust_dir' && cargo run --release --no-default-features --features '$feature' -p benchmarks --bin 'stamp_${bench}' -- -p '$threads' $params"
    done
}

run_uninstrumented() {
    local bin_dir="benchmarks/cpp/bin"
    log "  Uninstrumented baseline (1t)..."
    # Build stubs on demand (run_expli's make clean may have wiped them)
    make -C benchmarks/cpp -j$(nproc) stubs 2>&1 | tail -1 || {
        log "  [WARN] Stubs build failed"
        return
    }
    for bench in "${STAMP_BENCHES[@]}"; do
        local binary="$bin_dir/${bench}_stubs"
        [ ! -x "$binary" ] && continue
        run_one "expli" "uninstrumented" "$bench" "1" "1" "$binary" -p 1 ${P[$bench]}
    done
    # Non-STAMP benchmarks
    local tpcc_s="$bin_dir/tpcc_stubs"
    [ -x "$tpcc_s" ] && run_one "expli" "uninstrumented" "tpcc" "1" "1" "$tpcc_s" -t 1 -d 5000 -w 1
    local ycsb_s="$bin_dir/ycsb_stubs"
    [ -x "$ycsb_s" ] && run_one "expli" "uninstrumented" "ycsb" "1" "1" "$ycsb_s" -t 1 -d 5000 -k 1000 -i 500 -w a
    local stm7_s="$bin_dir/stmbench7_stubs"
    [ -x "$stm7_s" ] && run_one "expli" "uninstrumented" "stmbench7" "1" "1" "$stm7_s" 1 5000
}

# ── Main ──────────────────────────────────────────────────────────────────
main() {
    echo ""
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║  Clean Benchmark Runner                              ║"
    echo "║  3 impls × 4 backends × $(echo $THREAD_LIST | wc -w) threads × $SAMPLES samples  ║"
    echo "╚══════════════════════════════════════════════════════╝"
    echo "  Results: $RESULTS_DIR"
    echo ""

    build_all

    echo ""
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║  RUN PHASE                                           ║"
    echo "╚══════════════════════════════════════════════════════╝"

    for threads in $THREAD_LIST; do
        for sample in $(seq 1 $SAMPLES); do
            echo ""
            log "=== Threads=$threads sample=$sample ==="

            # Plugin
            for be in tsxsgl tinystm_wbctl tinystm_wt norec singlelock; do
                run_plugin "$be" "$threads" "$sample"
            done

            # Expli
            for be_entry in "tinystm_wbctl TINYSTM" "tinystm_wt TINYSTM" "norec NOREC" "sgl SGL"; do
                read -r be_tag be_var <<< "$be_entry"
                run_expli "$be_var" "$threads" "$sample" "$be_tag"
            done

            # Rust
            for fe_entry in "tinystm_wbctl wbctl" "tinystm_wt wt" "norec norec" "sgl tsxsgl"; do
                read -r be_tag feature <<< "$fe_entry"
                run_rust "$be_tag" "$threads" "$sample" "$feature"
            done
        done
    done

    # Uninstrumented baseline
    echo ""
    log "=== Uninstrumented baseline ==="
    run_uninstrumented

    # Analysis
    echo ""
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║  ANALYSIS                                            ║"
    echo "╚══════════════════════════════════════════════════════╝"

    # Write embedded analysis script
    cat > "$RESULTS_DIR/analyze.py" << 'PYEOF'
#!/usr/bin/env python3
"""Parse benchmark results, compute mean/stddev, output CSV and summary."""
import os, sys, re, csv, math
from collections import defaultdict

RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else "."
RAW_DIR = os.path.join(RESULTS_DIR, "raw")

TIME_PATTERNS = [
    re.compile(r'Time\s*[=:]\s*([\d.]+)'),
    re.compile(r'Elapsed time\s*[=:]\s*([\d.]+)\s*seconds'),
    re.compile(r'Time taken for all is\s*([\d.]+)\s*sec'),
    re.compile(r'Elapsed:\s+(\d+)\s+ms'),
    re.compile(r'Time\s*=\s*(\d+)\s*ms'),
    re.compile(r'Results\s*\((\d+)\s*ms\)'),
]
OPS_PATTERNS = [
    re.compile(r'Ops/sec\s*:\s*([\d.]+)'),
    re.compile(r'([\d.]+)\s*ops/sec'),
    re.compile(r'(\d+)\s*ops/sec'),
    re.compile(r'([\d.]+)\s*Ops/sec'),
    re.compile(r'Rate:\s*([\d.]+)\s*ops/sec'),
    re.compile(r'Business ops:\s+\d+\s+\(([\d.]+)\s+ops/sec\)'),
]

def parse_file(filepath):
    try:
        with open(filepath) as f:
            text = f.read()
    except Exception:
        return None, None
    time_sec = None
    ops_sec = None
    for pat in TIME_PATTERNS:
        m = pat.search(text)
        if m:
            val = float(m.group(1))
            time_sec = val / 1000.0 if 'ms' in pat.pattern else val
            break
    for pat in OPS_PATTERNS:
        m = pat.search(text)
        if m:
            ops_sec = float(m.group(1))
            break
    if ops_sec is None and time_sec is not None and time_sec > 0:
        total_m = re.search(r'Total ops\s*[=:]\s*(\d+)', text)
        if total_m:
            ops_sec = float(total_m.group(1)) / time_sec
    return time_sec, ops_sec

file_pattern = re.compile(
    r'(?P<impl>[^_]+)_(?P<backend>[^_]+)_(?P<bench>[^_]+)_(?P<threads>\d+)t_s(?P<sample>\d+)'
)

data = []
for root, dirs, files in os.walk(RAW_DIR):
    for fname in files:
        if not fname.endswith('.txt'):
            continue
        m = file_pattern.match(fname)
        if not m:
            continue
        fpath = os.path.join(root, fname)
        time_sec, ops_sec = parse_file(fpath)
        if time_sec is None and ops_sec is None:
            continue
        d = m.groupdict()
        d['time_sec'] = time_sec
        d['ops_sec'] = ops_sec
        data.append(d)

# Aggregate
agg = defaultdict(list)
for d in data:
    key = (d['impl'], d['backend'], d['bench'], int(d['threads']))
    agg[key].append(d)

results = []
for key, samples in agg.items():
    impl, backend, bench, threads = key
    times = [s['time_sec'] for s in samples if s['time_sec'] is not None]
    ops = [s['ops_sec'] for s in samples if s['ops_sec'] is not None]
    n = max(len(times), len(ops))
    metric_type = 'ops/sec' if ops else 'time(sec)'
    values = ops or times
    if not values:
        continue
    mean = sum(values) / len(values)
    variance = sum((v - mean)**2 for v in values) / len(values)
    stddev = math.sqrt(variance)
    results.append((impl, backend, bench, threads, mean, stddev, n, metric_type))

results.sort(key=lambda r: (r[0], r[1], r[2], r[3]))

# CSV
with open(os.path.join(RESULTS_DIR, 'results.csv'), 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['impl', 'backend', 'benchmark', 'threads', 'mean', 'stddev', 'n', 'metric'])
    w.writerows(results)

# Summary
summary_lines = []
summary_lines.append("="*90)
summary_lines.append("  BENCHMARK RESULTS SUMMARY")
summary_lines.append("="*90)
summary_lines.append(f"{'Impl':8s} {'Backend':14s} {'Threads':>7s} {'Mean':>14s} {'StdDev':>10s} {'N':>4s} {'Metric':>10s}")
summary_lines.append("-"*90)
for r in results:
    impl, backend, bench, threads, mean, stddev, n, mtype = r
    mean_s = f"{mean:.1f}" if mtype == 'ops/sec' else f"{mean:.4f}"
    std_s = f"{stddev:.1f}" if mtype == 'ops/sec' else f"{stddev:.4f}"
    summary_lines.append(f"  {impl:6s} {backend:14s} {bench:12s} {threads:2d} {mean_s:>14s} {std_s:>10s} {n:4d} {mtype:>10s}")

# Speedup vs uninstrumented
uninstr = {}
for r in results:
    if r[0] == 'expli' and r[1] == 'uninstrumented' and r[3] == 1:
        uninstr[r[2]] = r[4]

if uninstr:
    summary_lines.append("")
    summary_lines.append("-"*90)
    summary_lines.append("  SPEEDUP vs Uninstrumented (1t)")
    summary_lines.append("-"*90)
    for r in results:
        if r[1] == 'uninstrumented' or r[3] != 1:
            continue
        impl, backend, bench, threads, mean, stddev, n, mtype = r
        base = uninstr.get(bench)
        if base and mean > 0:
            if mtype == 'ops/sec':
                ratio = mean / base
            else:
                ratio = base / mean
            summary_lines.append(f"  {impl:6s} {backend:14s} {bench:12s} {ratio:>6.2f}x")

with open(os.path.join(RESULTS_DIR, 'SUMMARY.txt'), 'w') as f:
    f.write('\n'.join(summary_lines))
    f.write('\n')

print('\n'.join(summary_lines))
PYEOF

    python3 "$RESULTS_DIR/analyze.py" "$RESULTS_DIR" 2>&1 | tee "$SUMMARY"

    echo ""
    log "Complete. Results in $RESULTS_DIR"
    echo "  SUMMARY:  $SUMMARY"
    echo "  CSV:      $CSV"
}

main "$@"
