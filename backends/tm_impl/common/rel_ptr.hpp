#pragma once

/**
 * RelPtr<T> — Relative Pointer for shared memory / persistent storage
 *
 * Stores an int64_t offset from a base address instead of an absolute
 * pointer.  The offset is invariant across process restarts (ASLR-safe)
 * as long as the base address is correctly restored.
 *
 * Usage:
 *   // In shared memory or mmap'd file:
 *   struct Node {
 *       RelPtr<Node> next;   // 8 bytes, not sizeof(ptr)
 *       RelPtr<Node> prev;
 *       int key, value;
 *   };
 *
 *   // Works like a regular pointer everywhere:
 *   node->next = &other;     // stores offset
 *   other = node->next;      // loads + adds base
 *   if (!node->next) ...     // nullptr check
 *
 * The base address is stored in a thread-local variable set by
 * RelPtr<T>::set_base() before any RelPtr operations.  For the
 * PersistentSGL/DistributedSGL backends this is called automatically
 * in tm_init().
 *
 * Layout: int64_t offset (0 = null).
 * Thread-safe: yes (all operations are on the stored offset only).
 */

#include <cstdint>
#include <cstdlib>
#include <type_traits>

template <typename T>
class RelPtr {
    static_assert(std::is_standard_layout_v<T> || std::is_void_v<T>,
                  "RelPtr requires standard-layout or void type");

    int64_t off_;   // 0 means nullptr

public:
    RelPtr() noexcept : off_(0) {}

    RelPtr(std::nullptr_t) noexcept : off_(0) {}

    explicit RelPtr(T* ptr) noexcept {
        if (ptr)
            off_ = reinterpret_cast<char*>(ptr) - get_base();
        else
            off_ = 0;
    }

    // Copy
    RelPtr(const RelPtr& other) noexcept : off_(other.off_) {}
    RelPtr& operator=(const RelPtr& other) noexcept { off_ = other.off_; return *this; }
    RelPtr& operator=(T* ptr) noexcept { *this = RelPtr(ptr); return *this; }
    RelPtr& operator=(std::nullptr_t) noexcept { off_ = 0; return *this; }

    // Conversion to/from other RelPtr types (for base/derived)
    template <typename U>
    RelPtr(const RelPtr<U>& other) noexcept : off_(other.off_) {}
    template <typename U>
    RelPtr& operator=(const RelPtr<U>& other) noexcept { off_ = other.off_; return *this; }

    ~RelPtr() = default;

    // Dereference
    T* get() const noexcept {
        if (off_ == 0) return nullptr;
        return reinterpret_cast<T*>(get_base() + off_);
    }

    T& operator*() const noexcept { return *get(); }
    T* operator->() const noexcept { return get(); }

    explicit operator bool() const noexcept { return off_ != 0; }

    // Comparisons
    bool operator==(const RelPtr& other) const noexcept { return off_ == other.off_; }
    bool operator!=(const RelPtr& other) const noexcept { return off_ != other.off_; }
    bool operator==(std::nullptr_t) const noexcept { return off_ == 0; }
    bool operator!=(std::nullptr_t) const noexcept { return off_ != 0; }

    // For containers: need a hashable key
    int64_t offset() const noexcept { return off_; }

    // ── Base address management ─────────────────────────────
    // Set once per process at startup (e.g., in tm_init).
    // MUST point to the start of the shared mmap region.
private:
    static char*& base_ref() {
        static char* base = nullptr;
        return base;
    }

    static char* get_base() {
        char* b = base_ref();
        if (!b) {
            fprintf(stderr, "RelPtr: base address not set! Call RelPtr<T>::set_base() first.\n");
            std::abort();
        }
        return b;
    }

public:
    static void set_base(void* base) noexcept { base_ref() = static_cast<char*>(base); }
    static void* get_base_void() noexcept { return base_ref(); }
};

// RelPtr<void> specialization (no dereference)
template <>
class RelPtr<void> {
    int64_t off_;
public:
    RelPtr() noexcept : off_(0) {}
    RelPtr(std::nullptr_t) noexcept : off_(0) {}
    explicit RelPtr(void* ptr) noexcept {
        if (ptr)
            off_ = reinterpret_cast<char*>(ptr) - get_base();
        else
            off_ = 0;
    }
    void* get() const noexcept { return off_ == 0 ? nullptr : reinterpret_cast<void*>(get_base() + off_); }
    explicit operator bool() const noexcept { return off_ != 0; }
    int64_t offset() const noexcept { return off_; }
    bool operator==(std::nullptr_t) const noexcept { return off_ == 0; }

    template <typename T>
    RelPtr<T> to() const noexcept { RelPtr<T> r; r = *this; return r; }

    static void set_base(void* base) noexcept { char*& b = base_ref(); b = static_cast<char*>(base); }
private:
    static char*& base_ref() { static char* base = nullptr; return base; }
    static char* get_base() {
        char* b = base_ref();
        if (!b) { std::fprintf(stderr, "RelPtr: base not set\n"); std::abort(); }
        return b;
    }
};

// Hash support for use in std::unordered_map
namespace std {
    template <typename T>
    struct hash<RelPtr<T>> {
        size_t operator()(const RelPtr<T>& p) const noexcept {
            return hash<int64_t>()(p.offset());
        }
    };
}
