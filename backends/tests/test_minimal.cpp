/**
 * Minimal test - just verify basic threading works
 */

#include <iostream>
#include <thread>
#include <vector>

extern "C" {
    void tm_init();
    void tm_init_thread();
    void tm_exit_thread();
    int tm_begin();
    int tm_end();
    uint32_t tm_read_i4(volatile uint32_t* addr);
    void tm_write_i4(volatile uint32_t* addr, uint32_t val);
}

volatile uint32_t counter = 0;

void worker(int id) {
    tm_init_thread();
    for (int i = 0; i < 10; i++) {
        tm_begin();
        uint32_t v = tm_read_i4(&counter);
        tm_write_i4(&counter, v + 1);
        tm_end();
    }
    tm_exit_thread();
}

int main() {
    std::cout << "Minimal test\n";
    std::cout.flush();

    tm_init();
    std::cout << "After tm_init\n";
    std::cout.flush();

    std::vector<std::thread> threads;
    for (int i = 0; i < 2; i++) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Counter: " << counter << "\n";
    return 0;
}