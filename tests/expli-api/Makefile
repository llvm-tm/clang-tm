CXX ?= clang++
CXXFLAGS = -std=c++20 -O0 -g -pthread
INC = -I../../ -I../../expli_instr/cpp -I../../expli_instr/cpp/include -I../../backends/tm_impl/common -I../../backends/tm_impl -I../../backends/tm_impl/tiny_stm
FFLAGS = -fsanitize=fuzzer -fno-omit-frame-pointer
LDFLAGS = -pthread

RUNTIME_DIR = ../../backends/tm_impl
RUNTIME_SRC = $(RUNTIME_DIR)/tiny_stm/TinySTM_runtime.cpp $(RUNTIME_DIR)/tm_region_allocator/tm_region_allocator.cpp
HOOKS_SRC = ../../backends/tm_impl/common/tm_hooks.cpp
RUNTIME_FLAGS = -DDESIGN_WBCTL -DTM_BACKEND_TINYSTM -DTM_REGION_SIZE=$(TM_REGION_SIZE)

# Default region size: 512 MB suffices for all tests on macOS.
# Override: make TM_REGION_SIZE=16000000000
TM_REGION_SIZE ?= 536870912

SRCS = test_stress_ds.cpp test_phase_switch.cpp
BINS = $(SRCS:.cpp=)

.PHONY: all clean run fuzz

all: $(BINS)

test_stress_ds: test_stress_ds.cpp $(RUNTIME_SRC) $(HOOKS_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(HOOKS_SRC) $(LDFLAGS)

test_phase_switch: test_phase_switch.cpp $(RUNTIME_SRC) $(HOOKS_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(HOOKS_SRC) $(LDFLAGS)

# Fuzz targets are built from tests/fuzz/ but output here for convenience.
test_tx_fuzzer: ../fuzz/test_tx_fuzzer.cpp $(RUNTIME_SRC) $(HOOKS_SRC)
	$(CXX) $(CXXFLAGS) $(FFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(HOOKS_SRC) $(LDFLAGS)

test_ds_fuzzer: ../fuzz/test_ds_fuzzer.cpp $(RUNTIME_SRC) $(HOOKS_SRC)
	$(CXX) $(CXXFLAGS) $(FFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(HOOKS_SRC) $(LDFLAGS)

fuzz: test_tx_fuzzer test_ds_fuzzer

# Portable timeout: use gtimeout (macOS coreutils) if available
TIMEOUT_CMD := timeout
ifeq ($(shell command -v timeout 2>/dev/null),)
    ifneq ($(shell command -v gtimeout 2>/dev/null),)
        TIMEOUT_CMD := gtimeout
    endif
endif

run: all
	@for b in $(BINS); do \
		echo "=== $$b ==="; \
		$(TIMEOUT_CMD) 60 ./$$b 2>&1 || true; \
		echo; \
	done

clean:
	rm -f $(BINS) test_tx_fuzzer test_ds_fuzzer
