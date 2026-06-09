include ../../../plugin/llvm-tool-helper.mk
CLANG_TM   := ../../../plugin/clang-tm
RUNTIMES   := ../../../backends/tm_impl
BACKENDS   := ../../../backends

CXXFLAGS   := -std=c++20 -O3 -pthread
COMMON_INC := -I$(BACKENDS)/tm_impl/common
BIN_DIR    := bin

.PHONY: all clean

BACKEND_TARGETS := \
	persist_dudetm \
	persist_nvhtm persist_spht \
	rbtree_persist_dudetm

all: $(BIN_DIR) $(BACKEND_TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

DUDETM_INC := -I$(BACKENDS)/tm_impl/tiny_stm -I$(BACKENDS)/tm_impl/dudetm \
              -I$(BACKENDS)/tm_impl -I$(BACKENDS)/tm_impl/common

persist_dudetm: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/dudetm/DUDETM_runtime.cpp \
		$(DUDETM_INC) -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
		-o $(BIN_DIR)/$@ $<

persist_nvhtm: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/nvhtm/NVHTM_runtime.cpp \
		$(COMMON_INC) -I$(BACKENDS)/tm_impl/nvhtm -DLLVM_TM_PLUGIN \
		-o $(BIN_DIR)/$@ $<

persist_spht: persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/spht/SPHT_runtime.cpp \
		$(COMMON_INC) -I$(BACKENDS)/tm_impl/spht -DLLVM_TM_PLUGIN \
		-o $(BIN_DIR)/$@ $<

rbtree_persist_dudetm: rbtree_persist_test.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/dudetm/DUDETM_runtime.cpp \
		$(DUDETM_INC) -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
		-o $(BIN_DIR)/$@ $<

clean:
	rm -rf $(BIN_DIR)
	rm -f benchmark_results/dudetm_persist.bin
