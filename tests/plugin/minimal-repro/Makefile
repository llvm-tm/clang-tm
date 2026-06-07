CLANG_TM ?= ../../../plugin/clang-tm
CXXFLAGS ?= -std=c++20 -O1 -pthread -fno-inline -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -fno-stack-protector
BACKENDS ?= ../../../backends
RUNTIMES ?= $(BACKENDS)/tm_impl
BIN_DIR ?= bin

# Source file: repro2.cpp for STMbench7-like (global vector<CompositePart>),
# repro.cpp for simpler vector<StructWithVector> local TX.
SOURCE ?= repro2.cpp

TARGETS = \
	test_repro_norec \
	test_repro_tinystm_wbctl \
	test_repro_swisstm \
	test_repro_tl2

# Build with repro.cpp instead: make SOURCE=repro.cpp all
$(TARGETS): $(SOURCE) | $(BIN_DIR)

test_repro_norec: $(SOURCE) | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/norec/NOrec_runtime.cpp \
	-I$(BACKENDS)/tm_impl/norec -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

test_repro_tinystm_wbctl: $(SOURCE) | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tiny_stm/TinySTM_runtime.cpp \
	-DDESIGN_WBCTL -I$(BACKENDS)/tm_impl/tiny_stm -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

test_repro_swisstm: $(SOURCE) | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/swisstm/SwissTM_runtime.cpp \
	-I$(BACKENDS)/tm_impl/swisstm -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

test_repro_tl2: $(SOURCE) | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tl2/tl2_runtime.cpp \
	-I$(BACKENDS)/tm_impl/tl2 -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BIN_DIR) out

# Run all reproducers
run: all
	@for bin in $(TARGETS); do \
		echo "=== $$bin ==="; \
		./$(BIN_DIR)/$$bin 5 || true; \
	done

# Run with NOrec backend (fastest crash)
run_norec: test_repro_norec
	./$(BIN_DIR)/test_repro_norec 5

# Run with WBCTL backend
run_wbctl: test_repro_tinystm_wbctl
	./$(BIN_DIR)/test_repro_tinystm_wbctl 5

.PHONY: all clean run run_norec run_wbctl
