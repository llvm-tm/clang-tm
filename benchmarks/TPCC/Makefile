include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

.PHONY: all clean

all: tpcc_singlelock tpcc_tl2 tpcc_tinystm tpcc_persistentsgl tpcc_uninstrumented

tpcc_uninstrumented: TPCC.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

$(eval $(call tm_target, tpcc_singlelock, TPCC.cpp, singlelock))
$(eval $(call tm_target, tpcc_tl2, TPCC.cpp, tl2))
$(eval $(call tm_target, tpcc_tinystm, TPCC.cpp, tinystm))
$(eval $(call tm_target, tpcc_persistentsgl, TPCC.cpp, persistentsgl))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
