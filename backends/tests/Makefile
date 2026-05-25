# Backend unit tests — compile directly against runtimes (no LLVM plugin)
# Each test source is compiled once per backend.

include ../../llvm_tm_plugin/llvm-tool-helper.mk
CXX := $(LLVM_CXX)
CXXFLAGS := -std=c++20 -O0 -pthread -g
BIN_DIR := bin
RUNTIME_DIR := ../runtimes

# Backend definitions: each has include paths, a TM_BACKEND_* define,
# optional DESIGN_* define, and a runtime source file.
#
# TinySTM has three flavors sharing the same runtime.
define TINYSTM_FLAVOR_template
$(1)_CXXFLAGS := -I../TinySTM -I.. -DTM_BACKEND_TINYSTM $(2)
$(1)_RUNTIME := $$(RUNTIME_DIR)/TinySTM_runtime.cpp
endef

$(eval $(call TINYSTM_FLAVOR_template,tinystm_wbctl,-DDESIGN_WBCTL))
$(eval $(call TINYSTM_FLAVOR_template,tinystm_wbetl,-DDESIGN_WBETL))
$(eval $(call TINYSTM_FLAVOR_template,tinystm_wt,-DDESIGN_WT))

# Other backends
define BACKEND_template
$(1)_CXXFLAGS := $(2)
$(1)_RUNTIME := $(3)
endef

$(eval $(call BACKEND_template,tl2,-I../TL2 -I.. -DTM_BACKEND_TL2,$(RUNTIME_DIR)/tl2_runtime.cpp))
$(eval $(call BACKEND_template,norec,-I../NOrec -I.. -DTM_BACKEND_NOREC,$(RUNTIME_DIR)/NOrec_runtime.cpp))
$(eval $(call BACKEND_template,swisstm,-I../SwissTM -I.. -DTM_BACKEND_SWISSTM,$(RUNTIME_DIR)/SwissTM_runtime.cpp))
$(eval $(call BACKEND_template,sgl,-I.. -DTM_BACKEND_SGL,$(RUNTIME_DIR)/SingleGlobalLock_runtime.cpp))

BACKENDS := tinystm_wbctl tinystm_wbetl tinystm_wt tl2 norec swisstm sgl
TESTS := test_single test_multi

# ── Build rules ───────────────────────────────────────────────────────

.PHONY: all clean run

# All backends × all tests
TARGETS := $(foreach B,$(BACKENDS),$(foreach T,$(TESTS),$(BIN_DIR)/$(B)_$(T)))

all: $(BIN_DIR) $(TARGETS)

# Pattern rule: $(BIN_DIR)/{backend}_{test} → test_{test}.cpp + runtime
# $* is the full backend name, e.g. "tinystm_wbctl".
$(BIN_DIR)/%_test_single: test_single.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $($*_CXXFLAGS) \
		-o $@ $< $($*_RUNTIME)

$(BIN_DIR)/%_test_multi: test_multi.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $($*_CXXFLAGS) \
		-o $@ $< $($*_RUNTIME)

# ── Run ───────────────────────────────────────────────────────────────

run: all
	@set -e; \
	for b in $(BACKENDS); do \
		echo ""; \
		echo "================================================"; \
		echo "  Backend: $$b"; \
		echo "================================================"; \
		for t in $(TESTS); do \
			echo ""; \
			echo "--- $$t ---"; \
			$(BIN_DIR)/$${b}_$${t}; \
		done; \
	done
	@echo ""; \
	echo "================================================"; \
	echo "  ALL TESTS COMPLETE"; \
	echo "================================================"

# Run a single backend
run_%: $(BIN_DIR)/%_test_single $(BIN_DIR)/%_test_multi
	@echo "=== $* test_single ===" && $(BIN_DIR)/$*_test_single
	@echo "=== $* test_multi ===" && $(BIN_DIR)/$*_test_multi

# ── Directories ──────────────────────────────────────────────────────

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR)

# ── Debug: print backend/target variables ────────────────────────────

print-vars:
	@echo "BACKENDS: $(BACKENDS)"
	@echo "TESTS: $(TESTS)"
	@echo "TARGETS: $(TARGETS)"
	@for b in $(BACKENDS); do \
		echo "$$b CXXFLAGS: $$($$b _CXXFLAGS)"; \
		echo "$$b RUNTIME: $$($$b _RUNTIME)"; \
	done

# ── Pattern rule helpers ─────────────────────────────────────────────
# Extract the backend prefix from a target like "tinystm_wbctl_test_single"
# The first word after splitting on '_' gives the base backend name.
# For tinystm_wbctl, firstword gives "tinystm" which doesn't match the
# variable "tinystm_wbctl_CXXFLAGS".  Use a two-level approach.
#
# Actually, the pattern rule matches %_test_single where % is the full
# backend name (e.g., "tinystm_wbctl").  So $* gives "tinystm_wbctl".
# Then $(tinystm_wbctl_CXXFLAGS) should resolve correctly.
#
# But make variable references are flat: $($*_CXXFLAGS) where $* =
# "tinystm_wbctl" gives "$(tinystm_wbctl_CXXFLAGS)" which is not the
# same as "tinystm_wbctl_CXXFLAGS" with underscores — make concatenates
# literally.  Since the variable itself is named "tinystm_wbctl_CXXFLAGS",
# and $* is "tinystm_wbctl", then $($*_CXXFLAGS) = $(tinystm_wbctl_CXXFLAGS)
# which IS correct!  Make substitutes $* first, then appends _CXXFLAGS.
#
# So the pattern rules above work correctly.
