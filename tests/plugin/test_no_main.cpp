#include "tm_common.hpp"
#include <cstdint>
#include <cstdio>

int main()
{
	stm::tm_region_init();
	printf("Hello\n");
	stm::tm_region_destroy();
	return 0;
}
