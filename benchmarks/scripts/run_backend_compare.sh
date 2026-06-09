#!/usr/bin/env bash
set -euo pipefail

# ── Configuration ──────────────────────────────────────────────────
BACKENDS=(tinystm_wbctl norec singlelock)
BENCHMARKS=(vacation kmeans labyrinth genome intruder ssca2 bayes yada)
THREADS=(1 2 4 7 10 14 21 28 35 42 49 56)
SAMPLES=3
BASE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PLUGIN_DIR="$BASE_DIR/plugin"
RESULTS_DIR="$BASE_DIR/benchmark_results"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$RESULTS_DIR/backend_compare_$TIMESTAMP"

mkdir -p "$OUT_DIR/raw/uninstrumented"
for bk in "${BACKENDS[@]}"; do
    mkdir -p "$OUT_DIR/raw/$bk"
done

cat > "$OUT_DIR/META" <<EOF
started: $(date -Iseconds)
backends: ${BACKENDS[*]}
benchmarks: STAMP(8) + tpcc + stmbench7
threads: ${THREADS[*]}
samples: $SAMPLES
EOF

exec > >(tee "$OUT_DIR/runner.log") 2>&1

# ── Helper: run a single combination ──────────────────────────────
run_one() {
    local label="$1"     # e.g. "tinystm_wbctl/intruder_4t_s1"
    local bin="$2"       # path to binary
    shift 2
    local out="$OUT_DIR/raw/$label.txt"

    if [[ -f "$out" ]]; then
        echo "[SKIP] $out exists"
        return 0
    fi

    echo "[RUN]  $label"
    if timeout 120 "$bin" "$@" 2>/dev/null >"$out.tmp"; then
        mv "$out.tmp" "$out"
        echo "[OK]   $label"
    else
        local ec=$?
        echo "[FAIL] $label (exit=$ec)"
        mv "$out.tmp" "$out.fail"
    fi
}

# ── STAMP benchmarks ──────────────────────────────────────────────
STAMP_BIN="$PLUGIN_DIR/STAMP/bin"
declare -A STAMP_ARGS
STAMP_ARGS[vacation]="-b vacation -p"
STAMP_ARGS[kmeans]="-b kmeans -p"
STAMP_ARGS[labyrinth]="-b labyrinth -p"
STAMP_ARGS[genome]="-b genome -p"
STAMP_ARGS[intruder]="-b intruder -p"
STAMP_ARGS[ssca2]="-b ssca2 -p"
STAMP_ARGS[bayes]="-b bayes -p"
STAMP_ARGS[yada]="-b yada -p"

for bk in "${BACKENDS[@]}"; do
    for bm in "${BENCHMARKS[@]}"; do
        for t in "${THREADS[@]}"; do
            for s in $(seq 1 $SAMPLES); do
                label="$bk/${bm}_${t}t_s${s}"
                bin="$STAMP_BIN/stamp_$bk"
                run_one "$label" "$bin" ${STAMP_ARGS[$bm]} "$t"
            done
        done
    done
done

# Uninstrumented baseline (1 thread only)
for bm in "${BENCHMARKS[@]}"; do
    label="uninstrumented/${bm}_1t_s1"
    bin="$STAMP_BIN/stamp_uninstrumented"
    run_one "$label" "$bin" ${STAMP_ARGS[$bm]} 1
done

# ── TPC-C ─────────────────────────────────────────────────────────
TPCC_BIN="$PLUGIN_DIR/tpcc/bin"
# tinystm_wbctl crashes on TPC-C (STL-in-TM bug), skip it
for bk in norec singlelock; do
    for t in "${THREADS[@]}"; do
        for s in $(seq 1 $SAMPLES); do
            label="$bk/tpcc_${t}t_s${s}"
            bin="$TPCC_BIN/tpcc_$bk"
            run_one "$label" "$bin" -t "$t" -d 5000
        done
    done
done

# Uninstrumented baseline
run_one "uninstrumented/tpcc_1t_s1" "$TPCC_BIN/tpcc_uninstrumented" -t 1 -d 5000

# ── STMbench7 ─────────────────────────────────────────────────────
STMBENCH_BIN="$PLUGIN_DIR/stmbench7/bin"
for bk in "${BACKENDS[@]}"; do
    for t in "${THREADS[@]}"; do
        for s in $(seq 1 $SAMPLES); do
            label="$bk/stmbench7_${t}t_s${s}"
            bin="$STMBENCH_BIN/stmbench_$bk"
            run_one "$label" "$bin" -t "$t" -d 5000 -w 1
        done
    done
done

# Uninstrumented baseline
run_one "uninstrumented/stmbench7_1t_s1" "$STMBENCH_BIN/stmbench_uninstrumented" -t 1 -d 5000 -w 1

# ── Analyze ───────────────────────────────────────────────────────
echo ""
echo "=========================================="
echo " All runs complete — starting analysis"
echo "=========================================="

cat > "$OUT_DIR/analyze.py" << 'PYEOF'
import csv, os, re, sys, glob, math
from collections import defaultdict

RAW = sys.argv[1] if len(sys.argv) > 1 else "raw"
OUT_CSV = os.path.join(os.path.dirname(RAW), "results.csv")
OUT_SUM = os.path.join(os.path.dirname(RAW), "SUMMARY.txt")

TIME_PATTERNS = [
    (r"Elapsed time\s*=\s*(\d+\.?\d*)\s*seconds", 1),
    (r"Elapsed time\s*=\s*(\d+\.?\d*)\s*sec",      1),
    (r"Time\s*=\s*(\d+\.?\d*)\s*seconds",            1),
    (r"Time\s*=\s*(\d+\.?\d*)\s*sec",                1),
    (r"Time taken for all is (\d+\.?\d*) sec\.?",    1),
    (r"Elapsed:\s*(\d+)\s*ms",                      0.001),
    (r"Time\s*=\s*(\d+)\s*ms",                       0.001),
    (r"Results\s*\((\d+)\s*ms\)",                    0.001),
    (r"Time\s*=\s*(\d+\.?\d*)",                      1),
    (r"Time:\s*(\d+\.?\d*)\s*seconds",               1),
    (r"Learn time\s*=\s*(\d+\.?\d*)",                1),
]
OPS_PATTERNS = [
    (r"Ops/sec:\s*(\d+\.?\d*)", 1),
    (r"(\d+\.?\d*)\s*ops/sec",  1),
    (r"Rate:\s*(\d+\.?\d*)",    1),
]

def parse_time(text):
    if re.search(r"Business ops:\s*\d+\s*\(\d+\s*ops/sec\)", text):
        return None
    if re.search(r"Ops/sec:", text):
        return None
    for pat, mul in TIME_PATTERNS:
        m = re.search(pat, text, re.IGNORECASE)
        if m:
            return float(m.group(1)) * mul
    return None

def parse_ops(text):
    for pat, mul in OPS_PATTERNS:
        m = re.search(pat, text, re.IGNORECASE)
        if m:
            return float(m.group(1)) * mul
    m = re.search(r"Business ops:\s*\d+\s*\((\d+)\s*ops/sec\)", text)
    if m:
        return float(m.group(1))
    return None

results = []
for root, dirs, files in os.walk(RAW):
    for fn in files:
        if not fn.endswith(".txt"):
            continue
        fpath = os.path.join(root, fn)
        backend = os.path.basename(root)
        m = re.match(r'(.+)_(\d+)t_s(\d+)\.txt', fn)
        if not m:
            print(f"[WARN] skipping unrecognized file: {fpath}")
            continue
        bench = m.group(1)
        threads = int(m.group(2))
        sample = int(m.group(3))

        with open(fpath) as f:
            text = f.read()

        ops = parse_ops(text)
        if ops is not None:
            results.append((backend, bench, threads, sample, "ops_per_sec", ops))
        else:
            ts = parse_time(text)
            if ts is not None:
                results.append((backend, bench, threads, sample, "time_sec", ts))
            else:
                print(f"[WARN] no time or ops in {fpath}")

agg = defaultdict(list)
for backend, bench, threads, sample, metric, val in results:
    key = (backend, bench, threads, metric)
    agg[key].append(val)

rows = []
for key, vals in sorted(agg.items()):
    backend, bench, threads, metric = key
    n = len(vals)
    mean = sum(vals) / n
    if n > 1:
        variance = sum((v - mean)**2 for v in vals) / (n - 1)
        stddev = math.sqrt(variance)
    else:
        stddev = 0.0
    rows.append((backend, bench, threads, n, mean, stddev, metric))

with open(OUT_CSV, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["backend", "benchmark", "threads", "n", "mean", "stddev", "metric"])
    for r in rows:
        w.writerow(r)

print(f"Wrote {OUT_CSV} ({len(rows)} rows)")

with open(OUT_SUM, "w") as f:
    f.write("=" * 80 + "\n")
    f.write("Backend Comparison Summary\n")
    f.write(f"Generated: {__import__('datetime').datetime.now().isoformat()}\n")
    f.write("=" * 80 + "\n\n")

    by_backend = defaultdict(list)
    for r in rows:
        by_backend[r[0]].append(r)

    for backend, rlist in sorted(by_backend.items()):
        f.write(f"\n--- {backend} ---\n")
        f.write(f"{'benchmark':<14} {'threads':>7} {'mean':>14} {'stddev':>10} {'metric':<14} {'n':>3}\n")
        f.write("-" * 70 + "\n")
        for r in rlist:
            bk, bm, th, n, mean, stddev, metric = r
            if metric == "time_sec":
                f.write(f"{bm:<14} {th:>7} {mean:>10.4f}s  {stddev:>8.4f}  {metric:<14} {n:>3}\n")
            else:
                f.write(f"{bm:<14} {th:>7} {mean:>10.1f}  {stddev:>8.1f}  {metric:<14} {n:>3}\n")

    uninstrumented = {r[1]: (r[4], r[6]) for r in rows if r[0] == "uninstrumented"}
    if uninstrumented:
        f.write("\n\n=== Overhead vs Uninstrumented (1 thread, time_sec) ===\n")
        f.write(f"{'benchmark':<14} {'uninstr':>10} {'wbctl':>12} {'norec':>12} {'sgl':>12}\n")
        f.write("-" * 60 + "\n")
        for bm in sorted(uninstrumented.keys()):
            ui_val, ui_metric = uninstrumented[bm]
            if ui_metric != "time_sec":
                continue
            vals = {}
            for r in rows:
                if r[0] != "uninstrumented" and r[1] == bm and r[2] == 1 and r[6] == "time_sec":
                    vals[r[0]] = r[4]
            wb = f"{vals.get('tinystm_wbctl', 0)/ui_val:.2f}x" if 'tinystm_wbctl' in vals else "N/A"
            nr = f"{vals.get('norec', 0)/ui_val:.2f}x" if 'norec' in vals else "N/A"
            sg = f"{vals.get('singlelock', 0)/ui_val:.2f}x" if 'singlelock' in vals else "N/A"
            f.write(f"{bm:<14} {ui_val:>8.4f}s  {wb:>10} {nr:>10} {sg:>10}\n")

    f.write("\n\n=== Throughput Benchmarks (ops/sec, 1 thread) ===\n")
    f.write(f"{'benchmark':<14} {'uninstr':>10} {'norec':>12} {'sgl':>12}\n")
    f.write("-" * 50 + "\n")
    for bm in sorted(uninstrumented.keys()):
        ui_val, ui_metric = uninstrumented[bm]
        if ui_metric != "ops_per_sec":
            continue
        vals = {}
        for r in rows:
            if r[1] == bm and r[2] == 1 and r[6] == "ops_per_sec":
                vals[r[0]] = r[4]
        nr = f"{vals.get('norec', 0):>8.0f}" if 'norec' in vals else "N/A"
        sg = f"{vals.get('singlelock', 0):>8.0f}" if 'singlelock' in vals else "N/A"
        f.write(f"{bm:<14} {ui_val:>8.0f}  {nr:>10} {sg:>10}\n")

print(f"Wrote {OUT_SUM}")
PYEOF

python3 "$OUT_DIR/analyze.py" "$OUT_DIR/raw"
echo "Done: $OUT_DIR"
