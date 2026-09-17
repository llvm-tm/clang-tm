# llvm-tool-helper.mk — LLVM tool discovery for Makefiles
#
# Include this before using $(LLVM_CXX), $(LLVM_OPT), etc.
#
# LLVM_VERSION selects the desired LLVM major version (default: 22). Search
# order is llvm-config-<major>, llvm-config-<major>.1, then the legacy
# llvm-config-22/22.1/generic fallbacks. When LLVM_VERSION is explicitly set
# from the environment or command line, the discovered major version must match
# it. The default remains LLVM 22; CI may explicitly set LLVM_VERSION=23.
#
# Usage:
#   include /path/to/llvm-tool-helper.mk
#   $(LLVM_CXX) -std=c++20 ...

LLVM_VERSION ?= 22
LLVM_VERSION_MAJOR_REQ := $(firstword $(subst ., ,$(LLVM_VERSION)))
LLVM_VERSION_STRICT := $(if $(filter environment environment-override command-line,$(origin LLVM_VERSION)),1,)

LLVM_CONFIG_CANDIDATES := llvm-config-$(LLVM_VERSION) llvm-config-$(LLVM_VERSION).1
ifeq ($(LLVM_VERSION),22)
LLVM_CONFIG_CANDIDATES += llvm-config-22 llvm-config-22.1 llvm-config
else
LLVM_CONFIG_CANDIDATES += llvm-config
endif
LLVM_CONFIG := $(firstword $(foreach cfg,$(LLVM_CONFIG_CANDIDATES),$(shell command -v $(cfg) 2>/dev/null || true)))

ifeq ($(LLVM_CONFIG),)
ifneq ($(LLVM_VERSION_STRICT),)
$(error LLVM $(LLVM_VERSION) requested explicitly, but llvm-config-$(LLVM_VERSION) was not found)
endif
endif

ifneq ($(LLVM_CONFIG),)
LLVM_VER := $(shell $(LLVM_CONFIG) --version 2>/dev/null)
LLVM_MAJOR := $(firstword $(subst ., ,$(LLVM_VER)))
ifneq ($(LLVM_MAJOR),)
ifneq ($(LLVM_VERSION_STRICT),)
ifneq ($(LLVM_MAJOR),$(LLVM_VERSION_MAJOR_REQ))
$(error LLVM $(LLVM_VERSION) requested explicitly, but $(LLVM_CONFIG) reports $(LLVM_VER))
endif
endif
ifeq ($(shell test "$(LLVM_MAJOR)" -lt 22 2>/dev/null && echo yes),yes)
$(error LLVM 22+ required, found $(LLVM_VER))
endif
endif
LLVM_BINDIR := $(shell $(LLVM_CONFIG) --bindir 2>/dev/null)
else
LLVM_BINDIR :=
endif

LLVM_CXX    := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/clang++,clang++)
LLVM_OPT    := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/opt,opt)
LLVM_LINK   := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/llvm-link,llvm-link)
LLVM_DIS    := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/llvm-dis,llvm-dis)
LLVM_CC     := $(if $(LLVM_BINDIR),$(LLVM_BINDIR)/clang,clang)

ifneq ($(LLVM_CONFIG),)
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags 2>/dev/null)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags 2>/dev/null)
LLVM_LIBS     := $(shell $(LLVM_CONFIG) --libs core analysis passes 2>/dev/null)
else
LLVM_CXXFLAGS :=
LLVM_LDFLAGS  :=
LLVM_LIBS     :=
endif
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
