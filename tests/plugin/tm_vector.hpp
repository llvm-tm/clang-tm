#pragma once

#include <cstddef>
#include <cstdio>
#include <new>

// TM-safe replacement for std::vector.
// Uses ::operator new/delete (redirected by TM pass inside TX)
// with field-by-field element copy (no opaque memcpy).
template <typename T>
struct TMVector
{
    T*     m_data = nullptr;
    size_t m_size = 0;
    size_t m_cap  = 0;

    void grow(size_t min_cap)
    {
        size_t nc = m_cap ? m_cap : 16;
        fprintf(stderr, "DBG grow: this=%p m_cap=%zu m_size=%zu nc=%zu min_cap=%zu\n",
                this, m_cap, m_size, nc, min_cap);
        while (nc < min_cap) nc *= 2;
        T* nd = static_cast<T*>(::operator new(nc * sizeof(T)));
        for (size_t i = 0; i < m_size; i++)
            nd[i] = m_data[i];
        ::operator delete(m_data);
        m_data = nd;
        m_cap  = nc;
    }

    TMVector() = default;
    ~TMVector() { ::operator delete(m_data); }

    size_t size() const { return m_size; }
    bool   empty() const { return m_size == 0; }

    void push_back(const T& val)
    {
        if (m_size >= m_cap) grow(m_size + 1);
        m_data[m_size] = val;
        m_size++;
    }

    void clear() { m_size = 0; }

    T* begin() const { return m_data; }
    T* end()   const { return m_data + m_size; }
};
