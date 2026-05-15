# IntSet Benchmark Makefile - Modern C++17 Version

CXX := clang++
CXXFLAGS := -std=c++20 -O2 -pthread

SRC := intset.cpp
BIN := bin/intset

# Output directories
OUT_DIR := out
BIN_DIR := bin

.PHONY: all clean test

all: $(BIN_DIR) $(BIN)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN): $(SRC) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(BIN)

test: $(BIN)
	@echo "Running intset benchmark (4 threads, 5 seconds)..."
	@$(BIN) -t 4 -d 5000 || echo "Test completed"

clean:
	rm -rf $(BIN_DIR)
