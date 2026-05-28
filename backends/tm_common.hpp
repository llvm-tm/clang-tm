#pragma once

#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>



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

namespace stm
{

using word_t = uint64_t;

enum class ValueType : uint8_t {
	UINT8 = 1,
	UINT16 = 2,
	UINT32 = 3,
	UINT64 = 4,
	FLOAT = 5,
	DOUBLE = 6,
	POINTER = 7
};

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

template <typename T> struct any_type_mapping;

#define MAP_ANY(T, AT, M, AM)                                                            \
	template <> struct any_type_mapping<T> {                                             \
		static T &get(any_type_t &t) { return t.M; }                                     \
		static void set(any_type_t &t, T v) { t.M = v; }                                 \
		static void setp(any_type_t &t, void *a)                                         \
		{                                                                                \
			t.AM = std::atomic_ref<AT>(*static_cast<AT *>(a)).load(std::memory_order_acquire); \
		}                                                                                \
		static void store(any_type_t &t, void *a)                                        \
		{                                                                                \
			std::atomic_ref<AT>(*static_cast<AT *>(a)).store(t.AM, std::memory_order_release); \
		}                                                                                \
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
		if (!tx || !tx->active) {                                                        \
			return *addr;                                                                \
		}                                                                                \
		any_type_t r = tx->read_word((void *)addr, SZ);                                  \
		return return_any_type<TYPE>(r);                                                 \
	}

#define GENERATE_TM_WRITE(TYPE, SZ)                                                      \
	template <typename TX> inline void tm_write_##TYPE(TX *tx, TYPE *addr, TYPE val)     \
	{                                                                                    \
		if (!tx || !tx->active) {                                                        \
			*addr = val;                                                                 \
			return;                                                                      \
		}                                                                                \
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
