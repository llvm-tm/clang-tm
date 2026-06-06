include ../../../plugin/llvm-tool-helper.mk
CLANG_TM   := ../../../plugin/clang-tm
RUNTIMES   := ../../../backends/tm_impl
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
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/dudetm/DUDETM_runtime.cpp \
		-I$(BACKENDS)/tm_impl/tiny_stm -I$(BACKENDS)/tm_impl/dudetm -DDESIGN_WBCTL \
		-o $(BIN_DIR)/$@ $<

persist_nvhtm: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/nvhtm/NVHTM_runtime.cpp \
		-I$(BACKENDS)/tm_impl/nvhtm -o $(BIN_DIR)/$@ $<

persist_spht: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/spht/SPHT_runtime.cpp \
		-I$(BACKENDS)/tm_impl/spht -o $(BIN_DIR)/$@ $<

clean:
	rm -rf $(BIN_DIR)
	rm -f benchmark_results/dudetm_persist.bin
