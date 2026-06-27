#pragma once

// ═══════════════════════════════════════════════════════════════════════
//  tm_backend_macros.hpp — Shared macros for backend runtime files
//
//  These macros eliminate ~1,000 lines of duplicated boilerplate across
//  14 backend runtime files (NOrec, TL2, TinySTM, Romulus, LeftRight,
//  XTM, SwissTM, SGL, TSXSGL, SPHT, NVHTM, DUDETM, PersistentSGL,
//  DistributedSGL).
//
//  Usage:
//
//    #include "tm_backend_macros.hpp"
//
//    // Replace 14 read/write wrapper functions:
//    TM_DEFINE_READ_WRITE_HOOKS(my_backend)
//
//    // If backend's tm_write_i8 takes uint64_t (vs int64_t):
//    TM_DEFINE_READ_WRITE_HOOKS_WITH_I8_CAST(my_backend, static_cast<uint64_t>(v))
//
//    // Replace the TMRealHooks registration table:
//    TM_REAL_HOOKS_TABLE(my_prefix)
//    TM_REAL_HOOKS_TABLE_EXT(my_prefix)  // includes .get_env + .set_jmpbuf
//
//    // Replace plugin extern "C" functions (6 backends):
//    TM_DEFINE_PLUGIN_RW(my_backend)
//
// ═══════════════════════════════════════════════════════════════════════

#include <cassert>
#include <cstdint>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════
//  TM_DEFINE_READ_WRITE_HOOKS(ns) — 14 read/write wrappers
//
//  Generates static functions real_tm_read_i1 through real_tm_write_ptr
//  that delegate to ns::tm_read_i1(...), ns::tm_write_i1(...), etc.
//
//  This version does NOT cast write_i8's val parameter. Use the _WITH_I8_CAST
//  variant if your backend's tm_write_i8 expects uint64_t instead of int64_t.
//
//  ⚠ TL2: overrides read_ptr/write_ptr with volatile cast manually.
//  ⚠ TSXSGL: overrides write_i8 with volatile pointer cast manually.
//  ⚠ PersistentSGL: overrides all writes with persist logic manually.
// ═══════════════════════════════════════════════════════════════════════

#define TM_DEFINE_READ_WRITE_HOOKS(ns) \
    static uint8_t  real_tm_read_i1(uint8_t  *a) { return ns::tm_read_i1(a); } \
    static uint16_t real_tm_read_i2(uint16_t *a) { return ns::tm_read_i2(a); } \
    static uint32_t real_tm_read_i4(uint32_t *a) { return ns::tm_read_i4(a); } \
    static uint64_t real_tm_read_i8(uint64_t *a) { return ns::tm_read_i8(a); } \
    static float    real_tm_read_f4(float    *a) { return ns::tm_read_f4(a); } \
    static double   real_tm_read_f8(double   *a) { return ns::tm_read_f8(a); } \
    static void    *real_tm_read_ptr(void   **a) { return ns::tm_read_ptr(a); } \
    static void real_tm_write_i1(uint8_t  *a, uint8_t  v) { ns::tm_write_i1(a, v); } \
    static void real_tm_write_i2(uint16_t *a, uint16_t v) { ns::tm_write_i2(a, v); } \
    static void real_tm_write_i4(uint32_t *a, uint32_t v) { ns::tm_write_i4(a, v); } \
    static void real_tm_write_i8(uint64_t *a, int64_t  v) { ns::tm_write_i8(a, v); } \
    static void real_tm_write_f4(float    *a, float    v) { ns::tm_write_f4(a, v); } \
    static void real_tm_write_f8(double   *a, double   v) { ns::tm_write_f8(a, v); } \
    static void real_tm_write_ptr(void   **a, void    *v) { ns::tm_write_ptr(a, v); }

// Version with an explicit cast expression on write_i8's val parameter.
// Example: TM_DEFINE_READ_WRITE_HOOKS_WITH_I8_CAST(norec, static_cast<uint64_t>(v))
#define TM_DEFINE_READ_WRITE_HOOKS_WITH_I8_CAST(ns, cast_expr) \
    static uint8_t  real_tm_read_i1(uint8_t  *a) { return ns::tm_read_i1(a); } \
    static uint16_t real_tm_read_i2(uint16_t *a) { return ns::tm_read_i2(a); } \
    static uint32_t real_tm_read_i4(uint32_t *a) { return ns::tm_read_i4(a); } \
    static uint64_t real_tm_read_i8(uint64_t *a) { return ns::tm_read_i8(a); } \
    static float    real_tm_read_f4(float    *a) { return ns::tm_read_f4(a); } \
    static double   real_tm_read_f8(double   *a) { return ns::tm_read_f8(a); } \
    static void    *real_tm_read_ptr(void   **a) { return ns::tm_read_ptr(a); } \
    static void real_tm_write_i1(uint8_t  *a, uint8_t  v) { ns::tm_write_i1(a, v); } \
    static void real_tm_write_i2(uint16_t *a, uint16_t v) { ns::tm_write_i2(a, v); } \
    static void real_tm_write_i4(uint32_t *a, uint32_t v) { ns::tm_write_i4(a, v); } \
    static void real_tm_write_i8(uint64_t *a, int64_t  v) { ns::tm_write_i8(a, cast_expr); } \
    static void real_tm_write_f4(float    *a, float    v) { ns::tm_write_f4(a, v); } \
    static void real_tm_write_f8(double   *a, double   v) { ns::tm_write_f8(a, v); } \
    static void real_tm_write_ptr(void   **a, void    *v) { ns::tm_write_ptr(a, v); }

// ═══════════════════════════════════════════════════════════════════════
//  TM_DEFINE_PLUGIN_RW(ns) — Plugin extern "C" read/write functions
//
//  Generates tm_read_i16, tm_read_i32, tm_read_i64, tm_write_i16,
//  tm_write_i32, tm_write_i64, tm_read_z, tm_write_z, tm_memset
//  for plugin-instrumented binaries.
//
//  Requires TM_BUFFER_SIZE and __thread uint8_t tm_buffer[] to be
//  defined in the including file.
// ═══════════════════════════════════════════════════════════════════════

#define TM_DEFINE_PLUGIN_RW(ns) \
    void tm_read_i16(void *addr, void *out) { \
        auto *out_words = static_cast<uint64_t *>(out); \
        out_words[0] = ns::tm_read_i8(static_cast<uint64_t *>(addr) + 0); \
        out_words[1] = ns::tm_read_i8(static_cast<uint64_t *>(addr) + 1); \
    } \
    void tm_read_i32(void *addr, void *out) { \
        auto *out_words = static_cast<uint64_t *>(out); \
        for (int i = 0; i < 4; i++) \
            out_words[i] = ns::tm_read_i8(static_cast<uint64_t *>(addr) + i); \
    } \
    void tm_read_i64(void *addr, void *out) { \
        auto *out_words = static_cast<uint64_t *>(out); \
        for (int i = 0; i < 8; i++) \
            out_words[i] = ns::tm_read_i8(static_cast<uint64_t *>(addr) + i); \
    } \
    void tm_write_i16(void *addr, void *val) { \
        auto *val_words = static_cast<const uint64_t *>(val); \
        for (int i = 0; i < 2; i++) \
            ns::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]); \
    } \
    void tm_write_i32(void *addr, void *val) { \
        auto *val_words = static_cast<const uint64_t *>(val); \
        for (int i = 0; i < 4; i++) \
            ns::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]); \
    } \
    void tm_write_i64(void *addr, void *val) { \
        auto *val_words = static_cast<const uint64_t *>(val); \
        for (int i = 0; i < 8; i++) \
            ns::tm_write_i8(static_cast<uint64_t *>(addr) + i, val_words[i]); \
    } \
    void *tm_read_z(uint8_t *addr, uint64_t len) { \
        assert(len < TM_BUFFER_SIZE); \
        for (uint64_t i = 0; i < len / 8; i++) \
            tm_buffer[i] = ns::tm_read_i8(((uint64_t *)addr) + i); \
        uint64_t rem = len % 8; \
        for (uint64_t i = 0; i < rem; i++) \
            tm_buffer[i] = ns::tm_read_i1(addr + (len - rem - 1) + i); \
        return tm_buffer; \
    } \
    void tm_write_z(uint8_t *dst, uint8_t *src, uint64_t len) { \
        for (uint64_t i = 0; i < len / 8; i++) \
            ns::tm_write_i8(((uint64_t *)dst) + i, *(((uint64_t *)src) + i)); \
        uint64_t rem = len % 8; \
        for (uint64_t i = 0; i < rem; i++) \
            ns::tm_write_i1(dst + (len - rem - 1) + i, *(src + (len - rem - 1) + i)); \
    } \
    void tm_memset(uint8_t *addr, uint8_t val, uint64_t len) { \
        for (uint64_t i = 0; i < len; i++) \
            ns::tm_write_i1(&addr[i], val); \
    }

// ═══════════════════════════════════════════════════════════════════════
//  TM_REAL_HOOKS_TABLE(prefix) — TMRealHooks registration table
//
//  Generates "const TMRealHooks g_##prefix##_hooks = { ... }" with
//  all 20 standard fields (begin, end, malloc, calloc, realloc, free,
//  7 reads, 7 writes, get_thread_state).
//
//  Extended version includes .get_env and .set_jmpbuf.
// ═══════════════════════════════════════════════════════════════════════

#define TM_REAL_HOOKS_TABLE(prefix) \
    const TMRealHooks g_##prefix##_hooks = { \
        .begin    = real_tm_begin, \
        .end      = real_tm_end, \
        .malloc   = real_tm_malloc, \
        .calloc   = real_tm_calloc, \
        .realloc  = real_tm_realloc, \
        .free     = real_tm_free, \
        .read_i1  = real_tm_read_i1, \
        .read_i2  = real_tm_read_i2, \
        .read_i4  = real_tm_read_i4, \
        .read_i8  = real_tm_read_i8, \
        .read_f4  = real_tm_read_f4, \
        .read_f8  = real_tm_read_f8, \
        .read_ptr = real_tm_read_ptr, \
        .write_i1  = real_tm_write_i1, \
        .write_i2  = real_tm_write_i2, \
        .write_i4  = real_tm_write_i4, \
        .write_i8  = real_tm_write_i8, \
        .write_f4  = real_tm_write_f4, \
        .write_f8  = real_tm_write_f8, \
        .write_ptr = real_tm_write_ptr, \
        .get_thread_state = real_tm_get_thread_state, \
    }; // <-- semicolon required by macro invocation syntax

#define TM_REAL_HOOKS_TABLE_EXT(prefix) \
    const TMRealHooks g_##prefix##_hooks = { \
        .begin    = real_tm_begin, \
        .end      = real_tm_end, \
        .malloc   = real_tm_malloc, \
        .calloc   = real_tm_calloc, \
        .realloc  = real_tm_realloc, \
        .free     = real_tm_free, \
        .read_i1  = real_tm_read_i1, \
        .read_i2  = real_tm_read_i2, \
        .read_i4  = real_tm_read_i4, \
        .read_i8  = real_tm_read_i8, \
        .read_f4  = real_tm_read_f4, \
        .read_f8  = real_tm_read_f8, \
        .read_ptr = real_tm_read_ptr, \
        .write_i1  = real_tm_write_i1, \
        .write_i2  = real_tm_write_i2, \
        .write_i4  = real_tm_write_i4, \
        .write_i8  = real_tm_write_i8, \
        .write_f4  = real_tm_write_f4, \
        .write_f8  = real_tm_write_f8, \
        .write_ptr = real_tm_write_ptr, \
        .get_env    = real_tm_get_env, \
        .set_jmpbuf = real_tm_set_jmpbuf, \
        .get_thread_state = real_tm_get_thread_state, \
    };
