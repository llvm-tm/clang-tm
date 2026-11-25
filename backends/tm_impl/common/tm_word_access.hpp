#pragma once
// ── Width-exact atomic access to transactional data words ──────────
//
// The lock-based backends in this tree install committed values *in place*,
// while lockless speculative readers may be loading that very same word at the
// moment of the install (the reader's value is rejected later by guard/version
// validation — that is the intended protocol, and it is why readers do not take
// the owner's lock).  A plain load racing a plain store is nevertheless
// undefined behaviour under the C++ memory model, and ThreadSanitizer reports
// it as a data race for every backend that does this.
//
// These helpers express what the protocol actually means: a *relaxed* atomic
// load on the speculative side (its value is validated by version, not by
// ordering) and a *release* store on the installing side.  On x86-64 an aligned
// 1-8 byte access is already a single instruction, so the generated code is
// unchanged; what changes is that the compiler may no longer treat the pair as
// free to split, widen or re-order, and the pattern stops being UB.
//
// The __atomic builtins accept only integer or pointer types, while a
// transactional word may hold a float or a double, so the access goes through an
// integer view of the same width and the bit pattern is moved with memcpy rather
// than by casting a pointer to an unrelated type.

#include <cstdint>
#include <type_traits>

namespace tm_word
{

template <typename T>
using width_uint_t = std::conditional_t<
    sizeof(T) == 1,
    uint8_t,
    std::conditional_t<sizeof(T) == 2,
                       uint16_t,
                       std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>>;

// Relaxed, width-exact load. Accepts a pointer to a cv-qualified object (the
// backends declare transactional addresses volatile).
template <typename PT> inline std::remove_cv_t<PT> load_relaxed(PT *p)
{
	using T = std::remove_cv_t<PT>;
	static_assert(sizeof(T) <= 8, "transactional words are at most 8 bytes");
	using W = width_uint_t<T>;
	W w;
	if constexpr (sizeof(T) == 1)
		w = __atomic_load_n((uint8_t *)p, __ATOMIC_RELAXED);
	else if constexpr (sizeof(T) == 2)
		w = __atomic_load_n((uint16_t *)p, __ATOMIC_RELAXED);
	else if constexpr (sizeof(T) == 4)
		w = __atomic_load_n((uint32_t *)p, __ATOMIC_RELAXED);
	else
		w = __atomic_load_n((uint64_t *)p, __ATOMIC_RELAXED);
	T out;
	__builtin_memcpy(&out, &w, sizeof(T));
	return out;
}

// Release, width-exact store of a committed value.
template <typename PT, typename T> inline void store_release(PT *p, T v)
{
	static_assert(sizeof(T) <= 8, "transactional words are at most 8 bytes");
	using W = width_uint_t<T>;
	W w;
	__builtin_memcpy(&w, &v, sizeof(T));
	if constexpr (sizeof(T) == 1)
		__atomic_store_n((uint8_t *)p, w, __ATOMIC_RELEASE);
	else if constexpr (sizeof(T) == 2)
		__atomic_store_n((uint16_t *)p, w, __ATOMIC_RELEASE);
	else if constexpr (sizeof(T) == 4)
		__atomic_store_n((uint32_t *)p, w, __ATOMIC_RELEASE);
	else
		__atomic_store_n((uint64_t *)p, w, __ATOMIC_RELEASE);
}

} // namespace tm_word
