# TPC-C Benchmark Makefile
# Builds TPC-C benchmark with different STM runtimes

CXX := clang++
CXXFLAGS := -std=c++17 -O1 -pthread

PLUGIN := ../../llvm_tm_plugin/bin/libTMInstrument.so
PLUGIN_PASS := -load-pass-plugin=$(PLUGIN) -passes="tm-instrument"
RUNTIMES_DIR := ../../backends/runtimes
TL2_DIR := ../../backends/TL2
TINYSTM_DIR := ../../backends/TinySTM
SWISS_DIR := ../../backends/SwissTM

SRC := TPCC.cpp

.PHONY: all clean

all: out bin tpcc_uninstrumented tpcc_singlelock tpcc_tl2 tpcc_tinystm

out:
	mkdir -p out

bin:
	mkdir -p bin

tpcc_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/tpcc_uninstrumented

tpcc_singlelock: out bin
	clang -c -emit-llvm $(SRC) -o out/tpcc.bc
	opt $(PLUGIN_PASS) out/tpcc.bc -o out/tpcc_singlelock.opt.bc
	$(CXX) $(CXXFLAGS) out/tpcc_singlelock.opt.bc $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp -o bin/tpcc_singlelock

tpcc_tl2: out bin
	clang -c -emit-llvm $(SRC) -o out/tpcc.bc
	opt $(PLUGIN_PASS) out/tpcc.bc -o out/tpcc_tl2.opt.bc
	$(CXX) $(CXXFLAGS) out/tpcc_tl2.opt.bc $(RUNTIMES_DIR)/TL2_runtime.cpp -o bin/tpcc_tl2

tpcc_tinystm: out bin
	clang -c -emit-llvm $(SRC) -o out/tpcc.bc
	opt $(PLUGIN_PASS) out/tpcc.bc -o out/tpcc_tinystm.opt.bc
	$(CXX) $(CXXFLAGS) -DDESIGN_WBCTL out/tpcc_tinystm.opt.bc \
	  $(RUNTIMES_DIR)/TinySTM_runtime.cpp -I $(TINYSTM_DIR) -I ../../backends -o bin/tpcc_tinystm

clean:
	rm -rf out bin