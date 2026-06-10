TM_PLUGIN := $(abspath ../../../plugin/bin/libTMInstrument.so)
include ../../../plugin/tm_pipeline.mk
TM_COMPILE_FLAGS += -I$(realpath ..)
$(eval $(call tm_define_rules))

.PHONY: all clean

all: stmbench_singlelock stmbench_tl2 stmbench_tinystm stmbench_uninstrumented \
	stmbench_tsxsgl stmbench_swisstm \
	stmbench_tinystm_wbctl stmbench_tinystm_wbetl stmbench_tinystm_wt \
	stmbench_norec stmbench_dudetm stmbench_spht \
	stmbench7_queue_manual

stmbench_uninstrumented: STMbench7.cpp $(BACKENDS_DIR)/stubs/tm_stubs.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(TM_COMPILE_FLAGS) -I$(BACKENDS_DIR) -I$(BACKENDS_DIR)/tm_impl/common -I$(realpath ..) $+ -o $(BIN_DIR)/$@

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

# ─── Queue-manual benchmark ──────────────────────────────────────────────
# Queue runtime objects (compiled from backends)
QUEUE_RT_OBJS := $(OUT_DIR)/tinystm_runtime.o $(OUT_DIR)/queue_runtime.o $(OUT_DIR)/tm_region_allocator.o

# Override link flags for queue objects: DESIGN_WBCTL + LLVM_TM_PLUGIN needed
$(OUT_DIR)/tinystm_runtime.o: $(BACKENDS_DIR)/tm_impl/tiny_stm/TinySTM_runtime.cpp | $(OUT_DIR)
	$(CXX) -c -std=c++20 $(TM_LINK_OPT) -pthread -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@

$(OUT_DIR)/queue_runtime.o: $(BACKENDS_DIR)/tm_impl/queue/queue_runtime.cpp | $(OUT_DIR)
	$(CXX) -c -std=c++20 $(TM_LINK_OPT) -pthread -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@

$(OUT_DIR)/tm_region_allocator.o: $(BACKENDS_DIR)/tm_impl/tm_region_allocator/tm_region_allocator.cpp | $(OUT_DIR)
	$(CXX) -c -std=c++20 $(TM_LINK_OPT) -pthread -DDESIGN_WBCTL -DLLVM_TM_PLUGIN \
	  -I $(TINYSTM_DIR) -I $(BACKENDS_DIR)/tm_impl/common -I $(BACKENDS_DIR) $< -o $@

# Queue-manual build: uses existing tm-instrument-queue-manual pipeline
# (compile step handled by $(OUT_DIR)/%.bc: %.cpp pattern rule from tm_define_rules)
$(OUT_DIR)/stmbench7_queue_manual.instr.bc: $(OUT_DIR)/stmbench7_queue_manual.bc $(TM_PLUGIN) | $(OUT_DIR)
	$(CLANG_TM) --instrument-only --plugin=$(TM_PLUGIN) \
	  -passes="tm-instrument-queue-manual" $< -o $@

$(OUT_DIR)/stmbench7_queue_manual.opt.bc: $(OUT_DIR)/stmbench7_queue_manual.instr.bc | $(OUT_DIR)
	$(call tm_optimize,$<,$@)

$(BIN_DIR)/stmbench7_queue_manual: $(OUT_DIR)/stmbench7_queue_manual.opt.bc $(QUEUE_RT_OBJS) | $(BIN_DIR)
	$(CXX) -std=c++20 $(TM_LINK_OPT) -pthread $(OUT_DIR)/stmbench7_queue_manual.opt.bc $(QUEUE_RT_OBJS) -o $@

stmbench7_queue_manual: $(BIN_DIR)/stmbench7_queue_manual

.PHONY: stmbench7_queue_manual

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)
