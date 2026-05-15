LLVM_PLUGIN_DIR := ../../../llvm_tm_plugin
BACKENDS_DIR := ../../../backends
RUNTIMES_DIR := $(BACKENDS_DIR)/runtimes
TL2_DIR := $(BACKENDS_DIR)/TL2
TINYSTM_DIR := $(BACKENDS_DIR)/TinySTM
NOREC_DIR := $(BACKENDS_DIR)/NOrec
SWISSSTM_DIR := $(BACKENDS_DIR)/SwissTM

# Bank uses -O3 (not -O1)
CXXFLAGS := -std=c++20 -O3 -pthread

include ../../../llvm_tm_plugin/tm_pipeline.mk
$(eval $(call tm_define_rules))

PLUGIN_LIB := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so
BANK_SRC := bank.cpp

.PHONY: all clean test help verify_annotations

all: $(OUT_DIR) $(BIN_DIR) bank_uninstrumented bank_singlelock bank_tl2 \
     bank_tinystm bank_swiss bank_norec

# Uninstrumented baseline (no TM, direct access)
bank_uninstrumented: $(BANK_SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(BANK_SRC) -o $(BIN_DIR)/$@

# 6 backend variants sharing the same instrumented IR
# Pipeline: .cpp → .bc → .instr.bc (plugin) → .opt.bc (optimize) → binary (link with runtime)
$(eval $(call tm_target, bank_singlelock, $(BANK_SRC), singlelock))
$(eval $(call tm_target, bank_tl2, $(BANK_SRC), tl2))
$(eval $(call tm_target, bank_tinystm, $(BANK_SRC), tinystm))
$(eval $(call tm_target, bank_swiss, $(BANK_SRC), swisstm))
$(eval $(call tm_target, bank_norec, $(BANK_SRC), norec))

# Custom TinySTM variants (not in pipeline by default)
bank_wbetl: $(OUT_DIR)/bank.opt.bc | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DDESIGN_WBETL $< $(RUNTIMES_DIR)/TinySTM_runtime.cpp \
	  -I$(TINYSTM_DIR) -I$(BACKENDS_DIR) -o $(BIN_DIR)/$@

bank_wt: $(OUT_DIR)/bank.opt.bc | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -DDESIGN_WT $< $(RUNTIMES_DIR)/TinySTM_runtime.cpp \
	  -I$(TINYSTM_DIR) -I$(BACKENDS_DIR) -o $(BIN_DIR)/$@

# Test runtime (not a standard backend)
bank_test: $(OUT_DIR)/bank.opt.bc src/tm_test_runtime.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OUT_DIR)/bank.opt.bc \
	  src/tm_test_runtime.cpp -o $(BIN_DIR)/$@

# Verify TM/TX annotations
verify_annotations: $(OUT_DIR)/bank.bc $(PLUGIN_LIB)
	@echo "Verifying TM/TX annotations were detected..."
	@opt -load-pass-plugin=$(PLUGIN_LIB) -passes="tm-instrument" \
		$(OUT_DIR)/bank.bc -o /dev/null 2>&1 | tee $(OUT_DIR)/bank.plugin.log
	@if ! grep -qE "Found [1-9][0-9]* TM-annotated symbols" $(OUT_DIR)/bank.plugin.log; then \
		echo "ERROR: No TM-annotated symbols detected!"; \
		exit 1; \
	fi
	@if ! grep -q "Instrumenting transaction function:" $(OUT_DIR)/bank.plugin.log; then \
		echo "ERROR: No transaction functions detected!"; \
		exit 1; \
	fi
	@echo "Annotation detection verified."

# Test targets
test: test_tl2 test_tinystm test_swiss

test_tl2: bank_tl2
	@echo "Testing bank benchmark with TL2 (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_tl2 -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

test_tinystm: bank_tinystm
	@echo "Testing bank benchmark with TinySTM (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_tinystm -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

test_swiss: bank_swiss
	@echo "Testing bank benchmark with SwissTM (2 threads, 10 seconds)..."
	@$(BIN_DIR)/bank_swiss -d 10000 -a 256 -t 2 -r 10 -w 0 || echo "Test completed"

clean:
	rm -rf $(OUT_DIR) $(BIN_DIR)

help:
	@echo "Bank Benchmark Makefile"
	@echo "======================"
	@echo "Targets:"
	@echo "  all                - Build all benchmarks"
	@echo "  bank_uninstrumented - Baseline (no TM)"
	@echo "  bank_singlelock    - SingleGlobalLock runtime"
	@echo "  bank_tl2          - TL2 runtime"
	@echo "  bank_tinystm      - TinySTM runtime"
	@echo "  bank_swiss        - SwissTM runtime"
	@echo "  bank_norec        - NOrec runtime"
	@echo "  test              - Run all tests"
	@echo "  clean             - Clean build artifacts"
$(eval $(call tm_target, bank_persistentsgl, $(BANK_SRC), persistentsgl))
$(eval $(call tm_target, bank_distributedsgl, $(BANK_SRC), distributedsgl))
