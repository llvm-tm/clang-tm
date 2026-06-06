include ../../plugin/llvm-tool-helper.mk
CLANG_TM   := ../../plugin/clang-tm
RUNTIMES   := ../../backends/tm_impl
BACKENDS   := ../../backends

CXXFLAGS   := -std=c++20 -O3 -pthread

BIN_DIR    := bin

.PHONY: all clean

all: $(BIN_DIR) \
	bank_uninstrumented \
	bank_singlelock bank_norec bank_tl2 bank_tinystm bank_swiss \
	bank_persistentsgl \
	bank_dudetm bank_nvhtm bank_spht

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

bank_uninstrumented: bank.cpp | $(BIN_DIR)
	$(LLVM_CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

bank_singlelock: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/single_global_lock/SingleGlobalLock_runtime.cpp -o $(BIN_DIR)/$@ $<

bank_norec: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/norec/NOrec_runtime.cpp -I$(BACKENDS)/tm_impl/norec -o $(BIN_DIR)/$@ $<

bank_tl2: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tl2/tl2_runtime.cpp -o $(BIN_DIR)/$@ $<

bank_tinystm: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -DDESIGN_WBCTL -o $(BIN_DIR)/$@ $<

bank_tinystm_wbetl: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -DDESIGN_WBETL -o $(BIN_DIR)/$@ $<

bank_tinystm_wt: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -DDESIGN_WT -o $(BIN_DIR)/$@ $<

test_swiss: test_swiss.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/swisstm/SwissTM_runtime.cpp -I$(BACKENDS)/tm_impl/swisstm -o $(BIN_DIR)/$@ $<

test_tx: test_tx.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/swisstm/SwissTM_runtime.cpp -I$(BACKENDS)/tm_impl/swisstm -o $(BIN_DIR)/$@ $<

test_loop: test_loop.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/swisstm/SwissTM_runtime.cpp -I$(BACKENDS)/tm_impl/swisstm -o $(BIN_DIR)/$@ $<

test_ptr: test_ptr.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/swisstm/SwissTM_runtime.cpp -I$(BACKENDS)/tm_impl/swisstm -o $(BIN_DIR)/$@ $<

test_global: test_global.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/swisstm/SwissTM_runtime.cpp -I$(BACKENDS)/tm_impl/swisstm -o $(BIN_DIR)/$@ $<

bank_swiss: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/swisstm/SwissTM_runtime.cpp -I$(BACKENDS)/tm_impl/swisstm -o $(BIN_DIR)/$@ $<

bank_persistentsgl: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/persistent_sgl/PersistentSGL_runtime.cpp -o $(BIN_DIR)/$@ $<

bank_dudetm: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/dudetm/DUDETM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -I$(BACKENDS)/tm_impl/dudetm -DDESIGN_WBCTL -o $(BIN_DIR)/$@ $<

bank_nvhtm: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/nvhtm/NVHTM_runtime.cpp -I$(BACKENDS)/tm_impl/nvhtm -o $(BIN_DIR)/$@ $<

bank_spht: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/spht/SPHT_runtime.cpp -I$(BACKENDS)/tm_impl/spht -o $(BIN_DIR)/$@ $<

bank_tsxsgl: bank.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) -mrtm --runtime $(RUNTIMES)/tsx_sgl/TSXSGL_runtime.cpp -o $(BIN_DIR)/$@ $<

clean:
	rm -rf $(BIN_DIR)

