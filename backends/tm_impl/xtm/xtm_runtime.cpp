#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_set>

#include "xtm.hpp"
#include "../common/tm_alloc_overrides.hpp"

thread_local bool g_in_tx = false;
thread_local FreeNode *g_deferred_frees = nullptr;
thread_local std::unordered_set<void *> g_deferred_frees_set;
thread_local SpecAlloc *g_spec_allocs = nullptr;

namespace xtm {

std::atomic<uint64_t> g_tx_counter{1};
std::atomic<uint64_t> g_abort_counter{0};
XADTEntry *g_xadt = nullptr;
std::atomic<uint8_t> g_xf[XF_BITS];
__thread Transaction *current_tx = nullptr;
__thread sigjmp_buf *jmpbuf_ptr = nullptr;

} // namespace xtm

__thread sigjmp_buf *jmpbuf = nullptr;
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;
__thread sigjmp_buf tm_jmpbuf;
__thread int tm_init_thread_call_count = 0;

extern "C" {

void tm_init() {
    stm::tm_region_init();
    xtm::init();
}

void tm_exit() {
    xtm::exit();
    if (auto ac = xtm::g_abort_counter.load(); ac > 0) {
        fprintf(stderr, "\n=== XTM total aborts = %llu ===\n",
                (unsigned long long)ac);
    }
}

void tm_init_thread() {
    xtm::init_thread();
}

void tm_exit_thread() {}

void tm_begin() {
    if (tm_nested_call_counter == 1) {
        xtm::jmpbuf_ptr = (sigjmp_buf *)&tm_jmpbuf;
        xtm::begin();
    }
}

void tm_end() {
    if (tm_nested_call_counter == 1) {
        xtm::commit();
    }
}

void *tm_malloc(size_t size) {
    void *p = stm::tm_region_malloc(size);
    if (p) tm_track_spec_alloc(p);
    return p;
}

void *tm_calloc(size_t nmemb, size_t size) {
    void *p = stm::tm_region_calloc(nmemb, size);
    if (p) tm_track_spec_alloc(p);
    return p;
}

void *tm_realloc(void *ptr, size_t size) {
    return stm::tm_region_realloc(ptr, size);
}

void tm_free(void *ptr) {
    if (!ptr) return;
    tm_untrack_spec_alloc(ptr);
    stm::tm_region_free(ptr);
}

uint8_t  tm_read_i1(uint8_t  *addr) { return xtm::tm_read<uint8_t,  xtm::ValueType::UINT8>(addr); }
uint16_t tm_read_i2(uint16_t *addr) { return xtm::tm_read<uint16_t, xtm::ValueType::UINT16>(addr); }
uint32_t tm_read_i4(uint32_t *addr) { return xtm::tm_read<uint32_t, xtm::ValueType::UINT32>(addr); }
uint64_t tm_read_i8(uint64_t *addr) { return xtm::tm_read<uint64_t, xtm::ValueType::UINT64>(addr); }

void tm_write_i1(uint8_t  *addr, uint8_t  val) { xtm::tm_write<uint8_t,  xtm::ValueType::UINT8>(addr, val); }
void tm_write_i2(uint16_t *addr, uint16_t val) { xtm::tm_write<uint16_t, xtm::ValueType::UINT16>(addr, val); }
void tm_write_i4(uint32_t *addr, uint32_t val) { xtm::tm_write<uint32_t, xtm::ValueType::UINT32>(addr, val); }
void tm_write_i8(uint64_t *addr, uint64_t val) { xtm::tm_write<uint64_t, xtm::ValueType::UINT64>(addr, val); }

float  tm_read_f4(float  *addr) { return xtm::tm_read<float,  xtm::ValueType::FLOAT>(addr); }
double tm_read_f8(double *addr) { return xtm::tm_read<double, xtm::ValueType::DOUBLE>(addr); }

void tm_write_f4(float  *addr, float  val) { xtm::tm_write<float,  xtm::ValueType::FLOAT>(addr, val); }
void tm_write_f8(double *addr, double val) { xtm::tm_write<double, xtm::ValueType::DOUBLE>(addr, val); }

void *tm_read_ptr(void **addr) { return xtm::tm_read<void *, xtm::ValueType::POINTER>(addr); }
void  tm_write_ptr(void **addr, void *val) { xtm::tm_write<void *, xtm::ValueType::POINTER>(addr, val); }

} // extern "C"
