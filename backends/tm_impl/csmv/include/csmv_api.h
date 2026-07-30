#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>

// ── CSMV Configuration ──────────────────────────────────────────

#define CSMV_TABLE_SIZE  (1 << 20)   // 1M object entries
#define CSMV_MAX_READS   64          // max reads per tx
#define CSMV_MAX_WRITES  32          // max writes per tx
#define CSMV_GC_INTERVAL 1024        // GC every N commits

// ── CSMV Version Node ───────────────────────────────────────────
// Each version node holds a timestamp, the data value (word-sized),
// and a pointer to the previous (older) version.

struct CSMVVersionNode {
    uint64_t          timestamp;
    uint64_t          value;
    CSMVVersionNode  *next;  // older versions
};

// ── CSMV Object Entry ───────────────────────────────────────────
// Each object entry has a mutex for commit serialization and an
// atomic head pointer to the newest version node.

struct CSMVObjectEntry {
    std::mutex                lock;
    std::atomic<CSMVVersionNode*> head{nullptr};
};

// ── CSMV Thread Transaction State ───────────────────────────────

struct CSMVThreadTx {
    uint64_t start_clock;
    int      num_reads;
    int      num_writes;
    struct { CSMVObjectEntry *entry; uint64_t observed_ts; } reads[CSMV_MAX_READS];
    struct { CSMVObjectEntry *entry; void *data_addr; CSMVVersionNode *node; uint64_t val; uint8_t bytes; } writes[CSMV_MAX_WRITES];
    int      commit_count;
};

// ── Host API (CPU side) ─────────────────────────────────────────

// Lifecycle
void csmv_tm_init(void);
void csmv_tm_exit(void);
void csmv_tm_init_thread(void);
void csmv_tm_exit_thread(void);
void csmv_tm_begin(void);
void csmv_tm_end(void);

// Allocation (TM region)
void *csmv_tm_malloc(size_t sz);
void  csmv_tm_free(void *p);
void *csmv_tm_calloc(size_t nmemb, size_t sz);
void *csmv_tm_realloc(void *p, size_t sz);

// Reads
uint8_t  csmv_tm_read_i1(uint8_t *addr);
uint16_t csmv_tm_read_i2(uint16_t *addr);
uint32_t csmv_tm_read_i4(uint32_t *addr);
uint64_t csmv_tm_read_i8(uint64_t *addr);
float    csmv_tm_read_f4(float *addr);
double   csmv_tm_read_f8(double *addr);
void    *csmv_tm_read_ptr(void **addr);

// Writes
void csmv_tm_write_i1(uint8_t *addr, uint8_t val);
void csmv_tm_write_i2(uint16_t *addr, uint16_t val);
void csmv_tm_write_i4(uint32_t *addr, uint32_t val);
void csmv_tm_write_i8(uint64_t *addr, int64_t val);
void csmv_tm_write_f4(float *addr, float val);
void csmv_tm_write_f8(double *addr, double val);
void csmv_tm_write_ptr(void **addr, void *val);
