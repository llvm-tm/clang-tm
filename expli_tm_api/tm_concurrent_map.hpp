#pragma once

#include "tm_api.hpp"
#include "tm_map.hpp"
#include <mutex>

namespace expli {

// ── Thread-safe flat_map ───────────────────────────────────
template<typename K, typename V>
class ts_map {
    flat_map<K,V> map_;
    mutable std::mutex mtx_;
public:
    V *find(const K &k) {
        std::lock_guard<std::mutex> lk(mtx_);
        return map_.find(k);
    }
    void insert(const K &k, const V &v) {
        std::lock_guard<std::mutex> lk(mtx_);
        map_.insert(k, v);
    }
    void erase(const K &k) {
        std::lock_guard<std::mutex> lk(mtx_);
        map_.erase(k);
    }
    V &operator[](const K &k) {
        std::lock_guard<std::mutex> lk(mtx_);
        return map_[k];
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        map_.clear();
    }
    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return map_.size();
    }
};

// ── Thread-safe flat_multimap ──────────────────────────────
template<typename K, typename V>
class ts_multimap {
    flat_multimap<K,V> mmap_;
    mutable std::mutex mtx_;
public:
    void insert(const K &k, const V &v) {
        std::lock_guard<std::mutex> lk(mtx_);
        mmap_.insert(k, v);
    }
    V *find_first(const K &k) {
        std::lock_guard<std::mutex> lk(mtx_);
        return mmap_.find_first(k);
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mtx_);
        mmap_.clear();
    }
    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return mmap_.size();
    }
};

} // namespace expli
