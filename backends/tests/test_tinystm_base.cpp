#include <iostream>
#include <cstdint>

extern "C" {
    void tm_init();
    void tm_exit();
    void tm_init_thread();
    void tm_exit_thread();
    void tm_begin();
    void tm_end();
    uint32_t tm_read_i4(volatile uint32_t* addr);
    void tm_write_i4(volatile uint32_t* addr, uint32_t val);
}

volatile uint32_t counter = 0;

int main() {
    tm_init();
    tm_init_thread();
    
    for (int i = 0; i < 100; i++) {
        int ok = 0;
        while (!ok) {
            if (!tm_begin()) continue;
            counter++;
            ok = tm_end();
        }
    }
    
    std::cout << "Counter: " << counter << " (expected 100)\n";
    std::cout << (counter == 100 ? "PASS" : "FAIL") << "\n";
    
    tm_exit_thread();
    tm_exit();
    return counter == 100 ? 0 : 1;
}
