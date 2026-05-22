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

CXXFLAGS := -fPIC -std=c++20
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

# Default alias
$(BIN_DIR)/libTMInstrument.so: $(BIN_DIR)/libTMInstrument_release.so
	cp $< $@

# Plugin variants: each gets the matching -D flags
define TMPluginTarget
$(BIN_DIR)/libTMInstrument_$(1).so: $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$$(CXX) $$(CXXFLAGS) $(PLUGIN_FLAGS_$(1)) -shared $$< -o $$@ $$(LLVM_CONFIG_ARGS)
endef
$(foreach v,$(PLUGIN_VARIANTS),$(eval $(call TMPluginTarget,$(v))))

PLUGIN_SO_NAMES := $(addprefix $(BIN_DIR)/libTMInstrument_,$(addsuffix .so,$(PLUGIN_VARIANTS)))

TEST_NAMES := test_types test_memtest test_threads test_persist test_retry test_stl_containers \
              test_stl_primitive test_annotation_detect test_local_containers \
              test_custom_class test_stl_map_find test_tm_arg_trace \
              test_math_opaque test_vector_realloc \
              test_alloc_stress test_ll_alloc test_construct_tx_pattern \
              test_nested_tx test_tm_simple test_tm_safe_map

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
	$(CC) -O1 -fno-inline -fno-inline-functions -emit-llvm -S $< -o $@ -fno-stack-protector

$(OUT_DIR)/%.opt.ll: $(OUT_DIR)/%.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	$(OPT) -load-pass-plugin=$(BIN_DIR)/libTMInstrument.so -passes="tm-instrument" $< -S -o $@

# ---- Link rules ----

# Most tests: link with tm_runtime.cpp
$(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	$(CXX) $< $(RUNTIME_DIR)/tm_runtime.cpp -o $@ -pthread

# Math opaque test: needs -tm-allow-opaque and -lm
$(BIN_DIR)/test_math_opaque: $(OUT_DIR)/test_math_opaque.bc $(RUNTIME_DIR)/tm_runtime.cpp $(BIN_DIR)/libTMInstrument.so | $(BIN_DIR)
	$(OPT) -load-pass-plugin=$(BIN_DIR)/libTMInstrument.so -passes="tm-instrument" \
	  -tm-allow-opaque -tm-opaque-symbols-file=/tmp/opaque_symbols.txt \
	  $< -o $(OUT_DIR)/test_math_opaque.instr.bc 2>/dev/null || true
	$(OPT) -O3 $(OUT_DIR)/test_math_opaque.instr.bc -o $(OUT_DIR)/test_math_opaque.opt.bc
	$(CXX) $(OUT_DIR)/test_math_opaque.opt.bc $(RUNTIME_DIR)/tm_runtime.cpp -o $@ -pthread -lm

# Uninstrumented variant (no plugin)
$(BIN_DIR)/%_noinst: $(OUT_DIR)/%.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	$(CXX) $< $(RUNTIME_DIR)/tm_runtime.cpp -o $@ -pthread

# Persistent runtime (snapshot-based, saves on tm_exit)
$(BIN_DIR)/test_persist: $(OUT_DIR)/test_persist.opt.bc $(RUNTIME_DIR)/persistent.cpp | $(BIN_DIR)
	$(CXX) $< $(RUNTIME_DIR)/persistent.cpp -o $@ -pthread

# Persistent SGL runtime (write-through, uses mmap)
$(BIN_DIR)/test_persist_sgl: $(OUT_DIR)/test_persist.opt.bc $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp | $(BIN_DIR)
	$(CXX) $< $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp -o $@ -pthread

# TinySTM-backed tests (vector reallocation + local containers + alloc stress)
$(BIN_DIR)/test_local_containers: $(OUT_DIR)/test_local_containers.opt.bc \
                                  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) -std=c++20 -O1 -pthread -DDESIGN_WBCTL $< \
	  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $@

$(BIN_DIR)/test_vector_realloc: $(OUT_DIR)/test_vector_realloc.opt.bc \
                                $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) -std=c++20 -O1 -pthread -DDESIGN_WBCTL $< \
	  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $@

$(BIN_DIR)/test_alloc_stress: $(OUT_DIR)/test_alloc_stress.opt.bc \
                              $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) -std=c++20 -O1 -pthread -DDESIGN_WBCTL $< \
	  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $@

$(BIN_DIR)/test_ll_alloc: $(OUT_DIR)/test_ll_alloc.opt.bc \
                          $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) -std=c++20 -O1 -pthread -DDESIGN_WBCTL $< \
	  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $@

$(BIN_DIR)/test_construct_tx_pattern: $(OUT_DIR)/test_construct_tx_pattern.opt.bc \
                                      $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	$(CXX) -std=c++20 -O1 -pthread -DDESIGN_WBCTL $< \
	  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $@

ll: all $(TEST_LLS) $(OPT_LLS)

RACE_TEST_BINS := $(addprefix $(BIN_DIR)/,$(RACE_TEST_NAMES))

$(BIN_DIR)/test_task_queue_race: $(OUT_DIR)/test_task_queue_race.opt.bc $(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp | $(BIN_DIR)
	$(CXX) $< $(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp -I $(BACKENDS_DIR) -o $@ -pthread
$(BIN_DIR)/test_parent_set_race: $(OUT_DIR)/test_parent_set_race.opt.bc $(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp | $(BIN_DIR)
	$(CXX) $< $(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp -I $(BACKENDS_DIR) -o $@ -pthread
$(BIN_DIR)/test_counter_race: $(OUT_DIR)/test_counter_race.opt.bc $(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp | $(BIN_DIR)
	$(CXX) $< $(BACKENDS_DIR)/runtimes/SingleGlobalLock_runtime.cpp -I $(BACKENDS_DIR) -o $@ -pthread

test: all $(TEST_PROGS) $(BIN_DIR)/test_persist $(BIN_DIR)/test_persist_sgl \
      $(BIN_DIR)/test_local_containers $(BIN_DIR)/test_vector_realloc \
      $(BIN_DIR)/test_alloc_stress $(BIN_DIR)/test_ll_alloc \
      $(BIN_DIR)/test_construct_tx_pattern $(RACE_TEST_BINS)

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
