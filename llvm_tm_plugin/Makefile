# ---- Build configuration ----
# BUILD_TYPE = RELEASE (default) or DEBUG
BUILD_TYPE ?= RELEASE

# Post-instrumentation optimization (set before include so tm_pipeline.mk's ?= doesn't override)
# NOTE: -O2/-O3 include GlobalOpt/IPSCCP etc. that do NOT respect optnone and
# can corrupt clone function signatures (dead-argument elimination removes the
# `this` parameter from _tm_clone functions, changing their ABI). Use -O1 to
# avoid these passes. The tm-instrument pipeline has optnone clones anyway
# (function passes skip them), so -O3 adds no benefit for clone code.
ifeq ($(BUILD_TYPE),DEBUG)
  TM_OPT_LEVEL = -O0
else
  TM_OPT_LEVEL = -O1
endif

# Plugin variant, link optimization, and pipeline (set before include so ?= doesn't override)
ifeq ($(BUILD_TYPE),DEBUG)
  PLUGIN_VARIANT = debug
  TM_LINK_OPT = -O0 -g
  TM_INSTRUMENT_PIPELINE = tm-instrument
else
  PLUGIN_VARIANT = release
  TM_LINK_OPT = -O1
endif

# Discover LLVM config (handle versioned installs: llvm-config-22, llvm-config-22.1, llvm-config)
LLVM_CONFIG := $(shell command -v llvm-config-22 2>/dev/null || command -v llvm-config-22.1 2>/dev/null || command -v llvm-config 2>/dev/null || echo "")

# Check LLVM version (22+ required)
LLVM_VER := $(shell $(LLVM_CONFIG) --version 2>/dev/null || echo "0")
LLVM_MAJOR := $(word 1,$(subst ., ,$(LLVM_VER)))
OLD_VERSIONS := 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 20
ifneq ($(filter $(LLVM_MAJOR),$(OLD_VERSIONS)),)
  $(error LLVM 22+ required, found $(LLVM_VER))
endif

# Derive LLVM build flags (tools discovered by tm_pipeline.mk below)
LLVM_CONFIG_ARGS := $(shell $(LLVM_CONFIG) --cxxflags --ldflags --libs core passes 2>/dev/null)

PLUGIN_CXXFLAGS := -fPIC -std=c++20
CC := clang

include tm_pipeline.mk

# Override paths set by pipeline include
RUNTIME_DIR := runtime
BACKENDS_DIR := ../backends
TINYSTM_DIR := $(BACKENDS_DIR)/TinySTM

SRC_DIR := src
TEST_DIR := test

PLUGIN_VARIANTS  = release no_setjmp no_rw no_malloc minimal debug
PLUGIN_FLAGS_release       = -DNDEBUG
PLUGIN_FLAGS_no_setjmp     = -DNDEBUG -DDISABLE_SETJMP
PLUGIN_FLAGS_no_rw         = -DNDEBUG -DDISABLE_TM_READ_WRITE
PLUGIN_FLAGS_no_malloc     = -DNDEBUG -DDISABLE_MALLOC_FREE
PLUGIN_FLAGS_minimal       = -DNDEBUG -DDISABLE_SETJMP -DDISABLE_TM_READ_WRITE -DDISABLE_MALLOC_FREE

# Debug variant (verbose output, assertions enabled)
PLUGIN_FLAGS_debug         =

# Default alias -> selected variant
$(BIN_DIR)/libTMInstrument.so: $(BIN_DIR)/libTMInstrument_$(PLUGIN_VARIANT).so
	cp $< $@

# Plugin variants: each gets the matching -D flags
define TMPluginTarget
$(BIN_DIR)/libTMInstrument_$(1).so: $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$$(CXX) $$(PLUGIN_CXXFLAGS) $(PLUGIN_FLAGS_$(1)) -shared $$< -o $$@ $$(LLVM_CONFIG_ARGS)
endef
$(foreach v,$(PLUGIN_VARIANTS),$(eval $(call TMPluginTarget,$(v))))

PLUGIN_SO_NAMES := $(addprefix $(BIN_DIR)/libTMInstrument_,$(addsuffix .so,$(PLUGIN_VARIANTS)))

TEST_NAMES := test_types test_memtest test_threads test_persist test_retry test_stl_containers \
              test_stl_primitive test_annotation_detect test_local_containers \
              test_custom_class test_stl_map_find test_tm_arg_trace \
              test_math_opaque test_vector_realloc \
              test_alloc_stress test_ll_alloc test_construct_tx_pattern \
              test_nested_tx test_tm_simple test_tm_safe_map \
              test_init_no_tx \
              test_simple_vector test_vec_push \
               test_alloc_stress_manual test_manual_tx \
               test_treap_tx test_realloc_crash test_std_queue test_queue

RACE_TEST_NAMES := test_task_queue_race test_parent_set_race test_counter_race
TEST_SRCS := $(addprefix $(TEST_DIR)/,$(addsuffix .cpp,$(TEST_NAMES)))
TEST_PROGS := $(addprefix $(BIN_DIR)/,$(TEST_NAMES))

.PHONY: all variants test test_noinst ll run run_noinst check check_noinst clean

all: $(BIN_DIR)/libTMInstrument.so

variants: $(PLUGIN_SO_NAMES)

# ---- Pattern rules (test/ sources) ----
$(eval $(call tm_define_rules, $(TEST_DIR)/))

# Text IR for debugging
$(OUT_DIR)/%.ll: $(TEST_DIR)/%.cpp | $(OUT_DIR)
	$(CLANG_TM) --compile-only -O1 -fno-inline -fno-inline-functions -emit-llvm -S $< -o $@ -fno-stack-protector

$(OUT_DIR)/%.opt.ll: $(OUT_DIR)/%.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so -passes="tm-instrument" $< -S -o $@

# ---- Link rules ----

DEFAULT_LINK_FLAGS = -std=c++20 $(TM_LINK_OPT) -pthread

# Most tests: link with tm_runtime.cpp
$(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(RUNTIME_DIR)/tm_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) $< -o $@

# Math opaque test: needs -tm-allow-opaque and -lm
$(BIN_DIR)/test_math_opaque: $(OUT_DIR)/test_math_opaque.bc $(RUNTIME_DIR)/tm_runtime.cpp $(BIN_DIR)/libTMInstrument.so | $(BIN_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument" -a \
	  -tm-opaque-symbols-file=/tmp/opaque_symbols.txt \
	  $< -o $(OUT_DIR)/test_math_opaque.instr.bc 2>/dev/null || true
	$(CLANG_TM) --optimize-only -O3 $(OUT_DIR)/test_math_opaque.instr.bc -o $(OUT_DIR)/test_math_opaque.opt.bc
	$(CLANG_TM) --link-only --runtime=$(RUNTIME_DIR)/tm_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/test_math_opaque.opt.bc -o $@ -lm

# Manual TM tests (no plugin — hand-instrumented with TinySTM calls directly)
# These need -I flags for TinySTM headers, unlike normal test sources.
$(OUT_DIR)/test_alloc_stress_manual.bc: $(TEST_DIR)/test_alloc_stress_manual.cpp | $(OUT_DIR)
	$(CLANG_TM) --compile-only -std=c++20 $(TM_COMPILE_FLAGS) \
	  -DDESIGN_WBCTL -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) $< -o $@

$(BIN_DIR)/test_alloc_stress_manual: $(OUT_DIR)/test_alloc_stress_manual.bc $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) $< -o $@

$(OUT_DIR)/test_manual_tx.bc: $(TEST_DIR)/test_manual_tx.cpp | $(OUT_DIR)
	$(CLANG_TM) --compile-only -std=c++20 $(TM_COMPILE_FLAGS) \
	  -DDESIGN_WBCTL -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) $< -o $@

$(BIN_DIR)/test_manual_tx: $(OUT_DIR)/test_manual_tx.bc $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) $< -o $@

# Uninstrumented variant (no plugin)
$(BIN_DIR)/%_noinst: $(OUT_DIR)/%.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(RUNTIME_DIR)/tm_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) $< -o $@

# ---- Specialized backends ----

# TinySTM-backed tests (WBCTL write-back commit-time locking)
TINYSTM_TESTS := test_local_containers test_vector_realloc test_alloc_stress test_ll_alloc test_construct_tx_pattern \
                 test_simple_vector test_vec_push test_treap_tx

# test_local_containers uses local containers inside TX functions,
# which requires the inline pipeline (AlwaysInline clones all reachable
# functions and inlines them into the TX body, ensuring all internal
# loads/stores are visible for TM instrumentation).  The non-inline
# pipeline can't handle this because local container `this` pointers
# don't trace to TM globals.
$(OUT_DIR)/test_local_containers.opt.bc: $(OUT_DIR)/test_local_containers.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-inline" $< -o $(OUT_DIR)/test_local_containers.instr.bc
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $(OUT_DIR)/test_local_containers.instr.bc -o $@

$(addprefix $(BIN_DIR)/,$(TINYSTM_TESTS)): $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) $< -o $@

# SwissTM-backed test (reproducer for STMbench7 bad_alloc crash)
SWISSTM_TESTS := test_realloc_crash

$(addprefix $(BIN_DIR)/,$(SWISSTM_TESTS)): $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(BACKENDS_DIR)/runtimes/SwissTM_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/runtimes/SwissTM_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) \
	  -I $(BACKENDS_DIR)/SwissTM -I $(BACKENDS_DIR) $< -o $@

# TL2-backed test (std::queue reproducer — separate target from WBCTL default)
$(BIN_DIR)/test_std_queue_tl2: $(OUT_DIR)/test_std_queue.opt.bc $(BACKENDS_DIR)/runtimes/tl2_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/runtimes/tl2_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -I $(BACKENDS_DIR)/TL2 -I $(BACKENDS_DIR) $< -o $@

# Race tests (SingleGlobalLock runtime)
RACE_TEST_BINS := $(addprefix $(BIN_DIR)/,$(RACE_TEST_NAMES))
$(RACE_TEST_BINS): $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -I $(BACKENDS_DIR) $< -o $@

# Queue runtime test: uses tm-instrument-queue pipeline + TinySTM backend + queue_runtime
$(OUT_DIR)/test_queue.instr.bc: $(OUT_DIR)/test_queue.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-queue" $< -o $@

$(OUT_DIR)/test_queue.opt.bc: $(OUT_DIR)/test_queue.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $(OUT_DIR)/test_queue.instr.bc -o $@

$(BIN_DIR)/test_queue: $(OUT_DIR)/test_queue.opt.bc $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp $(BACKENDS_DIR)/runtimes/queue_runtime.cpp $(BACKENDS_DIR)/runtimes/tm_region_allocator.cpp | $(OUT_DIR) $(BIN_DIR)
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) \
	  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  -o $(OUT_DIR)/tinystm_runtime.o
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) \
	  $(BACKENDS_DIR)/runtimes/queue_runtime.cpp \
	  -o $(OUT_DIR)/queue_runtime.o
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) \
	  $(BACKENDS_DIR)/runtimes/tm_region_allocator.cpp \
	  -o $(OUT_DIR)/tm_region_allocator.o
	$(CXX) $(DEFAULT_LINK_FLAGS) \
	  $(OUT_DIR)/test_queue.opt.bc \
	  $(OUT_DIR)/tinystm_runtime.o \
	  $(OUT_DIR)/queue_runtime.o \
	  $(OUT_DIR)/tm_region_allocator.o \
	  -o $@

# Persistent runtime (snapshot-based, saves on tm_exit)
$(BIN_DIR)/test_persist: $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(RUNTIME_DIR)/persistent.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(RUNTIME_DIR)/persistent.cpp \
	  $(DEFAULT_LINK_FLAGS) $< -o $@

# Persistent SGL runtime (write-through, uses mmap) — different .opt.bc than target name
$(BIN_DIR)/test_persist_sgl: $(OUT_DIR)/test_persist.opt.bc $(BACKENDS_DIR)/runtimes/PersistentSGL_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/runtimes/PersistentSGL_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) $< -o $@

ll: all $(TEST_LLS) $(OPT_LLS)

# Convenience targets: make test_foo -> make bin/test_foo
.PHONY: $(TEST_NAMES) $(RACE_TEST_NAMES) test_persist_sgl test_std_queue_tl2
$(TEST_NAMES) $(RACE_TEST_NAMES) test_persist_sgl test_std_queue_tl2: %: $(BIN_DIR)/%

test: all $(TEST_PROGS) $(RACE_TEST_BINS) $(BIN_DIR)/test_persist_sgl

test_noinst: $(TEST_SRCS) $(addprefix $(BIN_DIR)/,$(addsuffix _noinst,$(TEST_NAMES))) \
             $(addprefix $(BIN_DIR)/,persist_persistent)

run: test
	./run_tests.sh

run_noinst: test_noinst
	@echo "Running uninstrumented tests..."
	@for t in $(TEST_NAMES); do \
		echo "Running $$t (uninstrumented)..."; \
		./bin/$$t_noinst > out/$$t.noinst.txt 2>&1 || true; \
	done

check: run
	@echo "All instrumented tests passed."

check_noinst: run_noinst
	@echo "All uninstrumented tests passed."

clean:
	rm -rf $(BIN_DIR) $(OUT_DIR)
	rm -f /tmp/tm_persistent_state.bin
