include ../../../plugin/llvm-tool-helper.mk
CLANG_TM   := ../../../plugin/clang-tm
RUNTIMES   := ../../../backends/tm_impl
BACKENDS   := ../../../backends

CXXFLAGS   := -std=c++20 -O3 -pthread -I$(BACKENDS)/tm_impl/common \
              -I$(BACKENDS)/stubs

BIN_DIR    := bin

.PHONY: all clean

all: $(BIN_DIR) \
	social_tm_uninstrumented \
	social_tm_norec \
	social_tm_tinystm_wbctl \
	social_tm_tinystm_wt

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

social_tm_uninstrumented: social_tm.cpp | $(BIN_DIR)
	$(LLVM_CXX) $(CXXFLAGS) $< -o $(BIN_DIR)/$@

social_tm_norec: social_tm.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/norec/NOrec_runtime.cpp -I$(BACKENDS)/tm_impl/norec -o $(BIN_DIR)/$@ $<

social_tm_tinystm_wbctl: social_tm.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -DDESIGN_WBCTL -o $(BIN_DIR)/$@ $<

social_tm_tinystm_wt: social_tm.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp -I$(BACKENDS)/tm_impl/tiny_stm -DDESIGN_WT -o $(BIN_DIR)/$@ $<

# ── Live-app simulation .so ───────────────────────────────────
# Build social_tm.cpp as a shared library with TM instrumentation,
# but WITHOUT linking any TM runtime.  All tm_* symbols remain
# undefined and are resolved at load time by libsim_backend.so.
PLUGIN_SO := $(shell ls ../../../plugin/bin/libTMInstrument*.so 2>/dev/null | head -1)

# Match Rust binary architecture
UNAME_M := $(shell uname -m)
TM_TARGET :=
ifeq ($(UNAME_M),arm64)
  RUST_HOST := $(shell rustc -vV 2>/dev/null | grep 'host:' | awk '{print $$2}')
  ifneq ($(RUST_HOST),aarch64-apple-darwin)
    TM_TARGET := -target x86_64-apple-darwin
  endif
endif

social_tm_live.so: social_tm.cpp social_tm_live_wrapper.cpp | $(BIN_DIR)
	$(LLVM_CXX) $(TM_TARGET) -std=c++20 -O1 -fno-inline -emit-llvm -c \
	  -I$(BACKENDS)/tm_impl/common -I$(BACKENDS)/stubs \
	  -DLLVM_TM_PLUGIN \
	  -o $(BIN_DIR)/social_tm_live.bc social_tm.cpp
	$(LLVM_OPT) -load-pass-plugin=$(PLUGIN_SO) \
	  -passes=tm-instrument-then-inline \
	  -o $(BIN_DIR)/social_tm_live.instr.bc $(BIN_DIR)/social_tm_live.bc
	$(LLVM_OPT) -O3 \
	  -o $(BIN_DIR)/social_tm_live.opt.bc $(BIN_DIR)/social_tm_live.instr.bc
	$(LLVM_CXX) $(TM_TARGET) -std=c++20 -O1 -fno-inline -emit-llvm -c \
	  -o $(BIN_DIR)/wrapper.bc social_tm_live_wrapper.cpp
	$(LLVM_OPT) -O3 \
	  -o $(BIN_DIR)/wrapper.opt.bc $(BIN_DIR)/wrapper.bc
	$(LLVM_CXX) $(TM_TARGET) -shared -fPIC -O3 -pthread \
	  -undefined dynamic_lookup -Wl,-flat_namespace \
	  -o $(BIN_DIR)/$@ $(BIN_DIR)/social_tm_live.opt.bc $(BIN_DIR)/wrapper.opt.bc

clean:
	rm -rf $(BIN_DIR)
