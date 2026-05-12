#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

TM int8_t tm_i8 = 10;
TM int16_t tm_i16 = 20;
TM int32_t tm_i32 = 30;
TM int64_t tm_i64 = 40;
TM float tm_f4 = 1.5f;
TM double tm_f8 = 2.5;
TM volatile void* tm_ptr = nullptr;

extern "C" void consume_ptr(volatile void* ptr);

TX void tm_types() {
    tm_i8 = tm_i8 + 1;
    int8_t r8 = tm_i8;

    tm_i16 = tm_i16 + 1;
    int16_t r16 = tm_i16;

    tm_i32 = tm_i32 + 1;
    int32_t r32 = tm_i32;

    tm_i64 = tm_i64 + 1;
    int64_t r64 = tm_i64;

    tm_f4 = tm_f4 + 1.0f;
    float rf4 = tm_f4;

    tm_f8 = tm_f8 + 1.0;
    double rf8 = tm_f8;

    tm_ptr = &tm_i32;
    asm volatile("" : "+m"(tm_ptr));
    consume_ptr(tm_ptr);

    (void)r8;
    (void)r16;
    (void)r32;
    (void)r64;
    (void)rf4;
    (void)rf8;
}

MAIN int main() {
    tm_types();
    return 0;
}
