#!/usr/bin/env bash
# ============================================================================
# Full Benchmark Comparison: 3 impls × 3 TinySTM backends × 11 benches
# ============================================================================
# Runs WBCTL, WT, and NOrec across:
#   - Plugin (LLVM instrumented)
#   - Expli C++
#   - Rust
# Benches: STAMP(8) + TPC-C + YCSB + STMbench7
# Threads: 1 2 4 8  (configurable via THREADS env)
# Samples: 1         (configurable via SAMPLES env)
# ============================================================================

if set -o pipefail 2>/dev/null; then set -euo pipefail; else set -eu; fi

cd "$(dirname "$0")"
BASE_DIR="$PWD"

# Portable timeout: use gtimeout (macOS coreutils) if available
if command -v timeout &>/dev/null; then
    TIMEOUT_CMD="timeout"
elif command -v gtimeout &>/dev/null; then
    TIMEOUT_CMD="gtimeout"
else
    echo "FATAL: neither timeout nor gtimeout found. Install coreutils." >&2
    exit 1
fi

# ── Configuration ──────────────────────────────────────────────────────────
THREAD_LIST="${THREADS:-"1 2 4 8"}"
SAMPLES="${SAMPLES:-1}"
TIMEOUT="${TIMEOUT:-300}"
RESULTS_DIR="${RESULTS_DIR:-"$BASE_DIR/benchmark_results/full_compare_$(date +%Y%m%d_%H%M%S)"}"

RAW_DIR="$RESULTS_DIR/raw"
SUMMARY="$RESULTS_DIR/SUMMARY.txt"
CSV="$RESULTS_DIR/results.csv"
PROGRESS="$RESULTS_DIR/progress.txt"
SKIP_FILE="$RESULTS_DIR/skip_combos.txt"

mkdir -p "$RAW_DIR"/{plugin,expli,rust}/{wbctl,wt,norec}

# Known-broken combos
echo "plugin wbctl stmbench7" >> "$SKIP_FILE"   # std::vector reallocation crash
echo "plugin wt stmbench7" >> "$SKIP_FILE"       # same vector issue

# ── Per-benchmark params ───────────────────────────────────────────────────
# Params lookup (bash 3.x compat — no associative arrays)
plugin_params() {
    case "$1" in
        vacation) echo "-n 2 -q 90 -r 16384 -u 98" ;;
        kmeans)   echo "" ;;
        labyrinth) echo "-x 8 -y 8 -z 8 -n 64" ;;
        genome)   echo "-g 16384 -s 64 -n 1000000" ;;
        intruder) echo "-a 10 -l 128 -n 5120 -s 1" ;;
        ssca2)    echo "-s 14 -u 1.0 -l 3 -p 3 -i 3" ;;
        bayes)    echo "-v 16 -r 32 -n 2 -p 2 -e 4" ;;
        yada)     echo "-a 20 -j 0.5" ;;
    esac
}
expli_params() {
    case "$1" in
        vacation) echo "-r 16384 -n 2 -u 98 -t 4096" ;;
        kmeans)   echo "-k 8 -d 2 -n 200" ;;
        labyrinth) echo "-x 8 -y 8 -z 8 -n 64" ;;
        genome)   echo "-g 16384 -s 64 -n 1000000" ;;
        intruder) echo "-a 10 -l 128 -n 5120 -s 1" ;;
        ssca2)    echo "-s 14 -u 1.0 -l 3 -m 3 -i 3" ;;
        bayes)    echo "-v 16 -r 32 -n 2 -p 2 -e 4" ;;
        yada)     echo "-a 20 -j 0.5" ;;
    esac
}
rust_params() {
    case "$1" in
        vacation) echo "-r 16384 -n 2 -u 98 -t 4096" ;;
        kmeans)   echo "-k 8 -d 2 -n 200" ;;
        labyrinth) echo "-x 8 -y 8 -z 8 -n 64" ;;
        genome)   echo "-g 16384 -s 64 -n 1000000" ;;
        intruder) echo "-a 10 -l 128 -n 5120 -s 1" ;;
        ssca2)    echo "-s 14 -u 1.0 -l 3 -m 3 -i 3" ;;
        bayes)    echo "-v 16 -r 32 -n 2 -e 4 -i 2" ;;
        yada)     echo "-a 20 -j 0.5" ;;
    esac
}

STAMP_BENCHES=(vacation kmeans labyrinth genome intruder ssca2 bayes yada)
OTHER_BENCHES=(tpcc ycsb stmbench7)

# ── Stats ──────────────────────────────────────────────────────────────────
COMPLETED=0; TIMEOUTS=0; CRASHES=0; FAILS=0; SKIPPED=0; TOTAL=0

log() { echo "$*"; echo "$*" >> "$PROGRESS"; }

is_skipped() { grep -qx "$1 $2 $3" "$SKIP_FILE" 2>/dev/null; }
mark_broken() {
    if ! grep -qx "$1 $2 $3" "$SKIP_FILE" 2>/dev/null; then
        echo "$1 $2 $3" >> "$SKIP_FILE"
        printf "  *** %-60s ALL REMAINING SKIPPED ***\n" "${1}_${2}_${3}" >&2
    fi
}

# ── run_one ────────────────────────────────────────────────────────────────
run_one() {
    local impl="$1" backend="$2" bench="$3" threads="$4" sample="$5"
    shift 5; local binary="$1"; shift
    local label="${impl}_${backend}_${bench}_${threads}t_s${sample}"
    local outfile="$RAW_DIR/$impl/$backend/${bench}_${threads}t_s${sample}.txt"
    TOTAL=$((TOTAL + 1))

    if is_skipped "$impl" "$backend" "$bench"; then
        SKIPPED=$((SKIPPED + 1))
        printf "  %-60s %s\n" "$label" "SKIP"
        log "SKIP $label"
        return 0
    fi
    if [ -f "$outfile" ] && grep -qE 'PASS|Results|Time\s*[=:]' "$outfile" 2>/dev/null; then
        SKIPPED=$((SKIPPED + 1))
        printf "  %-60s %s\n" "$label" "SKIP (exists)"
        return 0
    fi

    set +e
    $TIMEOUT_CMD "$TIMEOUT" "$binary" "$@" > "$outfile" 2>&1
    local rc=$?; set -e

    local status=""
    if [ "$rc" = 124 ]; then status="TIMEOUT"; TIMEOUTS=$((TIMEOUTS + 1))
    elif [ "$rc" -ge 128 ]; then local sig=$((rc - 128)); status="CRASH(sig=$sig)"; CRASHES=$((CRASHES + 1))
    elif [ "$rc" = 0 ]; then status="OK"; COMPLETED=$((COMPLETED + 1))
    else status="FAIL(exit=$rc)"; FAILS=$((FAILS + 1))
    fi

    local fsize=$(wc -c < "$outfile" 2>/dev/null || echo 0)
    if [ "$rc" != 0 ] && [ "$fsize" -gt 0 ] && [ "$fsize" -lt 600 ]; then
        grep -qE 'PASS|Results|Time\s*[=:]' "$outfile" 2>/dev/null || mark_broken "$impl" "$backend" "$bench"
    fi

    printf "  %-60s %s\n" "$label" "$status"
    log "$label $status (rc=$rc)"
}

# ── Rust runner (interleaved: build then run per backend) ────────────────
# ── Expli C++ runner (interleaved: build then run per backend) ──────────
run_expli_backend() {
    local backend="$1"
    local bindir="$2"

    for bench in "${STAMP_BENCHES[@]}"; do
        local bin="$bindir/$bench"
        [ -x "$bin" ] || continue
        local params="$(expli_params "$bench")"
        local tflag="-p"
        [ "$bench" = "bayes" ] || [ "$bench" = "yada" ] && tflag="-t"
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one expli "$backend" "$bench" "$t" "$s" \
                    "$bin" "$tflag" "$t" $params
            done
        done
    done

    for ob in tpcc ycsb stmbench7; do
        local bin="$bindir/$ob"
        [ -x "$bin" ] || continue
        case "$ob" in
            tpcc)  run_ob expli "$backend" tpcc "$bindir/tpcc" ;;
            ycsb)  run_ob expli "$backend" ycsb "$bindir/ycsb" ;;
            stmbench7) run_ob expli "$backend" stmbench7 "$bindir/stmbench7" ;;
        esac
    done
}

run_ob() {
    local impl="$1" backend="$2" bench="$3" bin="$4"
    for t in $THREAD_LIST; do
        for s in $(seq 1 $SAMPLES); do
            case "$bench" in
                tpcc)  run_one "$impl" "$backend" "$bench" "$t" "$s" "$bin" -t "$t" -d 5000 ;;
                ycsb)  run_one "$impl" "$backend" "$bench" "$t" "$s" "$bin" -t "$t" -d 5000 -w a ;;
                stmbench7) run_one "$impl" "$backend" "$bench" "$t" "$s" "$bin" "$t" 5000 ;;
            esac
        done
    done
}

# ── Rust runner (interleaved: build then run per backend) ────────────────
run_rust_backend() {
    local backend="$1"
    local tdir="$BASE_DIR/rust_tm_api/target/release"

    for bench in "${STAMP_BENCHES[@]}"; do
        local bin="$tdir/stamp_${bench}"
        [ -x "$bin" ] || continue
        local params="$(rust_params "$bench")"
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one rust "$backend" "$bench" "$t" "$s" \
                    "$bin" -p "$t" $params
            done
        done
    done

    for ob in tpcc ycsb stmbench7; do
        local bin="$tdir/$ob"
        [ -x "$bin" ] || continue
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                case "$ob" in
                    tpcc)  run_one rust "$backend" "$ob" "$t" "$s" "$bin" -p "$t" -d 5000 ;;
                    ycsb)  run_one rust "$backend" "$ob" "$t" "$s" "$bin" -p "$t" -d 5000 -w a ;;
                    stmbench7) run_one rust "$backend" "$ob" "$t" "$s" "$bin" -p "$t" -d 5000 ;;
                esac
            done
        done
    done
}

# ═════════════════════════════════════════════════════════════════════════════
# PHASE 1: BUILD
# ═════════════════════════════════════════════════════════════════════════════
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  PHASE 1: BUILDING ALL BINARIES                              ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# ── 1a. Plugin builds ──────────────────────────────────────────────────────
build_plugin() {
    local dir="$1" target="$2"
    local bin_path="$3"
    [ -x "$bin_path" ] && { echo "  [build] $target exists"; return 0; }
    echo "  [build] $target ..."
    set +e
    make -C "$dir" "$target" TM_LINK_OPT="-O3" TM_OPT_LEVEL="-O3" \
        > "/tmp/build_${target}.log" 2>&1
    local rc=$?; set -e
    if [ "$rc" != 0 ]; then echo "  [build] $target FAILED"; tail -5 "/tmp/build_${target}.log"
    else echo "  [build] $target OK"; fi
}

echo ""
echo "=== Plugin STAMP ==="
mkdir -p "$BASE_DIR/plugin-benchmarks/STAMP/bin"
build_plugin "$BASE_DIR/plugin-benchmarks/STAMP" stamp_tinystm_wbctl  "$BASE_DIR/plugin-benchmarks/STAMP/bin/stamp_tinystm_wbctl"
build_plugin "$BASE_DIR/plugin-benchmarks/STAMP" stamp_tinystm_wt    "$BASE_DIR/plugin-benchmarks/STAMP/bin/stamp_tinystm_wt"
build_plugin "$BASE_DIR/plugin-benchmarks/STAMP" stamp_norec         "$BASE_DIR/plugin-benchmarks/STAMP/bin/stamp_norec"

echo ""
echo "=== Plugin TPCC ==="
mkdir -p "$BASE_DIR/plugin-benchmarks/tpcc/bin"
build_plugin "$BASE_DIR/plugin-benchmarks/tpcc" tpcc_tinystm_wbctl "$BASE_DIR/plugin-benchmarks/tpcc/bin/tpcc_tinystm_wbctl"
build_plugin "$BASE_DIR/plugin-benchmarks/tpcc" tpcc_tinystm_wt   "$BASE_DIR/plugin-benchmarks/tpcc/bin/tpcc_tinystm_wt"
build_plugin "$BASE_DIR/plugin-benchmarks/tpcc" tpcc_norec         "$BASE_DIR/plugin-benchmarks/tpcc/bin/tpcc_norec"

echo ""
echo "=== Plugin STMbench7 ==="
mkdir -p "$BASE_DIR/plugin-benchmarks/stmbench7/bin"
build_plugin "$BASE_DIR/plugin-benchmarks/stmbench7" stmbench_tinystm_wbctl "$BASE_DIR/plugin-benchmarks/stmbench7/bin/stmbench_tinystm_wbctl"
build_plugin "$BASE_DIR/plugin-benchmarks/stmbench7" stmbench_tinystm_wt   "$BASE_DIR/plugin-benchmarks/stmbench7/bin/stmbench_tinystm_wt"
build_plugin "$BASE_DIR/plugin-benchmarks/stmbench7" stmbench_norec        "$BASE_DIR/plugin-benchmarks/stmbench7/bin/stmbench_norec"

echo ""
echo "=== Plugin YCSB ==="
mkdir -p "$BASE_DIR/plugin-benchmarks/ycsb/bin"
build_plugin "$BASE_DIR/plugin-benchmarks/ycsb" ycsb_tinystm_wbctl "$BASE_DIR/plugin-benchmarks/ycsb/bin/ycsb_tinystm_wbctl"
build_plugin "$BASE_DIR/plugin-benchmarks/ycsb" ycsb_tinystm_wt   "$BASE_DIR/plugin-benchmarks/ycsb/bin/ycsb_tinystm_wt"
build_plugin "$BASE_DIR/plugin-benchmarks/ycsb" ycsb_norec        "$BASE_DIR/plugin-benchmarks/ycsb/bin/ycsb_norec"

# ── 1c. Expli C++ ──────────────────────────────────────────────────────────
echo ""
echo "=== Expli C++ ==="
for be in TINYSTM WT NOREC; do
    echo "  [build] BACKEND=$be ..."
    set +e
    make -C "$BASE_DIR/expli-benchmarks" clean > /dev/null 2>&1
    make -C "$BASE_DIR/expli-benchmarks" all BACKEND="$be" > "/tmp/build_expli_${be}.log" 2>&1
    rc=$?; set -e
    if [ "$rc" != 0 ]; then echo "  [build] BACKEND=$be FAILED"; tail -5 "/tmp/build_expli_${be}.log"
    else echo "  [build] BACKEND=$be OK"; fi
    # Map backend name for expli: TINYSTM→wbctl, WT→wt, NOREC→norec
    case "$be" in
        TINYSTM) run_expli_backend wbctl "$BASE_DIR/expli-benchmarks/bin" ;;
        WT)      run_expli_backend wt    "$BASE_DIR/expli-benchmarks/bin" ;;
        NOREC)   run_expli_backend norec "$BASE_DIR/expli-benchmarks/bin" ;;
    esac
done

# ── 1d. Rust ───────────────────────────────────────────────────────────────
echo ""
echo "=== Rust ==="
for feat in wbctl wt norec; do
    echo "  [build] --features $feat ..."
    set +e
    cargo build --release --manifest-path "$BASE_DIR/rust_tm_api/Cargo.toml" \
        --no-default-features --features "$feat" \
        > "/tmp/build_rust_${feat}.log" 2>&1
    rc=$?; set -e
    if [ "$rc" != 0 ]; then echo "  [build] --features $feat FAILED"; tail -10 "/tmp/build_rust_${feat}.log"
    else echo "  [build] --features $feat OK"; fi
    # Run immediately after building (prevents stale binaries when
    # the next feature build overwrites them in target/release/)
    run_rust_backend "$feat"
done

# ═════════════════════════════════════════════════════════════════════════════
# PHASE 2: RUN
# ═════════════════════════════════════════════════════════════════════════════
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  PHASE 2: RUNNING BENCHMARKS                                 ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

log "Starting runs: $(date)"

echo "Thread levels: $THREAD_LIST"
echo "Samples: $SAMPLES"
echo "Timeout: ${TIMEOUT}s"
echo ""

# ── 2a. Plugin ─────────────────────────────────────────────────────────────
PB="$BASE_DIR/plugin-benchmarks"
run_plugin_impl() {
    local backend="$1" stm_app="$2" tpcc_app="$3" stm7_app="$4" ycsb_app="$5"
    local stm_dir="$PB/STAMP/bin" tpcc_dir="$PB/tpcc/bin"
    local stm7_dir="$PB/stmbench7/bin" ycsb_dir="$PB/ycsb/bin"

    for bench in "${STAMP_BENCHES[@]}"; do
        local bin="$stm_dir/${stm_app}stamp_${backend}"
        [ -x "$bin" ] || continue
        local params="$(plugin_params "$bench")"
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one plugin "$backend" "$bench" "$t" "$s" \
                    "$bin" -b "$bench" -t "$t" $params
            done
        done
    done

    if [ -x "$tpcc_dir/${tpcc_app}tpcc_${backend}" ]; then
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one plugin "$backend" tpcc "$t" "$s" \
                    "$tpcc_dir/${tpcc_app}tpcc_${backend}" -t "$t" -d 5000
            done
        done
    fi

    if [ -x "$ycsb_dir/${ycsb_app}ycsb_${backend}" ]; then
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one plugin "$backend" ycsb "$t" "$s" \
                    "$ycsb_dir/${ycsb_app}ycsb_${backend}" -t "$t" -d 5000 -w a
            done
        done
    fi

    if [ -x "$stm7_dir/${stm7_app}stmbench_${backend}" ]; then
        for t in $THREAD_LIST; do
            for s in $(seq 1 $SAMPLES); do
                run_one plugin "$backend" stmbench7 "$t" "$s" \
                    "$stm7_dir/${stm7_app}stmbench_${backend}" -t "$t" -d 5000 -w 1
            done
        done
    fi
}
run_plugin_impl wbctl "stamp_tinystm_" "tpcc_tinystm_" "stmbench_tinystm_" "ycsb_tinystm_"
run_plugin_impl wt    "stamp_tinystm_" "tpcc_tinystm_" "stmbench_tinystm_" "ycsb_tinystm_"
run_plugin_impl norec "stamp_"         "tpcc_"         "stmbench_"         "ycsb_"

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  RUN PHASE COMPLETE                                          ║"
echo "╚══════════════════════════════════════════════════════════════╝"
log "Run phase complete: $(date)"

# ═════════════════════════════════════════════════════════════════════════════
# PHASE 3: ANALYSIS
# ═════════════════════════════════════════════════════════════════════════════
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  PHASE 3: ANALYZING RESULTS                                  ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

cat > "$RESULTS_DIR/analyze.py" << 'PYEOF'
#!/usr/bin/env python3
import os, sys, re, csv, math
from collections import defaultdict

rd = sys.argv[1]
TIME_PATS = [
    re.compile(r'Time\s*[=:]\s*([\d.]+)'),
    re.compile(r'Results\s*\((\d+)\s*ms\)'),
    re.compile(r'Elapsed:\s+(\d+)\s+ms'),
    re.compile(r'Time\s*=\s*(\d+)\s*ms'),
]
OPS_PATS = [
    re.compile(r'Rate:\s*([\d.]+)\s*ops/sec'),
    re.compile(r'Ops/sec\s*:\s*([\d.]+)'),
    re.compile(r'([\d.]+)\s*ops/sec'),
]

def parse(fpath):
    try:
        with open(fpath) as f: txt = f.read()
    except: return None, None
    ts, ops = None, None
    for p in TIME_PATS:
        m = p.search(txt)
        if m:
            v = float(m.group(1))
            ts = v / 1000.0 if 'ms' in p.pattern else v
            break
    for p in OPS_PATS:
        m = p.search(txt)
        if m: ops = float(m.group(1)); break
    if ops is None and ts and ts > 0:
        tm = re.search(r'Total ops\s*[=:]\s*(\d+)', txt)
        if tm: ops = float(tm.group(1)) / ts
    return ts, ops

data = []
raw = os.path.join(rd, 'raw')
for root, dirs, files in os.walk(raw):
    for fn in files:
        if not fn.endswith('.txt'): continue
        fp = os.path.join(root, fn)
        rel = os.path.relpath(fp, raw)
        parts = rel.replace('.txt', '').split('/')
        if len(parts) < 3: continue
        impl, backend = parts[0], parts[1]
        m = re.match(r'(.+)_(\d+)t_s(\d+)', parts[2])
        if not m: continue
        bench, thr, samp = m.group(1), int(m.group(2)), int(m.group(3))
        ts, ops = parse(fp)
        if ts is None and ops is None: continue
        metric = ops if (bench in ('tpcc','ycsb','stmbench7') and ops) else (ts or ops or 0)
        data.append((impl, backend, bench, thr, samp, ts, ops, metric))

agg = defaultdict(list)
for d in data:
    key = (d[0], d[1], d[2], d[3])
    agg[key].append(d[7])

rows = []
for key, vals in agg.items():
    impl, backend, bench, thr = key
    n = len(vals)
    mean = sum(vals) / n
    std = (sum((v-mean)**2 for v in vals) / n) ** 0.5 if n > 1 else 0
    mtype = 'ops/sec' if bench in ('tpcc','ycsb','stmbench7') else 'time_sec'
    rows.append((impl, backend, bench, thr, mean, std, n, mtype))

rows.sort()

csv_path = os.path.join(rd, 'results.csv')
with open(csv_path, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['impl','backend','benchmark','threads','mean','stddev','n','metric'])
    for r in rows: w.writerow(r)

summary = []
summary.append("=" * 80)
summary.append(f"  Full Benchmark Comparison ({os.path.basename(rd)})")
summary.append(f"  Data points: {len(data)}, Aggregated: {len(rows)}")
summary.append("=" * 80)

cur_bench = None
for r in rows:
    impl, backend, bench, thr, mean, std, n, mt = r
    if bench != cur_bench:
        summary.append(f"\n{'─'*80}")
        summary.append(f"  {bench.upper()}")
        summary.append(f"{'─'*80}")
        summary.append(f"  {'Impl':8s} {'Backend':8s} {'Thr':>5s} {'Mean':>14s} {'StdDev':>10s}")
        summary.append(f"  {'─'*8} {'─'*8} {'─'*5} {'─'*14} {'─'*10}")
        cur_bench = bench
    ms = f"{mean:.1f}" if mt == 'ops/sec' else f"{mean:.6f}"
    ss = f"{std:.1f}" if mt == 'ops/sec' else f"{std:.6f}"
    summary.append(f"  {impl:8s} {backend:8s} {thr:5d} {ms:>14s} {ss:>10s}")

with open(os.path.join(rd, 'summary.txt'), 'w') as f:
    f.write('\n'.join(summary) + '\n')

print('\n'.join(summary))
print(f"\nCSV: {csv_path}")
PYEOF

cd "$RESULTS_DIR" 2>/dev/null
python3 analyze.py "$(cd "$RESULTS_DIR" && pwd -P)" 2>&1

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  ALL DONE                                                    ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "Results: $RESULTS_DIR"
echo "STATS: completed=$COMPLETED timeout=$TIMEOUTS crash=$CRASHES fail=$FAILS skipped=$SKIPPED total=$TOTAL"
