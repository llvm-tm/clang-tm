#pragma once

#include <cstddef>
#include <initializer_list>
#include <new>

// Local scratch containers for use inside TX_FUNC functions.
// These are NOT TM-tracked — they use regular heap allocation for
// local scratch data inside transactions.  TM safety comes from
// explicit TM_READ_I8/TM_WRITE_I8 calls on shared data, not from
// the containers themselves.
//
// Compared to std::vector/std::set:
//   - No STL dependencies (no opaque libstdc++ internals)
//   - All loads/stores are inline and pass-visible (plugin path)
//   - Explicit element copy via loop (no memcpy on non-trivially-copyable types)

// ── ScratchVector<T> — local dynamic array ──────────────────────────
template <typename T>
class ScratchVector {
    T*      m_data = nullptr;
    size_t  m_size = 0;
    size_t  m_cap  = 0;

    void grow(size_t min_cap) {
        size_t nc = m_cap ? m_cap + m_cap / 2 : 16;  // 1.5x growth
        while (nc < min_cap) nc += nc / 2;
        T* nd = static_cast<T*>(::operator new(nc * sizeof(T)));
        for (size_t i = 0; i < m_size; i++) {
            new (&nd[i]) T(m_data[i]);
            m_data[i].~T();
        }
        ::operator delete(m_data);
        m_data = nd;
        m_cap  = nc;
    }

public:
    ScratchVector() = default;
    ScratchVector(std::initializer_list<T> il) {
        reserve(il.size());
        for (const auto& v : il)
            new (&m_data[m_size++]) T(v);
    }
    explicit ScratchVector(size_t n, const T& val = T{}) : m_size(0), m_cap(0) {
        resize(n, val);
    }
    ~ScratchVector() {
        for (size_t i = 0; i < m_size; i++) m_data[i].~T();
        ::operator delete(m_data);
    }

    void resize(size_t n, const T& val = T{}) {
        if (n < m_size) {
            for (size_t i = n; i < m_size; i++) m_data[i].~T();
        } else if (n > m_size) {
            if (n > m_cap) grow(n);
            for (size_t i = m_size; i < n; i++)
                new (&m_data[i]) T(val);
        }
        m_size = n;
    }

    void reserve(size_t n) { if (n > m_cap) grow(n); }
    void clear() { for (size_t i = 0; i < m_size; i++) m_data[i].~T(); m_size = 0; }

    void push_back(const T& v) {
        if (m_size >= m_cap) grow(m_size + 1);
        new (&m_data[m_size]) T(v);
        m_size++;
    }
    void pop_back() {
        if (m_size) { m_size--; m_data[m_size].~T(); }
    }

    T&       back()       { return m_data[m_size - 1]; }
    const T& back() const { return m_data[m_size - 1]; }
    T&       front()       { return m_data[0]; }
    const T& front() const { return m_data[0]; }

    T&       operator[](size_t i)       { return m_data[i]; }
    const T& operator[](size_t i) const { return m_data[i]; }

    size_t size()     const { return m_size; }
    size_t capacity() const { return m_cap; }
    bool   empty()    const { return m_size == 0; }

    T*       begin()       { return m_data; }
    const T* begin() const { return m_data; }
    T*       end()         { return m_data + m_size; }
    const T* end()   const { return m_data + m_size; }
};

// ── ScratchSet<T> — sorted array with binary search (like boost::flat_set) ──
// Best for small-to-medium sets.  O(log n) find, O(n) insert (shift).
template <typename T>
class ScratchSet {
    T*      m_data = nullptr;
    size_t  m_size = 0;
    size_t  m_cap  = 0;

    void grow(size_t min_cap) {
        size_t nc = m_cap ? m_cap * 2 : 64;
        while (nc < min_cap) nc *= 2;
        T* nd = static_cast<T*>(::operator new(nc * sizeof(T)));
        for (size_t i = 0; i < m_size; i++) {
            new (&nd[i]) T(m_data[i]);
            m_data[i].~T();
        }
        ::operator delete(m_data);
        m_data = nd;
        m_cap  = nc;
    }

    size_t lower_bound(const T& v) const {
        size_t lo = 0, hi = m_size;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (m_data[mid] < v) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

public:
    ScratchSet() = default;
    ~ScratchSet() { for (size_t i = 0; i < m_size; i++) m_data[i].~T(); ::operator delete(m_data); }

    bool contains(const T& v) const {
        size_t i = lower_bound(v);
        return i < m_size && !(m_data[i] < v) && !(v < m_data[i]);
    }

    void insert(const T& v) {
        size_t i = lower_bound(v);
        if (i < m_size && !(m_data[i] < v) && !(v < m_data[i])) return;
        if (m_size >= m_cap) grow(m_cap ? m_cap * 2 : 64);
        for (size_t j = m_size; j > i; j--)
            new (&m_data[j]) T(m_data[j - 1]);
        new (&m_data[i]) T(v);
        m_size++;
    }

    void erase(const T& v) {
        size_t i = lower_bound(v);
        if (i >= m_size || m_data[i] < v || v < m_data[i]) return;
        m_data[i].~T();
        for (size_t j = i; j + 1 < m_size; j++)
            new (&m_data[j]) T(m_data[j + 1]);
        m_size--;
    }

    void clear() { for (size_t i = 0; i < m_size; i++) m_data[i].~T(); m_size = 0; }

    size_t size()  const { return m_size; }
    bool   empty() const { return m_size == 0; }

    T*       begin()       { return m_data; }
    const T* begin() const { return m_data; }
    T*       end()         { return m_data + m_size; }
    const T* end()   const { return m_data + m_size; }
};
