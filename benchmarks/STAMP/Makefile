CXXFLAGS := -std=c++20 -O1 -pthread
include ../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

.PHONY: all clean

all: stamp_uninstrumented stamp_tinystm stamp_norec stamp_singlelock

stamp_uninstrumented: STAMP.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

$(eval $(call tm_target, stamp_tinystm, STAMP.cpp, tinystm))
$(eval $(call tm_target, stamp_norec, STAMP.cpp, norec))
$(eval $(call tm_target, stamp_singlelock, STAMP.cpp, singlelock))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
