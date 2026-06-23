include ../../../plugin/llvm-tool-helper.mk
CLANG_TM   := ../../../plugin/clang-tm
RUNTIMES   := ../../../backends/tm_impl
BACKENDS   := ../../../backends

CXXFLAGS   := -std=c++20 -O3 -pthread -I$(BACKENDS)/tm_impl/common \
              -I$(BACKENDS)/stubs

BIN_DIR    := bin

.PHONY: all clean

all: $(BIN_DIR) \
	social_tm_uninstrumented \
	social_tm_norec \
	social_tm_tinystm_wbctl \
	social_tm_tinystm_wt

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

social_tm_uninstrumented: social_tm.cpp | $(BIN_DIR)
	$(LLVM_CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

social_tm_norec: social_tm.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/norec/NOrec_runtime.cpp -I$(BACKENDS)/tm_impl/norec -o $(BIN_DIR)/$@ $<

social_tm_tinystm_wbctl: social_tm.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -DDESIGN_WBCTL -o $(BIN_DIR)/$@ $<

social_tm_tinystm_wt: social_tm.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -DDESIGN_WT -o $(BIN_DIR)/$@ $<

clean:
	rm -rf $(BIN_DIR)
