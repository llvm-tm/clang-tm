// tm_trace_runtime.cpp
// Runtime trace collector for --emit-tm-trace pipeline.
// Compile and link alongside the benchmark binary.
// Set TM_TRACE_FILE env var to redirect output (default: tm_trace.jsonl).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>

static constexpr size_t TRACE_BUF_SIZE = 1 << 20;

struct TraceEntry {
    uint64_t timestamp;
    uint32_t thread_id;
    uint32_t type_code; // 0=read, 1=write, 2=begin, 3=end, 4=malloc, 5=free, 6=abort, 7=computation
    uint64_t addr;
    uint64_t width;
    uint64_t value;
};

static TraceEntry g_trace_buf[TRACE_BUF_SIZE];
static std::atomic<uint64_t> g_trace_head{0};
static std::atomic<uint64_t> g_trace_ts{0};
static FILE *g_trace_file = nullptr;

__attribute__((constructor)) static void tm_trace_init() {
    const char *path = getenv("TM_TRACE_FILE");
    if (path && path[0]) {
        g_trace_file = fopen(path, "w");
        if (!g_trace_file)
            fprintf(stderr, "tm_trace: cannot open '%s', using tm_trace.jsonl\n", path);
    }
    if (!g_trace_file) {
        g_trace_file = fopen("tm_trace.jsonl", "w");
    }
}

__attribute__((destructor)) static void tm_trace_fini() {
    uint64_t head = g_trace_head.load();
    if (!g_trace_file)
        return;
    for (uint64_t i = 0; i < head; i++) {
        TraceEntry &e = g_trace_buf[i];
        fprintf(g_trace_file,
                "%lu %u %u 0x%lx %lu 0x%lx\n",
                e.timestamp, e.thread_id, e.type_code,
                e.addr, e.width, e.value);
    }
    fflush(g_trace_file);
    if (g_trace_file && g_trace_file != stderr && g_trace_file != stdout)
        fclose(g_trace_file);
}

// Hashing thread IDs to a small space for readability
static uint32_t tm_trace_tid() {
    uint64_t id = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return (uint32_t)(id & 0xffff);
}

static void tm_trace_push(uint32_t type_code, void *addr, uint64_t width, uint64_t value) {
    uint64_t idx = g_trace_head.fetch_add(1, std::memory_order_relaxed);
    if (idx >= TRACE_BUF_SIZE)
        return;
    uint64_t ts = g_trace_ts.fetch_add(1, std::memory_order_relaxed);
    g_trace_buf[idx] = {
        .timestamp = ts,
        .thread_id = tm_trace_tid(),
        .type_code = type_code,
        .addr = (uint64_t)(uintptr_t)addr,
        .width = width,
        .value = value,
    };
}

// Internal implementation called through the DATA pointer below
extern "C" void tm_trace_impl(uint32_t type_code, void *addr, uint64_t width, uint64_t value) {
    tm_trace_push(type_code, addr, width, value);
}

// Hook pointer variable that the LLVM pass loads through
extern "C" void (*tm_trace)(uint32_t, void*, uint64_t, uint64_t) = tm_trace_impl;
