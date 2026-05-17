#ifndef TM_CACHE_UNORDERED_MAP_H
#define TM_CACHE_UNORDERED_MAP_H

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace tm_stl {

namespace detail {
    template<typename K, typename = void>
    struct default_hash {
        size_t operator()(const K& k) const {
            return static_cast<size_t>(k);
        }
    };
}

template<typename K, typename V, typename Hash = detail::default_hash<K>>
class unordered_map {
    struct Entry {
        std::pair<K, V> kv;
        size_t hash;
        Entry* next;
    };

    Entry** _buckets = nullptr;
    size_t _bucket_count = 0;
    size_t _size = 0;

    size_t hash_key(const K& k) const {
        return Hash{}(k);
    }

    void grow() {
        size_t new_bc = _bucket_count == 0 ? 8 : _bucket_count * 2;
        Entry** new_buckets = static_cast<Entry**>(
            std::calloc(new_bc, sizeof(Entry*)));
        for (size_t i = 0; i < _bucket_count; ++i) {
            Entry* e = _buckets[i];
            while (e) {
                Entry* next = e->next;
                size_t bi = e->hash % new_bc;
                e->next = new_buckets[bi];
                new_buckets[bi] = e;
                e = next;
            }
        }
        if (_buckets) std::free(_buckets);
        _buckets = new_buckets;
        _bucket_count = new_bc;
    }

    Entry* find_entry(const K& k) const {
        if (_bucket_count == 0) return nullptr;
        size_t h = hash_key(k);
        size_t bi = h % _bucket_count;
        Entry* e = _buckets[bi];
        while (e) {
            if (e->hash == h && e->kv.first == k) return e;
            e = e->next;
        }
        return nullptr;
    }

public:
    using key_type = K;
    using mapped_type = V;
    using value_type = std::pair<const K, V>;

    unordered_map() = default;

    ~unordered_map() {
        for (size_t i = 0; i < _bucket_count; ++i) {
            Entry* e = _buckets[i];
            while (e) {
                Entry* next = e->next;
                e->~Entry();
                std::free(e);
                e = next;
            }
        }
        if (_buckets) std::free(_buckets);
    }

    unordered_map(const unordered_map&) = delete;
    unordered_map& operator=(const unordered_map&) = delete;
    unordered_map(unordered_map&&) = delete;
    unordered_map& operator=(unordered_map&&) = delete;

    V& operator[](const K& k) {
        Entry* existing = find_entry(k);
        if (existing) return existing->kv.second;

        if (_size >= _bucket_count) grow();
        size_t h = hash_key(k);
        size_t bi = h % _bucket_count;
        void* mem = std::malloc(sizeof(Entry));
        Entry* ne = ::new (mem) Entry{};
        ne->kv.first = k;
        ne->kv.second = V();
        ne->hash = h;
        ne->next = _buckets[bi];
        _buckets[bi] = ne;
        ++_size;
        return ne->kv.second;
    }

    V& at(const K& k) {
        Entry* e = find_entry(k);
        return e->kv.second;
    }

    const V& at(const K& k) const {
        Entry* e = find_entry(k);
        return e->kv.second;
    }

    size_t count(const K& k) const {
        return find_entry(k) ? 1 : 0;
    }

    size_t erase(const K& k) {
        if (_bucket_count == 0) return 0;
        size_t h = hash_key(k);
        size_t bi = h % _bucket_count;
        Entry** prev = &_buckets[bi];
        Entry* e = _buckets[bi];
        while (e) {
            if (e->hash == h && e->kv.first == k) {
                *prev = e->next;
                e->~Entry();
                std::free(e);
                --_size;
                return 1;
            }
            prev = &e->next;
            e = e->next;
        }
        return 0;
    }

    void clear() {
        for (size_t i = 0; i < _bucket_count; ++i) {
            Entry* e = _buckets[i];
            while (e) {
                Entry* next = e->next;
                e->~Entry();
                std::free(e);
                e = next;
            }
            _buckets[i] = nullptr;
        }
        _size = 0;
    }

    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }

    // Note: iteration (range-for, iterators, for_each with lambdas)
    // cannot be safely instrumented inside TX functions because
    // closures and iterator objects are local (not TM-traced).
    // Use for_each with a function pointer or plain operator[] lookups.
};

} // namespace tm_stl

#endif // TM_CACHE_UNORDERED_MAP_H
