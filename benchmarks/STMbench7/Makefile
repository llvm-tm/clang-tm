include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

.PHONY: all clean

all: stmbench_singlelock stmbench_tl2 stmbench_tinystm stmbench_uninstrumented

stmbench_uninstrumented: STMbench7.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

$(eval $(call tm_target, stmbench_singlelock, STMbench7.cpp, singlelock))
$(eval $(call tm_target, stmbench_tl2, STMbench7.cpp, tl2))
$(eval $(call tm_target, stmbench_tinystm, STMbench7.cpp, tinystm))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
