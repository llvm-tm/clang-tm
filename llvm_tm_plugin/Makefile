LLVM_CONFIG_ARGS := $(shell ../llvm-config-args.sh)

CXX := clang++
CXXFLAGS := -fPIC -std=c++20
CC := clang

include tm_pipeline.mk

# Override paths set by pipeline include
RUNTIME_DIR := runtime
BACKENDS_DIR := ../backends
TINYSTM_DIR := $(BACKENDS_DIR)/TinySTM

SRC_DIR := src
TEST_DIR := test

PLUGIN_VARIANTS  = release no_setjmp no_rw no_malloc minimal
PLUGIN_FLAGS_release       =
PLUGIN_FLAGS_no_setjmp     = -DDISABLE_SETJMP
PLUGIN_FLAGS_no_rw         = -DDISABLE_TM_READ_WRITE
PLUGIN_FLAGS_no_malloc     = -DDISABLE_MALLOC_FREE
PLUGIN_FLAGS_minimal       = -DDISABLE_SETJMP -DDISABLE_TM_READ_WRITE -DDISABLE_MALLOC_FREE

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

TEST_NAMES := types memtest nested threads persist retry test_stl_containers \
              test_stl_primitive annotation_detect local_containers_test \
              custom_class_test
TEST_SRCS := $(addprefix $(TEST_DIR)/,$(addsuffix .cpp,$(TEST_NAMES)))
TEST_PROGS := $(addprefix $(BIN_DIR)/,$(TEST_NAMES))

.PHONY: all variants test test_noinst ll run run_noinst check check_noinst clean

all: $(BIN_DIR)/libTMInstrument.so

variants: $(PLUGIN_SO_NAMES)

# ---- Pattern rules (test/ sources) ----
$(eval $(call tm_define_rules, $(TEST_DIR)/))

# Text IR for debugging
$(OUT_DIR)/%.ll: $(TEST_DIR)/%.cpp | $(OUT_DIR)
	clang -O1 -fno-inline -fno-inline-functions -emit-llvm -S $< -o $@ -fno-stack-protector

$(OUT_DIR)/%.opt.ll: $(OUT_DIR)/%.bc $(BIN_DIR)/libTMInstrument.so | $(OUT_DIR)
	opt -load-pass-plugin=$(BIN_DIR)/libTMInstrument.so -passes="tm-instrument" $< -S -o $@

# ---- Link rules ----

# Most tests: link with tm_runtime.cpp
$(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIME_DIR)/tm_runtime.cpp -o $@ -pthread

# Uninstrumented variant (no plugin)
$(BIN_DIR)/%_noinst: $(OUT_DIR)/%.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIME_DIR)/tm_runtime.cpp -o $@ -pthread

# Persistent runtime (snapshot-based, saves on tm_exit)
$(BIN_DIR)/persist: $(OUT_DIR)/persist.opt.bc $(RUNTIME_DIR)/persistent.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIME_DIR)/persistent.cpp -o $@ -pthread

# Persistent SGL runtime (write-through, uses mmap)
$(BIN_DIR)/persist_sgl: $(OUT_DIR)/persist.opt.bc $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp -o $@ -pthread

# TinySTM-backed test
$(BIN_DIR)/local_containers_test: $(OUT_DIR)/local_containers_test.opt.bc \
                                  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp | $(BIN_DIR)
	clang++ -std=c++20 -O1 -pthread -DDESIGN_WBCTL $< \
	  $(BACKENDS_DIR)/runtimes/TinySTM_runtime.cpp \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR) -o $@

ll: all $(TEST_LLS) $(OPT_LLS)

test: all $(TEST_PROGS) $(BIN_DIR)/persist $(BIN_DIR)/persist_sgl $(BIN_DIR)/local_containers_test

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
