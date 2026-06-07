CXX ?= clang++
CXXFLAGS = -std=c++20 -O0 -g -pthread
INC = -I../../ -I../../backends/tm_impl/common -I../../backends/tm_impl -I../../backends/tm_impl/tiny_stm
LDFLAGS = -pthread

RUNTIME_DIR = ../../backends/tm_impl
RUNTIME_SRC = $(RUNTIME_DIR)/tiny_stm/TinySTM_runtime.cpp $(RUNTIME_DIR)/tm_region_allocator/tm_region_allocator.cpp
RUNTIME_FLAGS = -DDESIGN_WBCTL -DTM_BACKEND_TINYSTM

SRCS = test_ds.cpp test_tx.cpp
BINS = $(SRCS:.cpp=)

.PHONY: all clean run

all: $(BINS)

test_ds: test_ds.cpp $(RUNTIME_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(LDFLAGS)

test_tx: test_tx.cpp $(RUNTIME_SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(RUNTIME_FLAGS) -o $@ $< $(RUNTIME_SRC) $(LDFLAGS)

run: all
	@for b in $(BINS); do \
		echo "=== $$b ==="; \
		./$$b 2>&1 || true; \
		echo; \
	done

clean:
	rm -f $(BINS)
