CXX ?= clang++
CXXFLAGS = -std=c++20 -O0 -g -pthread
INC = -I../../ -I../../backends/tm_impl/common -I../../backends/tm_impl -I../../backends/tm_impl/tiny_stm -I../../expli_instr/cpp/include
LDFLAGS = -pthread

RUNTIME_DIR = ../../backends/tm_impl
RUNTIME_SRC = $(RUNTIME_DIR)/tiny_stm/TinySTM_runtime.cpp $(RUNTIME_DIR)/tm_region_allocator/tm_region_allocator.cpp
RUNTIME_FLAGS = -DDESIGN_WBCTL -DTM_BACKEND_TINYSTM -DTM_REGION_SIZE=$(TM_REGION_SIZE)

# Default region size: 512 MB suffices for all tests on macOS.
# Override: make TM_REGION_SIZE=16000000000
TM_REGION_SIZE ?= 536870912

SRCS = test_stress_ds.cpp
BINS = $(SRCS:.cpp=)

.PHONY: all clean run

all: $(BINS)

test_stress_ds: test_stress_ds.cpp $(RUNTIME_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(LDFLAGS)

run: all
	@for b in $(BINS); do \
		echo "=== $$b ==="; \
		perl -e 'alarm shift @ARGV; exec @ARGV' 30 ./$$b 2>&1 || true; \
		echo; \
	done

clean:
	rm -f $(BINS)
