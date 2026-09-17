# Top-level Makefile for TM API C++ Project
# Builds: LLVM plugin, plugin benchmarks, expli benchmarks, tests

include plugin/llvm-tool-helper.mk
LLVM_CONFIG_ARGS := $(shell $(LLVM_CONFIG) --cxxflags --ldflags --system-libs --libs core analysis 2>/dev/null || echo "-I$(shell brew --prefix llvm 2>/dev/null || echo /usr/lib/llvm-22)/include -L$(shell brew --prefix llvm 2>/dev/null || echo /usr/lib/llvm-22)/lib -lLLVM-22")

CXX ?= $(LLVM_CXX)
CXXFLAGS += -std=c++20 -pthread
ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g
else
  CXXFLAGS += -O2
endif
BACKEND ?= tinystm

PROJECT_ROOT := $(shell pwd)
LLVM_PLUGIN_DIR := $(PROJECT_ROOT)/plugin
BACKENDS_DIR := $(PROJECT_ROOT)/backends
PLUGIN_BENCHMARKS_DIR := $(PROJECT_ROOT)/benchmarks/plugin
EXPLI_BENCHMARKS_DIR := $(PROJECT_ROOT)/benchmarks/cpp
TESTS_DIR := $(PROJECT_ROOT)/tests

PLUGIN := $(LLVM_PLUGIN_DIR)/bin/libTMInstrument.so

# `make` with no args prints help (discovery-first for newcomers).
# Build everything with `make all`; run the ~60s smoke test with `make check-fast`.
.DEFAULT_GOAL := help

.PHONY: all clean plugin plugin-benchmarks expli-benchmarks tests check check-fast check-all post-merge-check compiledb clean-book clean-proofs help info test_run fmt fmt-check

all: info plugin plugin-benchmarks expli-benchmarks

info:
	@echo "TM API C++ Build System"
	@echo "==================="
	@echo "Project Root:  $(PROJECT_ROOT)"
	@echo "LLVM Config: $(LLVM_CONFIG)"
	@echo "Backend:    $(BACKEND)"
	@echo "CXX:       $(CXX)"
	@echo ""

plugin:
	$(MAKE) -C $(LLVM_PLUGIN_DIR)

plugin-benchmarks: plugin
	@echo "Building plugin benchmarks..."
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/bank bank_tinystm BACKEND=$(BACKEND) 2>&1
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/bank bank_singlelock BACKEND=$(BACKEND) 2>&1
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/datastructures bin/avltree_SingleGlobalLock 2>&1
	@$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/STAMP stamp_uninstrumented 2>&1
	@echo "Plugin benchmarks built."

expli-benchmarks:
	@echo "Building expli benchmarks..."
	@$(MAKE) -C $(EXPLI_BENCHMARKS_DIR) all BACKEND=$(BACKEND) 2>&1
	@echo "Expli benchmarks built."

tests: plugin
	@echo "Building all tests..."
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) clean 2>&1
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) all
	@echo "Tests built."

check: tests
	@echo "Running tests..."
	@$(MAKE) -C $(LLVM_PLUGIN_DIR) run 2>&1

# --- Fast smoke test (~60s, fails on the first error) --------------------
# The "does it work?" command: plugin build + 18 instrumented plugin tests +
# C++ test_tx/test_ds for three representative backends + both Rust workspaces.
# Use `make check-all` for the full multi-backend sweep.
CHECK_FAST_BACKENDS := TINYSTM NOREC TL2

check-fast:
	@echo "=== check-fast [1/4] plugin build + 18 plugin tests ==="
	$(MAKE) -C $(LLVM_PLUGIN_DIR)
	$(MAKE) -C $(LLVM_PLUGIN_DIR) run
	@echo "=== check-fast [2/4] C++ test_tx/test_ds: $(CHECK_FAST_BACKENDS) ==="
	@for be in $(CHECK_FAST_BACKENDS); do \
		echo "--- $$be ---"; \
		$(MAKE) -C $(EXPLI_BENCHMARKS_DIR) -j4 bin/test_tx bin/test_ds BACKEND=$$be > /tmp/check-fast-$$be-build.log 2>&1 \
			|| { echo "  build FAIL"; tail -5 /tmp/check-fast-$$be-build.log; exit 1; }; \
		$(EXPLI_BENCHMARKS_DIR)/bin/test_tx > /tmp/check-fast-$$be-tx.log 2>&1 \
			|| { echo "  test_tx FAIL"; tail -5 /tmp/check-fast-$$be-tx.log; exit 1; }; \
		echo "  test_tx: $$(tail -1 /tmp/check-fast-$$be-tx.log)"; \
		$(EXPLI_BENCHMARKS_DIR)/bin/test_ds > /tmp/check-fast-$$be-ds.log 2>&1 \
			|| { echo "  test_ds FAIL"; tail -5 /tmp/check-fast-$$be-ds.log; exit 1; }; \
		echo "  test_ds: $$(tail -1 /tmp/check-fast-$$be-ds.log)"; \
	done
	@echo "=== check-fast [3/4] Rust simulator tests ==="
	cargo test --manifest-path simulator/Cargo.toml
	@echo "=== check-fast [4/4] Rust workspace tests ==="
	cargo test --manifest-path explicit_api/rust/workspace/Cargo.toml -- --test-threads=1
	@echo "=== check-fast: ALL PASSED ==="

clean:
	-$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/bank clean 2>&1
	-$(MAKE) -C $(PLUGIN_BENCHMARKS_DIR)/datastructures clean 2>&1
	-$(MAKE) -C $(LLVM_PLUGIN_DIR) clean 2>&1
	-$(MAKE) -C $(EXPLI_BENCHMARKS_DIR) clean 2>&1
	-$(MAKE) -C docs/proofs clean 2>&1
	-$(MAKE) -C docs/book cleanall 2>&1
	rm -rf $(LLVM_PLUGIN_DIR)/bin $(LLVM_PLUGIN_DIR)/out
	rm -f /tmp/tm_persistent_state.bin
	@echo "Clean complete."

help:
	@echo "TM API C++ Build System"
	@echo "======================"
	@echo ""
	@echo "Quick start:"
	@echo "  make help             - Show this help (default target)"
	@echo "  make check-fast       - ~60s smoke: plugin tests + 3 C++ backends + Rust"
	@echo "  make all              - Build plugin + plugin benchmarks + expli benchmarks"
	@echo ""
	@echo "Build targets:"
	@echo "  plugin              - Build the LLVM TM instrumentation plugin"
	@echo "  plugin-benchmarks   - Build plugin-based benchmarks (bank, avltree, STAMP)"
	@echo "  expli-benchmarks    - Build explicit C++ API benchmarks (BACKEND=<name>)"
	@echo "  tests               - Build the instrumented plugin test suite"
	@echo ""
	@echo "Test targets:"
	@echo "  check               - Build + run the instrumented plugin tests"
	@echo "  check-fast          - Fast smoke: plugin + TINYSTM/NOREC/TL2 + Rust workspaces"
	@echo "  check-all           - Full sweep: test_tx/test_ds across all C++ backends"
	@echo "  post-merge-check    - Full post-merge matrix (plugin+C+++Rust+sim+integrity)"
	@echo "  test_run            - Run a couple of plugin benchmarks"
	@echo "  compiledb           - Generate compile_commands.json for clangd"
	@echo ""
	@echo "Other targets:"
	@echo "  gem5                - Clone + build gem5 (X86_TSX simulation target)"
	@echo "  gem5-clean          - Remove gem5 build artifacts"
	@echo "  fmt                 - Apply clang-format (C++) + rustfmt (Rust)"
	@echo "  fmt-check           - Verify formatting + clippy (no changes; CI gate)"
	@echo "  clean               - Clean all build artifacts"
	@echo "  clean-book          - Remove docs/book LaTeX build artifacts"
	@echo "  clean-proofs        - Remove TLA+ proof/TLC artifacts under docs/proofs"
	@echo ""
	@echo "Options:"
	@echo "  BACKEND=<name>      - expli benchmark backend (e.g. tinystm, norec, tl2)"
	@echo "  DEBUG=1             - Build with -O0 -g"
	@echo "  STATIC=0            - benchmarks/cpp: link dynamically instead of -static"
	@echo ""
	@echo "Examples:"
	@echo "  make check-fast"
	@echo "  make expli-benchmarks BACKEND=tl2"
	@echo "  make -C benchmarks/cpp bin/bank BACKEND=norec"

test_run: plugin-benchmarks
	@$(PLUGIN_BENCHMARKS_DIR)/bank/bin/bank_singlelock -t 2 -d 1000 2>&1
	@$(PLUGIN_BENCHMARKS_DIR)/datastructures/bin/avltree_SingleGlobalLock 1 10 100 2>&1
	@$(PLUGIN_BENCHMARKS_DIR)/STAMP/bin/stamp_uninstrumented -b kmeans -t 2 2>&1

# Run all explicit C++ API benchmarks across all supported backends
BACKENDS_TESTS := TINYSTM WBETL WT NOREC NORECBF SWISSTM TL2 TSC_TM MVLOG SGL LEFTRIGHT ROMULUS XTM SPHT TSXSGL GPU_STM_CPU CSMV
check-all:
	@echo "=== Building and running all tests across all backends ==="
	@for be in $(BACKENDS_TESTS); do \
		echo "--- Backend: $$be ---"; \
		$(MAKE) -C $(EXPLI_BENCHMARKS_DIR) clean BACKEND=$$be 2>&1 > /dev/null; \
		if $(MAKE) -C $(EXPLI_BENCHMARKS_DIR) -j4 bin/test_tx bin/test_ds BACKEND=$$be 2>&1 > /tmp/check-$$be-build.log; then \
			echo "  Build: OK"; \
			if [ "$$be" = "SGL" ] || [ "$$be" = "LEFTRIGHT" ] || [ "$$be" = "ROMULUS" ]; then \
				echo "  Run: SKIPPED (these backends use explicit tm_init/tm_exit — run ./bin/test_tx manually)"; \
			else \
				if $(EXPLI_BENCHMARKS_DIR)/bin/test_tx > /tmp/check-$$be-tx.log 2>&1; then \
					echo "  test_tx: $$(tail -1 /tmp/check-$$be-tx.log)"; \
				else \
					echo "  test_tx: FAIL"; \
				fi; \
				if $(EXPLI_BENCHMARKS_DIR)/bin/test_ds > /tmp/check-$$be-ds.log 2>&1; then \
					echo "  test_ds: $$(tail -1 /tmp/check-$$be-ds.log)"; \
				else \
					echo "  test_ds: FAIL"; \
				fi; \
			fi; \
		else \
			echo "  Build: FAIL"; \
			tail -3 /tmp/check-$$be-build.log; \
		fi; \
		echo ""; \
	done
	@echo "=== All backend tests complete ==="

# --- Post-merge verification (S31) ------------------------------------------
# Runs the full matrix from docs/POST_MERGE_TEST_PLAN.md (toolchain, plugin,
# C++ backends, Rust workspace, simulator, merge-integrity, TLA+ smoke, gem5).
# Fail-fast with per-section timeouts; target <10 min on CI. Gated weekly in
# .github/workflows/nightly.yml.
#   make post-merge-check                       # full matrix
#   POST_MERGE_ONLY=cpp make post-merge-check   # one section
post-merge-check:
	./tools/post-merge-check.sh

# --- IDE / clangd support (S08) ---------------------------------------------
# Generate root compile_commands.json from the plugin and a representative C++
# benchmark backend build. Requires either `bear` or `compiledb`.
compiledb:
	@./tools/generate-compiledb.sh

# --- Format / lint (S15) ----------------------------------------------------
# `make fmt`      — apply clang-format (C++) + rustfmt (Rust) across the repo.
# `make fmt-check`— verify formatting without changing files (CI gate).
CLANG_FORMAT ?= $(shell command -v clang-format-22 || command -v clang-format || true)
CPP_SRC_DIRS  := backends plugin benchmarks/cpp benchmarks/plugin explicit_api/cpp tests
RUST_DIRS     := explicit_api/rust/workspace simulator benchmarks/rust

fmt:
	@test -n "$(CLANG_FORMAT)" || { echo "ERROR: clang-format not found (install clang-format-22)"; exit 1; }
	@echo "=== fmt: C++ ==="
	@find $(CPP_SRC_DIRS) \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -not -path '*/target/*' -print0 2>/dev/null \
		| xargs -0 -r $(CLANG_FORMAT) -i --style=file
	@echo "=== fmt: Rust (rustfmt) ==="
	@for d in $(RUST_DIRS); do \
		echo "  $$d"; \
		(cd $$d && cargo fmt --all) || exit 1; \
	done
	@echo "=== fmt: done ==="

fmt-check:
	@test -n "$(CLANG_FORMAT)" || { echo "ERROR: clang-format not found (install clang-format-22)"; exit 1; }
	@echo "=== fmt-check: C++ (clang-format --dry-run --Werror) ==="
	@find $(CPP_SRC_DIRS) \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -not -path '*/target/*' -print0 2>/dev/null \
		| xargs -0 -r $(CLANG_FORMAT) --dry-run --Werror --style=file
	@echo "=== fmt-check: Rust (cargo fmt --check) ==="
	@for d in $(RUST_DIRS); do \
		echo "  $$d"; \
		(cd $$d && cargo fmt --all --check) || exit 1; \
	done
	@echo "=== fmt-check: Rust (clippy -D warnings: tm, simulator) ==="
	@(cd explicit_api/rust/workspace && cargo clippy --features wbctl -p tm -- -D warnings)
	@(cd simulator && cargo clippy -- -D warnings)
	@echo "=== fmt-check: all clean ==="

clean-book:
	-$(MAKE) -C docs/book cleanall 2>&1
	@echo "Book build artifacts removed."

clean-proofs:
	-$(MAKE) -C docs/proofs clean 2>&1
	@echo "TLA+ proof artifacts removed."

# --- gem5 simulation -------------------------------------------------------
.PHONY: gem5 gem5-clean
gem5:
	./gem5_sim/setup.sh

gem5-clean:
	rm -rf gem5_sim/gem5/build
