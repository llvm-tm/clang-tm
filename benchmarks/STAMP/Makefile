CXXFLAGS := -std=c++20 -O1 -pthread
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

# STAMP opaque functions: each TX-opaque call is annotated with
# __attribute__((annotate("tm_allow_opaque"))) and a comment explaining
# why it cannot be auto-instrumented. No blanket -tm-allow-opaque.

.PHONY: all clean

all: stamp_uninstrumented stamp_tinystm stamp_norec stamp_singlelock \
	stamp_tsxsgl stamp_tl2 stamp_swisstm \
	stamp_tinystm_wbctl stamp_tinystm_wbetl stamp_tinystm_wt

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

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
