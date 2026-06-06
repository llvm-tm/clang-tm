#pragma once
#include <cstddef>
#include <cstdint>
#include <csetjmp>

// ── Thread-local state (common to all backends) ──
extern "C" {
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;
}

// ── extern "C" declarations per backend ──
extern "C" {
void tm_init();
void tm_exit();
void tm_init_thread();
void tm_exit_thread();
void tm_begin();
void tm_end();
void *tm_malloc(std::size_t n);
void *tm_calloc(std::size_t n, std::size_t s);
void *tm_realloc(void *p, std::size_t n);
void tm_free(void *p);
void tm_serialize_lock();
void tm_serialize_unlock();

#if defined(TM_BACKEND_TINYSTM)

uint32_t tm_read_i4(uint32_t *addr);
void tm_write_i4(uint32_t *addr, uint32_t val);
uint64_t tm_read_i8(uint64_t *addr);
void tm_write_i8(uint64_t *addr, uint64_t val);
uint16_t tm_read_i2(uint16_t *addr);
void tm_write_i2(uint16_t *addr, uint16_t val);
uint8_t tm_read_i1(uint8_t *addr);
void tm_write_i1(uint8_t *addr, uint8_t val);
float tm_read_f4(float *addr);
void tm_write_f4(float *addr, float val);
double tm_read_f8(double *addr);
void tm_write_f8(double *addr, double val);
void *tm_read_ptr(void **addr);
void tm_write_ptr(void **addr, void *val);

#elif defined(TM_BACKEND_TL2) || defined(TM_BACKEND_NOREC) || defined(TM_BACKEND_SWISSTM)

uint32_t tm_read_i4(uint32_t *addr, uint32_t symbol_id);
void tm_write_i4(uint32_t *addr, uint32_t val, uint32_t symbol_id);
uint64_t tm_read_i8(uint64_t *addr, uint32_t symbol_id);
void tm_write_i8(uint64_t *addr, uint64_t val, uint32_t symbol_id);
uint16_t tm_read_i2(uint16_t *addr, uint32_t symbol_id);
void tm_write_i2(uint16_t *addr, uint16_t val, uint32_t symbol_id);
uint8_t tm_read_i1(uint8_t *addr, uint32_t symbol_id);
void tm_write_i1(uint8_t *addr, uint8_t val, uint32_t symbol_id);
float tm_read_f4(float *addr, uint32_t symbol_id);
void tm_write_f4(float *addr, float val, uint32_t symbol_id);
double tm_read_f8(double *addr, uint32_t symbol_id);
void tm_write_f8(double *addr, double val, uint32_t symbol_id);
void *tm_read_ptr(void **addr, uint32_t symbol_id);
void tm_write_ptr(void **addr, void *val, uint32_t symbol_id);

#elif defined(TM_BACKEND_SGL)

uint32_t tm_read_i4(volatile uint32_t *addr);
void tm_write_i4(volatile uint32_t *addr, uint32_t val);
uint64_t tm_read_i8(volatile uint64_t *addr);
void tm_write_i8(volatile uint64_t *addr, uint64_t val);
uint16_t tm_read_i2(volatile uint16_t *addr);
void tm_write_i2(volatile uint16_t *addr, uint16_t val);
uint8_t tm_read_i1(volatile uint8_t *addr);
void tm_write_i1(volatile uint8_t *addr, uint8_t val);
float tm_read_f4(volatile float *addr);
void tm_write_f4(volatile float *addr, float val);
double tm_read_f8(volatile double *addr);
void tm_write_f8(volatile double *addr, double val);
void *tm_read_ptr(volatile void **addr);
void tm_write_ptr(volatile void **addr, void *val);

#endif
}

// ── Uniform C++ inline wrappers ──
#if defined(TM_BACKEND_TINYSTM)

inline uint32_t tm_test_read_i4(uint32_t *a) { return tm_read_i4(a); }
inline void tm_test_write_i4(uint32_t *a, uint32_t v) { tm_write_i4(a, v); }
inline uint64_t tm_test_read_i8(uint64_t *a) { return tm_read_i8(a); }
inline void tm_test_write_i8(uint64_t *a, uint64_t v) { tm_write_i8(a, v); }
inline uint16_t tm_test_read_i2(uint16_t *a) { return tm_read_i2(a); }
inline void tm_test_write_i2(uint16_t *a, uint16_t v) { tm_write_i2(a, v); }
inline uint8_t tm_test_read_i1(uint8_t *a) { return tm_read_i1(a); }
inline void tm_test_write_i1(uint8_t *a, uint8_t v) { tm_write_i1(a, v); }
inline float tm_test_read_f4(float *a) { return tm_read_f4(a); }
inline void tm_test_write_f4(float *a, float v) { tm_write_f4(a, v); }
inline double tm_test_read_f8(double *a) { return tm_read_f8(a); }
inline void tm_test_write_f8(double *a, double v) { tm_write_f8(a, v); }
inline void *tm_test_read_ptr(void **a) { return tm_read_ptr(a); }
inline void tm_test_write_ptr(void **a, void *v) { tm_write_ptr(a, v); }

#elif defined(TM_BACKEND_SGL)

inline uint32_t tm_test_read_i4(uint32_t *a) { return tm_read_i4(a); }
inline void tm_test_write_i4(uint32_t *a, uint32_t v) { tm_write_i4(a, v); }
inline uint64_t tm_test_read_i8(uint64_t *a) { return tm_read_i8(a); }
inline void tm_test_write_i8(uint64_t *a, uint64_t v) { tm_write_i8(a, v); }
inline uint16_t tm_test_read_i2(uint16_t *a) { return tm_read_i2(a); }
inline void tm_test_write_i2(uint16_t *a, uint16_t v) { tm_write_i2(a, v); }
inline uint8_t tm_test_read_i1(uint8_t *a) { return tm_read_i1(a); }
inline void tm_test_write_i1(uint8_t *a, uint8_t v) { tm_write_i1(a, v); }
inline float tm_test_read_f4(float *a) { return tm_read_f4(a); }
inline void tm_test_write_f4(float *a, float v) { tm_write_f4(a, v); }
inline double tm_test_read_f8(double *a) { return tm_read_f8(a); }
inline void tm_test_write_f8(double *a, double v) { tm_write_f8(a, v); }
inline void *tm_test_read_ptr(void **a) { return tm_read_ptr((volatile void **)a); }
inline void tm_test_write_ptr(void **a, void *v) { tm_write_ptr((volatile void **)a, v); }

#else

inline uint32_t tm_test_read_i4(uint32_t *a) { return tm_read_i4(a, 0); }
inline void tm_test_write_i4(uint32_t *a, uint32_t v) { tm_write_i4(a, v, 0); }
inline uint64_t tm_test_read_i8(uint64_t *a) { return tm_read_i8(a, 0); }
inline void tm_test_write_i8(uint64_t *a, uint64_t v) { tm_write_i8(a, v, 0); }
inline uint16_t tm_test_read_i2(uint16_t *a) { return tm_read_i2(a, 0); }
inline void tm_test_write_i2(uint16_t *a, uint16_t v) { tm_write_i2(a, v, 0); }
inline uint8_t tm_test_read_i1(uint8_t *a) { return tm_read_i1(a, 0); }
inline void tm_test_write_i1(uint8_t *a, uint8_t v) { tm_write_i1(a, v, 0); }
inline float tm_test_read_f4(float *a) { return tm_read_f4(a, 0); }
inline void tm_test_write_f4(float *a, float v) { tm_write_f4(a, v, 0); }
inline double tm_test_read_f8(double *a) { return tm_read_f8(a, 0); }
inline void tm_test_write_f8(double *a, double v) { tm_write_f8(a, v, 0); }
inline void *tm_test_read_ptr(void **a) { return tm_read_ptr(a, 0); }
inline void tm_test_write_ptr(void **a, void *v) { tm_write_ptr(a, v, 0); }

#endif
