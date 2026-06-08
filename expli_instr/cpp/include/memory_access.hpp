#pragma once

#include <type_traits>
#include "tm_api.hpp"

// ── MemoryAccess<UseTM> — compile-time switch between TM and plain access ──
// Lets a single data structure implementation work both inside and outside
// transactions without code duplication.
//
//   MemoryAccess<true>::load(&n->key)   → tm_read_i8 (when key is a long)
//   MemoryAccess<false>::load(&n->key)  → *(&n->key) (plain dereference)
//
// Usage in a data structure:
//
//   template<bool UseTM, typename K, typename V>
//   V* find(RBTree<K,V>* tree, K key) {
//       using MA = MemoryAccess<UseTM>;
//       Node* n = MA::load(&tree->root);
//       while (n) {
//           if (key == MA::load(&n->key)) return &n->val;
//           n = (key < MA::load(&n->key))
//               ? MA::load(&n->left) : MA::load(&n->right);
//       }
//       return nullptr;
//   }
//
// Inside a TX: call with UseTM=true  → tracked by TM runtime
// Outside a TX: call with UseTM=false → zero-overhead plain access

template<bool UseTM>
struct MemoryAccess;

template<>
struct MemoryAccess<true> {
    template<typename T>
    static T load(const T* addr) {
        if constexpr (std::is_pointer_v<T>) {
            return (T)tm_read_ptr((void**)const_cast<T*>(addr));
        } else if constexpr (std::is_same_v<T, float>) {
            return (T)tm_read_f4(const_cast<float*>(addr));
        } else if constexpr (std::is_same_v<T, double>) {
            return (T)tm_read_f8(const_cast<double*>(addr));
        } else if constexpr (sizeof(T) == 1) {
            return (T)tm_read_i1((uint8_t*)addr);
        } else if constexpr (sizeof(T) == 2) {
            return (T)tm_read_i2((uint16_t*)addr);
        } else if constexpr (sizeof(T) == 4) {
            return (T)tm_read_i4((uint32_t*)addr);
        } else if constexpr (sizeof(T) == 8) {
            return (T)tm_read_i8((uint64_t*)addr);
        } else {
            static_assert(!sizeof(T*),
                "MemoryAccess::load: unsupported type size. "
                "Custom structs must be accessed field-by-field.");
        }
    }

    template<typename T>
    static void store(T* addr, const T& val) {
        if constexpr (std::is_pointer_v<T>) {
            tm_write_ptr((void**)addr, (void*)val);
        } else if constexpr (std::is_same_v<T, float>) {
            tm_write_f4(addr, val);
        } else if constexpr (std::is_same_v<T, double>) {
            tm_write_f8(addr, val);
        } else if constexpr (sizeof(T) == 1) {
            tm_write_i1((uint8_t*)addr, (uint8_t)val);
        } else if constexpr (sizeof(T) == 2) {
            tm_write_i2((uint16_t*)addr, (uint16_t)val);
        } else if constexpr (sizeof(T) == 4) {
            tm_write_i4((uint32_t*)addr, (uint32_t)val);
        } else if constexpr (sizeof(T) == 8) {
            tm_write_i8((uint64_t*)addr, (int64_t)val);
        } else {
            static_assert(!sizeof(T*),
                "MemoryAccess::store: unsupported type size. "
                "Custom structs must be accessed field-by-field.");
        }
    }
};

template<>
struct MemoryAccess<false> {
    template<typename T>
    static T load(const T* addr) { return *addr; }
    template<typename T>
    static void store(T* addr, const T& val) { *addr = val; }
};
