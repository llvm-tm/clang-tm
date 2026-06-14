# Top-level Makefile for TM API C++ Project
# Builds: LLVM plugin, plugin benchmarks, expli benchmarks, tests

include plugin/llvm-tool-helper.mk
LLVM_CONFIG_ARGS := $(shell $(LLVM_CONFIG) --cxxflags --ldflags --system-libs --libs core analysis 2>/dev/null || echo "-I$(shell brew --prefix llvm 2>/dev/null || echo /usr/lib/llvm-22)/include -L$(shell brew --prefix llvm 2>/dev/null || echo /usr/lib/llvm-22)/lib -lLLVM-22")

CXX ?= $(LLVM_CXX)
CXXFLAGS += -std=c++20 -pthread
ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g
else
  CXXFLAGS += -O2
endif
BACKEND ?= tinystm

PROJECT_ROOT := $(shell pwd)
LLVM_PLUGIN_DIR := $(PROJECT_ROOT)/plugin
BACKENDS_DIR := $(PROJECT_ROOT)/backends
PLUGIN_BENCHMARKS_DIR := $(PROJECT_ROOT)/benchmarks/plugin
EXPLI_BENCHMARKS_DIR := $(PROJECT_ROOT)/benchmarks/cpp
TESTS_DIR := $(PROJECT_ROOT)/tests

PLUGIN := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

.PHONY: all clean plugin plugin-benchmarks expli-benchmarks tests check help info test_run

all: info plugin plugin-benchmarks expli-benchmarks

info:
	@echo "TM API C++ Build System"
	@echo "==================="
	@echo "Project Root:  $(PROJECT_ROOT)"
	@echo "LLVM Config: $(LLVM_CONFIG)"
	@echo "Backend:    $(BACKEND)"
	@echo "CXX:       $(CXX)"
	@echo ""

plugin: $(PLUGIN)

$(PLUGIN): $(LLVM_PLUGIN_DIR)/passes/TMInstrumentPass.cpp | $(LLVM_PLUGIN_DIR)/bin
	$(CXX) -fPIC -std=c++20 -shared $< -o $@ \
	  -I$(LLVM_PLUGIN_DIR)/passes -I$(LLVM_PLUGIN_DIR)/analysis \
	  -I$(BACKENDS_DIR)/tm_impl/common $(LLVM_CONFIG_ARGS)

$(LLVM_PLUGIN_DIR)/bin:
	mkdir -p $@

plugin-benchmarks: plugin
	@echo "Building plugin benchmarks..."
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/bank bank_tinystm BACKEND=$(BACKEND) 2>&1
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/bank bank_singlelock BACKEND=$(BACKEND) 2>&1
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/datastructures bin/avltree_SingleGlobalLock 2>&1
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/STAMP stamp_uninstrumented 2>&1
	@echo "Plugin benchmarks built."

expli-benchmarks:
	@echo "Building expli benchmarks..."
	@$(MAKE) -C $(EXPLI_BENCHMARKS_DIR) all BACKEND=$(BACKEND) 2>&1
	@echo "Expli benchmarks built."

tests: plugin
	@echo "Building all tests..."
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) clean 2>&1
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) all
	@echo "Tests built."

check: tests
	@echo "Running tests..."
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) run 2>&1

clean:
	-$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/bank clean 2>&1
	-$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/datastructures clean 2>&1
	-$(MAKE) -C $(LLVM_PLUGIN_DIR) clean 2>&1
	-$(MAKE) -C $(EXPLI_BENCHMARKS_DIR) clean 2>&1
	rm -rf $(LLVM_PLUGIN_DIR)/bin $(LLVM_PLUGIN_DIR)/out
	rm -f /tmp/tm_persistent_state.bin
	@echo "Clean complete."

help:
	@echo "TM API C++ Build System"
	@echo "======================"
	@echo ""
	@echo "Targets:"
	@echo "  all               - Plugin + plugin benchmarks + expli benchmarks"
	@echo "  plugin            - Build LLVM TM plugin"
	@echo "  plugin-benchmarks - Build plugin-based benchmarks"
	@echo "  expli-benchmarks  - Build explicit C++ API benchmarks"
	@echo "  tests             - Build plugin tests"
	@echo "  check             - Build and run plugin tests"
	@echo "  clean             - Clean all build artifacts"
	@echo ""
	@echo "Options: BACKEND=tl2, DEBUG=1"

test_run: plugin-benchmarks
	@$(PLUGIN_BENCHMARKS_DIR)/bank/bin/bank_singlelock -t 2 -d 1000 2>&1
	@$(PLUGIN_BENCHMARKS_DIR)/datastructures/bin/avltree_SingleGlobalLock 1 10 100 2>&1
	@$(PLUGIN_BENCHMARKS_DIR)/STAMP/bin/stamp_uninstrumented -b kmeans -t 2 2>&1

# Run all explicit C++ API benchmarks across all supported backends
BACKENDS_TESTS := TINYSTM WBETL WT NOREC SWISSTM TL2 SGL LEFTRIGHT ROMULUS XTM SPHT TSXSGL
check-all:
	@echo "=== Building and running all tests across all backends ==="
	@for be in $(BACKENDS_TESTS); do \
		echo "--- Backend: $$be ---"; \
		$(MAKE) -C $(EXPLI_BENCHMARKS_DIR) clean BACKEND=$$be 2>&1 > /dev/null; \
		if $(MAKE) -C $(EXPLI_BENCHMARKS_DIR) -j4 bin/test_tx bin/test_ds BACKEND=$$be 2>&1 > /tmp/check-$$be-build.log; then \
			echo "  Build: OK"; \
			if [ "$$be" = "SGL" ] || [ "$$be" = "LEFTRIGHT" ] || [ "$$be" = "ROMULUS" ]; then \
				echo "  (runtime needs explicit init — skipping run)"; \
			else \
				if $(EXPLI_BENCHMARKS_DIR)/bin/test_tx > /tmp/check-$$be-tx.log 2>&1; then \
					echo "  test_tx: $$(tail -1 /tmp/check-$$be-tx.log)"; \
				else \
					echo "  test_tx: FAIL"; \
				fi; \
				if $(EXPLI_BENCHMARKS_DIR)/bin/test_ds > /tmp/check-$$be-ds.log 2>&1; then \
					echo "  test_ds: $$(tail -1 /tmp/check-$$be-ds.log)"; \
				else \
					echo "  test_ds: FAIL"; \
				fi; \
			fi; \
		else \
			echo "  Build: FAIL"; \
			tail -3 /tmp/check-$$be-build.log; \
		fi; \
		echo ""; \
	done
	@echo "=== All backend tests complete ==="
