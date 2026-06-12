#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <utility>

extern "C" void tm_debug_grow(void* this_ptr, size_t m_cap, size_t m_size, size_t nc, size_t min_cap, size_t sz);

template <typename T>
class TMSafeVector {
    T*      m_data;
    size_t  m_size;
    size_t  m_cap;

    void grow(size_t min_cap) {
        size_t nc = m_cap ? m_cap : 4;
        while (nc < min_cap) nc *= 2;
        T* nd = static_cast<T*>(::operator new(nc * sizeof(T)));
        for (size_t i = 0; i < m_size; i++) {
            new (&nd[i]) T(static_cast<T&&>(m_data[i]));
            m_data[i].~T();
        }
        ::operator delete(m_data);
        m_data = nd;
        m_cap  = nc;
    }

public:
    using value_type      = T;
    using reference       = T&;
    using const_reference = const T&;
    using iterator        = T*;
    using const_iterator  = const T*;

    TMSafeVector() : m_data(nullptr), m_size(0), m_cap(0) {}
    explicit TMSafeVector(size_t n) : m_data(nullptr), m_size(0), m_cap(0) { resize(n); }
    TMSafeVector(const TMSafeVector& o) : m_data(nullptr), m_size(0), m_cap(0) {
        reserve(o.m_size);
        for (size_t i = 0; i < o.m_size; i++)
            new (&m_data[i]) T(o.m_data[i]);
        m_size = o.m_size;
    }
    TMSafeVector& operator=(const TMSafeVector& o) {
        if (this != &o) {
            clear();
            reserve(o.m_size);
            for (size_t i = 0; i < o.m_size; i++)
                new (&m_data[i]) T(o.m_data[i]);
            m_size = o.m_size;
        }
        return *this;
    }
    TMSafeVector(TMSafeVector&& o) noexcept
        : m_data(o.m_data), m_size(o.m_size), m_cap(o.m_cap) {
        o.m_data = nullptr;
        o.m_size = 0;
        o.m_cap  = 0;
    }
    TMSafeVector& operator=(TMSafeVector&& o) noexcept {
        if (this != &o) {
            clear();
            ::operator delete(m_data);
            m_data = o.m_data;
            m_size = o.m_size;
            m_cap  = o.m_cap;
            o.m_data = nullptr;
            o.m_size = 0;
            o.m_cap  = 0;
        }
        return *this;
    }
    ~TMSafeVector() { clear(); ::operator delete(m_data); }

    void reserve(size_t n) { if (n > m_cap) grow(n); }
    void resize(size_t n) {
        if (n < m_size) {
            for (size_t i = n; i < m_size; i++)
                m_data[i].~T();
        } else if (n > m_size) {
            reserve(n);
            for (size_t i = m_size; i < n; i++)
                new (&m_data[i]) T();
        }
        m_size = n;
    }
    void clear() {
        for (size_t i = 0; i < m_size; i++)
            m_data[i].~T();
        m_size = 0;
    }

    void push_back(const T& v) {
        if (m_size >= m_cap) grow(m_size + 1);
        new (&m_data[m_size]) T(v);
        m_size++;
    }
    void push_back(T&& v) {
        if (m_size >= m_cap) grow(m_size + 1);
        new (&m_data[m_size]) T(static_cast<T&&>(v));
        m_size++;
    }
    template <typename... Args>
    void emplace_back(Args&&... args) {
        if (m_size >= m_cap) grow(m_size + 1);
        new (&m_data[m_size]) T(static_cast<Args&&>(args)...);
        m_size++;
    }
    void pop_back() {
        if (m_size) {
            m_size--;
            m_data[m_size].~T();
        }
    }

    iterator erase(iterator pos) {
        if (pos < m_data || pos >= m_data + m_size)
            return m_data + m_size;
        size_t idx = static_cast<size_t>(pos - m_data);
        m_data[idx].~T();
        for (size_t i = idx + 1; i < m_size; i++) {
            new (&m_data[i - 1]) T(static_cast<T&&>(m_data[i]));
            m_data[i].~T();
        }
        m_size--;
        return m_data + idx;
    }
    iterator erase(iterator first, iterator last) {
        if (first >= last || first < m_data || first > m_data + m_size)
            return m_data + m_size;
        if (last > m_data + m_size) last = m_data + m_size;
        size_t cnt = static_cast<size_t>(last - first);
        size_t idx = static_cast<size_t>(first - m_data);
        for (size_t i = idx; i < idx + cnt; i++)
            m_data[i].~T();
        for (size_t i = idx + cnt; i < m_size; i++) {
            new (&m_data[i - cnt]) T(static_cast<T&&>(m_data[i]));
            m_data[i].~T();
        }
        m_size -= cnt;
        return m_data + idx;
    }

    T&       operator[](size_t i)       { return m_data[i]; }
    const T& operator[](size_t i) const { return m_data[i]; }

    T&       front()       { return m_data[0]; }
    const T& front() const { return m_data[0]; }
    T&       back()        { return m_data[m_size - 1]; }
    const T& back() const  { return m_data[m_size - 1]; }

    size_t size()     const { return m_size; }
    size_t capacity() const { return m_cap; }
    bool   empty()    const { return m_size == 0; }

    iterator       begin()       { return m_data; }
    const_iterator begin() const { return m_data; }
    iterator       end()         { return m_data + m_size; }
    const_iterator end()   const { return m_data + m_size; }

    T*       data()       { return m_data; }
    const T* data() const { return m_data; }
};
