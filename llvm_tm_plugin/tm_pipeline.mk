# ============================================================
# tm_pipeline.mk — TM Plugin Compilation Pipeline
#
# Include in your Makefile to get the canonical pipeline:
#   1. clang++ -fno-inline -emit-llvm              (.bc)
#   2. opt -load-pass-plugin (instrumentation)     (.instr.bc)
#   3. opt -O3 (optimize instrumented IR)          (.opt.bc)
#   4. clang++ (link with STM runtime)             (binary)
#
# Pipeline variants (TM_INSTRUMENT_PIPELINE):
#   tm-instrument-inline (default) — inlines then instruments (176 TM ops)
#   tm-instrument             — clones survive as separate functions (38 TM ops)
#   tm-instrument-then-inline — instruments clones pre-inline then inlines (204 TM ops)
#
# BUILD_TYPE:
#   RELEASE (default) — tm-instrument-inline, -O3, -O1 link
#   DEBUG             — tm-instrument, -O0, -O0 -g link (see llvm_tm_plugin/DEBUG.md)
#
# Optional: Opaque symbol resolution step (runs between 2 and 4):
#   tm-resolve-opaque.py resolves system library symbols (e.g., sqrt, cos)
#   and generates LLVM IR stub declarations for the link step.
#
# Usage:
#   include path/to/tm_pipeline.mk
#
#   # Create pattern rules for .bc -> .instr.bc -> .opt.bc
#   # (sources in current directory):
#   $(eval $(call tm_define_rules))
#   # (sources in test/ subdirectory):
#   $(eval $(call tm_define_rules, test/))
#
#   # Create a complete binary target:
#   $(eval $(call tm_target, binary_name, source.cpp, backend))
#
#   # Or compose manually:
#   out/%.bc: %.cpp
#       $(call tm_compile_ir,$<,$@)
#   out/%.instr.bc: out/%.bc
#       $(call tm_instrument,$<,$@)
#   out/%.opt.bc: out/%.instr.bc
#       $(call tm_optimize,$<,$@)
#   bin/myapp: out/myapp.opt.bc
#       $(call tm_link,$<,tinystm,$@)
#
#   # With opaque symbol resolution:
#   TM_OPAQUE_SYMBOLS_FILE = out/opaque_symbols.txt
#   TM_LINK_LIBS = -lm
#   bin/myapp: out/myapp.opt.bc
#       $(call tm_instrument,$<,$@)  # writes $(TM_OPAQUE_SYMBOLS_FILE)
#       $(call tm_resolve_opaque,$(TM_OPAQUE_SYMBOLS_FILE))
#       $(call tm_link,$<,tinystm,$@)
#
# Available backends:
#   tinystm, tl2, singlelock, swisstm, norec
# ============================================================

# ---- Configurable paths (override before include if needed) ----

.DEFAULT_GOAL   := all

# Optimization level for the post-instrumentation pass.
# Default: -O3 (inlines tm_read/tm_write). Set to -O0 for debugging.
TM_OPT_LEVEL     ?= -O3
TM_LINK_OPT      ?= -O1

# Compile flags for source → LLVM bitcode (step 1 of pipeline).
TM_COMPILE_FLAGS ?= -O1 -fno-inline -fno-vectorize -fno-slp-vectorize \
                    -fno-unroll-loops -fno-stack-protector -pthread

CXXFLAGS         ?= -std=c++20 -O1 -pthread
LLVM_PLUGIN_DIR  := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BACKENDS_DIR     ?= $(abspath $(LLVM_PLUGIN_DIR)/../backends)

# Discover LLVM tools via clang-tm script (handles versioned installs robustly)
# clang-tm auto-discovers the correct clang++, opt, and llvm-link via llvm-config
CLANG_TM         := $(LLVM_PLUGIN_DIR)/clang-tm
CXX              := $(shell $(CLANG_TM) --print-tool cxx 2>/dev/null || \
                       $(or $(shell command -v clang++-22 2>/dev/null),clang++))
OPT              := $(shell $(CLANG_TM) --print-tool opt 2>/dev/null || \
                       $(or $(shell command -v opt-22 2>/dev/null),opt))
LLVM_LINK        := $(shell $(CLANG_TM) --print-tool llvm-link 2>/dev/null || \
                       $(or $(shell command -v llvm-link-22 2>/dev/null),llvm-link))
RUNTIMES_DIR     ?= $(BACKENDS_DIR)/runtimes
TINYSTM_DIR      ?= $(BACKENDS_DIR)/TinySTM
TL2_DIR          ?= $(BACKENDS_DIR)/TL2
SWISSTM_DIR      ?= $(BACKENDS_DIR)/SwissTM
NOREC_DIR        ?= $(BACKENDS_DIR)/NOrec
DUDETM_DIR       ?= $(BACKENDS_DIR)/DUDETM
SPHT_DIR          ?= $(BACKENDS_DIR)/SPHT
OUT_DIR          ?= out
BIN_DIR          ?= bin
TM_PLUGIN        ?= $(BIN_DIR)/libTMInstrument.so

# Include machine-local config (if it exists) for arch-specific flags
# e.g., TM_DEFINES_tsxsgl = -mrtm
-include $(abspath $(LLVM_PLUGIN_DIR)/../machine_config.mk)

# ---- Backend mappings ----

TM_RUNTIME_tinystm      = $(RUNTIMES_DIR)/TinySTM_runtime.cpp
TM_RUNTIME_tl2          = $(RUNTIMES_DIR)/tl2_runtime.cpp
TM_RUNTIME_singlelock   = $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp
TM_RUNTIME_swisstm      = $(RUNTIMES_DIR)/SwissTM_runtime.cpp
TM_RUNTIME_norec        = $(RUNTIMES_DIR)/NOrec_runtime.cpp
TM_RUNTIME_persistentsgl = $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp
TM_RUNTIME_distributedsgl = $(RUNTIMES_DIR)/DistributedSGL_runtime.cpp
TM_RUNTIME_tsxsgl        = $(RUNTIMES_DIR)/TSXSGL_runtime.cpp
TM_RUNTIME_tinystm_wbctl = $(RUNTIMES_DIR)/TinySTM_runtime.cpp
TM_RUNTIME_tinystm_wbetl = $(RUNTIMES_DIR)/TinySTM_runtime.cpp
TM_RUNTIME_tinystm_wt    = $(RUNTIMES_DIR)/TinySTM_runtime.cpp
TM_RUNTIME_dudetm        = $(RUNTIMES_DIR)/DUDETM_runtime.cpp
TM_RUNTIME_spht          = $(RUNTIMES_DIR)/SPHT_runtime.cpp

TM_DEFINES_tinystm      = -DDESIGN_WBCTL
TM_DEFINES_tl2          =
TM_DEFINES_singlelock   =
TM_DEFINES_norec        =
TM_DEFINES_persistentsgl =
TM_DEFINES_distributedsgl =
TM_DEFINES_tsxsgl       ?=
TM_DEFINES_tinystm_wbctl = -DDESIGN_WBCTL -DNDEBUG
TM_DEFINES_tinystm_wbetl = -DDESIGN_WBETL -DNDEBUG
TM_DEFINES_tinystm_wt    = -DDESIGN_WT -DNDEBUG
TM_DEFINES_dudetm        = -DDESIGN_WBCTL

TM_INCLUDES_persistentsgl =
TM_INCLUDES_distributedsgl =
TM_INCLUDES_tl2          = -I$(TL2_DIR) -I$(BACKENDS_DIR)
TM_INCLUDES_singlelock   =
TM_INCLUDES_norec        = -I$(NOREC_DIR) -I$(BACKENDS_DIR)
TM_INCLUDES_tinystm     = -I$(TINYSTM_DIR) -I$(BACKENDS_DIR)
TM_INCLUDES_norec       = -I$(NOREC_DIR) -I$(BACKENDS_DIR)
TM_INCLUDES_tinystm_wbctl = -I$(TINYSTM_DIR) -I$(BACKENDS_DIR)
TM_INCLUDES_tinystm_wbetl = -I$(TINYSTM_DIR) -I$(BACKENDS_DIR)
TM_INCLUDES_tinystm_wt    = -I$(TINYSTM_DIR) -I$(BACKENDS_DIR)
TM_DEFINES_spht          = -DTM_BACKEND_SPHT -mrtm
TM_INCLUDES_spht         = -I$(SPHT_DIR) -I$(BACKENDS_DIR)

TM_INCLUDES_dudetm        = -I$(TINYSTM_DIR) -I$(DUDETM_DIR) -I$(BACKENDS_DIR)

# ---- Opaque symbol resolution (external tools) ----

TM_RESOLVE_OPAQUE ?= $(LLVM_PLUGIN_DIR)/tm-resolve-opaque.py
TM_OPAQUE_SYMBOLS_FILE ?=
TM_OPAQUE_STUBS_DIR  ?= $(OUT_DIR)/opaque-resolved
TM_OPAQUE_STUBS      ?= $(TM_OPAQUE_STUBS_DIR)/tm-opaque-resolved.bc

# Libraries added at the final link step (e.g., -lm for math functions).
# These are only needed when opaque system library functions are called
# inside TM transactions with -tm-allow-opaque.
TM_LINK_LIBS ?=

# ---- Canned recipes (individual steps) ----

define tm_compile_ir
	$(CLANG_TM) --compile-only -std=c++20 $(TM_COMPILE_FLAGS) $1 -o $2
endef

define tm_compile_ir_debug
	$(CLANG_TM) --compile-only -std=c++20 -O0 -g -fno-inline \
		-fno-vectorize -fno-slp-vectorize -fno-unroll-loops \
		-fno-stack-protector -pthread $1 -o $2
endef

# Default: non-inline pipeline avoids write-set/memory asymmetry for local containers.
# See AGENTS.md "Key Decisions" for reasoning.
TM_INSTRUMENT_PIPELINE ?= tm-instrument

define tm_instrument
$(CLANG_TM) --instrument-only --plugin=$(TM_PLUGIN) \
    -passes="$(TM_INSTRUMENT_PIPELINE)" $(TM_INSTRUMENT_FLAGS) \
    $(if $(TM_OPAQUE_SYMBOLS_FILE),--opaque-symbols-file=$(TM_OPAQUE_SYMBOLS_FILE)) \
    $1 -o $2
endef

define tm_optimize
$(CLANG_TM) --optimize-only $(TM_OPT_LEVEL) $1 -o $2
endef

define tm_resolve_opaque
mkdir -p $(TM_OPAQUE_STUBS_DIR)
$(TM_RESOLVE_OPAQUE) --symbols $1 --output $(TM_OPAQUE_STUBS_DIR)
endef

define tm_link
# Use clang-tm in link-only mode: compiles runtime to BC, merges, optimizes, links.
# $(1) = .opt.bc input, $(2) = backend name, $(3) = output binary
$(CLANG_TM) --link-only --runtime=$(TM_RUNTIME_$(strip $2)) \
    $(TM_LINK_OPT) -pthread \
    $(TM_DEFINES_$(strip $2)) $(TM_INCLUDES_$(strip $2)) \
    $(strip $1) -o $(strip $3)
endef

# ---- Convenience: create pattern rules ----
# $(eval $(call tm_define_rules))         — sources in current dir
# $(eval $(call tm_define_rules, test/))  — sources in test/ dir

define tm_define_rules
$(OUT_DIR)/%.bc: $(strip $1)%.cpp $(OUT_DIR)
	$$(call tm_compile_ir,$$<,$$@)

$(OUT_DIR)/%.instr.bc: $(OUT_DIR)/%.bc $(TM_PLUGIN) $(OUT_DIR)
	$$(call tm_instrument,$$<,$$@)

$(OUT_DIR)/%.opt.bc: $(OUT_DIR)/%.instr.bc $(OUT_DIR)
	$$(call tm_optimize,$$<,$$@)
endef

# ---- Convenience: create a complete binary target ----
# $(eval $(call tm_target, binary_name, source.cpp, backend))

define tm_target
$(addprefix $(BIN_DIR)/,$1): $(addprefix $(OUT_DIR)/,$(addsuffix .opt.bc,$(basename $(notdir $2)))) $(TM_RUNTIME_$(strip $3)) | $(BIN_DIR)
	$$(call tm_link,$$<,$3,$$@)

$(1): $(addprefix $(BIN_DIR)/,$1)
endef

# Keep optimized IR (prevent Make from deleting intermediate pattern rule outputs)
.PRECIOUS: $(OUT_DIR)/%.opt.bc

# ---- Directory creation ----

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# ---- Clean helper ----

.PHONY: tm_clean
tm_clean:
	rm -f $(OUT_DIR)/*.bc $(OUT_DIR)/*.instr.bc $(OUT_DIR)/*.opt.bc
