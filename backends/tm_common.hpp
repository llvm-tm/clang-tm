#pragma once

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "tm_platform.hpp"

// Global abort counter — each runtime defines its own.
// Use `extern` in benchmark code to read it from the runtime.
// (Not `inline` because std::atomic is not constexpr.)

#ifndef NDEBUG
#define TM_ASSERT(cond, msg)                                                             \
	do {                                                                                 \
		if (!(cond)) {                                                                   \
			fprintf(stderr,                                                              \
			        "TM ASSERTION FAILED: %s (%s:%d)\n",                                 \
			        msg,                                                                 \
			        __FILE__,                                                            \
			        __LINE__);                                                           \
			fflush(stderr);                                                              \
			assert(cond);                                                                \
		}                                                                                \
	} while (0)
#else
#define TM_ASSERT(cond, msg) /* EMPTY */
#endif

#define TM_ASSERT_VALID_TX(tx, msg)                                                    \
	TM_ASSERT((tx) != nullptr, msg);                                                   \
	TM_ASSERT((tx)->active, "Transaction must be active: " msg);                        \
	TM_ASSERT(!(tx)->aborted, "Transaction must not be aborted: " msg)

namespace stm
{

using word_t = uint64_t;

// ── Random Exponential Backoff ────────────────────────────────
constexpr int K_MAX_BACKOFF_DELAY_US = 100000;

inline void      //
random_backoff(  //
    unsigned abort_count)
{
	thread_local std::mt19937 rng = [] {
		std::random_device rd;
		return std::mt19937(rd());
	}();
	std::exponential_distribution<> dist((double)1 / (double)(abort_count + 1));
	int delay = std::min(dist(rng), (double)K_MAX_BACKOFF_DELAY_US);
	std::this_thread::sleep_for(std::chrono::microseconds(delay));
}

enum class ValueType : uint8_t {
	UINT8 = 1,
	UINT16 = 2,
	UINT32 = 3,
	UINT64 = 4,
	FLOAT = 5,
	DOUBLE = 6,
	POINTER = 7
};

inline unsigned type_size(ValueType t)
{
	switch (t) {
	case ValueType::UINT8:
		return 1;
	case ValueType::UINT16:
		return 2;
	case ValueType::UINT32:
	case ValueType::FLOAT:
		return 4;
	case ValueType::UINT64:
	case ValueType::DOUBLE:
	case ValueType::POINTER:
		return 8;
	}
	return 0;
}

// ── Map TL2-style DataType uint8_t value to byte width ─────────
// Values: UINT8=1 → 1, UINT16=2 → 2, UINT32=4 → 4, UINT64=8 → 8,
//         PTR=16 → 8, FLOAT=32 → 4, DOUBLE=64 → 8
inline unsigned data_type_size(uint8_t dt_val) {
	if (dt_val == 1)  return 1;  // UINT8
	if (dt_val == 2)  return 2;  // UINT16
	if (dt_val == 4)  return 4;  // UINT32 / FLOAT
	if (dt_val == 8)  return 8;  // UINT64 / DOUBLE
	if (dt_val == 16) return 8;  // PTR
	if (dt_val == 32) return 4;  // FLOAT
	if (dt_val == 64) return 8;  // DOUBLE
	return 0;
}

// All bits set (e.g., for 8-byte writes where shift-by-64 would be UB).
static constexpr uint64_t ALL_ONES = ~0ULL;

// Generate a bitmask covering `nbytes` bytes in the lowest positions.
// Example: BYTE_MASK(2) = 0xFFFF.  For nbytes >= 8, returns ALL_ONES
// (avoids UB from 1ULL << 64).
#define BYTE_MASK(nbytes) ((nbytes) >= 8 ? stm::ALL_ONES : ((1ULL << ((nbytes) * 8)) - 1))

struct any_type_t {
	union {
		uint8_t u1;
		uint16_t u2;
		uint32_t u4;
		uint64_t u8;
		float f4;
		double f8;
		void *ptr;
	};
};

template <typename T> inline T get_any_value(const any_type_t &nv) {
	if constexpr (std::is_same_v<T, uint8_t>)
		return nv.u1;
	else if constexpr (std::is_same_v<T, uint16_t>)
		return nv.u2;
	else if constexpr (std::is_same_v<T, uint32_t>)
		return nv.u4;
	else if constexpr (std::is_same_v<T, uint64_t>)
		return nv.u8;
	else if constexpr (std::is_same_v<T, float>)
		return nv.f4;
	else if constexpr (std::is_same_v<T, double>)
		return nv.f8;
	else
		return static_cast<T>(nv.ptr);
}

// Extract sub-word of type T from any_type_t at byte offset.
// All union members share the same starting address, so memcpy
// from (uint8_t*)&val + offset reads the correct raw bytes
// regardless of which member was last written.
// e.g., extract_sub_value<uint16_t>(nv, 2) reads bytes 2-3 of
// the union (the top half of a UINT32, or bytes 2-3 of a UINT64).
template <typename T>
inline T extract_sub_value(const any_type_t &val, unsigned offset = 0) {
	T result{};
	memcpy(&result, reinterpret_cast<const uint8_t *>(&val) + offset, sizeof(T));
	return result;
}

// Extract raw uint64 from any typed value (bit pattern).
inline uint64_t any_to_u64(const any_type_t &val, ValueType type) {
	switch (type) {
	case ValueType::UINT8:   return val.u1;
	case ValueType::UINT16:  return val.u2;
	case ValueType::UINT32:  return val.u4;
	case ValueType::UINT64:  return val.u8;
	case ValueType::FLOAT:   return val.u4;
	case ValueType::DOUBLE:  return val.u8;
	case ValueType::POINTER: return reinterpret_cast<uint64_t>(val.ptr);
	default:                 return 0;
	}
}

// Store a uint64 bit-pattern back into an any_type_t for the given type.
inline void u64_to_any(any_type_t &t, uint64_t v, ValueType type) {
	switch (type) {
	case ValueType::UINT8:   t.u1  = static_cast<uint8_t>(v);   break;
	case ValueType::UINT16:  t.u2  = static_cast<uint16_t>(v);  break;
	case ValueType::UINT32:  t.u4  = static_cast<uint32_t>(v);  break;
	case ValueType::UINT64:  t.u8  = v;                          break;
	case ValueType::FLOAT:   t.u4  = static_cast<uint32_t>(v);  break;
	case ValueType::DOUBLE:  memcpy(&t.f8, &v, 8);              break;
	case ValueType::POINTER: t.ptr = reinterpret_cast<void *>(v); break;
	default: break;
	}
}

// Extract uint64 from any_type_t via memcpy (avoids unaligned atomic_ref on ARM64).
template <typename AT> //
inline void any_to_word(any_type_t &t, AT v)
{
	memcpy(&t.u8, &v, sizeof(AT));
}

template <typename T> struct any_type_mapping;

#define MAP_ANY(T, AT, M, AM)                                                            \
	template <> struct any_type_mapping<T> {                                             \
		static T &get(any_type_t &t) { return t.M; }                                     \
		static void set(any_type_t &t, T v) { t.M = v; }                                 \
		static void setp(any_type_t &t, void *a) { memcpy(&t.AM, a, sizeof(AT)); }       \
		static void store(any_type_t &t, void *a) { memcpy(a, &t.AM, sizeof(AT)); }      \
	};

MAP_ANY(uint8_t, uint8_t, u1, u1)
MAP_ANY(uint16_t, uint16_t, u2, u2)
MAP_ANY(uint32_t, uint32_t, u4, u4)
MAP_ANY(uint64_t, uint64_t, u8, u8)
MAP_ANY(float, uint32_t, f4, u4)
MAP_ANY(double, uint64_t, f8, u8)
MAP_ANY(void *, uint64_t, ptr, u8)

template <typename T> //
inline T return_any_type(any_type_t &t)
{
	return any_type_mapping<T>::get(t);
}

#define FILL_ANY_TYPE_CASE(ENUM, T, t, addr)                                             \
	case ValueType::ENUM:                                                                \
		any_type_mapping<T>::setp(t, addr);                                              \
		break;

inline void        //
fill_any_type(     //
    any_type_t &t, //
    void *addr,    //
    ValueType sz   //
)
{
	switch (sz) {
		FILL_ANY_TYPE_CASE(UINT8, uint8_t, t, addr)
		FILL_ANY_TYPE_CASE(UINT16, uint16_t, t, addr)
		FILL_ANY_TYPE_CASE(UINT32, uint32_t, t, addr)
		FILL_ANY_TYPE_CASE(UINT64, uint64_t, t, addr)
		FILL_ANY_TYPE_CASE(FLOAT, float, t, addr)
		FILL_ANY_TYPE_CASE(DOUBLE, double, t, addr)
		FILL_ANY_TYPE_CASE(POINTER, void *, t, addr)
	default:
		break;
	}
}

inline any_type_t     //
read_value_from_addr( //
    void *addr,       //
    ValueType sz      //
)
{
	any_type_t res = {.u8 = 0L};
	fill_any_type(res, addr, sz);
	return res;
}

#define WRITE_VALUE_TO_ADDR_CASE(ENUM, T, t, addr)                                       \
	case ValueType::ENUM:                                                                \
		any_type_mapping<T>::store(t, addr);                                             \
		break;

inline void          //
write_value_to_addr( //
    void *addr,      //
    any_type_t val,  //
    ValueType sz     //
)
{
	any_type_t res = {.u8 = 0L};
	switch (sz) {
		WRITE_VALUE_TO_ADDR_CASE(UINT8, uint8_t, val, addr);
		WRITE_VALUE_TO_ADDR_CASE(UINT16, uint16_t, val, addr);
		WRITE_VALUE_TO_ADDR_CASE(UINT32, uint32_t, val, addr);
		WRITE_VALUE_TO_ADDR_CASE(UINT64, uint64_t, val, addr);
		WRITE_VALUE_TO_ADDR_CASE(FLOAT, float, val, addr);
		WRITE_VALUE_TO_ADDR_CASE(DOUBLE, double, val, addr);
		WRITE_VALUE_TO_ADDR_CASE(POINTER, void *, val, addr);
	default:
		break;
	}
}

constexpr int OFFSET_BITS = 3; // for 64bit
constexpr int OFFSET_MASK = 7;

struct ByteOffset {
	word_t base_addr;
	word_t offset;

	ByteOffset()
	    : base_addr(0),
	      offset(0)
	{
	}
	ByteOffset(word_t addr)
	    : base_addr(addr & ~OFFSET_MASK),
	      offset(addr & OFFSET_MASK)
	{
	}
};

inline bool same_location( //
    const ByteOffset &a,   //
    const ByteOffset &b    //
)
{
	return a.base_addr == b.base_addr && a.offset == b.offset;
}

#define GENERATE_TM_READ(TYPE, SZ)                                                       \
	template <typename TX> inline TYPE tm_read_##TYPE(TX *tx, TYPE *addr)                \
	{                                                                                    \
		TM_ASSERT(tx && tx->active, "tm_read: tx not active");                                                     \
		any_type_t r = tx->read_word((void *)addr, SZ);                                  \
		return return_any_type<TYPE>(r);                                                 \
	}

#define GENERATE_TM_WRITE(TYPE, SZ)                                                      \
	template <typename TX> inline void tm_write_##TYPE(TX *tx, TYPE *addr, TYPE val)     \
	{                                                                                    \
		TM_ASSERT(tx && tx->active, "tm_write: tx not active");                                                     \
		any_type_t w;                                                                    \
		fill_any_type(w, &val, SZ);                                                      \
		tx->write_word((void *)addr, w, SZ);                                             \
	}

#define GENERATE_TM_OPS(TYPE, SZ, PREFIX)                                                \
	GENERATE_TM_READ(TYPE, SZ)                                                           \
	GENERATE_TM_WRITE(TYPE, SZ)

GENERATE_TM_OPS(uint8_t, ValueType::UINT8, i1)
GENERATE_TM_OPS(uint16_t, ValueType::UINT16, i2)
GENERATE_TM_OPS(uint32_t, ValueType::UINT32, i4)
GENERATE_TM_OPS(uint64_t, ValueType::UINT64, i8)
GENERATE_TM_OPS(float, ValueType::FLOAT, f4)
GENERATE_TM_OPS(double, ValueType::DOUBLE, f8)

} // namespace stm
