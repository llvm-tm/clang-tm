# Backend unit tests — compile directly against runtimes (no LLVM plugin)
# Each test is compiled once per backend, producing:
#   bin/<backend>/<test_name>

include ../../../plugin/llvm-tool-helper.mk
CXX := $(LLVM_CXX)
CXXFLAGS := -std=c++20 -O0 -pthread -g
RUNTIME_DIR := ../../../backends/tm_impl
HOOKS_SRC := $(RUNTIME_DIR)/common/tm_hooks.cpp
INC_BASE := -I../../../backends/tm_impl/common -I$(RUNTIME_DIR)

# ── Backend definitions ────────────────────────────────────────────
BACKENDS := tl2 tinystm wt wbetl norec swisstm xtm

INC_tl2     := $(INC_BASE) -I../../../backends/tm_impl/tl2
RUNTIME_tl2 := $(RUNTIME_DIR)/tl2/tl2_runtime.cpp
FLAGS_tl2   := -DTM_BACKEND_TL2

TINYSTM_STUB := -I../../../backends/stubs
INC_tinystm := $(INC_BASE) -I../../../backends/tm_impl/tiny_stm $(TINYSTM_STUB)
RUNTIME_tinystm := $(RUNTIME_DIR)/tiny_stm/TinySTM_runtime.cpp
FLAGS_tinystm := -DDESIGN_WBCTL -DTM_BACKEND_TINYSTM

INC_wt := $(INC_BASE) -I../../../backends/tm_impl/tiny_stm $(TINYSTM_STUB)
RUNTIME_wt := $(RUNTIME_DIR)/tiny_stm/TinySTM_runtime.cpp
FLAGS_wt := -DDESIGN_WT -DTM_BACKEND_TINYSTM

INC_wbetl := $(INC_BASE) -I../../../backends/tm_impl/tiny_stm $(TINYSTM_STUB)
RUNTIME_wbetl := $(RUNTIME_DIR)/tiny_stm/TinySTM_runtime.cpp
FLAGS_wbetl := -DDESIGN_WBETL -DTM_BACKEND_TINYSTM

INC_norec   := $(INC_BASE) -I../../../backends/tm_impl/norec
RUNTIME_norec := $(RUNTIME_DIR)/norec/NOrec_runtime.cpp
FLAGS_norec := -DTM_BACKEND_NOREC

INC_swisstm := $(INC_BASE) -I../../../backends/tm_impl/swisstm
RUNTIME_swisstm := $(RUNTIME_DIR)/swisstm/SwissTM_runtime.cpp
FLAGS_swisstm := -DTM_BACKEND_SWISSTM

INC_xtm := $(INC_BASE) -I../../../backends/tm_impl/xtm
RUNTIME_xtm := $(RUNTIME_DIR)/xtm/xtm_runtime.cpp
FLAGS_xtm := -DTM_BACKEND_XTM

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
bin/$(1)/$(2): test_$(2).cpp $(RUNTIME_$(1)) $(RUNTIME_DIR)/tm_region_allocator/tm_region_allocator.cpp $(HOOKS_SRC) test_helpers.hpp | bin/$(1)/
	$$(CXX) $$(CXXFLAGS) $$(INC_$(1)) $$(FLAGS_$(1)) -o $$@ $$< $$(RUNTIME_$(1)) $(RUNTIME_DIR)/tm_region_allocator/tm_region_allocator.cpp $(HOOKS_SRC)
endef

# Flatten: for each (backend, test) pair, eval a build rule
RULES := $(foreach b,$(BACKENDS),$(foreach t,$(TESTS),$(eval $(call BUILD_RULE,$(b),$(t)))))

# ── Standalone allocator test rules ─────────────────────────────────
# Each allocator test just links against the region allocator .cpp
# (no TM runtime needed).
REGION_ALLOC_SRC := $(RUNTIME_DIR)/tm_region_allocator/tm_region_allocator.cpp
REGION_ALLOC_DEPS := $(REGION_ALLOC_SRC) ../../../backends/tm_impl/common/tm_platform.hpp ../../../backends/tm_impl/common/tm_region_allocator.hpp

bin/region_alloc: test_region_alloc.cpp $(REGION_ALLOC_DEPS) | bin/
	$(CXX) $(CXXFLAGS) -I../../../backends/tm_impl/common -o $@ $< $(REGION_ALLOC_SRC)

bin/region_stress: test_region_stress.cpp $(REGION_ALLOC_DEPS) | bin/
	$(CXX) $(CXXFLAGS) -I../../../backends/tm_impl/common -o $@ $< $(REGION_ALLOC_SRC)

# ── All target ─────────────────────────────────────────────────────
BINS := $(foreach b,$(BACKENDS),$(foreach t,$(TESTS),bin/$(b)/$(t)))
ALLOC_BINS := $(addprefix bin/, $(ALLOC_TESTS))
all: $(BINS) $(ALLOC_BINS)

# ── Run targets ────────────────────────────────────────────────────
RUN_TARGETS := $(addprefix run-,$(BACKENDS))

$(RUN_TARGETS): run-%: $(foreach t,$(TESTS),bin/%/$(t))
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
