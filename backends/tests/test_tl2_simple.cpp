/**
 * Simple debug test for TL2
 */
#include <iostream>
#include <cstdint>
#include "tl2.hpp"

using namespace tl2;

int main() {
    std::cout << "TL2 Simple Test\n";

    init();
    init_thread();

    word_t counter = 0;

    int iterations = 10;
    for (int i = 0; i < iterations; i++) {
        begin();
        word_t val = tm_read_i8((volatile uint64_t *)&counter);
        val++;
        tm_write_i8((volatile uint64_t *)&counter, val);
        bool ok = commit();
        std::cout << "i=" << i << " ok=" << ok << " counter=" << counter << "\n";
    }

    exit_thread();

    std::cout << "Final: " << counter << " (expected: " << iterations << ")\n";
    return counter == iterations ? 0 : 1;
}
