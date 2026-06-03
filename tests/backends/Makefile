# Backend unit tests — compile directly against runtimes (no LLVM plugin)
# Each test is compiled once per backend, producing:
#   bin/<backend>/<test_name>

include ../../llvm_tm_plugin/llvm-tool-helper.mk
CXX := $(LLVM_CXX)
CXXFLAGS := -std=c++20 -O0 -pthread -g
RUNTIME_DIR := ../../backends/runtimes
INC_BASE := -I../../backends -I$(RUNTIME_DIR)

# ── Backend definitions ────────────────────────────────────────────
BACKENDS := tl2 tinystm wt wbetl norec swisstm

INC_tl2     := $(INC_BASE) -I../../backends/TL2
RUNTIME_tl2 := $(RUNTIME_DIR)/tl2_runtime.cpp
FLAGS_tl2   := -DTM_BACKEND_TL2

INC_tinystm := $(INC_BASE) -I../../backends/TinySTM
RUNTIME_tinystm := $(RUNTIME_DIR)/TinySTM_runtime.cpp
FLAGS_tinystm := -DDESIGN_WBCTL -DTM_BACKEND_TINYSTM

INC_wt := $(INC_BASE) -I../../backends/TinySTM
RUNTIME_wt := $(RUNTIME_DIR)/TinySTM_runtime.cpp
FLAGS_wt := -DDESIGN_WT -DTM_BACKEND_TINYSTM

INC_wbetl := $(INC_BASE) -I../../backends/TinySTM
RUNTIME_wbetl := $(RUNTIME_DIR)/TinySTM_runtime.cpp
FLAGS_wbetl := -DDESIGN_WBETL -DTM_BACKEND_TINYSTM

INC_norec   := $(INC_BASE) -I../../backends/NOrec
RUNTIME_norec := $(RUNTIME_DIR)/NOrec_runtime.cpp
FLAGS_norec := -DTM_BACKEND_NOREC

INC_swisstm := $(INC_BASE) -I../../backends/SwissTM
RUNTIME_swisstm := $(RUNTIME_DIR)/SwissTM_runtime.cpp
FLAGS_swisstm := -DTM_BACKEND_SWISSTM

INC_nvhtm := $(INC_BASE) -I../../backends/NVHTM
RUNTIME_nvhtm := $(RUNTIME_DIR)/NVHTM_runtime.cpp
FLAGS_nvhtm := -DTM_BACKEND_NVHTM -mrtm

INC_spht := $(INC_BASE) -I../../backends/SPHT
RUNTIME_spht := $(RUNTIME_DIR)/SPHT_runtime.cpp
FLAGS_spht := -DTM_BACKEND_SPHT -mrtm

# ── Test source files ──────────────────────────────────────────────
TESTS := counter_st counter_mt write_set_validation overlap_stress read_lock_interference readall_write_interference

# ── Standalone allocator tests (no backend runtime) ─────────────────
ALLOC_TESTS := region_alloc region_stress

.PHONY: all clean list run% list-backends run-alloc

# ── Explicit per-backend directory rules ───────────────────────────
$(foreach b,$(BACKENDS),$(eval bin/$(b)/: ; mkdir -p $$@))

bin/:
	mkdir -p $@

# ── Per-backend, per-test build rules ──────────────────────────────
# Generate: bin/<b>/<t>: test_<t>.cpp <runtime> test_helpers.hpp | bin/<b>/
define BUILD_RULE
bin/$(1)/$(2): test_$(2).cpp $(RUNTIME_$(1)) $(RUNTIME_DIR)/tm_region_allocator.cpp test_helpers.hpp | bin/$(1)/
	$$(CXX) $$(CXXFLAGS) $$(INC_$(1)) $$(FLAGS_$(1)) -o $$@ $$< $$(RUNTIME_$(1)) $(RUNTIME_DIR)/tm_region_allocator.cpp
endef

# Flatten: for each (backend, test) pair, eval a build rule
RULES := $(foreach b,$(BACKENDS),$(foreach t,$(TESTS),$(eval $(call BUILD_RULE,$(b),$(t)))))

# ── Standalone allocator test rules ─────────────────────────────────
# Each allocator test just links against the region allocator .cpp
# (no TM runtime needed).
REGION_ALLOC_SRC := $(RUNTIME_DIR)/tm_region_allocator.cpp
REGION_ALLOC_DEPS := $(REGION_ALLOC_SRC) ../../backends/tm_platform.hpp ../../backends/tm_region_allocator.hpp

bin/region_alloc: test_region_alloc.cpp $(REGION_ALLOC_DEPS) | bin/
	$(CXX) $(CXXFLAGS) -I../../backends -o $@ $< $(REGION_ALLOC_SRC)

bin/region_stress: test_region_stress.cpp $(REGION_ALLOC_DEPS) | bin/
	$(CXX) $(CXXFLAGS) -I../../backends -o $@ $< $(REGION_ALLOC_SRC)

# ── All target ─────────────────────────────────────────────────────
BINS := $(foreach b,$(BACKENDS),$(foreach t,$(TESTS),bin/$(b)/$(t)))
ALLOC_BINS := $(addprefix bin/, $(ALLOC_TESTS))
all: $(BINS) $(ALLOC_BINS)

# ── Run targets ────────────────────────────────────────────────────
RUN_TARGETS := $(addprefix run-,$(BACKENDS))

$(RUN_TARGETS): run-%:
	@echo "=== Backend: $* ==="
	@$(foreach t,$(TESTS),\
		echo "--- $(t) ---"; \
		bin/$*/$(t) 2>&1; \
		echo;)

run-allocs:
	@echo "=== TM Region Allocator Tests ==="
	@for t in $(ALLOC_TESTS); do \
		echo "--- $$t ---"; \
		bin/$$t 2>&1; \
		echo; \
	done

run: run-allocs $(RUN_TARGETS)
	@echo "=== All tests done ==="

# ── Convenience ────────────────────────────────────────────────────
list:
	@echo "Backends:"   $(BACKENDS)
	@echo "Tests: "     $(TESTS)
	@echo "Alloc tests:" $(ALLOC_TESTS)
	@echo "Targets: "   $(BINS) $(ALLOC_BINS)

clean:
	rm -rf bin
