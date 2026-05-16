# Backend unit tests — compile directly against runtimes (no LLVM plugin)

CXX := clang++
CXXFLAGS := -std=c++20 -O0 -pthread -g
BIN_DIR := bin
RUNTIME_DIR := ../runtimes

INC_TL2     := -I../TL2 -I..
INC_TINYSTM := -I../TinySTM -I..
INC_NOREC   := -I../NOrec -I..

.PHONY: all clean run test_tl2_simple test_tl2_multi test_NOrec_counter

all: $(BIN_DIR) \
	test_tl2_simple test_tl2_multi test_NOrec_counter \
	runtime__st runtime__mt tinystm_all

# ---- TL2 tests ----
test_tl2_simple: $(BIN_DIR)/test_tl2_simple
$(BIN_DIR)/test_tl2_simple: test_tl2_simple.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_TL2) -o $@ $<

test_tl2_multi: $(BIN_DIR)/test_tl2_multi
$(BIN_DIR)/test_tl2_multi: test_tl2_multi.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_TL2) -o $@ $<

# ---- NOrec tests ----
test_NOrec_counter: $(BIN_DIR)/test_NOrec_counter
$(BIN_DIR)/test_NOrec_counter: test_runtime_NOrec_counter.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_NOREC) -o $@ $< $(RUNTIME_DIR)/NOrec_runtime.cpp

# ---- Runtime tests (TinySTM WBCTL) ----
runtime__st: $(BIN_DIR)/runtime__st
$(BIN_DIR)/runtime__st: test_runtime__st.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WBCTL -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp

runtime__mt: $(BIN_DIR)/runtime__mt
$(BIN_DIR)/runtime__mt: test_runtime__mt.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WBCTL -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp

# ---- TinySTM flavor variants ----
TINYSTM_FLAVORS = wbctl wbetl wt
TINYSTM_BINS    = $(addprefix $(BIN_DIR)/runtime_tinystm_,$(TINYSTM_FLAVORS))

tinystm_all: $(TINYSTM_BINS)

$(BIN_DIR)/runtime_tinystm_wbctl: test_runtime_TinySTM_counter.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WBCTL -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp

$(BIN_DIR)/runtime_tinystm_wbetl: test_runtime_TinySTM_counter.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WBETL -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp

$(BIN_DIR)/runtime_tinystm_wt: test_runtime_TinySTM_counter.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INC_TINYSTM) -DDESIGN_WT -o $@ $< $(RUNTIME_DIR)/TinySTM_runtime.cpp

# ---- Run ----
run: tinystm_all
	@for b in $(TINYSTM_BINS); do echo "=== $$b ===" && ./$$b; done

# ---- Directories ----
$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR)
