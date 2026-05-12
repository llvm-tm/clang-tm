#include <cstdint>
#include <iostream>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

// TM-annotated global variables
TM int tm_int = 0;
TM int8_t tm_i8 = 10;
TM int32_t tm_i32 = 100;

// TX-annotated transaction functions
TX void tx_write_int() {
    tm_int = tm_int + 1;
}

TX void tx_write_types() {
    tm_i8 = tm_i8 + 1;
    tm_i32 = tm_i32 + 1;
}

TX int tx_read_int() {
    return tm_int;
}

MAIN int main() {
    std::cout << "annotation_detect: starting" << std::endl;
    
    // Call transaction functions
    tx_write_int();
    tx_write_types();
    
    int result = tx_read_int();
    std::cout << "annotation_detect: result = " << result << std::endl;
    
    std::cout << "annotation_detect: PASSED" << std::endl;
    return 0;
}
