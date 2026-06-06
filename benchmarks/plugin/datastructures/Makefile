include ../../plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

CXXFLAGS := -std=c++20 -O2 -pthread
LOCAL_BENCHES := avltree avltree_recursive rbtree hashmap bitmap list set heap

.PHONY: all clean

all: $(OUT_DIR) $(BIN_DIR) \
	$(foreach b,$(LOCAL_BENCHES),$(b)_uninstrumented $(b)_SingleGlobalLock $(b)_NOrec $(b)_TinySTM)

$(BIN_DIR)/%_uninstrumented: %.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ -pthread

$(foreach b,$(LOCAL_BENCHES),$(eval $(call tm_target,$(b)_SingleGlobalLock,$(b).cpp,singlelock)))
$(foreach b,$(LOCAL_BENCHES),$(eval $(call tm_target,$(b)_NOrec,$(b).cpp,norec)))
$(foreach b,$(LOCAL_BENCHES),$(eval $(call tm_target,$(b)_TinySTM,$(b).cpp,tinystm)))

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
