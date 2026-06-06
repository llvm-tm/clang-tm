#include <cstdio>
#include <cstdint>
#include "tm_common.hpp"

int main() {
    stm::tm_region_init();
    printf("Hello\n");
    stm::tm_region_destroy();
    return 0;
}
