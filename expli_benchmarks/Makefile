# Makefile for expli_benchmarks (explicit TM API, no plugin needed)
# Run directly: no LLVM plugin pipeline.

CXX      ?= clang++
CXXFLAGS  = -std=c++20 -O3 -march=native -pthread -I..
LDFLAGS   = -pthread

# --- Choose backend (default: TinySTM/WBCTL) --------------------
BACKEND ?= TINYSTM
# BACKEND ?= NOrec
# BACKEND ?= SWISSTM
# BACKEND ?= TL2
# BACKEND ?= SGL

CXXFLAGS += -I..
EXTRA_INC =

ifeq ($(BACKEND),TINYSTM)
  RUNTIME  = ../backends/runtimes/TinySTM_runtime.cpp
  DEFS     = -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL
  EXTRA_INC = -I../backends -I../backends/TinySTM
  BUILTINS = ../backends/TinySTM/tinystm_globals.hpp \
             ../backends/TinySTM/tinystm_common.hpp \
             ../backends/TinySTM/tinystm_wbctl.hpp
endif
ifeq ($(BACKEND),NOREC)
  RUNTIME  = ../backends/runtimes/NOrec_runtime.cpp
  DEFS     = -DTM_BACKEND_NOREC
  EXTRA_INC = -I../backends -I../backends/NOrec
  BUILTINS = ../backends/NOrec/NOrec.hpp
endif
ifeq ($(BACKEND),SWISSTM)
  RUNTIME  = ../backends/runtimes/SwissTM_runtime.cpp
  DEFS     = -DTM_BACKEND_SWISSTM
  EXTRA_INC = -I../backends -I../backends/SwissTM
  BUILTINS = ../backends/SwissTM/SwissTM.hpp
endif
ifeq ($(BACKEND),TL2)
  RUNTIME  = ../backends/runtimes/tl2_runtime.cpp
  DEFS     = -DTM_BACKEND_TL2
  EXTRA_INC = -I../backends -I../backends/TL2
  BUILTINS = ../backends/TL2/tl2.hpp
endif
ifeq ($(BACKEND),SGL)
  RUNTIME  = ../backends/runtimes/single_global_lock_runtime.cpp
  DEFS     = -DTM_BACKEND_SGL
  BUILTINS =
endif

# --- Benchmarks ---------------------------------------------------
BANK_SRC = bank/bank.cpp
BANK_BIN = bin/bank

# --- STMbench7 ----------------------------------------------------
BENCH7_SRC = STMbench7/STMbench7.cpp
BENCH7_BIN = bin/stmbench7

# --- expli_tm_api tests ------------------------------------------
TEST_DS_SRC = ../expli_tm_api/tests/test_ds.cpp
TEST_DS_BIN = bin/test_ds
TEST_TX_SRC = ../expli_tm_api/tests/test_tx.cpp
TEST_TX_BIN = bin/test_tx

BUILTINS += ../backends/tm_alloc_overrides.hpp \
            ../backends/tm_common.hpp \
            ../backends/tm_platform.hpp \
            ../backends/tm_spin_token.hpp \
            ../backends/tm_event_logger.hpp \
            ../backends/tm_debug.hpp

# Rules
.PHONY: all clean run-bank run-test-ds run-test-tx run-tests run

all: bin $(BANK_BIN) $(BENCH7_BIN) $(TEST_DS_BIN) $(TEST_TX_BIN)

bin:
	mkdir -p bin

$(BANK_BIN): bin $(BANK_SRC) $(RUNTIME) $(BUILTINS) Makefile
	$(CXX) $(CXXFLAGS) $(EXTRA_INC) $(DEFS) -o $@ $(BANK_SRC) $(RUNTIME) $(LDFLAGS)

$(BENCH7_BIN): bin $(BENCH7_SRC) $(RUNTIME) $(BUILTINS) Makefile
	$(CXX) $(CXXFLAGS) $(EXTRA_INC) $(DEFS) -o $@ $(BENCH7_SRC) $(RUNTIME) $(LDFLAGS)

$(TEST_DS_BIN): bin $(TEST_DS_SRC) $(RUNTIME) $(BUILTINS) Makefile
	$(CXX) $(CXXFLAGS) $(EXTRA_INC) $(DEFS) -o $@ $(TEST_DS_SRC) $(RUNTIME) $(LDFLAGS)

$(TEST_TX_BIN): bin $(TEST_TX_SRC) $(RUNTIME) $(BUILTINS) Makefile
	$(CXX) $(CXXFLAGS) $(EXTRA_INC) $(DEFS) -o $@ $(TEST_TX_SRC) $(RUNTIME) $(LDFLAGS)

run-bank: $(BANK_BIN)
	./$(BANK_BIN) -d 3000 -a 512 -t 4

run-test-ds: $(TEST_DS_BIN)
	./$(TEST_DS_BIN)

run-test-tx: $(TEST_TX_BIN)
	./$(TEST_TX_BIN)

run-bench7: $(BENCH7_BIN)
	./$(BENCH7_BIN) 4 5000

run-tests: run-test-ds run-test-tx
run: run-bank run-bench7 run-tests

clean:
	rm -rf bin
