#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_set>

#include "../Romulus/romulus.hpp"
#include "../tm_alloc_overrides.hpp"

thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;

namespace romulus {

std::atomic<uint64_t> g_global_clock{1};
std::atomic<uint64_t> thr_counter{1};
__thread Transaction *current_tx = nullptr;
std::atomic<uint64_t> g_tm_abort_count{0};
__thread sigjmp_buf *jmpbuf_ptr;

} // namespace romulus

__thread sigjmp_buf *jmpbuf;

__thread int32_t tm_nested_call_counter;
__thread int32_t tm_longjmp_ret;
__thread sigjmp_buf tm_jmpbuf;
__thread int tm_init_thread_call_count = 0;

extern "C" {

void tm_init() { romulus::init(); }

void tm_exit() {
    romulus::exit();
    if (auto ac = romulus::g_tm_abort_count.load(); ac > 0) {
        fprintf(stderr, "\n=== Romulus total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

void tm_init_thread() { romulus::init_thread(); }

void tm_exit_thread() {}

void tm_begin() {
    if (tm_nested_call_counter == 0) {
        tm_nested_call_counter = 1;
    } else {
        tm_nested_call_counter++;
        return;
    }
    romulus::jmpbuf_ptr = (sigjmp_buf *)&tm_jmpbuf;
    romulus::begin();
}

void tm_end() {
    if (tm_nested_call_counter == 1) {
        romulus::commit();
        tm_nested_call_counter = 0;
    } else if (tm_nested_call_counter > 1) {
        tm_nested_call_counter--;
    }
}

void *tm_malloc(size_t size) {
    void *p = ::operator new(size);
    if (p) tm_track_spec_alloc(p);
    return p;
}

void *tm_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = ::operator new(total);
    if (p) { std::memset(p, 0, total); tm_track_spec_alloc(p); }
    return p;
}

void *tm_realloc(void *ptr, size_t size) {
    if (!ptr) return tm_malloc(size);
    void *p = ::operator new(size);
    if (p) {
        std::memcpy(p, ptr, size);
        tm_free(ptr);
        tm_track_spec_alloc(p);
    }
    return p;
}

void tm_free(void *ptr) {
    if (!ptr) return;
    tm_untrack_spec_alloc(ptr);
    if (g_in_tx)
        tm_free_append_deferred(ptr);
    else
        ::operator delete(ptr);
}

uint8_t tm_read_i1(uint8_t *addr) { return romulus::tm_read<uint8_t, romulus::ValueType::UINT8>(addr); }
uint16_t tm_read_i2(uint16_t *addr) { return romulus::tm_read<uint16_t, romulus::ValueType::UINT16>(addr); }
uint32_t tm_read_i4(uint32_t *addr) { return romulus::tm_read<uint32_t, romulus::ValueType::UINT32>(addr); }
uint64_t tm_read_i8(uint64_t *addr) { return romulus::tm_read<uint64_t, romulus::ValueType::UINT64>(addr); }
void tm_write_i1(uint8_t *addr, uint8_t val) { romulus::tm_write<uint8_t, romulus::ValueType::UINT8>(addr, val); }
void tm_write_i2(uint16_t *addr, uint16_t val) { romulus::tm_write<uint16_t, romulus::ValueType::UINT16>(addr, val); }
void tm_write_i4(uint32_t *addr, uint32_t val) { romulus::tm_write<uint32_t, romulus::ValueType::UINT32>(addr, val); }
void tm_write_i8(uint64_t *addr, uint64_t val) { romulus::tm_write<uint64_t, romulus::ValueType::UINT64>(addr, val); }

float tm_read_f4(float *addr) { return romulus::tm_read<float, romulus::ValueType::FLOAT>(addr); }
double tm_read_f8(double *addr) { return romulus::tm_read<double, romulus::ValueType::DOUBLE>(addr); }
void tm_write_f4(float *addr, float val) { romulus::tm_write<float, romulus::ValueType::FLOAT>(addr, val); }
void tm_write_f8(double *addr, double val) { romulus::tm_write<double, romulus::ValueType::DOUBLE>(addr, val); }

void *tm_read_ptr(void **addr) { return romulus::tm_read<void *, romulus::ValueType::POINTER>(addr); }
void tm_write_ptr(void **addr, void *val) { romulus::tm_write<void *, romulus::ValueType::POINTER>(addr, val); }

} // extern "C"
