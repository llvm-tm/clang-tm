#pragma once

#include <algorithm>
#include <utility>
#include <vector>

// TM-safe replacement for std::map.
// Sorted vector with binary search — avoids libstdc++ opaque _Rb_tree_* functions.
template<typename K, typename V>
class TMSafeMap {
    std::vector<std::pair<K, V>> m_data;

    auto find_pos(const K& k) {
        return std::lower_bound(m_data.begin(), m_data.end(), k,
            [](const auto& p, const K& k) { return p.first < k; });
    }

    auto find_pos(const K& k) const {
        return std::lower_bound(m_data.begin(), m_data.end(), k,
            [](const auto& p, const K& k) { return p.first < k; });
    }

public:
    using const_iterator = typename std::vector<std::pair<K, V>>::const_iterator;

    const_iterator find(const K& k) const {
        auto it = find_pos(k);
        if (it != m_data.end() && it->first == k)
            return it;
        return m_data.end();
    }

    V& operator[](const K& k) {
        auto it = find_pos(k);
        if (it != m_data.end() && it->first == k)
            return it->second;
        it = m_data.insert(it, {k, V{}});
        return it->second;
    }

    size_t erase(const K& k) {
        auto it = find_pos(k);
        if (it != m_data.end() && it->first == k) {
            m_data.erase(it);
            return 1;
        }
        return 0;
    }

    void clear() { m_data.clear(); }
    size_t size() const { return m_data.size(); }
    bool empty() const { return m_data.empty(); }

    using iterator = typename std::vector<std::pair<K, V>>::const_iterator;
    iterator begin() const { return m_data.begin(); }
    iterator end() const { return m_data.end(); }
};

// TM-safe replacement for std::multimap.
template<typename K, typename V>
class TMSafeMultiMap {
    std::vector<std::pair<K, V>> m_data;

    auto lower_pos(const K& k) const {
        return std::lower_bound(m_data.begin(), m_data.end(), k,
            [](const auto& p, const K& k) { return p.first < k; });
    }

    auto upper_pos(const K& k) const {
        return std::upper_bound(m_data.begin(), m_data.end(), k,
            [](const auto& p, const K& k) { return p.first < k; });
    }

public:
    void insert(const std::pair<K, V>& p) {
        auto it = upper_pos(p.first);
        m_data.insert(it, p);
    }

    void clear() { m_data.clear(); }
    size_t size() const { return m_data.size(); }
    bool empty() const { return m_data.empty(); }

    using iterator = typename std::vector<std::pair<K, V>>::const_iterator;
    iterator begin() const { return m_data.begin(); }
    iterator end() const { return m_data.end(); }

    iterator lower_bound(const K& k) const {
        return lower_pos(k);
    }
};
