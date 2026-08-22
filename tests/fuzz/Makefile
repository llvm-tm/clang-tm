CXX ?= clang++
CXXFLAGS = -std=c++20 -O1 -g -fsanitize=fuzzer -fno-omit-frame-pointer -pthread
INC = -I../../ -I../../expli_instr/cpp -I../../expli_instr/cpp/include -I../../backends/tm_impl/common -I../../backends/tm_impl/tiny_stm
LDFLAGS = -pthread

RUNTIME_DIR = ../../backends/tm_impl
RUNTIME_SRC = $(RUNTIME_DIR)/tiny_stm/TinySTM_runtime.cpp $(RUNTIME_DIR)/tm_region_allocator/tm_region_allocator.cpp
HOOKS_SRC = $(RUNTIME_DIR)/common/tm_hooks.cpp
RUNTIME_FLAGS = -DDESIGN_WBCTL -DTM_BACKEND_TINYSTM -DTM_REGION_SIZE=536870912

BINS = test_tx_fuzzer test_ds_fuzzer

.PHONY: all clean

all: $(BINS)

test_tx_fuzzer: test_tx_fuzzer.cpp $(RUNTIME_SRC) $(HOOKS_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(HOOKS_SRC) $(LDFLAGS)

test_ds_fuzzer: test_ds_fuzzer.cpp $(RUNTIME_SRC) $(HOOKS_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(HOOKS_SRC) $(LDFLAGS)

clean:
	rm -f $(BINS)
