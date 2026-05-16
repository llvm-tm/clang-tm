# ============================================================
# tm_pipeline.mk — TM Plugin Compilation Pipeline
#
# Include in your Makefile to get the canonical 4-step pipeline:
#   1. clang++ -O3 -fno-inline -emit-llvm       (.bc)
#   2. opt -load-pass-plugin (instrumentation)   (.instr.bc)
#   3. opt -O3 (optimize instrumented IR)        (.opt.bc)
#   4. clang++ (link with STM runtime)           (binary)
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
# Available backends:
#   tinystm, tl2, singlelock, swisstm, norec
# ============================================================

# ---- Configurable paths (override before include if needed) ----

.DEFAULT_GOAL   := all

# Discover LLVM tools via llvm-config (handles versioned installs)
LLVM_BINDIR      := $(shell llvm-config --bindir 2>/dev/null)
CXX              := $(LLVM_BINDIR)/clang++
OPT              := $(LLVM_BINDIR)/opt
LLVM_LINK        := $(LLVM_BINDIR)/llvm-link
CXXFLAGS         ?= -std=c++20 -O1 -pthread
LLVM_PLUGIN_DIR  ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BACKENDS_DIR     ?= $(abspath $(LLVM_PLUGIN_DIR)/../backends)
RUNTIMES_DIR     ?= $(BACKENDS_DIR)/runtimes
TINYSTM_DIR      ?= $(BACKENDS_DIR)/TinySTM
TL2_DIR          ?= $(BACKENDS_DIR)/TL2
SWISSTM_DIR      ?= $(BACKENDS_DIR)/SwissTM
NOREC_DIR        ?= $(BACKENDS_DIR)/NOrec
OUT_DIR          ?= out
BIN_DIR          ?= bin
TM_PLUGIN        ?= $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

# ---- Backend mappings ----

TM_RUNTIME_tinystm      = $(RUNTIMES_DIR)/TinySTM_runtime.cpp
TM_RUNTIME_tl2          = $(RUNTIMES_DIR)/tl2_runtime.cpp
TM_RUNTIME_singlelock   = $(RUNTIMES_DIR)/SingleGlobalLock_runtime.cpp
TM_RUNTIME_swisstm      = $(RUNTIMES_DIR)/SwissTM_runtime.cpp
TM_RUNTIME_norec        = $(RUNTIMES_DIR)/NOrec_runtime.cpp
TM_RUNTIME_persistentsgl = $(RUNTIMES_DIR)/PersistentSGL_runtime.cpp
TM_RUNTIME_distributedsgl = $(RUNTIMES_DIR)/DistributedSGL_runtime.cpp

TM_DEFINES_tinystm      = -DDESIGN_WBCTL
TM_DEFINES_tl2          =
TM_DEFINES_singlelock   =
TM_DEFINES_norec        =
TM_DEFINES_persistentsgl =
TM_DEFINES_distributedsgl =

TM_INCLUDES_persistentsgl =
TM_INCLUDES_distributedsgl =
TM_INCLUDES_tinystm     = -I$(TINYSTM_DIR) -I$(BACKENDS_DIR)
TM_INCLUDES_norec       = -I$(NOREC_DIR) -I$(BACKENDS_DIR)

# ---- Canned recipes (individual steps) ----

define tm_compile_ir
	$(CXX) -std=c++20 -O3 -fno-inline -emit-llvm -c $1 -o $2 -fno-stack-protector -pthread
endef

define tm_compile_ir_debug
	$(CXX) -std=c++20 -O0 -g -emit-llvm -c $1 -o $2 -fno-stack-protector -pthread
endef

define tm_instrument
$(OPT) -load-pass-plugin=$(TM_PLUGIN) -passes="tm-instrument" $1 -o $2
endef

define tm_optimize
$(OPT) -O3 $1 -o $2
endef

define tm_link
# Compile runtime to bitcode (no -fno-inline — runtime must be eligible for inlining)
$(CXX) -std=c++20 -O3 -emit-llvm -c $(TM_RUNTIME_$(strip $2)) -o $@.runtime.bc $(TM_DEFINES_$(strip $2)) $(TM_INCLUDES_$(strip $2)) -fno-stack-protector -pthread
# Merge instrumented IR with runtime bitcode
$(LLVM_LINK) $1 $@.runtime.bc -o $@.merged.bc
# Optimize merged IR (inlines tm_read/tm_write etc.)
$(OPT) -O3 $@.merged.bc -o $@.merged.opt.bc
# Final link
$(CXX) $(CXXFLAGS) $(TM_DEFINES_$(strip $2)) $@.merged.opt.bc -o $3 $(TM_INCLUDES_$(strip $2))
# Cleanup intermediate files
rm -f $@.runtime.bc $@.merged.bc $@.merged.opt.bc
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
