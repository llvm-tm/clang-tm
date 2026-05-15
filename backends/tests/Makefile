# Makefile for STM backend tests
CXX = clang++
CXXFLAGS = -std=c++20 -O0 -pthread -g
LDFLAGS = -pthread
BIN_DIR = bin
RUNTIME_DIR = ../runtimes

INC_TL2 = -I../TL2 -I..
INC_TINYSTM = -I../TinySTM -I..
INC_NOREC = -I../NOrec -I..
INC_SWISS = -I../SwissTM -I..

TINYSTM_FLAVORS = wbctl wbetl wt
TINYSTM_BINS = $(addprefix bin/runtime_tinystm_,$(TINYSTM_FLAVORS))

.PHONY: all clean tinystm_all help

all: out bin tinystm_all \
	bin/test_tl2_simple bin/test_tl2_multi \
	bin/test_NOrec_counter \
	bin/runtime__st bin/runtime__mt

# Pattern rules for direct-compile tests
bin/test_tl2_%: test_tl2_%.cpp | out bin
	$(CXX) $(CXXFLAGS) $(INC_TL2) -o $@ $< $(LDFLAGS)

# Runtime stub tests (compile with backend runtime)
bin/runtime__%: test_runtime__%.cpp | out bin
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WBCTL -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

bin/test_NOrec_counter: test_runtime_NOrec_counter.cpp | out bin
	$(CXX) $(CXXFLAGS) $(INC_NOREC) -o $@ $< $(RUNTIME_DIR)/NOrec_runtime.cpp $(LDFLAGS)

# TinySTM flavor binaries — one pattern per flavor
bin/runtime_tinystm_wbctl: test_runtime_TinySTM_counter.cpp | out bin
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WBCTL -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

bin/runtime_tinystm_wbetl: test_runtime_TinySTM_counter.cpp | out bin
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WBETL -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

bin/runtime_tinystm_wt: test_runtime_TinySTM_counter.cpp | out bin
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WT -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp $(LDFLAGS)

tinystm_all: $(TINYSTM_BINS)

out:
	mkdir -p $(OUT_DIR)
bin:
	mkdir -p $(BIN_DIR)

run: $(TINYSTM_BINS)
	@for b in $(TINYSTM_BINS); do echo "=== $$b ===" && ./$$b; done

clean:
	rm -rf $(BIN_DIR) $(OUT_DIR)
