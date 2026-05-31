#!/usr/bin/env bash
# ============================================================================
# Full TM Instrumentation Assessment
# ============================================================================
# Uses the EXACT same flags as the Makefile pipeline (tm_pipeline.mk).
# Captures plugin debug output (clone counts, redirects) from stderr.
# Reports:
#   - raw IR lines (uninstrumented)
#   - instrumented IR lines
#   - clone function definitions
#   - total _tm_clone references (calls + defs)
#   - tm_read/tm_write/tm_begin/tm_end hook counts
#   - TX count (tm_begin calls)
#   - IR growth ratio
# ============================================================================

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
source "$SCRIPT_DIR/llvm_tm_plugin/llvm-tool-helper.sh"

PLUGIN="llvm_tm_plugin/bin/libTMInstrument.so"
CXX="$LLVM_CXX"
OPT="$LLVM_OPT"

# Exact flags from tm_pipeline.mk TM_COMPILE_FLAGS
CXXFLAGS="-std=c++20 -O1 -fno-inline -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -fno-stack-protector -pthread"

# Pipeline: tm-instrument (default in tm_pipeline.mk)
PIPELINE="tm-instrument"

ALL_BENCHES=(
    "bank:benchmarks/test/bank/bank.cpp"
    "avltree:benchmarks/datastructures/avltree.cpp"
    "avltree_recursive:benchmarks/datastructures/avltree_recursive.cpp"
    "stmbench7:benchmarks/STMbench7/STMbench7.cpp"
    "ycsb:benchmarks/YCSB/YCSB.cpp"
    "eigen:benchmarks/EigenBench/EigenBench.cpp"
    "stamp:benchmarks/STAMP/STAMP.cpp"
    "bitmap:benchmarks/datastructures/bitmap.cpp"
    "hashmap:benchmarks/datastructures/hashmap.cpp"
    "list:benchmarks/datastructures/list.cpp"
    "set:benchmarks/datastructures/set.cpp"
    "heap:benchmarks/datastructures/heap.cpp"
    "rbtree:benchmarks/datastructures/rbtree.cpp"
)

out_dir=$(mktemp -d /tmp/tm_assess_full_XXXXXX)
echo "Assessment working dir: $out_dir" >&2
echo "Plugin: $PLUGIN" >&2
echo "CXX: $CXX" >&2
echo "Flags: $CXXFLAGS" >&2

assess_one() {
    local label="$1"
    local src="$2"
    local extra_cxxflags="${3:-}"
    local extra_optflags="${4:-}"

    local base_ll="$out_dir/${label}.ll"
    local instr_ll="$out_dir/${label}.instr.ll"
    local logfile="$out_dir/${label}.log"
    local errfile="$out_dir/${label}.err"

    # 1. Compile to uninstrumented LLVM IR (text .ll)
    echo "  [1/3] Compiling $label..." >&2
    if ! $CXX $CXXFLAGS $extra_cxxflags -emit-llvm -S "$src" -o "$base_ll" 2>/dev/null; then
        echo "  SKIP: compilation failed for $label" >&2
        return
    fi

    # 2. Instrument with TM plugin, capturing stderr for debug output
    echo "  [2/3] Instrumenting $label..." >&2
    if ! $OPT -load-pass-plugin="$PLUGIN" \
             -passes="$PIPELINE" \
             $extra_optflags \
             -S "$base_ll" -o "$instr_ll" 2>"$errfile"; then
        echo "  SKIP: instrumentation failed for $label" >&2
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

    # tm_end calls
    local tm_end_count
    tm_end_count=$(grep -c "call.*@tm_end()" "$instr_ll" 2>/dev/null || echo 0)
    tm_end_count=$(echo "$tm_end_count" | tr -d '[:space:]')

    # TM globals
    local tm_global_count
    tm_global_count=$(grep -c 'c"tm\\00"' "$base_ll" 2>/dev/null || echo 0)
    tm_global_count=$(echo "$tm_global_count" | tr -d '[:space:]')

    # All tm_read / tm_write hook calls
    local tm_read_write_count
    tm_read_write_count=$(grep -cE "tm_read_i[1-8]|tm_read_f[48]|tm_read_ptr|tm_read_i16|tm_read_i32|tm_read_i64|tm_write_i[1-8]|tm_write_f[48]|tm_write_ptr|tm_write_i16|tm_write_i32|tm_write_i64|tm_memset|tm_memcpy" "$instr_ll" 2>/dev/null || echo 0)
    tm_read_write_count=$(echo "$tm_read_write_count" | tr -d '[:space:]')

    # Read hooks separately
    local tm_read_count
    tm_read_count=$(grep -cE "call.*@tm_read_i[1-8]|call.*@tm_read_f[48]|call.*@tm_read_ptr|call.*@tm_read_i16|call.*@tm_read_i32|call.*@tm_read_i64" "$instr_ll" 2>/dev/null || echo 0)
    tm_read_count=$(echo "$tm_read_count" | tr -d '[:space:]')

    # Write hooks separately
    local tm_write_count
    tm_write_count=$(grep -cE "call.*@tm_write_i[1-8]|call.*@tm_write_f[48]|call.*@tm_write_ptr|call.*@tm_write_i16|call.*@tm_write_i32|call.*@tm_write_i64|call.*@tm_memset|call.*@tm_memcpy" "$instr_ll" 2>/dev/null || echo 0)
    tm_write_count=$(echo "$tm_write_count" | tr -d '[:space:]')

    # Clone definitions: unique _tm_clone function definitions
    local clone_defs_raw
    clone_defs_raw=$(grep "_tm_clone" "$instr_ll" | grep "define" | sed 's/.*@//' | sed 's/(.*//' | sort -u | wc -l | tr -d '[:space:]')

    # Total _tm_clone references (calls + defs)
    local clone_refs
    clone_refs=$(grep -c "_tm_clone" "$instr_ll" 2>/dev/null || echo 0)
    clone_refs=$(echo "$clone_refs" | tr -d '[:space:]')

    # _tm_clone call sites specifically (call instructions)
    local clone_calls
    clone_calls=$(grep -c "call.*_tm_clone" "$instr_ll" 2>/dev/null || echo 0)
    clone_calls=$(echo "$clone_calls" | tr -d '[:space:]')

    # Extract plugin debug output (clone counts from stderr)
    local plugin_clones=""
    local plugin_redirects=""
    if [ -f "$errfile" ]; then
        plugin_clones=$(grep "computeClonableFunctions" "$errfile" | sed 's/.*computeClonableFunctions: //' | sed 's/ functions clonable.*//' | tr '\n' ' ' | sed 's/ *$//')
        plugin_redirects=$(grep "redirectCallsToClones" "$errfile" | wc -l | tr -d '[:space:]')
    fi

    # Ratio
    local ratio
    if [ "$base_lines" -gt 0 ]; then
        ratio=$(echo "scale=2; $instr_lines / $base_lines" | bc 2>/dev/null || echo "1.00")
    else
        ratio="1.00"
    fi

    # Sanitize label
    local latex_label
    latex_label=$(echo "$label" | sed 's/_/\\_/g')

    # Print results line (fixed-width columns)
    printf "  %-18s | %6d | %6d | %3d | %3d | %3d | %4d | %4d | %5d | %4d | %4d | %s | %4.2f\n" \
        "$latex_label" \
        "$base_lines" \
        "$instr_lines" \
        "$tx_count" \
        "$tm_end_count" \
        "$tm_global_count" \
        "$tm_read_count" \
        "$tm_write_count" \
        "$tm_read_write_count" \
        "$clone_defs_raw" \
        "$clone_calls" \
        "${plugin_clones:-?}" \
        "$ratio"

    # Save detailed info
    {
        echo "=== $label ==="
        echo "Raw IR lines: $base_lines"
        echo "Instrumented IR lines: $instr_lines"
        echo "TX (tm_begin): $tx_count"
        echo "tm_end: $tm_end_count"
        echo "TM globals: $tm_global_count"
        echo "TM reads: $tm_read_count"
        echo "TM writes: $tm_write_count"
        echo "TM hooks total: $tm_read_write_count"
        echo "Clone defs (unique): $clone_defs_raw"
        echo "Clone call sites: $clone_calls"
        echo "Total _tm_clone refs: $clone_refs"
        echo "Ratio: $ratio"
        echo "Plugin debug:"
        cat "$errfile" 2>/dev/null | grep -E "(computeClonableFunctions|redirectCallsToClones|After-redirect)"
        echo ""
    } > "$out_dir/${label}.summary.txt"
}

echo ""
echo "Full TM Instrumentation Assessment"
echo "=================================="
echo ""
printf "  %-18s | %6s | %6s | %3s | %3s | %3s | %4s | %4s | %5s | %4s | %4s | %s | %s\n" \
    "Benchmark" "Raw" "Instr" "TX" "End" "Glob" "Rd" "Wr" "Total RW" "Cln" "Calls" "Plugin" "Ratio"
printf "  %-18s-+-%6s-+-%6s-+-%3s-+-%3s-+-%3s-+-%4s-+-%4s-+-%5s-+-%4s-+-%4s-+-%s-+-%s\n" \
    "-----------------" "------" "------" "---" "---" "---" "----" "----" "-----" "----" "----" "------" "----"

for entry in "${ALL_BENCHES[@]}"; do
    label="${entry%%:*}"
    src="${entry#*:}"
    extra_cxx=""
    extra_opt=""
    case "$label" in
        stmbench7)
            extra_cxx="-Ibenchmarks/datastructures -Ibackends"
            ;;
        ycsb)
            extra_opt="-tm-allow-opaque"
            ;;
    esac
    assess_one "$label" "$src" "$extra_cxx" "$extra_opt"
done

echo ""
echo "Detailed summaries in: $out_dir/*.summary.txt"
echo ""

# Print LaTeX table
echo ""
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

for entry in "${ALL_BENCHES[@]}"; do
    label="${entry%%:*}"
    src="${entry#*:}"
    latex_label=$(echo "$label" | sed 's/_/\\_/g')
    sf="$out_dir/${label}.summary.txt"
    if [ -f "$sf" ]; then
        base_lines=$(grep "^Raw IR lines:" "$sf" | cut -d' ' -f4)
        instr_lines=$(grep "^Instrumented IR lines:" "$sf" | cut -d' ' -f4)
        tx_count=$(grep "^TX (tm_begin):" "$sf" | cut -d' ' -f3)
        tm_global_count=$(grep "^TM globals:" "$sf" | cut -d' ' -f3)
        tm_read_write_count=$(grep "^TM hooks total:" "$sf" | cut -d' ' -f4)
        clone_defs_raw=$(grep "^Clone defs (unique):" "$sf" | cut -d' ' -f4)
        clone_calls=$(grep "^Clone call sites:" "$sf" | cut -d' ' -f4)
        ratio=$(grep "^Ratio:" "$sf" | cut -d' ' -f2)
        printf "  %s & %5d & %5d & %3d & %3d & %4d & %4d & %6d & %4.2f \\\\\n" \
            "$latex_label" \
            "$base_lines" \
            "$instr_lines" \
            "$tx_count" \
            "$tm_global_count" \
            "$tm_read_write_count" \
            "$clone_defs_raw" \
            "$clone_calls" \
            "$ratio"
    fi
done

echo "\\bottomrule"
echo "\\end{tabular}"
echo "\\caption{LLVM IR instrumentation assessment across benchmarks.}"
echo "\\label{tab:instrumentation-assessment}"
echo "\\end{table}"
