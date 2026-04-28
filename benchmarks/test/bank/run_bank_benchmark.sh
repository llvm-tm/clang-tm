#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

NUM_SAMPLES=10
THREADS="1 2 4 8"
DURATION_MS=3000
OUTPUT_DIR=""

usage() {
    cat <<EOF
Usage: $0 [options]

Options:
    -n <num>       Number of samples (default: $NUM_SAMPLES)
    -t <threads>    Thread counts (space-separated, default: $THREADS)
    -d <ms>        Duration per test in ms (default: $DURATION_MS)
    -o <dir>       Output directory (default: auto-generated with timestamp)
    -h, --help     Show this help message

Examples:
    $0 -n 5 -t "1 2 4"
    $0 -n 10 -t "1 2 4 8" -d 5000
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n) NUM_SAMPLES="$2"; shift 2 ;;
        -t) THREADS="$2"; shift 2 ;;
        -d) DURATION_MS="$2"; shift 2 ;;
        -o) OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1"; usage; exit 1 ;;
    esac
done

if [[ -z "$OUTPUT_DIR" ]]; then
    TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    OUTPUT_DIR="$SCRIPT_DIR/results_${TIMESTAMP}"
fi

mkdir -p "$OUTPUT_DIR"
echo "Results will be stored in: $OUTPUT_DIR"

BACKENDS=("uninstrumented" "singlelock" "tl2" "tinystm")

echo "Building bank benchmarks..."
make bank_uninstrumented bank_singlelock bank_tl2 bank_tinystm 2>/dev/null || true

run_benchmark() {
    local backend="$1"
    local threads="$2"
    local sample="$3"
    local output_file="$OUTPUT_DIR/${backend}_t${threads}_s${sample}.txt"

    local bin_path=""
    case "$backend" in
        uninstrumented) bin_path="$SCRIPT_DIR/bin/bank_uninstrumented" ;;
        singlelock) bin_path="$SCRIPT_DIR/bin/bank_singlelock" ;;
        tl2) bin_path="$SCRIPT_DIR/bin/bank_tl2" ;;
        tinystm) bin_path="$SCRIPT_DIR/bin/bank_tinystm" ;;
        *) echo "Unknown backend: $backend"; return 1 ;;
    esac

    if [[ ! -x "$bin_path" ]]; then
        echo "Binary not found: $bin_path"
        return 1
    fi

    "$bin_path" -d "$DURATION_MS" -t "$threads" > "$output_file" 2>&1 || true
    echo "  -> $output_file"
}

extract_metric() {
    local file="$1"
    local metric="$2"

    case "$metric" in
        txns_sec)
            grep "Txns/sec:" "$file" | awk '{print $2}' | tr -d ',' ;;
        total_txns)
            grep "^Total txns:" "$file" | awk '{print $3}' ;;
        elapsed_ms)
            grep "^Elapsed time:" "$file" | awk '{print $3}' ;;
        final_total)
            grep "^Final bank total:" "$file" | awk '{print $4}' ;;
        pass)
            grep -q "PASS" "$file" && echo "1" || echo "0" ;;
        *)
            echo "" ;;
    esac
}

echo ""
echo "Running benchmarks..."
echo "===================="
echo "Samples: $NUM_SAMPLES"
echo "Threads: $THREADS"
echo "Duration: ${DURATION_MS}ms"
echo ""

for backend in "${BACKENDS[@]}"; do
        echo "Testing backend: $backend"
        for threads in $THREADS; do
            echo "  Threads: $threads"
            for sample in $(seq 1 "$NUM_SAMPLES"); do
                if [[ "$backend" == "tl2" || "$backend" == "tinystm" ]]; then
                    run_benchmark "$backend" "$threads" "$sample" || echo "    Warning: Backend $backend failed at threads=$threads"
                else
                    run_benchmark "$backend" "$threads" "$sample"
                fi
            done
        done
    done

echo ""
echo "Computing statistics..."
echo "===================="

RESULTS_FILE="$OUTPUT_DIR/results.txt"
{
    echo "# Bank Benchmark Results"
    echo "# Generated: $(date)"
    echo "# Samples: $NUM_SAMPLES"
    echo "# Duration: ${DURATION_MS}ms"
    echo "#"
    echo "# Backend           Threads   Txns/sec (avg)    Txns/sec (std)   Total-Txns (avg)   Pass-Rate"
    echo "# ================================================================================"

    for backend in "${BACKENDS[@]}"; do
        for threads in $THREADS; do
            values_txns=()
            values_total=()
            pass_count=0

            for sample in $(seq 1 "$NUM_SAMPLES"); do
                file="$OUTPUT_DIR/${backend}_t${threads}_s${sample}.txt"
                if [[ -f "$file" ]]; then
                    txns=$(extract_metric "$file" "txns_sec")
                    total=$(extract_metric "$file" "total_txns")
                    pass=$(extract_metric "$file" "pass")

                    if [[ -n "$txns" && "$txns" != "0" ]]; then
                        values_txns+=("$txns")
                    fi
                    if [[ -n "$total" ]]; then
                        values_total+=("$total")
                    fi
                    if [[ "$pass" == "1" ]]; then
                        ((pass_count++))
                    fi
                fi
            done

            if [[ ${#values_txns[@]} -gt 0 ]]; then
                avg_txns=$(printf '%s\n' "${values_txns[@]}" | awk '{sum+=$1; sumsq+=$1*$1} END {printf "%.0f", sum/NR}')
                std_txns=$(printf '%s\n' "${values_txns[@]}" | awk -v avg="$avg_txns" '{sum+=($1-avg)^2} END {printf "%.0f", sqrt(sum/NR)}')
                pass_rate=$((pass_count * 100 / NUM_SAMPLES))

                printf "%-18s %6s   %10s   %10s   %14s   %7s%%\n" \
                    "$backend" "$threads" "$avg_txns" "$std_txns" "$avg_txns" "$pass_rate"
            fi
        done
    done
} > "$RESULTS_FILE"

echo "Results saved to: $RESULTS_FILE"
echo ""
cat "$RESULTS_FILE"
echo ""
echo "Done!"