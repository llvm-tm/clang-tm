# YCSB Benchmark Makefile
# Builds YCSB benchmark with different STM runtimes

CXX := clang++
CXXFLAGS := -std=c++17 -O1 -pthread

PLUGIN := ../../llvm_tm_plugin/bin/libTMInstrument.so
PLUGIN_PASS := -load-pass-plugin=$(PLUGIN) -passes="tm-instrument"
RUNTIMES_DIR := ../../backends/runtimes
TL2_DIR := ../../backends/TL2
TINYSTM_DIR := ../../backends/TinySTM

SRC := YCSB.cpp

.PHONY: all clean

all: out bin ycsb_uninstrumented ycsb_singlelock ycsb_tl2 ycsb_tinystm

out:
	mkdir -p out

bin:
	mkdir -p bin

ycsb_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/ycsb_uninstrumented

ycsb_singlelock: out bin
	clang -c -emit-llvm $(SRC) -o out/ycsb.bc
	opt $(PLUGIN_PASS) out/ycsb.bc -o out/ycsb_singlelock.opt.bc
	$(CXX) $(CXXFLAGS) out/ycsb_singlelock.opt.bc $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp -o bin/ycsb_singlelock

ycsb_tl2: out bin
	clang -c -emit-llvm $(SRC) -o out/ycsb.bc
	opt $(PLUGIN_PASS) out/ycsb.bc -o out/ycsb_tl2.opt.bc
	$(CXX) $(CXXFLAGS) out/ycsb_tl2.opt.bc $(RUNTIMES_DIR)/TL2_runtime.cpp -o bin/ycsb_tl2

ycsb_tinystm: out bin
	clang -c -emit-llvm $(SRC) -o out/ycsb.bc
	opt $(PLUGIN_PASS) out/ycsb.bc -o out/ycsb_tinystm.opt.bc
	$(CXX) $(CXXFLAGS) -DDESIGN_WBCTL out/ycsb_tinystm.opt.bc \
	  $(RUNTIMES_DIR)/TinySTM_runtime.cpp -I $(TINYSTM_DIR) -I ../../backends -o bin/ycsb_tinystm

clean:
	rm -rf out bin