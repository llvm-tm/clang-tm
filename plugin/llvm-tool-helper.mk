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
# clang 22 prefers gcc 16 headers, but libstdc++-16-dev may be missing.
# ONLY when /usr/include/c++/16 is absent, fall back to the newest available
# GCC toolchain that has headers, so -I search finds C++ headers.  The flag is
# appended to LLVM_CXX so ALL targets using $(LLVM_CXX) inherit it.
ifneq ($(LLVM_CXX),)
  LLVM_STDLIB_FLAG :=
  ifeq ($(wildcard /usr/include/c++/16),)
    ifneq ($(wildcard /usr/lib/gcc/x86_64-linux-gnu/15),)
      LLVM_STDLIB_FLAG += --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/15
    else ifneq ($(wildcard /usr/lib/gcc/x86_64-linux-gnu/12),)
      LLVM_STDLIB_FLAG += --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/12
    endif
  endif
  LLVM_CXX := $(LLVM_CXX) $(LLVM_STDLIB_FLAG)
endif
