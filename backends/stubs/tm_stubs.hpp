// ── TM read/write stub generator ─────────────────────────────────
// Before including, define:
//   TM_STUB_TX         — current_tx variable (e.g., current_tx_wbctl)
//   TM_STUB_READ_FN    — read function pointer (e.g., read_word_ctl)
//   TM_STUB_WRITE_FN   — write function pointer
// For TinySTM-style templates (need RL/WL params):
//   #define TM_STUB_HAVE_TYPES
//   #define TM_STUB_RL  — ReadLogEntry type
//   #define TM_STUB_WL  — WriteLogEntry type

#pragma once

#ifdef TM_STUB_HAVE_TYPES

#define TM_READ_STUB(NAME, TYPE, VT)                                                     \
	inline TYPE tm_read_##NAME(TYPE *addr)                                               \
	{                                                                                    \
		return tm_read<TYPE, VT, TM_STUB_RL, TM_STUB_WL, TM_STUB_READ_FN>(TM_STUB_TX,    \
		                                                                  addr);         \
	}
#define TM_WRITE_STUB(NAME, TYPE, VT)                                                    \
	inline void tm_write_##NAME(TYPE *addr, TYPE val)                                    \
	{                                                                                    \
		tm_write<TYPE, VT, TM_STUB_RL, TM_STUB_WL, TM_STUB_WRITE_FN>(TM_STUB_TX,         \
		                                                             addr,               \
		                                                             val);               \
	}

#else

#define TM_READ_STUB(NAME, TYPE, VT)                                                     \
	inline TYPE tm_read_##NAME(TYPE *addr)                                               \
	{                                                                                    \
		return tm_read<TYPE, VT, TM_STUB_READ_FN>(TM_STUB_TX, addr);                     \
	}
#define TM_WRITE_STUB(NAME, TYPE, VT)                                                    \
	inline void tm_write_##NAME(TYPE *addr, TYPE val)                                    \
	{                                                                                    \
		tm_write<TYPE, VT, TM_STUB_WRITE_FN>(TM_STUB_TX, addr, val);                     \
	}

#endif

// ── Read stubs ─────────────────────────────────────────────────
TM_READ_STUB(i1, uint8_t, ValueType::UINT8)
TM_READ_STUB(i2, uint16_t, ValueType::UINT16)
TM_READ_STUB(i4, uint32_t, ValueType::UINT32)
TM_READ_STUB(i8, uint64_t, ValueType::UINT64)
TM_READ_STUB(f4, float, ValueType::FLOAT)
TM_READ_STUB(f8, double, ValueType::DOUBLE)
TM_READ_STUB(ptr, void *, ValueType::POINTER)

// ── Write stubs ────────────────────────────────────────────────
TM_WRITE_STUB(i1, uint8_t, ValueType::UINT8)
TM_WRITE_STUB(i2, uint16_t, ValueType::UINT16)
TM_WRITE_STUB(i4, uint32_t, ValueType::UINT32)
TM_WRITE_STUB(i8, uint64_t, ValueType::UINT64)
TM_WRITE_STUB(f4, float, ValueType::FLOAT)
TM_WRITE_STUB(f8, double, ValueType::DOUBLE)
TM_WRITE_STUB(ptr, void *, ValueType::POINTER)

#undef TM_READ_STUB
#undef TM_WRITE_STUB
#undef TM_STUB_TX
#undef TM_STUB_READ_FN
#undef TM_STUB_WRITE_FN
#ifdef TM_STUB_HAVE_TYPES
#undef TM_STUB_HAVE_TYPES
#undef TM_STUB_RL
#undef TM_STUB_WL
#endif
