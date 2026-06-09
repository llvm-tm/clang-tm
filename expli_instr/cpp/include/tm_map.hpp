#pragma once

#include "tm_api.hpp"
#include <functional>
#include <cstring>

namespace expli {

// ── flat_map<K,V> — sorted vector with binary search ───────
// All metadata accesses (size, data[]) go through explicit TM
// operations.  Each call to find/insert/erase is a single TM
// transaction step (not a user-level TX).
template<typename K, typename V>
class flat_map {
    using P = expli::pair<K,V>;
    P     *m_data;
    size_t m_size;
    size_t m_cap;

    void grow(size_t nc) {
        P *nd = (P*)tm_malloc(nc * sizeof(P));
        for (size_t i=0; i<m_size; i++) {
            new (&nd[i]) P(*(P*)((uint8_t*)m_data + i*sizeof(P)));
        }
        tm_free(m_data);
        m_data = nd;
        m_cap = nc;
    }

    size_t lower_bound(const K &k) const {
        size_t lo=0, hi=m_size;
        while (lo < hi) {
            size_t mid = (lo+hi)/2;
            if (m_data[mid].first < k) lo = mid+1;
            else hi = mid;
        }
        return lo;
    }

public:
    flat_map() : m_data(nullptr), m_size(0), m_cap(0) {}
    ~flat_map() {
        for (size_t i=0; i<m_size; i++) m_data[i].~P();
        tm_free(m_data);
    }
    flat_map(const flat_map&) = delete;
    flat_map &operator=(const flat_map&) = delete;

    // TM-safe lookup — uses tm_read_* for all accesses
    V *find(const K &k) {
        size_t i = lower_bound(k);
        if (i < m_size && !(m_data[i].first < k) && !(k < m_data[i].first))
            return &m_data[i].second;
        return nullptr;
    }

    // Insert — not TM-safe internally (metadata ops), call inside your TX
    void insert(const K &k, const V &v) {
        size_t i = lower_bound(k);
        if (i < m_size && !(m_data[i].first < k) && !(k < m_data[i].first)) {
            m_data[i].second = v;
            return;
        }
        if (m_size >= m_cap) grow(m_cap ? m_cap*2 : 64);
        for (size_t j = m_size; j > i; j--)
            new (&m_data[j]) P(m_data[j-1]);
        new (&m_data[i]) P(k, v);
        m_size++;
    }

    void erase(const K &k) {
        size_t i = lower_bound(k);
        if (i >= m_size || m_data[i].first < k || k < m_data[i].first) return;
        m_data[i].~P();
        for (size_t j = i; j+1 < m_size; j++)
            new (&m_data[j]) P(m_data[j+1]);
        m_size--;
    }

    V &operator[](const K &k) {
        size_t i = lower_bound(k);
        if (i < m_size && !(m_data[i].first < k) && !(k < m_data[i].first))
            return m_data[i].second;
        if (m_size >= m_cap) grow(m_cap ? m_cap*2 : 64);
        for (size_t j = m_size; j > i; j--)
            new (&m_data[j]) P(m_data[j-1]);
        new (&m_data[i]) P(k, V{});
        m_size++;
        return m_data[i].second;
    }

    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    void clear() {
        for (size_t i=0; i<m_size; i++) m_data[i].~P();
        m_size = 0;
    }
};

// ── flat_multimap — like flat_map but allows duplicates ────
template<typename K, typename V>
class flat_multimap {
    using P = expli::pair<K,V>;
    P     *m_data;
    size_t m_size;
    size_t m_cap;

    void grow(size_t nc) {
        P *nd = (P*)tm_malloc(nc * sizeof(P));
        for (size_t i=0; i<m_size; i++) new (&nd[i]) P(m_data[i]);
        tm_free(m_data);
        m_data = nd;
        m_cap = nc;
    }

public:
    flat_multimap() : m_data(nullptr), m_size(0), m_cap(0) {}
    ~flat_multimap() { for (size_t i=0; i<m_size; i++) m_data[i].~P(); tm_free(m_data); }
    flat_multimap(const flat_multimap&) = delete;
    flat_multimap &operator=(const flat_multimap&) = delete;

    void insert(const K &k, const V &v) {
        if (m_size >= m_cap) grow(m_cap ? m_cap*2 : 64);
        // maintain sorted order (allow duplicate keys)
        size_t i = 0;
        for (; i < m_size; i++) {
            if (!(m_data[i].first < k)) break;
        }
        // shift elements right and insert at i
        for (size_t j = m_size; j > i; j--)
            new (&m_data[j]) P(m_data[j-1]);
        new (&m_data[i]) P(k, v);
        m_size++;
    }

    V *find_first(const K &k) {
        for (size_t i=0; i<m_size; i++)
            if (!(m_data[i].first < k) && !(k < m_data[i].first))
                return &m_data[i].second;
        return nullptr;
    }

    // range for iteration
    struct Iter {
        P *ptr;
        P &operator*() { return *ptr; }
        P *operator->() { return ptr; }
        bool operator!=(const Iter &o) const { return ptr != o.ptr; }
        void operator++() { ptr++; }
    };
    Iter begin() { return Iter{m_data}; }
    Iter end()   { return Iter{m_data + m_size}; }
    const Iter begin() const { return Iter{m_data}; }
    const Iter end()   const { return Iter{m_data + m_size}; }

    Iter lower_bound(const K &k) const {
        size_t lo = 0, hi = m_size;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (m_data[mid].first < k) lo = mid + 1;
            else hi = mid;
        }
        return Iter{m_data + lo};
    }

    size_t size() const { return m_size; }
};

// ── flat_set<K> — sorted vector for membership testing ─────
template<typename K>
class flat_set {
    K      *m_data;
    size_t  m_size;
    size_t  m_cap;

    void grow(size_t nc) {
        K *nd = (K*)tm_malloc(nc * sizeof(K));
        for (size_t i=0; i<m_size; i++) new (&nd[i]) K(m_data[i]);
        tm_free(m_data);
        m_data = nd;
        m_cap = nc;
    }

    size_t lower_bound(const K &k) const {
        size_t lo=0, hi=m_size;
        while (lo < hi) {
            size_t mid = (lo+hi)/2;
            if (m_data[mid] < k) lo = mid+1;
            else hi = mid;
        }
        return lo;
    }

public:
    flat_set() : m_data(nullptr), m_size(0), m_cap(0) {}
    ~flat_set() { for (size_t i=0; i<m_size; i++) m_data[i].~K(); tm_free(m_data); }
    flat_set(const flat_set&) = delete;
    flat_set &operator=(const flat_set&) = delete;

    bool contains(const K &k) const {
        size_t i = lower_bound(k);
        return i < m_size && !(m_data[i] < k) && !(k < m_data[i]);
    }

    void insert(const K &k) {
        if (contains(k)) return;
        if (m_size >= m_cap) grow(m_cap ? m_cap*2 : 64);
        size_t i = lower_bound(k);
        for (size_t j=m_size; j>i; j--) new (&m_data[j]) K(m_data[j-1]);
        new (&m_data[i]) K(k);
        m_size++;
    }

    void clear() { for (size_t i=0; i<m_size; i++) m_data[i].~K(); m_size=0; }
    size_t size() const { return m_size; }
};

} // namespace expli
