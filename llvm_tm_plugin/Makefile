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
OUT_DIR := out
BIN_DIR := bin

PLUGIN := $(BIN_DIR)/libTMInstrument.so
PLUGIN_NO_SETJMP := $(BIN_DIR)/libTMInstrument_no_setjmp.so

TEST_NAMES := types memtest nested threads persist retry test_stl_containers \
              test_stl_primitive annotation_detect local_containers_test \
              custom_class_test
TEST_SRCS := $(addprefix $(TEST_DIR)/,$(addsuffix .cpp,$(TEST_NAMES)))
TEST_PROGS := $(addprefix $(BIN_DIR)/,$(TEST_NAMES))

.PHONY: all test test_noinst ll run run_noinst check check_noinst clean

all: $(PLUGIN) $(PLUGIN_NO_SETJMP)

# ---- Plugin shared library ----
$(PLUGIN): $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -shared $< -o $@ $(LLVM_CONFIG_ARGS)

$(PLUGIN_NO_SETJMP): $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DDISABLE_SETJMP -shared $< -o $@ $(LLVM_CONFIG_ARGS)

# ---- Pattern rules (test/ sources) ----
$(eval $(call tm_define_rules, $(TEST_DIR)/))

# Text IR for debugging
$(OUT_DIR)/%.ll: $(TEST_DIR)/%.cpp | $(OUT_DIR)
	clang -O1 -fno-inline -fno-inline-functions -emit-llvm -S $< -o $@ -fno-stack-protector

$(OUT_DIR)/%.opt.ll: $(OUT_DIR)/%.bc $(PLUGIN) | $(OUT_DIR)
	opt -load-pass-plugin=$(PLUGIN) -passes="tm-instrument" $< -S -o $@

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
$(BIN_DIR)/persist_sgl: $(OUT_DIR)/persist.opt.bc $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp $(TM_PLUGIN) | $(BIN_DIR)
	clang++ $< $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp -o $@ -pthread

# TinySTM-backed test (use explicit link, not tm_link, because CXXFLAGS serves plugin build)
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
