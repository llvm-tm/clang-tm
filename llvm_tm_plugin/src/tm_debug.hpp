// tm_debug.hpp
// Debug macros for TM plugin, following TinySTM pattern
// When NDEBUG is defined, all debug output and asserts are disabled

#ifndef TM_DEBUG_HPP
#define TM_DEBUG_HPP

#include <cstdio>
#include <cassert>

// Debug output macro - prints function name and message
#ifndef NDEBUG
#define TM_DEBUG(fmt, ...)                                                     \
  do {                                                                         \
    fprintf(stderr, "[TM Plugin] %s: " fmt "\n", __func__, ##__VA_ARGS__);    \
    fflush(stderr);                                                            \
  } while (0)
#else
#define TM_DEBUG(fmt, ...) /* EMPTY */
#endif

// Assert macro with message - only active when NDEBUG is not defined
#ifndef NDEBUG
#define TM_ASSERT(cond, msg)                                                   \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "[TM Plugin ASSERTION FAILED] %s (%s:%d): %s\n",         \
              __func__, __FILE__, __LINE__, msg);                              \
      fflush(stderr);                                                          \
      assert(cond);                                                            \
    }                                                                          \
  } while (0)
#else
#define TM_ASSERT(cond, msg) /* EMPTY */
#endif

// Assert that a value is not null
#ifndef NDEBUG
#define TM_ASSERT_NOT_NULL(ptr, msg) TM_ASSERT((ptr) != nullptr, msg)
#else
#define TM_ASSERT_NOT_NULL(ptr, msg) /* EMPTY */
#endif

#endif // TM_DEBUG_HPP
