# EigenBench Benchmark Makefile
# Builds EigenBench benchmark with different STM runtimes

CXX := clang++
CXXFLAGS := -std=c++17 -O1 -pthread

PLUGIN := ../../llvm_tm_plugin/bin/libTMInstrument.so
PLUGIN_PASS := -load-pass-plugin=$(PLUGIN) -passes="tm-instrument"
RUNTIMES_DIR := ../../backends/runtimes

SRC := EigenBench.cpp

.PHONY: all clean

all: out bin eigen_uninstrumented eigen_singlelock eigen_tl2 eigen_tinystm

out:
	mkdir -p out

bin:
	mkdir -p bin

eigen_uninstrumented: bin
	$(CXX) $(CXXFLAGS) $(SRC) -o bin/eigen_uninstrumented

eigen_singlelock: out bin
	clang -c -emit-llvm $(SRC) -o out/eigen.bc
	opt $(PLUGIN_PASS) out/eigen.bc -o out/eigen_singlelock.opt.bc
	$(CXX) $(CXXFLAGS) out/eigen_singlelock.opt.bc $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp -o bin/eigen_singlelock

eigen_tl2: out bin
	clang -c -emit-llvm $(SRC) -o out/eigen.bc
	opt $(PLUGIN_PASS) out/eigen.bc -o out/eigen_tl2.opt.bc
	$(CXX) $(CXXFLAGS) out/eigen_tl2.opt.bc $(RUNTIMES_DIR)/TL2_runtime.cpp -o bin/eigen_tl2

eigen_tinystm: out bin
	clang -c -emit-llvm $(SRC) -o out/eigen.bc
	opt $(PLUGIN_PASS) out/eigen.bc -o out/eigen_tinystm.opt.bc
	$(CXX) $(CXXFLAGS) out/eigen_tinystm.opt.bc $(RUNTIMES_DIR)/TinySTM_runtime.cpp -o bin/eigen_tinystm

clean:
	rm -rf out bin