# Backend unit tests — compile directly against runtimes (no LLVM plugin)
# Each test is compiled once per backend, producing:
#   bin/<backend>/<test_name>

include ../../llvm_tm_plugin/llvm-tool-helper.mk
CXX := $(LLVM_CXX)
CXXFLAGS := -std=c++20 -O0 -pthread -g
RUNTIME_DIR := ../runtimes
INC_BASE := -I.. -I$(RUNTIME_DIR)

# ── Backend definitions ────────────────────────────────────────────
BACKENDS := tl2 tinystm wt norec swisstm nvhtm spht

INC_tl2     := $(INC_BASE) -I../TL2
RUNTIME_tl2 := $(RUNTIME_DIR)/tl2_runtime.cpp
FLAGS_tl2   := -DTM_BACKEND_TL2

INC_tinystm := $(INC_BASE) -I../TinySTM
RUNTIME_tinystm := $(RUNTIME_DIR)/TinySTM_runtime.cpp
FLAGS_tinystm := -DDESIGN_WBCTL -DTM_BACKEND_TINYSTM

INC_wt := $(INC_BASE) -I../TinySTM
RUNTIME_wt := $(RUNTIME_DIR)/TinySTM_runtime.cpp
FLAGS_wt := -DDESIGN_WT -DTM_BACKEND_TINYSTM

INC_norec   := $(INC_BASE) -I../NOrec
RUNTIME_norec := $(RUNTIME_DIR)/NOrec_runtime.cpp
FLAGS_norec := -DTM_BACKEND_NOREC

INC_swisstm := $(INC_BASE) -I../SwissTM
RUNTIME_swisstm := $(RUNTIME_DIR)/SwissTM_runtime.cpp
FLAGS_swisstm := -DTM_BACKEND_SWISSTM

INC_nvhtm := $(INC_BASE) -I../NVHTM
RUNTIME_nvhtm := $(RUNTIME_DIR)/NVHTM_runtime.cpp
FLAGS_nvhtm := -DTM_BACKEND_NVHTM -mrtm

INC_spht := $(INC_BASE) -I../SPHT
RUNTIME_spht := $(RUNTIME_DIR)/SPHT_runtime.cpp
FLAGS_spht := -DTM_BACKEND_SPHT -mrtm

# ── Test source files ──────────────────────────────────────────────
TESTS := counter_st counter_mt write_set_validation eager_read_null_address

.PHONY: all clean list run% list-backends

# ── Explicit per-backend directory rules ───────────────────────────
$(foreach b,$(BACKENDS),$(eval bin/$(b)/: ; mkdir -p $$@))

# ── Per-backend, per-test build rules ──────────────────────────────
# Generate: bin/<b>/<t>: test_<t>.cpp <runtime> test_helpers.hpp | bin/<b>/
define BUILD_RULE
bin/$(1)/$(2): test_$(2).cpp $(RUNTIME_$(1)) test_helpers.hpp | bin/$(1)/
	$$(CXX) $$(CXXFLAGS) $$(INC_$(1)) $$(FLAGS_$(1)) -o $$@ $$< $$(RUNTIME_$(1))
endef

# Flatten: for each (backend, test) pair, eval a build rule
RULES := $(foreach b,$(BACKENDS),$(foreach t,$(TESTS),$(eval $(call BUILD_RULE,$(b),$(t)))))

# ── All target ─────────────────────────────────────────────────────
BINS := $(foreach b,$(BACKENDS),$(foreach t,$(TESTS),bin/$(b)/$(t)))
all: $(BINS)

# ── Run targets ────────────────────────────────────────────────────
RUN_TARGETS := $(addprefix run-,$(BACKENDS))

$(RUN_TARGETS): run-%:
	@echo "=== Backend: $* ==="
	@$(foreach t,$(TESTS),\
		echo "--- $(t) ---"; \
		bin/$*/$(t) 2>&1; \
		echo;)

run: $(RUN_TARGETS)
	@echo "=== All backends done ==="

# ── Convenience ────────────────────────────────────────────────────
list:
	@echo "Backends:" $(BACKENDS)
	@echo "Tests:   " $(TESTS)
	@echo "Targets: " $(BINS)

clean:
	rm -rf bin
