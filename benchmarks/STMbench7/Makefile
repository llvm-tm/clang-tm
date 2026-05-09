# STMbench7 Benchmark Makefile
# Builds STMbench7 benchmark with different STM runtimes

CXX := clang++
CXXFLAGS := -std=c++17 -O1 -pthread

PLUGIN := ../../llvm_tm_plugin/bin/libTMInstrument.so
PLUGIN_PASS := -load-pass-plugin=$(PLUGIN) -passes="tm-instrument"
RUNTIMES_DIR := ../../backends/runtimes
TL2_DIR := ../../backends/TL2
TINYSTM_DIR := ../../backends/TinySTM

SRC := STMbench7.cpp

.PHONY: all clean

all: out bin stmbench_uninstrumented stmbench_singlelock stmbench_tl2 stmbench_tinystm

out:
	mkdir -p out

bin:
	mkdir -p bin

stmbench_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/stmbench_uninstrumented

stmbench_singlelock: out bin
	clang -c -emit-llvm $(SRC) -o out/stmbench7.bc
	opt $(PLUGIN_PASS) out/stmbench7.bc -o out/stmbench_singlelock.opt.bc
	$(CXX) $(CXXFLAGS) out/stmbench_singlelock.opt.bc $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp -o bin/stmbench_singlelock

stmbench_tl2: out bin
	clang -c -emit-llvm $(SRC) -o out/stmbench7.bc
	opt $(PLUGIN_PASS) out/stmbench7.bc -o out/stmbench_tl2.opt.bc
	$(CXX) $(CXXFLAGS) out/stmbench_tl2.opt.bc $(RUNTIMES_DIR)/TL2_runtime.cpp -o bin/stmbench_tl2

stmbench_tinystm: out bin
	clang -c -emit-llvm $(SRC) -o out/stmbench7.bc
	opt $(PLUGIN_PASS) out/stmbench7.bc -o out/stmbench_tinystm.opt.bc
	$(CXX) $(CXXFLAGS) -DDESIGN_WBCTL out/stmbench_tinystm.opt.bc \
	  $(RUNTIMES_DIR)/TinySTM_runtime.cpp -I $(TINYSTM_DIR) -I ../../backends -o bin/stmbench_tinystm

clean:
	rm -rf out bin