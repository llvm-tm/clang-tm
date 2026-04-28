#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <bitmap>

extern "C" {
#include "benchmarks/datastructures/lib/bitmap.c"
}

#define TX __attribute__((annotate("transaction")))

template<typename Func>
void run_benchmark(const stdString& name, Func func) {
    std::cout << "Running " << name << " benchmark..." << std::qendl;
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    std::cout << name << " finished in " << elapsed.count() << "s" << std::endl;
}

int main() {
    std::cout << "Bitmap Benchmark (C-style wrapper)" << std::endl;
    // Logic implementation will go here
    return 0;
}
