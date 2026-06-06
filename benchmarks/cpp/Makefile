# Makefile for expli_benchmarks (explicit TM API, no plugin needed)
# Run directly: no LLVM plugin pipeline.

CXX      ?= clang++
CXXFLAGS  = -std=c++20 -O3 -march=native -pthread -I$(abspath ..)
LDFLAGS   = -pthread

# --- Choose backend (default: TinySTM/WBCTL) --------------------
BACKEND ?= TINYSTM

CXXFLAGS += -I..
EXTRA_INC =
RUNTIME =
REGION_ALLOC = ../backends/tm_impl/tm_region_allocator/tm_region_allocator.cpp

ifeq ($(BACKEND),TINYSTM)
  RUNTIME  = ../backends/tm_impl/tiny_stm/TinySTM_runtime.cpp
  DEFS     = -DTM_BACKEND_TINYSTM -DDESIGN_WBCTL
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/tiny_stm
endif
ifeq ($(BACKEND),WBETL)
  RUNTIME  = ../backends/tm_impl/tiny_stm/TinySTM_runtime.cpp
  DEFS     = -DTM_BACKEND_TINYSTM -DDESIGN_WBETL
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/tiny_stm
endif
ifeq ($(BACKEND),WT)
  RUNTIME  = ../backends/tm_impl/tiny_stm/TinySTM_runtime.cpp
  DEFS     = -DTM_BACKEND_TINYSTM -DDESIGN_WT
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/tiny_stm
endif
ifeq ($(BACKEND),NOREC)
  RUNTIME  = ../backends/tm_impl/norec/NOrec_runtime.cpp
  DEFS     = -DTM_BACKEND_NOREC
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/norec
endif
ifeq ($(BACKEND),SWISSTM)
  RUNTIME  = ../backends/tm_impl/swisstm/SwissTM_runtime.cpp
  DEFS     = -DTM_BACKEND_SWISSTM
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/swisstm
endif
ifeq ($(BACKEND),TL2)
  RUNTIME  = ../backends/tm_impl/tl2/tl2_runtime.cpp
  DEFS     = -DTM_BACKEND_TL2
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/tl2
endif
ifeq ($(BACKEND),SGL)
  RUNTIME  = ../backends/tm_impl/single_global_lock/SingleGlobalLock_runtime.cpp
  DEFS     = -DTM_BACKEND_SGL
  EXTRA_INC = -I../backends/tm_impl/common
endif
ifeq ($(BACKEND),LEFTRIGHT)
  RUNTIME  = ../backends/tm_impl/leftright/leftright_runtime.cpp
  DEFS     = -DTM_BACKEND_LEFTRIGHT
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/leftright
endif
ifeq ($(BACKEND),ROMULUS)
  RUNTIME  = ../backends/tm_impl/romulus/romulus_runtime.cpp
  DEFS     = -DTM_BACKEND_ROMULUS
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/romulus
endif
ifeq ($(BACKEND),XTM)
  RUNTIME  = ../backends/tm_impl/xtm/xtm_runtime.cpp
  DEFS     = -DTM_BACKEND_XTM
  EXTRA_INC = -I../backends/tm_impl/common -I../backends/tm_impl/xtm
endif

BUILTINS = $(addprefix ../backends/,tm_alloc_overrides.hpp tm_common.hpp \
           tm_platform.hpp tm_spin_token.hpp tm_event_logger.hpp tm_debug.hpp)

# --- Source directories (logical structure) -----------------------
# NOTE: on case-insensitive filesystems (macOS), directory names
# differing only by case refer to the same directory.  We use the
# actual lower-case names where case-collision would occur.
# Logical name      → actual directory:
#   EigenBench       → eigenbench/
#   YCSB             → ycsb/
#   TPC-C            → TPC-C/
#   STMbench7        → STMbench7/
#   STAMP            → STAMP/ (vacation/, labyrinth/, kmeans/, genome/, intruder/, ssca2/)
#   datastructures   → datastructures/
#   tests            → tests/ (bank/, fuzz/)

# --- tests --------------------------------------------------------
BANK_SRC     = bank/bank.cpp
BANK_BIN     = bin/bank
FUZZ_CTR_SRC = fuzz-counter/fuzz_counter.cpp
FUZZ_CTR_BIN = bin/fuzz_counter
FUZZ_BANK_SRC = fuzz-bank/fuzz_bank.cpp
FUZZ_BANK_BIN = bin/fuzz_bank

# --- EigenBench --------------------------------------------------
EIGEN_SRC   = eigenbench/eigenbench.cpp
EIGEN_BIN   = bin/eigenbench

# --- STMbench7 ----------------------------------------------------
BENCH7_SRC  = stmbench7/STMbench7.cpp
BENCH7_BIN  = bin/stmbench7

# --- STAMP --------------------------------------------------------
VACATION_SRC   = STAMP/vacation/vacation.cpp
VACATION_BIN   = bin/vacation
LABYRINTH_SRC  = STAMP/labyrinth/labyrinth.cpp
LABYRINTH_BIN  = bin/labyrinth
KMEANS_SRC     = STAMP/kmeans/kmeans.cpp
KMEANS_BIN     = bin/kmeans
GENOME_SRC     = STAMP/genome/genome.cpp
GENOME_BIN     = bin/genome
INTRUDER_SRC   = STAMP/intruder/intruder.cpp
INTRUDER_BIN   = bin/intruder
SSCA2_SRC      = STAMP/ssca2/ssca2.cpp
SSCA2_BIN      = bin/ssca2
BAYES_SRC      = STAMP/bayes/bayes.cpp
BAYES_BIN      = bin/bayes
YADA_SRC       = STAMP/yada/yada.cpp
YADA_BIN       = bin/yada

# --- TPC-C --------------------------------------------------------
TPCC_SRC    = tpcc/tpcc.cpp
TPCC_BIN    = bin/tpcc

# --- YCSB --------------------------------------------------------
YCSB_SRC    = ycsb/ycsb.cpp
YCSB_BIN    = bin/ycsb

# --- datastructures (placeholder) ---------------------------------
# TODO: implement linked-list, hash-map, treap, skip-list, queue, stack

# --- expli_tm_api tests ------------------------------------------
TEST_DS_SRC = ../tests/expli-api/test_ds.cpp
TEST_DS_BIN = bin/test_ds
TEST_TX_SRC = ../tests/expli-api/test_tx.cpp
TEST_TX_BIN = bin/test_tx

# --- Stubs runtime (uninstrumented baseline) --------------------
STUBS = ../backends/stubs/tm_stubs.cpp

# Stubs binary names (appended with _stubs)
VACATION_STUBS   = bin/vacation_stubs
LABYRINTH_STUBS  = bin/labyrinth_stubs
KMEANS_STUBS     = bin/kmeans_stubs
GENOME_STUBS     = bin/genome_stubs
INTRUDER_STUBS   = bin/intruder_stubs
SSCA2_STUBS      = bin/ssca2_stubs
BAYES_STUBS      = bin/bayes_stubs
YADA_STUBS       = bin/yada_stubs
TPCC_STUBS       = bin/tpcc_stubs
YCSB_STUBS       = bin/ycsb_stubs
BENCH7_STUBS     = bin/stmbench7_stubs
BANK_STUBS       = bin/bank_stubs
EIGEN_STUBS      = bin/eigenbench_stubs
FUZZ_CTR_STUBS   = bin/fuzz_counter_stubs
FUZZ_BANK_STUBS  = bin/fuzz_bank_stubs

STAMP_STUBS = $(VACATION_STUBS) $(LABYRINTH_STUBS) $(KMEANS_STUBS) $(GENOME_STUBS) \
              $(INTRUDER_STUBS) $(SSCA2_STUBS) $(BAYES_STUBS) $(YADA_STUBS)

# --- Build rules --------------------------------------------------
STAMP_BINS = $(VACATION_BIN) $(LABYRINTH_BIN) $(KMEANS_BIN) $(GENOME_BIN) $(INTRUDER_BIN) $(SSCA2_BIN) $(BAYES_BIN) $(YADA_BIN)

.PHONY: all stubs clean run-bank run-eigenbench run-bench7 run-vacation run-labyrinth run-kmeans
.PHONY: run-genome run-intruder run-ssca2 run-bayes run-yada run-fuzz-counter run-fuzz-bank run-fuzz
.PHONY: run-tpcc run-ycsb run-test-ds run-test-tx run-tests run run-stamp

all: bin $(BANK_BIN) $(EIGEN_BIN) $(BENCH7_BIN) $(STAMP_BINS) \
     $(FUZZ_CTR_BIN) $(FUZZ_BANK_BIN) $(TPCC_BIN) $(YCSB_BIN) $(TEST_DS_BIN) $(TEST_TX_BIN)

stubs: bin $(BANK_STUBS) $(EIGEN_STUBS) $(BENCH7_STUBS) $(STAMP_STUBS) \
       $(FUZZ_CTR_STUBS) $(FUZZ_BANK_STUBS) $(TPCC_STUBS) $(YCSB_STUBS)

bin:
	mkdir -p bin STAMP/kmeans STAMP/genome STAMP/intruder STAMP/ssca2

define BUILD_RULE
$(1): bin $(2) $(RUNTIME) $(REGION_ALLOC) $(BUILTINS) Makefile
	$$(CXX) $$(CXXFLAGS) $$(EXTRA_INC) $$(DEFS) -o $$@ $(2) $$(RUNTIME) $$(REGION_ALLOC) $$(LDFLAGS)
endef

define BUILD_STUBS_RULE
$(1): bin $(2) $(STUBS) Makefile
	$$(CXX) $$(CXXFLAGS) -I../backends/tm_impl/common -o $$@ $(2) $(STUBS) $$(LDFLAGS)
endef

$(eval $(call BUILD_RULE,$(BANK_BIN),$(BANK_SRC)))
$(eval $(call BUILD_RULE,$(EIGEN_BIN),$(EIGEN_SRC)))
$(eval $(call BUILD_RULE,$(BENCH7_BIN),$(BENCH7_SRC)))
$(eval $(call BUILD_RULE,$(FUZZ_CTR_BIN),$(FUZZ_CTR_SRC)))
$(eval $(call BUILD_RULE,$(FUZZ_BANK_BIN),$(FUZZ_BANK_SRC)))
$(eval $(call BUILD_RULE,$(VACATION_BIN),$(VACATION_SRC)))
$(eval $(call BUILD_RULE,$(LABYRINTH_BIN),$(LABYRINTH_SRC)))
$(eval $(call BUILD_RULE,$(KMEANS_BIN),$(KMEANS_SRC)))
$(eval $(call BUILD_RULE,$(GENOME_BIN),$(GENOME_SRC)))
$(eval $(call BUILD_RULE,$(INTRUDER_BIN),$(INTRUDER_SRC)))
$(eval $(call BUILD_RULE,$(SSCA2_BIN),$(SSCA2_SRC)))
$(eval $(call BUILD_RULE,$(BAYES_BIN),$(BAYES_SRC)))
$(eval $(call BUILD_RULE,$(YADA_BIN),$(YADA_SRC)))
$(eval $(call BUILD_RULE,$(TPCC_BIN),$(TPCC_SRC)))
$(eval $(call BUILD_RULE,$(YCSB_BIN),$(YCSB_SRC)))
$(eval $(call BUILD_RULE,$(TEST_DS_BIN),$(TEST_DS_SRC)))
$(eval $(call BUILD_RULE,$(TEST_TX_BIN),$(TEST_TX_SRC)))

# --- Stubs build rules (uninstrumented baseline) ------------------
$(eval $(call BUILD_STUBS_RULE,$(BANK_STUBS),$(BANK_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(EIGEN_STUBS),$(EIGEN_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(BENCH7_STUBS),$(BENCH7_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(FUZZ_CTR_STUBS),$(FUZZ_CTR_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(FUZZ_BANK_STUBS),$(FUZZ_BANK_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(VACATION_STUBS),$(VACATION_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(LABYRINTH_STUBS),$(LABYRINTH_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(KMEANS_STUBS),$(KMEANS_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(GENOME_STUBS),$(GENOME_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(INTRUDER_STUBS),$(INTRUDER_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(SSCA2_STUBS),$(SSCA2_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(BAYES_STUBS),$(BAYES_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(YADA_STUBS),$(YADA_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(TPCC_STUBS),$(TPCC_SRC)))
$(eval $(call BUILD_STUBS_RULE,$(YCSB_STUBS),$(YCSB_SRC)))

# --- Run targets --------------------------------------------------
run-bank: $(BANK_BIN)
	./$(BANK_BIN) -d 3000 -a 512 -t 4

run-eigenbench: $(EIGEN_BIN)
	./$(EIGEN_BIN) -d 3000 -t 4

run-fuzz-counter: $(FUZZ_CTR_BIN)
	./$(FUZZ_CTR_BIN) 4 1000 8 42

run-fuzz-bank: $(FUZZ_BANK_BIN)
	./$(FUZZ_BANK_BIN) 4 1000 64 42

run-fuzz: run-fuzz-counter run-fuzz-bank

run-vacation: $(VACATION_BIN)
	./$(VACATION_BIN) -p 4 -r 16384 -n 2 -u 98 -t 4096

run-labyrinth: $(LABYRINTH_BIN)
	./$(LABYRINTH_BIN) -p 4 -x 8 -y 8 -z 8 -n 64

run-kmeans: $(KMEANS_BIN)
	./$(KMEANS_BIN) -p 4 -k 16 -d 2 -n 2048

run-genome: $(GENOME_BIN)
	./$(GENOME_BIN) -p 4 -g 16384 -s 64 -n 16777216

run-intruder: $(INTRUDER_BIN)
	./$(INTRUDER_BIN) -p 4 -a 10 -l 128 -n 1048576 -s 1

run-ssca2: $(SSCA2_BIN)
	./$(SSCA2_BIN) -p 4 -s 14 -u 1.0 -l 3 -m 3 -i 3

run-bayes: $(BAYES_BIN)
	./$(BAYES_BIN) -t 4 -v 32 -r 1024 -n 2 -p 2 -e 2

run-yada: $(YADA_BIN)
	./$(YADA_BIN) -t 4 -a 20 -j 0.5

run-stamp: run-vacation run-labyrinth run-kmeans run-genome run-intruder run-ssca2 run-bayes run-yada

run-test-ds: $(TEST_DS_BIN)
	./$(TEST_DS_BIN)

run-test-tx: $(TEST_TX_BIN)
	./$(TEST_TX_BIN)

run-bench7: $(BENCH7_BIN)
	./$(BENCH7_BIN) 4 5000

run-tpcc: $(TPCC_BIN)
	./$(TPCC_BIN) -t 4 -d 3000

run-ycsb: $(YCSB_BIN)
	./$(YCSB_BIN) -t 4 -d 3000

run-tests: run-test-ds run-test-tx
run: run-bank run-eigenbench run-bench7 run-stamp run-fuzz run-tpcc run-ycsb run-tests

clean:
	rm -rf bin
