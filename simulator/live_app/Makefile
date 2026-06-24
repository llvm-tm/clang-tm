CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -fPIC -pthread

# Match Rust binary architecture (x86_64 on Rosetta, arm64 otherwise)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),arm64)
  # Check if Rust is x86_64 under Rosetta
  RUST_HOST := $(shell rustc -vV 2>/dev/null | grep 'host:' | awk '{print $$2}')
  ifneq ($(RUST_HOST),aarch64-apple-darwin)
    CXXFLAGS += -target x86_64-apple-darwin
  endif
endif

all: libsim_backend.so

UFLAGS :=
# macOS: allow unresolved symbols (resolved at runtime by Rust binary)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
UFLAGS += -undefined dynamic_lookup -Wl,-flat_namespace
else
UFLAGS += --allow-shlib-undefined
endif

libsim_backend.so: sim_backend.cpp
	$(CXX) $(CXXFLAGS) $(UFLAGS) -shared -o $@ $^

clean:
	rm -f libsim_backend.so
