#pragma once

#include <algorithm>
#include <utility>
#include <vector>

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

    size_t size() const { return m_data.size(); }
    bool empty() const { return m_data.empty(); }

    using iterator = typename std::vector<std::pair<K, V>>::const_iterator;
    iterator begin() const { return m_data.begin(); }
    iterator end() const { return m_data.end(); }
};
