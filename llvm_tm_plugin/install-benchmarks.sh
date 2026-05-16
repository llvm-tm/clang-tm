#!/usr/bin/env bash
# =============================================================================
# install-benchmarks.sh — Set up and build TM benchmarks with installed clang-tm
#
# Creates a standalone benchmark workspace outside the repo that uses the
# system-wide clang-tm installation.
#
# Usage:
#   ./install-benchmarks.sh [OPTIONS]
#
# Options:
#   --prefix DIR     Prefix where clang-tm is installed   (env: PREFIX)
#   --destdir DIR    Staging directory                     (env: DESTDIR)
#   --benchdir DIR   Where to create the workspace         (env: BENCHDIR)
#                     default: ~/tm-benchmarks
#   -y, --yes        Non-interactive
#   --skip-build     Only copy sources, don't compile
#   --dry-run        Print what would be done, do nothing
#   -h, --help       Show this message
# =============================================================================

set -euo pipefail

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; NC=''
fi

info()  { printf "${GREEN}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
warn()  { printf "${YELLOW}==>${NC} ${BOLD}%s${NC}\n" "$*"; }
err()   { printf "${RED}==>${NC} ${BOLD}%s${NC}\n" "$*" >&2; }
die()   { err "$@"; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GITHUB_BASE="https://raw.githubusercontent.com/llvm-tm/clang-tm/main"

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"
BENCHDIR="${BENCHDIR:-$HOME/tm-benchmarks}"
YES=0
SKIP_BUILD=0
DRY_RUN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)      PREFIX="$2"; shift 2 ;;
        --prefix=*)    PREFIX="${1#*=}"; shift ;;
        --destdir)     DESTDIR="$2"; shift 2 ;;
        --destdir=*)   DESTDIR="${1#*=}"; shift ;;
        --benchdir)    BENCHDIR="$2"; shift 2 ;;
        --benchdir=*)  BENCHDIR="${1#*=}"; shift ;;
        -y|--yes)      YES=1; shift ;;
        --skip-build)  SKIP_BUILD=1; shift ;;
        --dry-run)     DRY_RUN=1; shift ;;
        -h|--help)
            sed -n 's/^# \?//p' "$0" | sed '1,/^$/d' | head -n -1
            exit 0 ;;
        *) die "Unknown option: $1. Try --help." ;;
    esac
done

# ---- Resolve clang-tm installation ----
CLANG_TM="${DESTDIR}${PREFIX}/bin/clang-tm"
if [ ! -f "$CLANG_TM" ]; then
    CLANG_TM="$(command -v clang-tm || true)"
fi
if [ -z "$CLANG_TM" ] || [ ! -f "$CLANG_TM" ]; then
    die "clang-tm not found. Run install.sh first, or ensure clang-tm is on PATH."
fi
info "Using clang-tm: $CLANG_TM"

CLANG_TM_BIN="$(dirname "$CLANG_TM")"
TM_LIB_DIR="$(cd "$CLANG_TM_BIN/../lib/clang-tm" && pwd)"
TM_INCLUDE="$TM_LIB_DIR/include"
TM_RUNTIMES="$TM_LIB_DIR/runtimes"

if [ ! -d "$TM_INCLUDE" ] || [ ! -d "$TM_RUNTIMES" ]; then
    die "clang-tm installation incomplete: missing $TM_INCLUDE or $TM_RUNTIMES"
fi

# ---- Confirm ----
info "Creating benchmark workspace at: ${BOLD}$BENCHDIR${NC}"
if [ "$YES" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    printf "Continue? [Y/n] "
    if [ -t 0 ]; then
        read -r reply
    elif [ -e /dev/tty ]; then
        read -r reply </dev/tty
    else
        reply="y"
    fi
    case "$reply" in
        n|N|no|NO) die "Aborted." ;;
    esac
fi

if [ "$DRY_RUN" -eq 1 ]; then
    info "Dry-run — nothing installed."
    exit 0
fi

# ---- Helper: copy file from repo or GitHub ----
copy_file() {
    local src_rel="$1"
    local dst="$2"
    local src="$PROJECT_ROOT/$src_rel"
    mkdir -p "$(dirname "$dst")"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
    else
        curl -fsSL -o "$dst" "$GITHUB_BASE/$src_rel"
    fi
}

copy_files() {
    local dir="$1"
    shift
    local dst="$BENCHDIR/$dir"
    mkdir -p "$dst"
    for f in "$@"; do
        copy_file "$dir/$f" "$dst/$f"
    done
}

# ---- Create workspace directory structure ----
mkdir -p "$BENCHDIR"

# ---- Copy benchmark sources ----
info "Copying benchmark sources..."

# test/bank
copy_files "benchmarks/test/bank" bank.cpp Makefile

# test/intset
copy_files "benchmarks/test/intset" intset.cpp Makefile

# datastructures
copy_files "benchmarks/datastructures" \
    avltree.cpp avltree_recursive.cpp rbtree.cpp hashmap.cpp \
    bitmap.cpp list.cpp set.cpp heap.cpp common.hpp Makefile

# STAMP
copy_files "benchmarks/STAMP" \
    STAMP.cpp stamp_common.hpp \
    bayes_bench.hpp genome_bench.hpp intruder_bench.hpp \
    kmeans_bench.hpp labyrinth_bench.hpp ssca2_bench.hpp \
    vacation_bench.hpp yada_bench.hpp Makefile

# STMbench7
copy_files "benchmarks/STMbench7" STMbench7.cpp Makefile

# EigenBench
copy_files "benchmarks/EigenBench" EigenBench.cpp Makefile

# TPCC
copy_files "benchmarks/TPCC" TPCC.cpp Makefile

# YCSB
copy_files "benchmarks/YCSB" YCSB.cpp Makefile

# ---- Copy infrastructure ----
info "Copying build infrastructure..."

mkdir -p "$BENCHDIR/llvm_tm_plugin"
copy_file "llvm_tm_plugin/clang-tm" "$BENCHDIR/llvm_tm_plugin/clang-tm"
copy_file "llvm_tm_plugin/tm_pipeline.mk" "$BENCHDIR/llvm_tm_plugin/tm_pipeline.mk"
copy_file "llvm_tm_plugin/llvm-tool-helper.mk" "$BENCHDIR/llvm_tm_plugin/llvm-tool-helper.mk"
chmod 755 "$BENCHDIR/llvm_tm_plugin/clang-tm"

# ---- Copy backend runtimes and headers ----
info "Copying backend runtimes and headers..."

mkdir -p "$BENCHDIR/backends/runtimes"
for f in "$TM_RUNTIMES"/*.cpp; do
    cp "$f" "$BENCHDIR/backends/runtimes/"
done

for dir in TinySTM NOrec SwissTM TL2; do
    if [ -d "$PROJECT_ROOT/backends/$dir" ]; then
        mkdir -p "$BENCHDIR/backends/$dir"
        cp "$PROJECT_ROOT/backends/$dir"/*.hpp "$BENCHDIR/backends/$dir/" 2>/dev/null || true
    fi
done

for f in tm_api.hpp tm_common.hpp tm_alloc_overrides.hpp rel_ptr.hpp; do
    if [ -f "$PROJECT_ROOT/backends/$f" ]; then
        cp "$PROJECT_ROOT/backends/$f" "$BENCHDIR/backends/"
    fi
done

# ---- Generate top-level Makefile ----
info "Generating top-level Makefile..."

cat > "$BENCHDIR/Makefile" << MAKEEOF
# Top-level Makefile for clang-tm benchmarks — auto-generated by install-benchmarks.sh
#
# Build all benchmarks with all supported backends.
# To build a subset: make <target> (tab-completion shows available targets).
#
# Example thread-count scaling:
#   make bank_tsxsgl && for t in 1 2 4 7 8 10 12 14 16 21 28 35 42 49 52; do \\
#     ./bin/bank_tsxsgl -t \$t -d 5000; done

CLANG_TM   := $CLANG_TM
LLVM_CONFIG := \$(shell command -v llvm-config-22 2>/dev/null || command -v llvm-config-22.1 2>/dev/null || command -v llvm-config 2>/dev/null || echo llvm-config)
LLVM_BINDIR := \$(shell \$(LLVM_CONFIG) --bindir 2>/dev/null)
LLVM_CXX    := \$(if \$(LLVM_BINDIR),\$(LLVM_BINDIR)/clang++,clang++)
LLVM_OPT    := \$(if \$(LLVM_BINDIR),\$(LLVM_BINDIR)/opt,opt)
CXXFLAGS   := -std=c++20 -O3 -pthread -I$TM_LIB_DIR
RUNTIMES   := $TM_RUNTIMES
BACKENDS   := $TM_LIB_DIR
BIN_DIR    := bin

.PHONY: all clean test \$(BIN_DIR)

all: \$(BIN_DIR) \\
	bank_all \\
	intset \\
	avltree_singlelock avltree_norec avltree_tinystm \\
	rbtree_singlelock rbtree_norec rbtree_tinystm \\
	hashmap_singlelock hashmap_norec hashmap_tinystm \\
	bitmap_singlelock bitmap_norec bitmap_tinystm \\
	list_singlelock list_norec list_tinystm \\
	set_singlelock set_norec set_tinystm \\
	heap_singlelock heap_norec heap_tinystm \\
	stamp_singlelock stamp_norec stamp_tinystm \\
	stmbench_singlelock stmbench_tl2 stmbench_tinystm \\
	eigen_singlelock eigen_tl2 eigen_tinystm \\
	tpcc_singlelock tpcc_tl2 tpcc_tinystm tpcc_persistentsgl \\
	ycsb_singlelock ycsb_tl2 ycsb_tinystm

\$(BIN_DIR):
	mkdir -p \$(BIN_DIR)

# =====================================================================
# Bank — supports all runtimes including TSXSGL
# =====================================================================
bank_all: \$(BIN_DIR) \\
	bank_uninstrumented bank_singlelock bank_norec bank_tl2 \\
	bank_tinystm bank_swiss bank_persistentsgl bank_tsxsgl

bank_uninstrumented: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(LLVM_CXX) \$(CXXFLAGS) \$< -o \$(BIN_DIR)/\$@

bank_singlelock: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

bank_norec: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

bank_tl2: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/tl2_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

bank_tinystm: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/TinySTM -DDESIGN_WBCTL --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

bank_swiss: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/SwissTM --runtime \$(RUNTIMES)/SwissTM_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

bank_persistentsgl: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/PersistentSGL_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

bank_tsxsgl: benchmarks/test/bank/bank.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -mrtm --runtime \$(RUNTIMES)/TSXSGL_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

# =====================================================================
# IntSet (uninstrumented only)
# =====================================================================
intset: benchmarks/test/intset/intset.cpp | \$(BIN_DIR)
	\$(LLVM_CXX) \$(CXXFLAGS) \$< -o \$(BIN_DIR)/\$@

# =====================================================================
# Data structures — avltree, rbtree, hashmap, bitmap, list, set, heap
# =====================================================================
# Each uses the 4-step TM pipeline (clang-tm wrapper):
#   clang-tm --runtime <runtime> [includes] source.cpp -o bin/target
TINYSTM_FLAGS := -I\$(BACKENDS)/TinySTM -DDESIGN_WBCTL
DS_SRC       := benchmarks/datastructures
DS_BIN       := \$(BIN_DIR)

\$(DS_BIN)/avltree_singlelock: \$(DS_SRC)/avltree.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$@ \$<
\$(DS_BIN)/avltree_norec: \$(DS_SRC)/avltree.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$@ \$<
\$(DS_BIN)/avltree_tinystm: \$(DS_SRC)/avltree.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$@ \$<
avltree_singlelock avltree_norec avltree_tinystm: %: \$(DS_BIN)/%

\$(DS_BIN)/rbtree_singlelock: \$(DS_SRC)/rbtree.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$@ \$<
\$(DS_BIN)/rbtree_norec: \$(DS_SRC)/rbtree.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$@ \$<
\$(DS_BIN)/rbtree_tinystm: \$(DS_SRC)/rbtree.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$@ \$<
rbtree_singlelock rbtree_norec rbtree_tinystm: %: \$(DS_BIN)/%

\$(DS_BIN)/hashmap_singlelock: \$(DS_SRC)/hashmap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$@ \$<
\$(DS_BIN)/hashmap_norec: \$(DS_SRC)/hashmap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$@ \$<
\$(DS_BIN)/hashmap_tinystm: \$(DS_SRC)/hashmap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$@ \$<
hashmap_singlelock hashmap_norec hashmap_tinystm: %: \$(DS_BIN)/%

\$(DS_BIN)/bitmap_singlelock: \$(DS_SRC)/bitmap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$@ \$<
\$(DS_BIN)/bitmap_norec: \$(DS_SRC)/bitmap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$@ \$<
\$(DS_BIN)/bitmap_tinystm: \$(DS_SRC)/bitmap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$@ \$<
bitmap_singlelock bitmap_norec bitmap_tinystm: %: \$(DS_BIN)/%

\$(DS_BIN)/list_singlelock: \$(DS_SRC)/list.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$@ \$<
\$(DS_BIN)/list_norec: \$(DS_SRC)/list.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$@ \$<
\$(DS_BIN)/list_tinystm: \$(DS_SRC)/list.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$@ \$<
list_singlelock list_norec list_tinystm: %: \$(DS_BIN)/%

\$(DS_BIN)/set_singlelock: \$(DS_SRC)/set.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$@ \$<
\$(DS_BIN)/set_norec: \$(DS_SRC)/set.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$@ \$<
\$(DS_BIN)/set_tinystm: \$(DS_SRC)/set.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$@ \$<
set_singlelock set_norec set_tinystm: %: \$(DS_BIN)/%

\$(DS_BIN)/heap_singlelock: \$(DS_SRC)/heap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$@ \$<
\$(DS_BIN)/heap_norec: \$(DS_SRC)/heap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$@ \$<
\$(DS_BIN)/heap_tinystm: \$(DS_SRC)/heap.cpp | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$@ \$<
heap_singlelock heap_norec heap_tinystm: %: \$(DS_BIN)/%

# =====================================================================
# STAMP
# =====================================================================
STAMP_SRC := benchmarks/STAMP/STAMP.cpp
STAMP_INC := -Ibenchmarks/STAMP

stamp_singlelock: \$(STAMP_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(STAMP_INC) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
stamp_norec: \$(STAMP_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(STAMP_INC) -I\$(BACKENDS)/NOrec --runtime \$(RUNTIMES)/NOrec_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
stamp_tinystm: \$(STAMP_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(STAMP_INC) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

# =====================================================================
# STMbench7
# =====================================================================
S7_SRC := benchmarks/STMbench7/STMbench7.cpp

stmbench_singlelock: \$(S7_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
stmbench_tl2: \$(S7_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/tl2_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
stmbench_tinystm: \$(S7_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

# =====================================================================
# EigenBench
# =====================================================================
EIGEN_SRC := benchmarks/EigenBench/EigenBench.cpp

eigen_singlelock: \$(EIGEN_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
eigen_tl2: \$(EIGEN_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/tl2_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
eigen_tinystm: \$(EIGEN_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

# =====================================================================
# TPC-C
# =====================================================================
TPCC_SRC := benchmarks/TPCC/TPCC.cpp

tpcc_singlelock: \$(TPCC_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
tpcc_tl2: \$(TPCC_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/tl2_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
tpcc_tinystm: \$(TPCC_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
tpcc_persistentsgl: \$(TPCC_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/PersistentSGL_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

# =====================================================================
# YCSB
# =====================================================================
YCSB_SRC := benchmarks/YCSB/YCSB.cpp

ycsb_singlelock: \$(YCSB_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/SingleGlobalLock_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
ycsb_tl2: \$(YCSB_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) --runtime \$(RUNTIMES)/tl2_runtime.cpp -o \$(BIN_DIR)/\$@ \$<
ycsb_tinystm: \$(YCSB_SRC) | \$(BIN_DIR)
	\$(CLANG_TM) \$(CXXFLAGS) \$(TINYSTM_FLAGS) --runtime \$(RUNTIMES)/TinySTM_runtime.cpp -o \$(BIN_DIR)/\$@ \$<

# =====================================================================
# Test / run
# =====================================================================
test: bank_tsxsgl
	@echo "=== Bank TSXSGL thread scaling ==="
	@for t in 1 2 4 7 8 10 12 14 16 21 28 35 42 49 52; do \\
		echo "--- bank_tsxsgl -t \$\$t ---"; \\
		\$(BIN_DIR)/bank_tsxsgl -t \$\$t -d 5000 2>&1 || echo "FAILED"; \\
	done

clean:
	rm -rf \$(BIN_DIR)
MAKEEOF

info "Workspace ready at: ${BOLD}$BENCHDIR${NC}"
echo ""
printf "  ${BOLD}Build all:${NC}\n"
printf "    make -C $BENCHDIR all -j\$(nproc)\n"
echo ""
printf "  ${BOLD}Run TSXSGL thread scaling test:${NC}\n"
printf "    make -C $BENCHDIR test\n"
echo ""
printf "  ${BOLD}Run individual benchmark:${NC}\n"
printf "    make -C $BENCHDIR bank_tsxsgl\n"
printf "    $BENCHDIR/bin/bank_tsxsgl -t 8 -d 5000\n"
echo ""

# ---- Build ----
if [ "$SKIP_BUILD" -eq 0 ]; then
    CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    info "Building all benchmarks with $CORES parallel jobs (this may take a while)..."
    make -C "$BENCHDIR" all -j"$CORES" 2>&1 | tail -20
    info "Build complete."
    echo ""
    info "Built binaries:"
    ls -1 "$BENCHDIR/bin/" 2>/dev/null || echo "(none)"
fi
