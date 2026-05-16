#!/usr/bin/env bash
# ============================================================================
# TM Instrumentation Assessment Script
# ============================================================================
# Measures LLVM IR instrumentation overhead for each benchmark:
#   - IR growth (uninstrumented vs instrumented line counts)
#   - Number of TX-annotated functions and TM-annotated globals
#   - Injected tm_read_* / tm_write_* hook calls
#   - Cloned function count (before/after fixed-point filtering)
#   - Call redirect count (calls to _tm_clone)
#   – Ratio of instrumentation vs original code
#
# Usage:
#   ./assess_instrumentation.sh              # assess all benchmarks
#   ./assess_instrumentation.sh bank          # assess one benchmark
#
# Output: LaTeX table ready for inclusion in the paper.
# ============================================================================

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
source "$SCRIPT_DIR/llvm_tm_plugin/llvm-tool-helper.sh"

PLUGIN="llvm_tm_plugin/bin/libTMInstrument.so"
CXX="$LLVM_CXX"
CXXFLAGS="-std=c++20 -O3 -fno-inline -fno-stack-protector"
OPT="$LLVM_OPT"

# ------------------------------------------------------------------
# Configuration: benchmark name → source file (relative to repo root)
# ------------------------------------------------------------------
ALL_BENCHES=(
    "bank:benchmarks/test/bank/bank.cpp"
    "avltree:benchmarks/datastructures/avltree.cpp"
    "avltree_recursive:benchmarks/datastructures/avltree_recursive.cpp"
    "stmbench7:benchmarks/STMbench7/STMbench7.cpp"
    "ycsb:benchmarks/YCSB/YCSB.cpp"
    "eigen:benchmarks/EigenBench/EigenBench.cpp"
)

# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------
out_dir=$(mktemp -d /tmp/tm_assess_XXXXXX)
trap "rm -rf $out_dir" EXIT

assess_one() {
    local label="$1"
    local src="$2"

    local base_ll="$out_dir/${label}.ll"
    local instr_ll="$out_dir/${label}.instr.ll"

    # 1. Compile to uninstrumented LLVM IR
    if ! $CXX $CXXFLAGS -emit-llvm -S "$src" -o "$base_ll" 2>/dev/null; then
        return  # skip if compilation fails
    fi

    # 2. Instrument with TM plugin
    if ! $OPT -load-pass-plugin="$PLUGIN" \
             -passes="tm-instrument" \
             -S "$base_ll" -o "$instr_ll" 2>/dev/null; then
        return
    fi

    # 3. Count metrics
    local base_lines instr_lines
    base_lines=$(wc -l < "$base_ll" | tr -d '[:space:]')
    instr_lines=$(wc -l < "$instr_ll" | tr -d '[:space:]')

    # TX count: count tm_begin calls in instrumented IR
    local tx_count
    tx_count=$(grep -c "call.*@tm_begin()" "$instr_ll" 2>/dev/null || echo 0)
    tx_count=$(echo "$tx_count" | tr -d '[:space:]')

    local tm_global_count
    tm_global_count=$(grep -c 'c"tm\\00"' "$base_ll" 2>/dev/null || echo 0)
    tm_global_count=$(echo "$tm_global_count" | tr -d '[:space:]')

    local tm_read_write_count
    tm_read_write_count=$(grep -cE "tm_read_i[1-8]|tm_read_f[48]|tm_read_ptr|tm_write_i[1-8]|tm_write_f[48]|tm_write_ptr" "$instr_ll" 2>/dev/null || echo 0)
    tm_read_write_count=$(echo "$tm_read_write_count" | tr -d '[:space:]')

    local clone_defs
    clone_defs=$(grep "_tm_clone" "$instr_ll" | grep "define" | sed 's/.*@//' | sed 's/(.*//' | sort -u | wc -l | tr -d '[:space:]')

    local call_redirects
    call_redirects=$(grep -c "_tm_clone" "$instr_ll" 2>/dev/null || echo 0)
    call_redirects=$(echo "$call_redirects" | tr -d '[:space:]')

    # Ratio: instrumented / uninstrumented (handle division by zero)
    local ratio
    if [ "$base_lines" -gt 0 ]; then
        ratio=$(echo "scale=2; $instr_lines / $base_lines" | bc 2>/dev/null || echo "1.00")
    else
        ratio="1.00"
    fi

    # Sanitize label for LaTeX
    local latex_label
    latex_label=$(echo "$label" | sed 's/_/\\_/g')

    # Print LaTeX table row
    printf "  %s & %5d & %5d & %3d & %3d & %4d & %4d & %6d & %4.2f \\\\\n" \
        "$latex_label" \
        "$base_lines" \
        "$instr_lines" \
        "$tx_count" \
        "$tm_global_count" \
        "$tm_read_write_count" \
        "$clone_defs" \
        "$call_redirects" \
        "$ratio"
}

# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------
echo "% ============================================="
echo "% TM Instrumentation Assessment"
echo "% Generated: $(date)"
echo "% ============================================="
echo ""
echo "\\begin{table}[t]"
echo "\\centering"
echo "\\small"
echo "\\begin{tabular}{lrrrrrrrr}"
echo "\\toprule"
echo "Benchmark & \\multicolumn{2}{c}{IR lines} & TX & TM & TM hooks & Clones & Redirects & Ratio \\\\"
echo "          & raw & instr & fns & gvs & r\\slash w & defs & to clone & instr\\slash raw \\\\"
echo "\\midrule"

# Determine which benchmarks to assess
if [ $# -gt 0 ]; then
    # User-specified benchmarks
    for arg in "$@"; do
        for entry in "${ALL_BENCHES[@]}"; do
            label="${entry%%:*}"
            if [ "$label" = "$arg" ]; then
                src="${entry#*:}"
                assess_one "$label" "$src"
            fi
        done
    done
else
    # All benchmarks
    for entry in "${ALL_BENCHES[@]}"; do
        label="${entry%%:*}"
        src="${entry#*:}"
        assess_one "$label" "$src"
    done
fi

echo "\\bottomrule"
echo "\\end{tabular}"
echo "\\caption{LLVM IR instrumentation assessment across benchmarks.}"
echo "\\label{tab:instrumentation-assessment}"
echo "\\end{table}"
