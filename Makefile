# Top-level Makefile for TM API C++ Project
# Builds: LLVM plugin, runtimes, benchmarks, and tests
#
# Targets:
#   all           - Build everything
#   plugin        - Build LLVM TM plugin
#   runtimes      - Build STM runtimes
#   benchmarks   - Build all benchmarks
#   tests        - Build plugin tests
#   clean        - Clean all build artifacts
#
# Options:
#   BACKEND=tl2       Build with TL2 backend (default TinySTM)
#   BACKEND=singlelock Build with SingleGlobalLock backend
#   DEBUG=1         Build with debug flags

# Detect LLVM installation
LLVM_CONFIG ?= llvm-config
LLVM_CONFIG_ARGS := $(shell $(LLVM_CONFIG) --cxxflags --ldflags --system-libs --libs core analysis 2>/dev/null || echo "-I/opt/homebrew/Cellar/llvm/22.1.4/include -L/opt/homebrew/Cellar/llvm/22.1.4/lib -lLLVM-22")

# Compiler settings
CXX ?= clang++
CXXFLAGS += -std=c++17 -pthread
ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g
else
  CXXFLAGS += -O2
endif

# Backend selection
BACKEND ?= tinystm

# Directories
PROJECT_ROOT := $(shell pwd)
LLVM_PLUGIN_DIR := $(PROJECT_ROOT)/llvm_tm_plugin
BACKENDS_DIR := $(PROJECT_ROOT)/backends
RUNTIMES_DIR := $(BACKENDS_DIR)/runtimes
BENCHMARKS_DIR := $(PROJECT_ROOT)/benchmarks
TESTS_DIR := $(PROJECT_ROOT)/backends/tests

# Plugin
PLUGIN := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

# All subdirectories
PLUGIN_SUBDIR := llvm_tm_plugin
RUNTIME_SUBDIRS := runtimes
BENCHMARK_SUBDIRS := \
	benchmarks/test/bank \
	benchmarks/datastructures \
	benchmarks/STMbench7 \
	benchmarks/YCSB \
	benchmarks/EigenBench \
	benchmarks/STAMP \
	benchmarks/TPCC \
	benchmarks/test/intset

# regression is broken - missing Makefile.common
# benchmarks/test/regression

.PHONY: all clean plugin runtimes benchmarks tests check help info

all: info plugin runtimes benchmarks

info:
	@echo "TM API C++ Build System"
	@echo "==================="
	@echo "Project Root:  $(PROJECT_ROOT)"
	@echo "LLVM Config: $(LLVM_CONFIG)"
	@echo "Backend:    $(BACKEND)"
	@echo "CXX:       $(CXX)"
	@echo "CXXFLAGS:  $(CXXFLAGS)"
	@echo ""
	@echo "Plugin:    $(PLUGIN)"

# ============================================================================
# LLVM Plugin
# ============================================================================

plugin: $(PLUGIN)

$(PLUGIN): $(LLVM_PLUGIN_DIR)/src/TMInstrumentPass.cpp | $(LLVM_PLUGIN_DIR)/bin
	$(CXX) -fPIC -std=c++17 -shared $< -o $@ $(LLVM_CONFIG_ARGS)

$(LLVM_PLUGIN_DIR)/bin:
	mkdir -p $@

# ============================================================================
# Runtimes
# ============================================================================

runtimes:
	@echo "Building runtimes (header-only, no build needed)..."

# ============================================================================
# Benchmarks
# ============================================================================

benchmarks: plugin
	@echo "Building all benchmarks..."
	@$(MAKE) -C $(BENCHMARKS_DIR)/test/bank clean 2>/dev/null || true
	@$(MAKE) -C $(BENCHMARKS_DIR)/test/bank bank_tinystm BACKEND=$(BACKEND)
	@$(MAKE) -C $(BENCHMARKS_DIR)/test/bank bank_singlelock BACKEND=$(BACKEND)
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures clean 2>/dev/null || true
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures bin/avltree_SingleGlobalLock
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures bin/rbtree_SingleGlobalLock
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures bin/hashmap_SingleGlobalLock
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures bin/bitmap_SingleGlobalLock
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures bin/list_SingleGlobalLock
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures bin/set_SingleGlobalLock
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures bin/heap_SingleGlobalLock
	@$(MAKE) -C $(BENCHMARKS_DIR)/STAMP clean 2>/dev/null || true
	@$(MAKE) -C $(BENCHMARKS_DIR)/STAMP stamp_uninstrumented
	@$(MAKE) -C $(BENCHMARKS_DIR)/STAMP stamp_tinystm BACKEND=$(BACKEND)
	@echo "Benchmarks built."

# ============================================================================
# Plugin Tests
# ============================================================================

tests: plugin
	@echo "Building plugin tests..."
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) clean 2>/dev/null || true
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) all

# ============================================================================
# Clean
# ============================================================================

clean:
	@echo "Cleaning all build artifacts..."
	@for dir in $(BENCHMARK_SUBDIRS); do \
		$(MAKE) -C $(dir) clean 2>/dev/null || true; \
	done
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) clean 2>/dev/null || true
	@$(MAKE) -C $(BENCHMARKS_DIR)/test/bank clean 2>/dev/null || true
	@$(MAKE) -C $(BENCHMARKS_DIR)/datastructures clean 2>/dev/null || true
	rm -rf $(LLVM_PLUGIN_DIR)/bin $(LLVM_PLUGIN_DIR)/out
	rm -f /tmp/tm_persistent_state.bin
	@echo "Clean complete."

# ============================================================================
# Help
# ============================================================================

help:
	@echo "TM API C++ Build System"
	@echo "======================"
	@echo ""
	@echo "Main Targets:"
	@echo "  all           - Build everything (plugin, runtimes, benchmarks)"
	@echo "  plugin       - Build LLVM TM plugin"
	@echo "  runtimes     - Prepare STM runtimes (header-only)"
	@echo "  benchmarks   - Build all benchmarks"
	@echo "  tests        - Build plugin tests"
	@echo "  clean        - Clean all build artifacts"
	@echo ""
	@echo "Options:"
	@echo "  BACKEND=tl2        - Use TL2 backend (default: tinystm)"
	@echo "  BACKEND=singlelock - Use SingleGlobalLock backend"
	@echo "  DEBUG=1            - Build with debug flags (-O0 -g)"
	@echo ""
	@echo "LLVM Options:"
	@echo "  LLVM_CONFIG=<path> - Specify llvm-config path"
	@echo ""
	@echo "Examples:"
	@echo "  make all                    # Build everything"
	@echo "  make plugin                # Just the plugin"
	@echo "  make benchmarks BACKEND=tl2 # Build with TL2"
	@echo "  make clean                 # Clean everything"

# ============================================================================
# Quick Test
# ============================================================================

test_run: benchmarks
	@echo ""
	@echo "Running benchmark tests..."
	@echo "=========================="
	@echo ""
	@echo "Bank benchmark (SingleGlobalLock):"
	@$(BENCHMARKS_DIR)/test/bank/bin/bank_singlelock -t 2 -d 1000 || echo "(timeout or error)"
	@echo ""
	@echo "AVL Tree benchmark:"
	@$(BENCHMARKS_DIR)/datastructures/bin/avltree_SingleGlobalLock 1 10 100 || echo "(timeout or error)"
	@echo ""
	@echo "STAMP benchmarks (uninstrumented):"
	@$(BENCHMARKS_DIR)/STAMP/bin/stamp_uninstrumented -t 2 -d 500 || echo "(timeout or error)"
	@echo ""
	@echo "Tests complete."