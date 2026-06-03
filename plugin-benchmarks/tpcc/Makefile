TM_PLUGIN := $(abspath ../../llvm_tm_plugin/bin/libTMInstrument.so)
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

.PHONY: all clean

all: tpcc_singlelock tpcc_tl2 tpcc_tinystm tpcc_persistentsgl tpcc_uninstrumented \
	tpcc_tsxsgl tpcc_swisstm \
	tpcc_tinystm_wbctl tpcc_tinystm_wbetl tpcc_tinystm_wt \
	tpcc_dudetm tpcc_spht

tpcc_uninstrumented: TPCC.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

$(eval $(call tm_target, tpcc_singlelock, TPCC.cpp, singlelock))
$(eval $(call tm_target, tpcc_tl2, TPCC.cpp, tl2))
$(eval $(call tm_target, tpcc_tinystm, TPCC.cpp, tinystm))
$(eval $(call tm_target, tpcc_persistentsgl, TPCC.cpp, persistentsgl))
$(eval $(call tm_target, tpcc_tsxsgl, TPCC.cpp, tsxsgl))
$(eval $(call tm_target, tpcc_swisstm, TPCC.cpp, swisstm))
$(eval $(call tm_target, tpcc_tinystm_wbctl, TPCC.cpp, tinystm_wbctl))
$(eval $(call tm_target, tpcc_tinystm_wbetl, TPCC.cpp, tinystm_wbetl))
$(eval $(call tm_target, tpcc_tinystm_wt, TPCC.cpp, tinystm_wt))
$(eval $(call tm_target, tpcc_dudetm, TPCC.cpp, dudetm))
$(eval $(call tm_target, tpcc_spht, TPCC.cpp, spht))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
