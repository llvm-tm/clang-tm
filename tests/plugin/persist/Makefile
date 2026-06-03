include ../../../llvm_tm_plugin/llvm-tool-helper.mk
CLANG_TM   := ../../../llvm_tm_plugin/clang-tm
RUNTIMES   := ../../../backends/runtimes
BACKENDS   := ../../../backends

CXXFLAGS   := -std=c++20 -O3 -pthread
BIN_DIR    := bin

.PHONY: all clean

BACKEND_TARGETS := \
	persist_dudetm \
	persist_nvhtm persist_spht

all: $(BIN_DIR) $(BACKEND_TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

persist_dudetm: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/DUDETM_runtime.cpp \
		-I$(BACKENDS)/TinySTM -I$(BACKENDS)/DUDETM -DDESIGN_WBCTL \
		-o $(BIN_DIR)/$@ $<

persist_nvhtm: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/NVHTM_runtime.cpp \
		-I$(BACKENDS)/NVHTM -o $(BIN_DIR)/$@ $<

persist_spht: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/SPHT_runtime.cpp \
		-I$(BACKENDS)/SPHT -o $(BIN_DIR)/$@ $<

clean:
	rm -rf $(BIN_DIR)
	rm -f benchmark_results/dudetm_persist.bin
