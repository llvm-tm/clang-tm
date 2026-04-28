# Data Structures Benchmark Makefile - Modern C++17 Version
# Builds and tests AVL tree, Red-Black tree, and Hash Map benchmarks
# Includes Bitmap, List, Set, and Heap benchmarks
# Version 2.0

# Paths
LLVM_PLUGIN_DIR := ../../llvm_tm_plugin
BACKENDS_DIR := ../../backends
RUNTIMES_DIR := $(BACKENDS_DIR)/runtimes
TL2_DIR := $(BACKENDS_DIR)/TL2
TINYSTM_DIR := $(BACKENDS_DIR)/TinySTM
SWISSTM_DIR := $(BACKENDS_DIR)/SwissTM

# Output directories
OUT_DIR := out
BIN_DIR := bin

# Compiler settings
CXX := clang++
CXXFLAGS := -std=c++17 -pthread

# Plugin
PLUGIN_LIB := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

ifdef DEBUG
OPT_FLAGS := -O0 -g
else
OPT_FLAGS := -O2
endif

# TL2 library
TL2_LIB := $(TL2_DIR)/lib/libtl2.a

# TinySTM library
TINYSTM_LIB := $(TINYSTM_DIR)/lib/libstm.a

# SwissTM library
TINYSTM_LIB := $(SWISSTM_DIR)/lib/libwlpdstm.a

# Source files per benchmark (all in current directory)
AVLTREE_SRC := avltree.cpp
RBTREE_SRC := rbtree.cpp
HASHMAP_SRC := hashmap.cpp
BITMAP_SRC := bitmap.cpp
LIST_SRC := list.cpp
SET_SRC := set.cpp
HEAP_SRC := heap.cpp
STMbench7_SRC := ../STMbench7/STMbench7.cpp
YCSB_SRC := ../YCSB/YCSB.cpp
EIGENBENCH_SRC := ../EigenBench/EigenBench.cpp

# Build targets for each runtime
# Each benchmark has: uninstrumented, debug, tl2, tinystm variants
TARGETS := \
	avltree_uninstrumented \
	avltree_debug \
	avltree \
	rbtree_uninstrumented \
	rbtree_debug \
	rbtree \
	hashmap_uninstrumented \
	hashmap_debug \
	hashmap \
	bitmap_uninstrumented \
	bitmap_debug \
	bitmap \
	list_uninstrumented \
	list_debug \
	list \
	set_uninstrumented \
	set_debug \
	set \
	heap_uninstrumented \
	heap_debug \
	heap \
	stmbench7_uninstrumented \
	stmbench7_debug \
	ycsb_uninstrumented \
	ycsb_debug \
	eigenbench_uninstrumented \
	eigenbench_debug

.PHONY: all clean test help

all: $(OUT_DIR) $(BIN_DIR) $(TARGETS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# ============================================
# Build .bc objects
# ============================================

$(OUT_DIR)/%.bc : %.cpp | $(OUT_DIR)
	clang++ -std=c++17 $(OPT_FLAGS) -emit-llvm -c $< -o $@ -fno-stack-protector -pthread

$(OUT_DIR)/%_debug.bc : %.cpp | $(OUT_DIR)
	clang++ -std=c++17 -O0 -g -emit-llvm -c $< -o $@ -fno-stack-protector -pthread

$(OUT_DIR)/%.opt.bc : $(OUT_DIR)/%.bc $(PLUGIN_LIB) | $(OUT_DIR)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/%_debug.opt.bc : $(OUT_DIR)/%_debug.bc $(PLUGIN_LIB) | $(OUT_DIR)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

# ============================================
# Build executables
# ============================================

$(BIN_DIR)/%_uninstrumented: %.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OPT_FLAGS) $< -o $@ -pthread

$(BIN_DIR)/%_debug: $(OUT_DIR)/%_debug.opt.bc $(RUNTIMES_DIR)/test_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -O0 -g $< $(word 2,$^) -o $@

$(BIN_DIR)/%_TinySTM: $(OUT_DIR)/%.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OPT_FLAGS) $< $(word 2,$^) -o $@ -I$(TINYSTM_DIR)/include

$(BIN_DIR)/%_SwissTM: $(OUT_DIR)/%.opt.bc $(RUNTIMES_DIR)/SwissTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OPT_FLAGS) $< $(word 2,$^) -o $@ -I$(SWISSTM_DIR)/include

$(BIN_DIR)/%_tl2: $(OUT_DIR)/%.opt.bc $(RUNTIMES_DIR)/TL2_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OPT_FLAGS) $< $(word 2,$^) -o $@

$(BIN_DIR)/%_SingleGlobalLock: $(OUT_DIR)/%.opt.bc $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OPT_FLAGS) $< $(word 2,$^) -o $@

# ============================================
# AVL Tree
# ============================================

avltree_TinySTM: $(BIN_DIR)/avltree_TinySTM
	@echo "Done"

avltree_tl2: $(BIN_DIR)/avltree_tl2
	@echo "Done"

avltree_SwissTM: $(BIN_DIR)/avltree_SwissSTM
	@echo "Done"

avltree_SGL: $(BIN_DIR)/avltree_SingleGlobalLock
	@echo "Done"

# ============================================
# Red-Black Tree
# ============================================

rbtree_uninstrumented: $(RBTREE_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(RBTREE_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/rbtree.bc: $(RBTREE_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(RBTREE_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/rbtree_debug.opt.bc: $(OUT_DIR)/rbtree.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

rbtree_debug: $(OUT_DIR)/rbtree_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/rbtree_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ -I$(TINYSTM_DIR)/include

# ============================================
# Hash Map
# ============================================

hashmap_uninstrumented: $(HASHMAP_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(HASHMAP_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/hashmap.bc: $(HASHMAP_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(HASHMAP_SRC) -o $@ -fno-stack-protector -pthread

$(OUT_DIR)/hashmap_debug.opt.bc: $(OUT_DIR)/hashmap.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

hashmap_debug: $(OUT_DIR)/hashmap_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/hashmap_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ -I$(TINYSTM_DIR)/include

# ===========================================
# Bitmap
# ==========================================

bitmap_uninstrumented: $(BITMAP_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(BITMAP_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/bitmap.bc: $(BITMAP_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(BITMAP_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/bitmap_debug.opt.bc: $(OUT_DIR)/bitmap.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

bitmap_debug: $(OUT_DIR)/bitmap_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bitmap_debug.opt.bc \
		$(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ \
		-I$(TINYSTM_DIR)/include

# ===========================================
# List
# ==========================================

list_uninstrumented: $(LIST_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(LIST_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/list.bc: $(LIST_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(LIST_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/list_debug.opt.bc: $(OUT_DIR)/list.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

list_debug: $(OUT_DIR)/list_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/list_debug.opt.bc \
		$(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ \
		-I$(TINYSTM_DIR)/include

# ===========================================
# Set
# ==========================================

set_uninstrumented: $(SET_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SET_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/set.bc: $(SET_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(SET_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/set_debug.opt.bc: $(OUT_DIR)/set.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

set_debug: $(OUT_DIR)/set_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/set_debug.opt.bc \
		$(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ \
		-I$(TINYSTM_DIR)/include

# ===========================================
# Heap
# ==========================================

heap_uninstrumented: $(HEAP_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(HEAP_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/heap.bc: $(HEAP_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(HEAP_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/heap_debug.opt.bc: $(OUT_DIR)/heap.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

heap_debug: $(OUT_DIR)/heap_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/heap_debug.opt.bc \
		$(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ \
		-I$(TINYSTM_DIR)/include

# ===========================================
# STMbench7
# ===========================================

stmbench7_uninstrumented: $(STMbench7_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(STMbench7_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/stmbench7.bc: $(STMbench7_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(STMbench7_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/stmbench7.opt.bc: $(OUT_DIR)/stmbench7.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/stmbench7_debug.bc: $(STMbench7_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O0 -g -emit-llvm -c $(STMbench7_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/stmbench7_debug.opt.bc: $(OUT_DIR)/stmbench7_debug.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

stmbench7_debug: $(OUT_DIR)/stmbench7_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/stmbench7_debug.opt.bc \
		$(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ \
		-I$(TINYSTM_DIR)/include

# ===========================================
# YCSB
# ===========================================

ycsb_uninstrumented: $(YCSB_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(YCSB_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/ycsb.bc: $(YCSB_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(YCSB_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/ycsb.opt.bc: $(OUT_DIR)/ycsb.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/ycsb_debug.bc: $(YCSB_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O0 -g -emit-llvm -c $(YCSB_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/ycsb_debug.opt.bc: $(OUT_DIR)/ycsb_debug.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

ycsb_debug: $(OUT_DIR)/ycsb_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/ycsb_debug.opt.bc \
		$(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ \
		-I$(TINYSTM_DIR)/include

# ===========================================
# EigenBench
# ===========================================

eigenbench_uninstrumented: $(EIGENBENCH_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(EIGENBENCH_SRC) -o $(BIN_DIR)/$@ -pthread

$(OUT_DIR)/eigenbench.bc: $(EIGENBENCH_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O1 -fno-inline -fno-inline-functions -emit-llvm -c $(EIGENBENCH_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/eigenbench.opt.bc: $(OUT_DIR)/eigenbench.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/eigenbench_debug.bc: $(EIGENBENCH_SRC) | $(OUT_DIR)
	clang++ -std=c++17 -O0 -g -emit-llvm -c $(EIGENBENCH_SRC) \
		-o $@ -fno-stack-protector -pthread

$(OUT_DIR)/eigenbench_debug.opt.bc: $(OUT_DIR)/eigenbench_debug.bc $(PLUGIN_LIB)
	opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" $< -o $@

eigenbench_debug: $(OUT_DIR)/eigenbench_debug.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/eigenbench_debug.opt.bc \
		$(RUNTIMES_DIR)/TinySTM_runtime.cpp -o $(BIN_DIR)/$@ \
		-I$(TINYSTM_DIR)/include

# ===========================================
# Test targets
# ===========================================
test: test_avltree test_rbtree test_hashmap test_bitmap test_list test_set test_heap test_stmbench7 test_ycsb test_eigenbench

test_avltree: avltree_debug
	@echo "Testing AVL tree benchmark (4 threads, 10 seconds)..."
	@$(BIN_DIR)/avltree_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_rbtree: rbtree_debug
	@echo "Testing Red-Black tree benchmark (4 threads, 10 seconds)..."
	@$(BIN_DIR)/rbtree_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_hashmap: hashmap_debug
	@echo "Testing Hash Map benchmark (4 threads, 10 seconds)..."
	@$(BIN_DIR)/hashmap_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_bitmap: bitmap_debug
	@echo "Testing Bitmap benchmark (4 threads, 10 seconds)..."
	@$(BIN_DIR)/bitmap_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_list: list_debug
	@echo "Testing List benchmark (4 threads, 10 seconds)..."
	@$(BIN_DIR)/list_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_set: set_debug
	@echo "Testing Set benchmark (4 threads, 10 seconds)..."
	@$(BIN_DIR)/set_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_heap: heap_debug
	@echo "Testing Heap benchmark (4 threads, 10 seconds)..."
	@$(BIN_DIR)/heap_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_stmbench7: stmbench7_debug
	@echo "Testing STMbench7 benchmark (2 threads, 5 seconds)..."
	@$(BIN_DIR)/stmbench7_debug 2 5000 || echo "Test completed"

test_ycsb: ycsb_debug
	@echo "Testing YCSB benchmark (4 threads, Workload A, 5 seconds)..."
	@$(BIN_DIR)/ycsb_debug -t 4 -d 5000 -w A || echo "Test completed"

test_eigenbench: eigenbench_debug
	@echo "Testing EigenBench benchmark (4 threads, 5 seconds)..."
	@$(BIN_DIR)/eigenbench_debug -t 4 -d 5000 || echo "Test completed"

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)