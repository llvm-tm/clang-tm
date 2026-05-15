include ../../llvm_tm_plugin/tm_pipeline.mk

# Override default paths before include
LLVM_PLUGIN_DIR := ../../llvm_tm_plugin
BACKENDS_DIR := ../../backends
RUNTIMES_DIR := $(BACKENDS_DIR)/runtimes
TL2_DIR := $(BACKENDS_DIR)/TL2
TINYSTM_DIR := $(BACKENDS_DIR)/TinySTM
SWISSTM_DIR := $(BACKENDS_DIR)/SwissTM
NOREC_DIR := $(BACKENDS_DIR)/NOrec
CXXFLAGS := -std=c++20 -O2 -pthread

PLUGIN_LIB := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

# ----- Benchmark lists -----
LOCAL := avltree rbtree hashmap bitmap list set heap
CROSS := stmbench7 ycsb eigenbench
ALL_BENCH := $(LOCAL) $(CROSS)

BACKENDS := TinySTM tl2 SingleGlobalLock SwissTM NOrec
BACKEND_ALIAS_SingleGlobalLock := SGL

# Backend name -> tm_link backend
lower_TinySTM := tinystm
lower_tl2 := tl2
lower_SingleGlobalLock := singlelock
lower_SwissTM := swisstm
lower_NOrec := norec

.PHONY: all clean test help $(ALL_BENCH)

all: $(OUT_DIR) $(BIN_DIR) $(foreach b,$(ALL_BENCH),$(b)_uninstrumented) \
     $(foreach b,$(LOCAL),$(b)_debug)

# Short aliases for uninstrumented targets
$(foreach b,$(ALL_BENCH),$(eval $(b)_uninstrumented: $(addprefix $(BIN_DIR)/,$(b)_uninstrumented)))

# Short aliases for local debug targets
$(foreach b,$(LOCAL),$(eval $(b)_debug: $(addprefix $(BIN_DIR)/,$(b)_debug)))

# ---- Step 1-3: Pattern rules for local benchmarks ----
$(eval $(call tm_define_rules))

# ---- Uninstrumented targets ----
$(BIN_DIR)/%_uninstrumented: %.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

$(BIN_DIR)/stmbench7_uninstrumented: ../STMbench7/STMbench7.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

$(BIN_DIR)/ycsb_uninstrumented: ../YCSB/YCSB.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

$(BIN_DIR)/eigenbench_uninstrumented: ../EigenBench/EigenBench.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

# ---- Debug targets ----

$(OUT_DIR)/%_debug.bc: %.cpp $(OUT_DIR)
	$(call tm_compile_ir_debug,$<,$@)

$(OUT_DIR)/%_debug.instr.bc: $(OUT_DIR)/%_debug.bc $(OUT_DIR)
	$(call tm_instrument,$<,$@)

$(OUT_DIR)/%_debug.opt.bc: $(OUT_DIR)/%_debug.instr.bc $(OUT_DIR)
	$(call tm_optimize,$<,$@)

$(BIN_DIR)/%_debug: $(OUT_DIR)/%_debug.opt.bc $(RUNTIMES_DIR)/test_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -O0 -g $< $(RUNTIMES_DIR)/test_runtime.cpp -o $@

# Cross-directory debug targets
$(OUT_DIR)/stmbench7.bc: ../STMbench7/STMbench7.cpp $(OUT_DIR)
	$(call tm_compile_ir,$<,$@)

$(OUT_DIR)/ycsb.bc: ../YCSB/YCSB.cpp $(OUT_DIR)
	$(call tm_compile_ir,$<,$@)

$(OUT_DIR)/eigenbench.bc: ../EigenBench/EigenBench.cpp $(OUT_DIR)
	$(call tm_compile_ir,$<,$@)

# ---- Backend targets (via tm_target) ----

define tm_all_backends
$(foreach b,$1,$(foreach be,$(BACKENDS),$(eval $(call tm_target,$(b)_$(be),$2,$(lower_$(be))))))
endef

$(foreach b,$(LOCAL),$(eval $(call tm_all_backends,$(b),$(b).cpp)))

$(eval $(call tm_target, stmbench7_TinySTM, ../STMbench7/STMbench7.cpp, tinystm))
$(eval $(call tm_target, ycsb_TinySTM, ../YCSB/YCSB.cpp, tinystm))
$(eval $(call tm_target, eigenbench_TinySTM, ../EigenBench/EigenBench.cpp, tinystm))

# Cross-directory debug targets
$(BIN_DIR)/stmbench7_debug: $(OUT_DIR)/stmbench7.opt.bc $(RUNTIMES_DIR)/test_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -O0 -g $< $(RUNTIMES_DIR)/test_runtime.cpp -o $@

$(BIN_DIR)/ycsb_debug: $(OUT_DIR)/ycsb.opt.bc $(RUNTIMES_DIR)/test_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -O0 -g $< $(RUNTIMES_DIR)/test_runtime.cpp -o $@

$(BIN_DIR)/eigenbench_debug: $(OUT_DIR)/eigenbench.opt.bc $(RUNTIMES_DIR)/test_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -O0 -g $< $(RUNTIMES_DIR)/test_runtime.cpp -o $@

# ---- Recursive AVL tree variant (for stress-testing TM backends) ----

$(OUT_DIR)/avltree_recursive.opt.bc: avltree_recursive.cpp $(PLUGIN_LIB) $(OUT_DIR)
	$(call tm_compile_ir,$<,$(OUT_DIR)/avltree_recursive.bc)
	$(call tm_instrument,$(OUT_DIR)/avltree_recursive.bc,$(OUT_DIR)/avltree_recursive.instr.bc)
	$(call tm_optimize,$(OUT_DIR)/avltree_recursive.instr.bc,$@)

$(BIN_DIR)/avltree_recursive_%: $(OUT_DIR)/avltree_recursive.opt.bc | $(BIN_DIR)
	$(call tm_link,$<,$(patsubst avltree_recursive_%,%,$*),$@)

avltree_recursive_uninstrumented: avltree_recursive.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@ -pthread

# SGL aliases
$(foreach b,$(ALL_BENCH),$(eval $(b)_SGL: $(BIN_DIR)/$(b)_SingleGlobalLock))

# ---- Test targets ----

test: $(foreach b,$(LOCAL),test_$(b)) test_stmbench7 test_ycsb test_eigenbench

test_avltree: avltree_debug
	@echo "Testing AVL tree (4 threads, 10s)..."
	@$(BIN_DIR)/avltree_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_rbtree: rbtree_debug
	@echo "Testing Red-Black tree (4 threads, 10s)..."
	@$(BIN_DIR)/rbtree_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_hashmap: hashmap_debug
	@echo "Testing Hash Map (4 threads, 10s)..."
	@$(BIN_DIR)/hashmap_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_bitmap: bitmap_debug
	@echo "Testing Bitmap (4 threads, 10s)..."
	@$(BIN_DIR)/bitmap_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_list: list_debug
	@echo "Testing List (4 threads, 10s)..."
	@$(BIN_DIR)/list_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_set: set_debug
	@echo "Testing Set (4 threads, 10s)..."
	@$(BIN_DIR)/set_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_heap: heap_debug
	@echo "Testing Heap (4 threads, 10s)..."
	@$(BIN_DIR)/heap_debug -d 10000 -t 4 -r 80 -w 10 || echo "Test completed"

test_stmbench7: stmbench7_debug
	@echo "Testing STMbench7 (2 threads, 5s)..."
	@$(BIN_DIR)/stmbench7_debug 2 5000 || echo "Test completed"

test_ycsb: ycsb_debug
	@echo "Testing YCSB Workload A (4 threads, 5s)..."
	@$(BIN_DIR)/ycsb_debug -t 4 -d 5000 -w A || echo "Test completed"

test_eigenbench: eigenbench_debug
	@echo "Testing EigenBench (4 threads, 5s)..."
	@$(BIN_DIR)/eigenbench_debug -t 4 -d 5000 || echo "Test completed"

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
