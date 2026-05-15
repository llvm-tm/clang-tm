include ../../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

CXXFLAGS := -std=c++20 -O3 -pthread

.PHONY: all clean

all: $(OUT_DIR) $(BIN_DIR) \
	bank_uninstrumented \
	bank_singlelock bank_norec bank_tl2 bank_tinystm bank_swiss \
	bank_persistentsgl bank_distributedsgl

bank_uninstrumented: bank.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

$(eval $(call tm_target, bank_singlelock, bank.cpp, singlelock))
$(eval $(call tm_target, bank_norec, bank.cpp, norec))
$(eval $(call tm_target, bank_tl2, bank.cpp, tl2))
$(eval $(call tm_target, bank_tinystm, bank.cpp, tinystm))
$(eval $(call tm_target, bank_swiss, bank.cpp, swisstm))
$(eval $(call tm_target, bank_persistentsgl, bank.cpp, persistentsgl))
$(eval $(call tm_target, bank_distributedsgl, bank.cpp, distributedsgl))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
