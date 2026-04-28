#include <iostream>
#include <thread>
#include <vector>
#include <cstdint>
#include <atomic>

extern "C" {
    void tm_init();
    void tm_exit();
    void tm_init_thread();
    void tm_exit_thread();
    int tm_begin();
    int tm_end();
    uint32_t tm_read_i4(volatile uint32_t* addr);
    void tm_write_i4(volatile uint32_t* addr, uint32_t val);
}

volatile uint32_t counter = 0;

void test_thread(int id) {
    tm_init_thread();
    for (int i = 0; i < 100; i++) {
        while (!tm_begin()) {}
        uint32_t v = tm_read_i4(&counter);
        v = v + 1;
        tm_write_i4(&counter, v);
        if (!tm_end()) {
            std::cerr << "Thread " << id << " abort\n";
        }
    }
    tm_exit_thread();
}

int main() {
    tm_init();
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(test_thread, i);
    }
    for (auto& t : threads) t.join();
    tm_exit();
    std::cout << "Counter: " << counter << " (expected 400)\n";
    return counter == 400 ? 0 : 1;
}
