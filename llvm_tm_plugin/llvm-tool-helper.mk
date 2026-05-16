# llvm-tool-helper.mk — LLVM tool discovery for Makefiles
#
# Include this before using $(LLVM_CXX), $(LLVM_OPT), etc.
# Looks for llvm-config-22, llvm-config-22.1, llvm-config in order,
# then derives tool paths from --bindir output. Falls back to bare names.
#
# Usage:
#   include /path/to/llvm-tool-helper.mk
#   $(LLVM_CXX) -std=c++20 ...

LLVM_CONFIG := $(shell command -v llvm-config-22 2>/dev/null || command -v llvm-config-22.1 2>/dev/null || command -v llvm-config 2>/dev/null || echo llvm-config)
LLVM_BINDIR := $(shell $(LLVM_CONFIG) --bindir 2>/dev/null)
LLVM_CXX    := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/clang++,clang++)
LLVM_OPT    := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/opt,opt)
LLVM_LINK   := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/llvm-link,llvm-link)
LLVM_DIS    := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/llvm-dis,llvm-dis)
LLVM_CC     := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/clang,clang)
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags 2>/dev/null)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags 2>/dev/null)
LLVM_LIBS     := $(shell $(LLVM_CONFIG) --libs core analysis passes 2>/dev/null)
