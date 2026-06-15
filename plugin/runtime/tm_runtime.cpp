#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <pthread.h>

#include "../../backends/tm_impl/common/tm_hooks.hpp"

#define TM_BUFFER_SIZE 1024

// Per-thread TM state struct — must match the LLVM pass's offset constants:
//   COUNTER_OFFSET = 0  (nested_call_counter)
//   JMPRET_OFFSET  = 4  (longjmp_ret)
struct TMThreadState {
    int32_t nested_call_counter;
    int32_t longjmp_ret;
};

// ── Per-thread state (must match what tm_hooks.hpp declares) ────────
__thread int32_t    tm_nested_call_counter = 0;
__thread int32_t    tm_longjmp_ret = 0;
__thread unsigned char tm_jmpbuf[256];
__thread uint8_t is_tm_init_thread_ready = 0;
thread_local uint8_t tm_buffer[TM_BUFFER_SIZE] = {0};

thread_local bool g_in_tx = false;

// ── Speculative-allocation tracking ───────────────────────
struct SpecAlloc {
    SpecAlloc* next;
    void* ptr;
};

thread_local SpecAlloc* g_spec_allocs = nullptr;

static void tm_track_spec_alloc(void* ptr)
{
    if (g_in_tx && ptr) {
        auto* node = static_cast<SpecAlloc*>(::malloc(sizeof(SpecAlloc)));
        node->ptr = ptr;
        node->next = g_spec_allocs;
        g_spec_allocs = node;
    }
}

static void tm_clear_spec_allocs()
{
    auto* node = g_spec_allocs;
    while (node) {
        auto* next = node->next;
        ::free(node->ptr);
        ::free(node);
        node = next;
    }
    g_spec_allocs = nullptr;
}

static void tm_flush_spec_allocs()
{
    auto* node = g_spec_allocs;
    while (node) {
        auto* next = node->next;
        ::free(node);
        node = next;
    }
    g_spec_allocs = nullptr;
}

// ── Deferred-free (transaction-safe free) ──────────────────
struct FreeNode {
    FreeNode* next;
    void* ptr;
};

thread_local FreeNode* g_deferred_frees = nullptr;

std::atomic<int8_t> tm_is_init_ready{0};

// ── Forward declarations ───────────────────────────────────
extern "C" void tm_clear_deferred_frees();
extern "C" void tm_flush_deferred_frees();

// ═══════════════════════════════════════════════════════════════
// Static stub implementations — internal linkage, no linker
// symbols.  The hook function-pointer globals below are the DATA
// symbols that the LLVM pass loads from for its indirect calls.
// ═══════════════════════════════════════════════════════════════

extern "C" {

static void stub_begin()
{
	auto *ts = (TMThreadState*)&tm_nested_call_counter;
	printf("tm_nested_call_counter=%d  --  ", ts->nested_call_counter);
	if (ts->nested_call_counter == 1) {
		printf("tm_begin outer\n");
		g_in_tx = true;
		tm_clear_spec_allocs();
		tm_clear_deferred_frees();
	} else {
		printf("tm_begin nested %d\n", ts->nested_call_counter);
	}
}

static void stub_end()
{
	auto *ts = (TMThreadState*)&tm_nested_call_counter;
	printf("tm_nested_call_counter=%d  --  ", ts->nested_call_counter);
	if (ts->nested_call_counter == 1) {
		tm_flush_spec_allocs();
		tm_flush_deferred_frees();
		g_in_tx = false;
		printf("tm_end outer\n");
	} else {
		printf("tm_end nested %d\n", ts->nested_call_counter);
	}
}

static int8_t  stub_read_i1(void  *a) { printf("tm_read_i1\n");       return *(int8_t*)a; }
static int16_t stub_read_i2(void  *a) { printf("tm_read_i2\n");       return *(int16_t*)a; }
static int32_t stub_read_i4(void  *a) { int32_t v = *(int32_t*)a; printf("tm_read_i4 (%p --> %i)\n", a, v); return v; }
static int64_t stub_read_i8(void  *a) { printf("tm_read_i8\n");       return *(int64_t*)a; }
static float   stub_read_f4(void  *a) { printf("tm_read_f4\n");       return *(float*)a; }
static double  stub_read_f8(void  *a) { printf("tm_read_f8\n");       return *(double*)a; }
static void   *stub_read_ptr(void **a) { printf("tm_read_ptr\n");      return *a; }

static void stub_write_i1(void  *a, uint8_t  v) { printf("tm_write_i1\n");  fflush(stdout); *(uint8_t*)a  = v; }
static void stub_write_i2(void  *a, int16_t  v) { printf("tm_write_i2\n");  *(int16_t*)a = v; }
static void stub_write_i4(void  *a, int32_t  v) { printf("tm_write_i4 (%p <-- %i)\n", a, v); *(int32_t*)a = v; }
static void stub_write_i8(void  *a, int64_t  v) { printf("tm_write_i8\n");  *(int64_t*)a = v; }
static void stub_write_f4(void  *a, float    v) { printf("tm_write_f4\n");  *(float*)a   = v; }
static void stub_write_f8(void  *a, double   v) { printf("tm_write_f8\n");  *(double*)a  = v; }
static void stub_write_ptr(void **a, void    *v) { printf("tm_write_ptr\n"); *a = v; }

static void *stub_malloc(size_t s) { void *p = ::malloc(s); tm_track_spec_alloc(p); return p; }
static void *stub_calloc(size_t n, size_t s) { void *p = ::calloc(n, s); tm_track_spec_alloc(p); return p; }
static void *stub_realloc(void *p, size_t s) { p = ::realloc(p, s); tm_track_spec_alloc(p); return p; }

static void stub_free(void *p)
{
	if (g_in_tx) {
		auto* node = static_cast<FreeNode*>(::malloc(sizeof(FreeNode)));
		node->ptr = p;
		node->next = g_deferred_frees;
		g_deferred_frees = node;
	} else {
		::free(p);
	}
}

static void stub_init()
{
	printf("tm_init\n");
	fflush(stdout);
	tm_is_init_ready.store(1);
}

static void stub_exit()
{
	tm_is_init_ready.store(0);
	printf("tm_exit\n");
	fflush(stdout);
}

static void stub_init_thread()
{
	if (is_tm_init_thread_ready == 0) {
		printf("tm_init_thread\n");
		fflush(stdout);
		is_tm_init_thread_ready = 1;
	}
}

static void stub_exit_thread()
{
	if (is_tm_init_thread_ready == 1) {
		printf("tm_exit_thread\n");
		fflush(stdout);
		is_tm_init_thread_ready = 0;
	}
}

static void stub_set_jmpbuf(void*) {}

static void *stub_get_env() { return (sigjmp_buf*)&tm_jmpbuf; }

static TMThreadState *stub_get_thread_state() {
    return (TMThreadState*)&tm_nested_call_counter;
}

static std::recursive_mutex g_serialize_mutex;
static void stub_serialize_lock() { g_serialize_mutex.lock(); }
static void stub_serialize_unlock() { g_serialize_mutex.unlock(); }

static void stub_memset(void *dst, uint8_t value, size_t sz) {
    assert(sz < TM_BUFFER_SIZE);
    memset(dst, value, sz);
    printf("tm_memset\n");
}

} // extern "C"

// ═══════════════════════════════════════════════════════════════
// Hook function-pointer globals — the LLVM pass loads from these
// DATA symbols to get function pointers for indirect calls.
// ═══════════════════════════════════════════════════════════════

extern "C" {

void     (*tm_init)()                          = stub_init;
void     (*tm_exit)()                          = stub_exit;
void     (*tm_init_thread)()                   = stub_init_thread;
void     (*tm_exit_thread)()                   = stub_exit_thread;
void     (*tm_begin)()                         = (void(*)())stub_begin;
void     (*tm_end)()                           = (void(*)())stub_end;
void     (*tm_set_jmpbuf)(void*)               = stub_set_jmpbuf;
void    *(*tm_get_env)()                       = stub_get_env;
void     (*tm_serialize_lock)()                = stub_serialize_lock;
void     (*tm_serialize_unlock)()              = stub_serialize_unlock;
void     (*tm_memset)(void*, uint8_t, size_t)  = stub_memset;

void    *(*tm_malloc)(size_t)               = (void*(*)(size_t))stub_malloc;
void    *(*tm_calloc)(size_t, size_t)       = (void*(*)(size_t,size_t))stub_calloc;
void    *(*tm_realloc)(void*, size_t)       = (void*(*)(void*,size_t))stub_realloc;
void     (*tm_free)(void*)                  = (void(*)(void*))stub_free;
uint8_t  (*tm_read_i1)(uint8_t*)            = (uint8_t(*)(uint8_t*))stub_read_i1;
uint16_t (*tm_read_i2)(uint16_t*)           = (uint16_t(*)(uint16_t*))stub_read_i2;
uint32_t (*tm_read_i4)(uint32_t*)           = (uint32_t(*)(uint32_t*))stub_read_i4;
uint64_t (*tm_read_i8)(uint64_t*)           = (uint64_t(*)(uint64_t*))stub_read_i8;
float    (*tm_read_f4)(float*)              = (float(*)(float*))stub_read_f4;
double   (*tm_read_f8)(double*)             = (double(*)(double*))stub_read_f8;
void    *(*tm_read_ptr)(void**)             = (void*(*)(void**))stub_read_ptr;
void     (*tm_write_i1)(uint8_t*, uint8_t)  = (void(*)(uint8_t*,uint8_t))stub_write_i1;
void     (*tm_write_i2)(uint16_t*, uint16_t)= (void(*)(uint16_t*,uint16_t))stub_write_i2;
void     (*tm_write_i4)(uint32_t*, uint32_t)= (void(*)(uint32_t*,uint32_t))stub_write_i4;
void     (*tm_write_i8)(uint64_t*, int64_t) = (void(*)(uint64_t*,int64_t))stub_write_i8;
void     (*tm_write_f4)(float*, float)      = (void(*)(float*,float))stub_write_f4;
void     (*tm_write_f8)(double*, double)    = (void(*)(double*,double))stub_write_f8;
void     (*tm_write_ptr)(void**, void*)      = (void(*)(void**,void*))stub_write_ptr;
void *(*tm_get_thread_state)() = (void*(*)())stub_get_thread_state;

// ── Functions called directly (not through hook pointers) ───
// These are called directly by the LLVM pass preamble, so they
// must be actual function symbols (TEXT), not function-pointer
// globals.

void tm_flush_deferred_frees()
{
	auto* node = g_deferred_frees;
	while (node) {
		auto* next = node->next;
		std::free(node->ptr);
		std::free(node);
		node = next;
	}
	g_deferred_frees = nullptr;
}

void tm_clear_deferred_frees()
{
	auto* node = g_deferred_frees;
	while (node) {
		auto* next = node->next;
		std::free(node);
		node = next;
	}
	g_deferred_frees = nullptr;
}

// sigsetjmp/longjmp implementation
int tm_setjmp() { return sigsetjmp(*(sigjmp_buf *)tm_jmpbuf, 0); }

void tm_longjmp(int val) { longjmp(*(sigjmp_buf *)tm_jmpbuf, val); }

void consume_ptr(volatile void *ptr) { (void)ptr; }

} // extern "C"

// sigsetjmp hook: the LLVM pass declares @tm_sigsetjmp as a DATA ptr.
// On Linux the hook is `__sigsetjmp` (a glibc internal name, no conflict).
// On macOS/BSD we use `tm_sigsetjmp` to avoid clashing with the POSIX
// function `sigsetjmp` from <setjmp.h>.
__asm__(".globl _tm_sigsetjmp\n"
        ".data\n"
        ".align 3\n"
        "_tm_sigsetjmp: .quad _sigsetjmp\n");
