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

#include "../../backends/tm_impl/common/tm_thread_state.hpp"

#define TM_BUFFER_SIZE 1024

extern "C" {

// tm_nested_call_counter and tm_longjmp_ret are thread-local globals.
// tm_get_thread_state() returns a pointer aliasing them so that both
// direct TLS access (test_retry.cpp) and TMThreadState* access (LLVM
// pass preamble via GEP at offsets 0 and 4) use the same storage.
__thread int32_t tm_nested_call_counter = 0;
__thread int32_t tm_longjmp_ret = 0;

TMThreadState *tm_get_thread_state() {
    return reinterpret_cast<TMThreadState*>(&tm_nested_call_counter);
}

__thread unsigned char tm_jmpbuf[256];
__thread uint8_t is_tm_init_thread_ready = 0;
thread_local uint8_t tm_buffer[TM_BUFFER_SIZE] = {0};

thread_local bool g_in_tx = false;

// ── Speculative-allocation tracking ───────────────────────
// When tm_malloc is called inside a transaction, the allocated memory
// is "speculative": it becomes permanent only if the transaction commits.
// On abort, the undo log restores container pointers and the speculative
// allocation must be freed to avoid leaks.
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
// Inside a transaction, tm_free records the pointer in a thread-local
// list instead of calling ::free immediately.  On commit, all pending
// frees are executed.  On abort (discard without freeing), the undo log
// has restored container pointers so the "freed" memory is still live.
struct FreeNode {
    FreeNode* next;
    void* ptr;
};

thread_local FreeNode* g_deferred_frees = nullptr;

std::atomic<int8_t> tm_is_init_ready{0};

void tm_init()
{
	printf("tm_init\n");
	fflush(stdout);
	tm_is_init_ready.store(1);
}

void tm_exit()
{
	tm_is_init_ready.store(0);
	printf("tm_exit\n");
	fflush(stdout);
}

void tm_init_thread()
{
	if (is_tm_init_thread_ready == 0) {
		printf("tm_init_thread\n");
		fflush(stdout);
		is_tm_init_thread_ready = 1;
	}
}

void tm_exit_thread()
{
	if (is_tm_init_thread_ready == 1) {
		printf("tm_exit_thread\n");
		fflush(stdout);
		is_tm_init_thread_ready = 0;
	}
}

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

static std::recursive_mutex g_serialize_mutex;

void tm_serialize_lock()
{
	g_serialize_mutex.lock();
}

void tm_serialize_unlock()
{
	g_serialize_mutex.unlock();
}

sigjmp_buf* tm_get_env() { return (sigjmp_buf*)&tm_jmpbuf; }

void tm_begin()
{
	auto *ts = tm_get_thread_state();
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

void tm_end()
{
	auto *ts = tm_get_thread_state();
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

// sigsetjmp/longjmp implementation
int tm_setjmp() { return sigsetjmp(*(sigjmp_buf *)tm_jmpbuf, 0); }

void tm_set_jmpbuf(void *buf) { }

void tm_longjmp(int val) { longjmp(*(sigjmp_buf *)tm_jmpbuf, val); }

int8_t tm_read_i1(void *addr)
{
	int8_t val = *(int8_t *)addr;
	printf("tm_read_i1\n");
	return val;
}

int16_t tm_read_i2(void *addr)
{
	int16_t val = *(int16_t *)addr;
	printf("tm_read_i2\n");
	return val;
}

int32_t tm_read_i4(void *addr)
{
	int32_t val = *(int32_t *)addr;
	printf("tm_read_i4 (%p --> %i)\n", addr, val);
	return val;
}

int64_t tm_read_i8(void *addr)
{
	int64_t val = *(int64_t *)addr;
	printf("tm_read_i8\n");
	return val;
}

float tm_read_f4(void *addr)
{
	float val = *(float *)addr;
	printf("tm_read_f4\n");
	return val;
}

double tm_read_f8(void *addr)
{
	double val = *(double *)addr;
	printf("tm_read_f8\n");
	return val;
}

void *tm_read_ptr(void *addr)
{
	void *val = *(void **)addr;
	printf("tm_read_ptr\n");
	return val;
}

void consume_ptr(volatile void *ptr) { (void)ptr; }

void tm_write_i1(void *addr, uint8_t val)
{
	fflush(stdout);
	fprintf(stderr, "CALLING tm_write_i1\n");
	fflush(stderr);
	printf("tm_write_i1\n");
	fflush(stdout);
	*(uint8_t *)addr = val;
}

void tm_write_i2(void *addr, int16_t val)
{
	printf("tm_write_i2\n");
	*(int16_t *)addr = val;
}

void tm_write_i4(void *addr, int32_t val)
{
	printf("tm_write_i4 (%p <-- %i)\n", addr, val);
	*(int32_t *)addr = val;
}

void tm_write_i8(void *addr, int64_t val)
{
	printf("tm_write_i8\n");
	*(int64_t *)addr = val;
}

void tm_write_f4(void *addr, float val)
{
	printf("tm_write_f4\n");
	*(float *)addr = val;
}

void tm_write_f8(void *addr, double val)
{
	printf("tm_write_f8\n");
	*(double *)addr = val;
}

void tm_write_ptr(void *addr, void *val)
{
	printf("tm_write_ptr\n");
	*(void **)addr = val;
}

void tm_memset(void *dst, uint8_t value, size_t sz)
{
	assert(sz < TM_BUFFER_SIZE);
	memset(dst, value, sz);
	printf("tm_memset\n");
}

// TM-aware allocators
void *tm_malloc(size_t size) { void* p = malloc(size); tm_track_spec_alloc(p); return p; }
void *tm_calloc(size_t nmemb, size_t size) { void* p = calloc(nmemb, size); tm_track_spec_alloc(p); return p; }
void *tm_realloc(void *ptr, size_t size) { void* p = realloc(ptr, size); tm_track_spec_alloc(p); return p; }

// Deferred-free: inside a transaction, record the pointer for commit-time
// free.  Outside a transaction, free immediately.
void tm_free(void *ptr)
{
	if (g_in_tx) {
		auto* node = static_cast<FreeNode*>(::malloc(sizeof(FreeNode)));
		node->ptr = ptr;
		node->next = g_deferred_frees;
		g_deferred_frees = node;
	} else {
		::free(ptr);
	}
}
}
