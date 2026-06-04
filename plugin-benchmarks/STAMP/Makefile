CXXFLAGS := -std=c++20 -O1 -pthread
TM_PLUGIN := $(abspath ../../llvm_tm_plugin/bin/libTMInstrument.so)
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

# STAMP opaque functions: each TX-opaque call is annotated with
# __attribute__((annotate("tm_allow_opaque"))) and a comment explaining
# why it cannot be auto-instrumented. No blanket -tm-allow-opaque.

# Standalone plugin-only sources (identical algorithm, LLVM plugin path)
BAYES_SRC := bayes.cpp
YADA_SRC  := yada.cpp

# Custom compile rules so these compile as standalone binaries (not part of STAMP.cpp)
$(OUT_DIR)/bayes.bc: $(BAYES_SRC) $(OUT_DIR)
	$(CLANG_TM) --compile-only -std=c++20 $(TM_COMPILE_FLAGS) $< -o $@

$(OUT_DIR)/yada.bc: $(YADA_SRC) $(OUT_DIR)
	$(CLANG_TM) --compile-only -std=c++20 $(TM_COMPILE_FLAGS) $< -o $@

.PHONY: all clean

all: stamp_uninstrumented stamp_tinystm stamp_norec stamp_singlelock \
	stamp_tsxsgl stamp_tl2 stamp_swisstm \
	stamp_tinystm_wbctl stamp_tinystm_wbetl stamp_tinystm_wt \
	stamp_dudetm stamp_spht \
	stamp_bayes_plugin_tinystm stamp_yada_plugin_tinystm

stamp_uninstrumented: STAMP.cpp tm_stubs.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $+ -o $(BIN_DIR)/$@

$(eval $(call tm_target, stamp_tinystm, STAMP.cpp, tinystm))
$(eval $(call tm_target, stamp_norec, STAMP.cpp, norec))
$(eval $(call tm_target, stamp_singlelock, STAMP.cpp, singlelock))
$(eval $(call tm_target, stamp_tsxsgl, STAMP.cpp, tsxsgl))
$(eval $(call tm_target, stamp_tl2, STAMP.cpp, tl2))
$(eval $(call tm_target, stamp_swisstm, STAMP.cpp, swisstm))
$(eval $(call tm_target, stamp_tinystm_wbctl, STAMP.cpp, tinystm_wbctl))
$(eval $(call tm_target, stamp_tinystm_wbetl, STAMP.cpp, tinystm_wbetl))
$(eval $(call tm_target, stamp_tinystm_wt, STAMP.cpp, tinystm_wt))
$(eval $(call tm_target, stamp_dudetm, STAMP.cpp, dudetm))
$(eval $(call tm_target, stamp_spht, STAMP.cpp, spht))

# Plugin-only standalone targets
$(eval $(call tm_target, stamp_bayes_plugin_tinystm, $(BAYES_SRC), tinystm))
$(eval $(call tm_target, stamp_yada_plugin_tinystm, $(YADA_SRC), tinystm))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
