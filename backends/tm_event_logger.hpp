#pragma once

/**
 * tm_event_logger.hpp — Thread-local ring-buffer event logger for TM backends.
 *
 * Activated by #define TM_EVENT_LOG before including this header.
 * When inactive, all macros expand to nothing (zero overhead).
 *
 * Usage:
 *   #define TM_EVENT_LOG
 *   #include "tm_event_logger.hpp"
 *   ...
 *   TM_EVENT(TX_BEGIN, (uint64_t)tx, 0);
 *   TM_EVENT(READ_LOCK, (uint64_t)addr, version);
 *
 * On SIGSEGV (or by calling dump_events()), the ring buffer is printed
 * to stderr with timestamps, thread IDs, and event payloads.
 */

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace stm {

#ifdef TM_EVENT_LOG

// ---------------------------------------------------------------------------
// Event type enum
// ---------------------------------------------------------------------------
enum class EventType : uint8_t {
    TX_BEGIN,
    TX_END,
    TX_ABORT,
    TX_RETRY,
    READ_LOCK_ACQUIRE,
    READ_VERSION_CHECK,
    WRITE_LOCK_ACQUIRE,
    WRITE_SET_INSERT,
    COMMIT_LOCK_ACQUIRE,
    COMMIT_WRITEBACK,
    COMMIT_SUCCESS,
    GAP_CHECK,
    LOCK_RELEASE,
    RETRY_END
};

static constexpr const char* event_name(EventType t) {
    switch (t) {
        case EventType::TX_BEGIN:            return "TX_BEGIN";
        case EventType::TX_END:              return "TX_END";
        case EventType::TX_ABORT:            return "TX_ABORT";
        case EventType::TX_RETRY:            return "TX_RETRY";
        case EventType::READ_LOCK_ACQUIRE:   return "READ_LOCK_ACQUIRE";
        case EventType::READ_VERSION_CHECK:  return "READ_VERSION_CHECK";
        case EventType::WRITE_LOCK_ACQUIRE:  return "WRITE_LOCK_ACQUIRE";
        case EventType::WRITE_SET_INSERT:    return "WRITE_SET_INSERT";
        case EventType::COMMIT_LOCK_ACQUIRE: return "COMMIT_LOCK_ACQUIRE";
        case EventType::COMMIT_WRITEBACK:    return "COMMIT_WRITEBACK";
        case EventType::COMMIT_SUCCESS:      return "COMMIT_SUCCESS";
        case EventType::GAP_CHECK:           return "GAP_CHECK";
        case EventType::LOCK_RELEASE:        return "LOCK_RELEASE";
        case EventType::RETRY_END:           return "RETRY_END";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Ring buffer constants
// ---------------------------------------------------------------------------
static constexpr size_t EVENT_RING_SIZE = 16384;
static constexpr size_t EVENT_RING_MASK = EVENT_RING_SIZE - 1;

// ---------------------------------------------------------------------------
// Event entry
// ---------------------------------------------------------------------------
struct EventEntry {
    uint64_t  timestamp;   // rdtsc
    uint64_t  thread_id;   // pthread_self() as uint64_t
    EventType type;
    uint64_t  addr1;       // primary address (e.g. lock addr, tx ptr)
    uint64_t  addr2;       // secondary address (e.g. read-set addr)
    uint64_t  data;        // version, size, or status
};

// ---------------------------------------------------------------------------
// Per-thread ring buffer
// ---------------------------------------------------------------------------
struct EventRing {
    EventEntry entries[EVENT_RING_SIZE];
    std::atomic<uint64_t> head{0};

    void log(EventType type, uint64_t addr1, uint64_t addr2, uint64_t data) {
        uint64_t pos = head.fetch_add(1, std::memory_order_relaxed) & EVENT_RING_MASK;
        entries[pos].timestamp  = rdtsc();
        entries[pos].thread_id  = (uint64_t)pthread_self();
        entries[pos].type       = type;
        entries[pos].addr1      = addr1;
        entries[pos].addr2      = addr2;
        entries[pos].data       = data;
    }

    // Dump ALL entries (sorted by position, oldest first).
    // If `count` is 0, dumps all; otherwise dumps up to `count` most recent.
    void dump(size_t count = 0, FILE *fp = stderr) {
        uint64_t h = head.load(std::memory_order_acquire);
        uint64_t start = (count > 0 && count < h) ? h - count : 0;
        fprintf(fp, "--- Event log (%llu entries, dumping from #%llu) ---\n",
                (unsigned long long)h, (unsigned long long)start);
        for (uint64_t i = start; i < h; i++) {
            EventEntry &e = entries[i & EVENT_RING_MASK];
            fprintf(fp, "[%12llu] thr=0x%llx %-20s addr1=0x%llx addr2=0x%llx data=%llu\n",
                    (unsigned long long)e.timestamp,
                    (unsigned long long)e.thread_id,
                    event_name(e.type),
                    (unsigned long long)e.addr1,
                    (unsigned long long)e.addr2,
                    (unsigned long long)e.data);
        }
        fflush(fp);
    }

private:
    static inline uint64_t rdtsc() {
#if defined(__x86_64__)
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
        uint64_t val;
        asm volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
        return val;
#else
        return 0;
#endif
    }
};

// ---------------------------------------------------------------------------
// Thread-local ring buffer instance
// ---------------------------------------------------------------------------
inline EventRing& get_event_ring() {
    thread_local EventRing ring;
    return ring;
}

// ---------------------------------------------------------------------------
// SIGSEGV handler — dump events then exit
// ---------------------------------------------------------------------------
static inline void sigsegv_handler(int sig, siginfo_t *info, void *ucontext) {
    fprintf(stderr, "\n=== SIGSEGV at address %p ===\n", info->si_addr);
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    get_event_ring().dump(512, stderr);
    _exit(1);
}

static inline void install_sigsegv_handler() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

// ===========================================================================
// Macros for use in TM backend code
// ===========================================================================
#define TM_EVENT(type, addr1, data) \
    do { \
        stm::get_event_ring().log(stm::EventType::type, (uint64_t)(addr1), 0, (uint64_t)(data)); \
    } while (0)

#define TM_EVENT2(type, addr1, addr2, data) \
    do { \
        stm::get_event_ring().log(stm::EventType::type, (uint64_t)(addr1), (uint64_t)(addr2), (uint64_t)(data)); \
    } while (0)

#define TM_EVENT_DUMP(count) \
    do { \
        stm::get_event_ring().dump(count, stderr); \
    } while (0)

#define TM_EVENT_INSTALL_SIGSEGV() \
    do { \
        stm::install_sigsegv_handler(); \
    } while (0)

#else // !TM_EVENT_LOG

#define TM_EVENT(type, addr1, data)        ((void)0)
#define TM_EVENT2(type, addr1, addr2, data) ((void)0)
#define TM_EVENT_DUMP(count)               ((void)0)
#define TM_EVENT_INSTALL_SIGSEGV()         ((void)0)

#endif // TM_EVENT_LOG

} // namespace stm
