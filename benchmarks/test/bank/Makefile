include ../../../llvm_tm_plugin/llvm-tool-helper.mk
CLANG_TM   := ../../../llvm_tm_plugin/clang-tm
RUNTIMES   := ../../../backends/runtimes
BACKENDS   := ../../../backends

CXXFLAGS   := -std=c++20 -O3 -pthread

BIN_DIR    := bin

.PHONY: all clean

all: $(BIN_DIR) \
	bank_uninstrumented \
	bank_singlelock bank_norec bank_tl2 bank_tinystm bank_swiss \
	bank_persistentsgl

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

bank_uninstrumented: bank.cpp | $(BIN_DIR)
	$(LLVM_CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

bank_singlelock: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/SingleGlobalLock_runtime.cpp -o $(BIN_DIR)/$@ $<

bank_norec: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/NOrec_runtime.cpp -I$(BACKENDS)/NOrec -o $(BIN_DIR)/$@ $<

bank_tl2: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tl2_runtime.cpp -o $(BIN_DIR)/$@ $<

bank_tinystm: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/TinySTM_runtime.cpp -I$(BACKENDS)/TinySTM -DDESIGN_WBCTL -o $(BIN_DIR)/$@ $<

bank_swiss: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/SwissTM_runtime.cpp -I$(BACKENDS)/SwissTM -o $(BIN_DIR)/$@ $<

bank_persistentsgl: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/PersistentSGL_runtime.cpp -o $(BIN_DIR)/$@ $<

bank_tsxsgl: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/TSXSGL_runtime.cpp -o $(BIN_DIR)/$@ $<

clean:
	rm -rf $(BIN_DIR)

