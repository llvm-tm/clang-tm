#include <cstdint>
#include <iostream>

#include "tm_test_common.hpp"

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
