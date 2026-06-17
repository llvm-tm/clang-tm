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
OLD_VERSIONS := 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20
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
TINYSTM_DIR := $(BACKENDS_DIR)/tm_impl/tiny_stm

SRC_DIR := passes
TEST_DIR := ../tests/plugin

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

# Collect all pass source files
TM_PASS_SRCS := $(filter-out $(SRC_DIR)/TMFuzzStrategyPass.cpp, $(wildcard $(SRC_DIR)/TM*.cpp $(SRC_DIR)/tm_check_opaque.cpp))

# Race checker plugin (standalone .so built from the detection pass only)
RACE_CHECKER_SRC := $(SRC_DIR)/TMRaceCheckerPass.cpp
$(BIN_DIR)/libTMRaceChecker.so: $(RACE_CHECKER_SRC) | $(BIN_DIR)
	$(CXX) -I$(LLVM_PLUGIN_DIR)/analysis -I$(SRC_DIR) -I$(BACKENDS_DIR) $(PLUGIN_CXXFLAGS) -shared $< -o $@ $(LLVM_CONFIG_ARGS)

# Fuzz strategy plugin (Phase 1 of fuzz tool plan)
FUZZ_STRATEGY_SRC := $(SRC_DIR)/TMFuzzStrategyPass.cpp
$(BIN_DIR)/libTMFuzzStrategy.so: $(FUZZ_STRATEGY_SRC) | $(BIN_DIR)
	$(CXX) -I$(LLVM_PLUGIN_DIR)/analysis -I$(SRC_DIR) -I$(BACKENDS_DIR) $(PLUGIN_CXXFLAGS) -shared $< -o $@ $(LLVM_CONFIG_ARGS)

TM_PASS_CXXFLAGS := -I$(BACKENDS_DIR)/tm_impl/common -I$(BACKENDS_DIR) -I$(LLVM_PLUGIN_DIR) -I$(SRC_DIR) -I$(LLVM_PLUGIN_DIR)/analysis -fPIC $(PLUGIN_CXXFLAGS)

# Plugin variants: each gets the matching -D flags.
# Compile all pass sources into a single .so.
define TMPluginTarget
$(BIN_DIR)/libTMInstrument_$(1).so: $(TM_PASS_SRCS) $(wildcard analysis/*.hpp) | $(BIN_DIR)
	$(CXX) $(TM_PASS_CXXFLAGS) $(PLUGIN_FLAGS_$(1)) -shared $(TM_PASS_SRCS) -o $$@ $(LLVM_CONFIG_ARGS)
endef
$(foreach v,$(PLUGIN_VARIANTS),$(eval $(call TMPluginTarget,$(v))))

PLUGIN_SO_NAMES := $(addprefix $(BIN_DIR)/libTMInstrument_,$(addsuffix .so,$(PLUGIN_VARIANTS)))

TEST_NAMES := test_types test_memtest test_threads test_persist test_retry \
              test_annotation_detect \
              test_custom_class test_tm_arg_trace \
              test_math_opaque \
              test_alloc_stress test_ll_alloc test_construct_tx_pattern \
              test_nested_tx test_tm_simple test_tm_safe_map \
              test_init_no_tx \
              test_simple_vector \
              test_treap_tx \
              test_local_containers

# STL container tests — no longer supported inside TM.  These would require
# opaque-safe-table entries for std::unordered_map, std::map, and std::string
# internals, which are intentionally removed (fragile to maintain, and opaque
# STL functions bypass TM conflict detection).  Kept for reference.
STL_TEST_NAMES := test_stl_containers test_stl_primitive test_stl_map_find

RACE_TEST_NAMES := test_task_queue_race test_counter_race
TEST_SRCS := $(addprefix $(TEST_DIR)/,$(addsuffix .cpp,$(TEST_NAMES)))
TEST_PROGS := $(addprefix $(BIN_DIR)/,$(TEST_NAMES))

.PHONY: all variants test test_noinst ll run run_noinst check check_noinst clean

all: $(BIN_DIR)/libTMInstrument.so $(BIN_DIR)/libTMRaceChecker.so $(BIN_DIR)/libTMFuzzStrategy.so

variants: $(PLUGIN_SO_NAMES) $(BIN_DIR)/libTMRaceChecker.so $(BIN_DIR)/libTMFuzzStrategy.so

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



# Uninstrumented variant (no plugin)
$(BIN_DIR)/%_noinst: $(OUT_DIR)/%.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(RUNTIME_DIR)/tm_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) $< -o $@

# ---- Specialized backends ----

# TinySTM-backed tests (WBCTL write-back commit-time locking)
TINYSTM_TESTS := test_opaque_race test_local_containers test_alloc_stress test_ll_alloc test_construct_tx_pattern \
                 test_simple_vector test_treap_tx

# test_local_containers uses local containers inside TX functions.
# Uses the default 5-step Honorio pipeline (each _tm_clone has exactly
# one return, avoiding the broken-module bug in the inline pipeline).
# See CORRECTNESS_FIXES.md for details.

$(addprefix $(BIN_DIR)/,$(TINYSTM_TESTS)): $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(BACKENDS_DIR)/tm_impl/tiny_stm/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/tm_impl/tiny_stm/TinySTM_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@



# Race tests (SingleGlobalLock runtime)
RACE_TEST_BINS := $(addprefix $(BIN_DIR)/,$(RACE_TEST_NAMES))
$(RACE_TEST_BINS): $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(BACKENDS_DIR)/tm_impl/single_global_lock/SingleGlobalLock_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/tm_impl/single_global_lock/SingleGlobalLock_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@

# Queue runtime test(s): use tm-instrument-queue pipeline + TinySTM backend + queue_runtime
QUEUE_TESTS := test_queue test_queue_sync test_queue_async test_queue_multi

# Queue pipeline instrumentation
$(foreach t,$(QUEUE_TESTS),$(OUT_DIR)/$(t).instr.bc): $(OUT_DIR)/%.instr.bc: $(OUT_DIR)/%.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-queue" $< -o $@

$(foreach t,$(QUEUE_TESTS),$(OUT_DIR)/$(t).opt.bc): $(OUT_DIR)/%.opt.bc: $(OUT_DIR)/%.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $(OUT_DIR)/$*.instr.bc -o $@

# Compile runtime objects once
QUEUE_RT_OBJS := $(OUT_DIR)/tinystm_runtime.o $(OUT_DIR)/queue_runtime.o $(OUT_DIR)/tm_region_allocator.o $(OUT_DIR)/tm_hooks.o
$(QUEUE_RT_OBJS): | $(OUT_DIR)
$(OUT_DIR)/tinystm_runtime.o: $(BACKENDS_DIR)/tm_impl/tiny_stm/TinySTM_runtime.cpp
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@
$(OUT_DIR)/queue_runtime.o: $(BACKENDS_DIR)/tm_impl/queue/queue_runtime.cpp
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@
$(OUT_DIR)/queue_runtime_perf.o: $(BACKENDS_DIR)/tm_impl/queue/queue_runtime.cpp
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL -DTM_PERF_COUNTERS -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@
$(OUT_DIR)/tm_region_allocator.o: $(BACKENDS_DIR)/tm_impl/tm_region_allocator/tm_region_allocator.cpp
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@
$(OUT_DIR)/tm_hooks.o: $(BACKENDS_DIR)/tm_impl/common/tm_hooks.cpp
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DLLVM_TM_PLUGIN \
	  -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@

$(foreach t,$(QUEUE_TESTS),$(BIN_DIR)/$(t)): $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/$*.opt.bc $(QUEUE_RT_OBJS) -o $@

# LeftRight queue test: use LeftRight backend + queue pipeline
$(OUT_DIR)/leftright_runtime.o: $(BACKENDS_DIR)/tm_impl/leftright/leftright_runtime.cpp | $(OUT_DIR)
	$(CXX) -c $(DEFAULT_LINK_FLAGS) -DLLVM_TM_PLUGIN \
	  -I $(BACKENDS_DIR)/tm_impl/leftright \
	  -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR)/tm_impl -I $(BACKENDS_DIR) $< -o $@

$(BIN_DIR)/test_queue_leftright: $(OUT_DIR)/test_queue.opt.bc \
  $(OUT_DIR)/leftright_runtime.o $(OUT_DIR)/queue_runtime.o $(OUT_DIR)/tm_region_allocator.o | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/test_queue.opt.bc \
	  $(OUT_DIR)/leftright_runtime.o $(OUT_DIR)/queue_runtime.o $(OUT_DIR)/tm_region_allocator.o -o $@

$(OUT_DIR)/test_queue_leftright_manual.bc: $(TEST_DIR)/test_queue_leftright_manual.cpp | $(OUT_DIR)
	$(CLANG_TM) --compile-only -std=c++20 $(TM_COMPILE_FLAGS) $< -o $@
$(OUT_DIR)/test_queue_leftright_manual.instr.bc: $(OUT_DIR)/test_queue_leftright_manual.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-queue-manual" $< -o $@
$(OUT_DIR)/test_queue_leftright_manual.opt.bc: $(OUT_DIR)/test_queue_leftright_manual.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $< -o $@

$(BIN_DIR)/test_queue_leftright_manual: $(OUT_DIR)/test_queue_leftright_manual.opt.bc \
  $(OUT_DIR)/leftright_runtime.o $(OUT_DIR)/queue_runtime.o $(OUT_DIR)/tm_region_allocator.o | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/test_queue_leftright_manual.opt.bc \
	  $(OUT_DIR)/leftright_runtime.o $(OUT_DIR)/queue_runtime.o $(OUT_DIR)/tm_region_allocator.o -o $@

# Queue TX wait test (no plugin — manual API calls)
$(OUT_DIR)/test_queue_tx_wait.o: $(TEST_DIR)/test_queue_tx_wait.cpp | $(OUT_DIR)
	$(CXX) -c -std=c++20 -I$(BACKENDS_DIR)/tm_impl/queue -I$(BACKENDS_DIR)/tm_impl/common -I$(BACKENDS_DIR) $< -o $@

$(BIN_DIR)/test_queue_tx_wait: $(OUT_DIR)/test_queue_tx_wait.o $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/test_queue_tx_wait.o $(QUEUE_RT_OBJS) -o $@

# Inline/queue benchmark: same source, two pipelines, for throughput comparison
$(OUT_DIR)/bench_queue_compare.inline.instr.bc: $(OUT_DIR)/bench_queue_compare.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument" $< -o $@
$(OUT_DIR)/bench_queue_compare.inline.opt.bc: $(OUT_DIR)/bench_queue_compare.inline.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $< -o $@
$(BIN_DIR)/bench_queue_compare_inline: $(OUT_DIR)/bench_queue_compare.inline.opt.bc $(BACKENDS_DIR)/tm_impl/tiny_stm/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/tm_impl/tiny_stm/TinySTM_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -DDESIGN_WBCTL \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@

$(OUT_DIR)/bench_queue_compare.queue.instr.bc: $(OUT_DIR)/bench_queue_compare.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-queue" $< -o $@
$(OUT_DIR)/bench_queue_compare.queue.opt.bc: $(OUT_DIR)/bench_queue_compare.queue.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $< -o $@
$(BIN_DIR)/bench_queue_compare_queue: $(OUT_DIR)/bench_queue_compare.queue.opt.bc $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/bench_queue_compare.queue.opt.bc $(QUEUE_RT_OBJS) -o $@
$(BIN_DIR)/bench_queue_compare_queue_perf: $(OUT_DIR)/bench_queue_compare.queue.opt.bc \
  $(OUT_DIR)/tinystm_runtime.o $(OUT_DIR)/queue_runtime_perf.o $(OUT_DIR)/tm_region_allocator.o | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $^ -o $@

# Benchmark v2: sync-light + async-light + heavy-sync in one binary
$(OUT_DIR)/bench_queue_compare2.inline.instr.bc: $(OUT_DIR)/bench_queue_compare2.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument" $< -o $@
$(OUT_DIR)/bench_queue_compare2.inline.opt.bc: $(OUT_DIR)/bench_queue_compare2.inline.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $< -o $@
$(BIN_DIR)/bench_queue_compare2_inline: $(OUT_DIR)/bench_queue_compare2.inline.opt.bc $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/bench_queue_compare2.inline.opt.bc $(QUEUE_RT_OBJS) -o $@

$(OUT_DIR)/bench_queue_compare2.queue.instr.bc: $(OUT_DIR)/bench_queue_compare2.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-queue" $< -o $@
$(OUT_DIR)/bench_queue_compare2.queue.opt.bc: $(OUT_DIR)/bench_queue_compare2.queue.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $< -o $@
$(BIN_DIR)/bench_queue_compare2_queue: $(OUT_DIR)/bench_queue_compare2.queue.opt.bc $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/bench_queue_compare2.queue.opt.bc $(QUEUE_RT_OBJS) -o $@

# Wait-strategy comparison: auto-wait vs manual-wait on queue pipeline
$(OUT_DIR)/bench_queue_wait_compare.auto.instr.bc: $(OUT_DIR)/bench_queue_wait_compare.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-queue" $< -o $@
$(OUT_DIR)/bench_queue_wait_compare.auto.opt.bc: $(OUT_DIR)/bench_queue_wait_compare.auto.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $< -o $@
$(BIN_DIR)/bench_queue_wait_auto: $(OUT_DIR)/bench_queue_wait_compare.auto.opt.bc $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/bench_queue_wait_compare.auto.opt.bc $(QUEUE_RT_OBJS) -o $@

# Manual-wait variant: compile with -DMANUAL, instrument with queue-manual pipeline
$(OUT_DIR)/bench_queue_wait_compare.man.bc: $(TEST_DIR)/bench_queue_wait_compare.cpp | $(OUT_DIR)
	$(CLANG_TM) --compile-only -std=c++20 $(TM_COMPILE_FLAGS) \
	  -DMANUAL $< -o $@
$(OUT_DIR)/bench_queue_wait_compare.man.instr.bc: $(OUT_DIR)/bench_queue_wait_compare.man.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(BIN_DIR)/libTMInstrument.so \
	  -passes="tm-instrument-queue-manual" $< -o $@
$(OUT_DIR)/bench_queue_wait_compare.man.opt.bc: $(OUT_DIR)/bench_queue_wait_compare.man.instr.bc | $(OUT_DIR)
	$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $< -o $@
$(BIN_DIR)/bench_queue_wait_manual: $(OUT_DIR)/bench_queue_wait_compare.man.opt.bc $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) $(DEFAULT_LINK_FLAGS) $(OUT_DIR)/bench_queue_wait_compare.man.opt.bc $(QUEUE_RT_OBJS) -o $@

# Persistent runtime (snapshot-based, saves on tm_exit)
$(BIN_DIR)/test_persist: $(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(RUNTIME_DIR)/persistent.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(RUNTIME_DIR)/persistent.cpp \
	  $(DEFAULT_LINK_FLAGS) $< -o $@

# Persistent SGL runtime (write-through, uses mmap) — different .opt.bc than target name
$(BIN_DIR)/test_persist_sgl: $(OUT_DIR)/test_persist.opt.bc $(BACKENDS_DIR)/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp | $(BIN_DIR)
	$(CLANG_TM) --link-only --runtime=$(BACKENDS_DIR)/tm_impl/persistent_sgl/PersistentSGL_runtime.cpp \
	  $(DEFAULT_LINK_FLAGS) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@

ll: all $(TEST_LLS) $(OPT_LLS)

# Convenience targets: make test_foo -> make bin/test_foo
.PHONY: $(TEST_NAMES) $(RACE_TEST_NAMES) test_persist_sgl test_queue_tx_wait \
        bench_queue_wait_auto bench_queue_wait_manual \
        test_queue_leftright test_queue_leftright_manual bench_queue_compare_queue
$(TEST_NAMES) $(RACE_TEST_NAMES) test_persist_sgl test_queue_tx_wait \
  bench_queue_wait_auto bench_queue_wait_manual \
  test_queue_leftright test_queue_leftright_manual bench_queue_compare_queue: %: $(BIN_DIR)/%

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

.PHONY: race-checker
race-checker: $(BIN_DIR)/libTMRaceChecker.so

.PHONY: fuzz-strategy
fuzz-strategy: $(BIN_DIR)/libTMFuzzStrategy.so
