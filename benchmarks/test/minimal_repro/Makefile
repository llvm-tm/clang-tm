CLANG_TM ?= ../../../llvm_tm_plugin/clang-tm
CXXFLAGS ?= -std=c++20 -O1 -pthread -fno-inline -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -fno-stack-protector
BACKENDS ?= ../../../backends
RUNTIMES ?= $(BACKENDS)/runtimes
BIN_DIR ?= bin

TARGETS = \
	test_repro_norec \
	test_repro_tinystm_wbctl \
	test_repro_swisstm \
	test_repro_tl2

all: $(TARGETS)

test_repro_norec: repro2.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/NOrec_runtime.cpp \
	-I$(BACKENDS)/NOrec -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

test_repro_tinystm_wbctl: repro2.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/TinySTM_runtime.cpp \
	-DDESIGN_WBCTL -I$(BACKENDS)/TinySTM -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

test_repro_swisstm: repro2.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/SwissTM_runtime.cpp \
	-I$(BACKENDS)/SwissTM -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

test_repro_tl2: repro2.cpp | $(BIN_DIR)
	$(CLANG_TM) $(CXXFLAGS) --runtime $(RUNTIMES)/tl2_runtime.cpp \
	-I$(BACKENDS)/TL2 -I$(BACKENDS) -o $(BIN_DIR)/$@ $<

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BIN_DIR) out

.PHONY: all clean
