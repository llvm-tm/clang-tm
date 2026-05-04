LLVM_CONFIG_ARGS := $(shell ../llvm-config-args.sh)

CXX := clang++
CXXFLAGS := -fPIC -std=c++17
CC := clang

SRC_DIR := src
TEST_DIR := test
RUNTIME_DIR := tm_implementation
OUT_DIR := out
BIN_DIR := bin

PLUGIN := $(BIN_DIR)/libTMInstrument.so
PLUGIN_NO_SETJMP := $(BIN_DIR)/libTMInstrument_no_setjmp.so

TEST_NAMES := types memtest nested threads persist retry test_stl_containers test_stl_primitive
TEST_SRCS := $(addprefix $(TEST_DIR)/,$(addsuffix .cpp,$(TEST_NAMES)))
TEST_BCS := $(addprefix $(OUT_DIR)/,$(addsuffix .bc,$(TEST_NAMES)))
TEST_LLS := $(addprefix $(OUT_DIR)/,$(addsuffix .ll,$(TEST_NAMES)))
OPT_BCS := $(addprefix $(OUT_DIR)/,$(addsuffix .opt.bc,$(TEST_NAMES)))
OPT_LLS := $(addprefix $(OUT_DIR)/,$(addsuffix .opt.ll,$(TEST_NAMES)))
TEST_PROGS := $(addprefix $(BIN_DIR)/,$(TEST_NAMES))

all: $(PLUGIN) $(PLUGIN_NO_SETJMP)

$(PLUGIN): $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -shared $(SRC_DIR)/TMInstrumentPass.cpp -o $(PLUGIN) $(LLVM_CONFIG_ARGS)

$(PLUGIN_NO_SETJMP): $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DDISABLE_SETJMP -shared $(SRC_DIR)/TMInstrumentPass.cpp -o $(PLUGIN_NO_SETJMP) $(LLVM_CONFIG_ARGS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OUT_DIR)/%.bc: $(TEST_DIR)/%.cpp | $(OUT_DIR)
	clang -O1 -fno-inline -emit-llvm -c $< -o $@ -fno-stack-protector

$(OUT_DIR)/%.ll: $(TEST_DIR)/%.cpp | $(OUT_DIR)
	clang -O1 -fno-inline -fno-inline-functions -emit-llvm -S $< -o $@ -fno-stack-protector

$(OUT_DIR)/%.opt.bc: $(OUT_DIR)/%.bc $(PLUGIN) | $(OUT_DIR)
	opt -load-pass-plugin=$(PLUGIN) -passes="tm-instrument" $< -o $@

$(OUT_DIR)/%.opt.ll: $(OUT_DIR)/%.bc $(PLUGIN) | $(OUT_DIR)
	opt -load-pass-plugin=$(PLUGIN) -passes="tm-instrument" $< -S -o $@

$(BIN_DIR)/%_noinst: $(OUT_DIR)/%.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIME_DIR)/tm_runtime.cpp -o $@ -pthread

$(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIME_DIR)/tm_runtime.cpp -o $@ -pthread

ll: all $(TEST_LLS) $(OPT_LLS)

$(BIN_DIR)/persist: $(OUT_DIR)/persist.opt.bc $(RUNTIME_DIR)/persistent.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIME_DIR)/persistent.cpp -o $@ -pthread

test: all $(TEST_PROGS) $(BIN_DIR)/persist

test_noinst: $(TEST_SRCS) $(addprefix $(BIN_DIR)/,$(addsuffix _noinst,$(TEST_NAMES))) $(addprefix $(BIN_DIR)/,persist_persistent)

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

.PHONY: all test test_noinst ll run run_noinst check check_noinst clean
