LLVM_CONFIG_ARGS := $(shell ../llvm-config-args.sh)

CXX := clang++
CXXFLAGS := -fPIC -std=c++17

SRC_DIR := src
TEST_DIR := test
RUNTIME_DIR := tm_implementation
OUT_DIR := out
BIN_DIR := bin

PLUGIN := $(BIN_DIR)/libTMInstrument.so
PLUGIN_NO_SETJMP := $(BIN_DIR)/libTMInstrument_no_setjmp.so

TEST_NAMES := types memtest nested
TEST_SRCS := $(addprefix $(TEST_DIR)/,$(addsuffix .cpp,$(TEST_NAMES)))
TEST_BCS := $(addprefix $(OUT_DIR)/,$(addsuffix .bc,$(TEST_NAMES)))
OPT_BCS := $(addprefix $(OUT_DIR)/,$(addsuffix .opt.bc,$(TEST_NAMES)))
TEST_PROGS := $(addprefix $(BIN_DIR)/,$(TEST_NAMES))

all: $(PLUGIN) $(PLUGIN_NO_SETJMP)

$(PLUGIN): $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DINCLUDE_NESTED_COUNTER -DINCLUDE_SETJMP -shared $(SRC_DIR)/TMInstrumentPass.cpp -o $(PLUGIN) $(LLVM_CONFIG_ARGS)

$(PLUGIN_NO_SETJMP): $(SRC_DIR)/TMInstrumentPass.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DDISABLE_STM_SETJMP -DINCLUDE_NESTED_COUNTER -shared $(SRC_DIR)/TMInstrumentPass.cpp -o $(PLUGIN_NO_SETJMP) $(LLVM_CONFIG_ARGS)

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OUT_DIR)/%.bc: $(TEST_DIR)/%.cpp | $(OUT_DIR)
	clang -O1 -fno-inline -fno-inline-functions -emit-llvm -c $< -o $@ -fno-stack-protector

$(OUT_DIR)/%.opt.bc: $(OUT_DIR)/%.bc $(PLUGIN) | $(OUT_DIR)
	opt -load-pass-plugin=$(PLUGIN) -passes="tm-instrument" $< -o $@

$(BIN_DIR)/%: $(OUT_DIR)/%.opt.bc $(RUNTIME_DIR)/tm_runtime.cpp | $(BIN_DIR)
	clang++ $< $(RUNTIME_DIR)/tm_runtime.cpp -o $@

test: all $(TEST_PROGS)

run: test
	./run_tests.sh

check: run

clean:
	rm -rf $(BIN_DIR) $(OUT_DIR)

.PHONY: all test run check clean
