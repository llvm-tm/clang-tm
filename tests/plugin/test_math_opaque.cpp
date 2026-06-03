#include <cmath>
#include <cstdio>

#include "tm_test_common.hpp"

TM double result = 0.0;

TX void compute_math(double a, double b, double c) {
    double r = sqrt(a);
    double x = cos(b);
    double y = sin(c);
    r = r + pow(r, x);
    y = y + log(r + y);
    result = sqrt(result + r + x + y);
}

MAIN int main() {
    compute_math(2.0, 1.0, 0.5);
    printf("result = %f\n", result);
    printf("Math opaque test PASSED\n");
    return 0;
}
