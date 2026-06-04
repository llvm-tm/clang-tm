TM_PLUGIN := $(abspath ../../llvm_tm_plugin/bin/libTMInstrument.so)
TM_COMPILE_FLAGS := -O1 -fno-inline -fno-vectorize -fno-slp-vectorize \
    -fno-unroll-loops -fno-stack-protector -pthread \
    -I$(realpath ..)
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

.PHONY: all clean

all: stmbench_singlelock stmbench_tl2 stmbench_tinystm stmbench_uninstrumented \
	stmbench_tsxsgl stmbench_swisstm \
	stmbench_tinystm_wbctl stmbench_tinystm_wbetl stmbench_tinystm_wt \
	stmbench_norec stmbench_dudetm stmbench_spht

stmbench_uninstrumented: STMbench7.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(BACKENDS_DIR) $< -o $(BIN_DIR)/$@

$(eval $(call tm_target, stmbench_singlelock, STMbench7.cpp, singlelock))
$(eval $(call tm_target, stmbench_tl2, STMbench7.cpp, tl2))
$(eval $(call tm_target, stmbench_tinystm, STMbench7.cpp, tinystm))
$(eval $(call tm_target, stmbench_tsxsgl, STMbench7.cpp, tsxsgl))
$(eval $(call tm_target, stmbench_swisstm, STMbench7.cpp, swisstm))
$(eval $(call tm_target, stmbench_tinystm_wbctl, STMbench7.cpp, tinystm_wbctl))
$(eval $(call tm_target, stmbench_tinystm_wbetl, STMbench7.cpp, tinystm_wbetl))
$(eval $(call tm_target, stmbench_tinystm_wt, STMbench7.cpp, tinystm_wt))
$(eval $(call tm_target, stmbench_norec, STMbench7.cpp, norec))
$(eval $(call tm_target, stmbench_dudetm, STMbench7.cpp, dudetm))
$(eval $(call tm_target, stmbench_spht, STMbench7.cpp, spht))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
