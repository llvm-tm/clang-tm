#ifndef TM_CACHE_MAP_H
#define TM_CACHE_MAP_H

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace tm_stl {

template<typename K, typename V>
class map {
    struct Entry {
        K key;
        V value;
    };
    Entry* _data = nullptr;
    size_t _size = 0;
    size_t _capacity = 0;

    size_t _lower_bound(const K& key) const {
        size_t lo = 0, hi = _size;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (_data[mid].key < key)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

public:
    using key_type = K;
    using mapped_type = V;
    using value_type = std::pair<const K, V>;

    class iterator {
        Entry* _entry;
    public:
        explicit iterator(Entry* e = nullptr) : _entry(e) {}
        using pointer = value_type*;
        using reference = value_type&;

        value_type& operator*() const {
            return reinterpret_cast<value_type&>(*_entry);
        }
        value_type* operator->() const {
            return reinterpret_cast<value_type*>(_entry);
        }
        iterator& operator++() { ++_entry; return *this; }
        iterator operator++(int) { auto t = *this; ++_entry; return t; }
        iterator& operator--() { --_entry; return *this; }
        iterator operator--(int) { auto t = *this; --_entry; return t; }
        bool operator==(const iterator& o) const { return _entry == o._entry; }
        bool operator!=(const iterator& o) const { return _entry != o._entry; }
        Entry* entry() const { return _entry; }
    };

    class const_iterator {
        const Entry* _entry;
    public:
        explicit const_iterator(const Entry* e = nullptr) : _entry(e) {}
        const_iterator(const iterator& it) : _entry(it.entry()) {}

        const value_type& operator*() const {
            return reinterpret_cast<const value_type&>(*_entry);
        }
        const value_type* operator->() const {
            return reinterpret_cast<const value_type*>(_entry);
        }
        const_iterator& operator++() { ++_entry; return *this; }
        const_iterator operator++(int) { auto t = *this; ++_entry; return t; }
        const_iterator& operator--() { --_entry; return *this; }
        const_iterator operator--(int) { auto t = *this; --_entry; return t; }
        bool operator==(const const_iterator& o) const { return _entry == o._entry; }
        bool operator!=(const const_iterator& o) const { return _entry != o._entry; }
    };

    map() = default;

    ~map() {
        for (size_t i = 0; i < _size; ++i)
            _data[i].~Entry();
        if (_data) std::free(_data);
    }

    map(const map& other) {
        if (other._size > 0) {
            _data = static_cast<Entry*>(std::malloc(other._size * sizeof(Entry)));
            for (size_t i = 0; i < other._size; ++i)
                ::new (static_cast<void*>(&_data[i])) Entry(other._data[i]);
            _size = other._size;
            _capacity = other._size;
        }
    }

    map(map&& other) noexcept
        : _data(other._data), _size(other._size), _capacity(other._capacity) {
        other._data = nullptr;
        other._size = 0;
        other._capacity = 0;
    }

    map& operator=(const map& other) {
        if (this != &other) {
            for (size_t i = 0; i < _size; ++i) _data[i].~Entry();
            if (_data) std::free(_data);
            _data = nullptr;
            _size = 0;
            _capacity = 0;
            if (other._size > 0) {
                _data = static_cast<Entry*>(std::malloc(other._size * sizeof(Entry)));
                for (size_t i = 0; i < other._size; ++i)
                    ::new (static_cast<void*>(&_data[i])) Entry(other._data[i]);
                _size = other._size;
                _capacity = other._size;
            }
        }
        return *this;
    }

    map& operator=(map&& other) noexcept {
        if (this != &other) {
            for (size_t i = 0; i < _size; ++i) _data[i].~Entry();
            if (_data) std::free(_data);
            _data = other._data;
            _size = other._size;
            _capacity = other._capacity;
            other._data = nullptr;
            other._size = 0;
            other._capacity = 0;
        }
        return *this;
    }

    V& operator[](const K& key) {
        size_t pos = _lower_bound(key);
        if (pos < _size && !(_data[pos].key < key) && !(key < _data[pos].key))
            return _data[pos].value;
        size_t new_cap = _capacity == 0 ? 1 : _capacity * 2;
        Entry* new_data = static_cast<Entry*>(std::malloc(new_cap * sizeof(Entry)));
        for (size_t i = 0; i < pos; ++i) {
            ::new (static_cast<void*>(&new_data[i])) Entry(std::move(_data[i]));
            _data[i].~Entry();
        }
        ::new (static_cast<void*>(&new_data[pos])) Entry{key, V()};
        for (size_t i = pos; i < _size; ++i) {
            ::new (static_cast<void*>(&new_data[i + 1])) Entry(std::move(_data[i]));
            _data[i].~Entry();
        }
        if (_data) std::free(_data);
        _data = new_data;
        _capacity = new_cap;
        ++_size;
        return _data[pos].value;
    }

    iterator find(const K& key) {
        size_t pos = _lower_bound(key);
        if (pos < _size && !(_data[pos].key < key) && !(key < _data[pos].key))
            return iterator(&_data[pos]);
        return end();
    }

    const_iterator find(const K& key) const {
        size_t pos = _lower_bound(key);
        if (pos < _size && !(_data[pos].key < key) && !(key < _data[pos].key))
            return const_iterator(&_data[pos]);
        return end();
    }

    std::pair<iterator, bool> insert(const value_type& val) {
        K k = val.first;
        size_t pos = _lower_bound(k);
        if (pos < _size && !(_data[pos].key < k) && !(k < _data[pos].key))
            return { iterator(&_data[pos]), false };
        if (_size >= _capacity) {
            size_t new_cap = _capacity == 0 ? 1 : _capacity * 2;
            Entry* new_data = static_cast<Entry*>(std::malloc(new_cap * sizeof(Entry)));
            for (size_t i = 0; i < pos; ++i) {
                ::new (static_cast<void*>(&new_data[i])) Entry(std::move(_data[i]));
                _data[i].~Entry();
            }
            ::new (static_cast<void*>(&new_data[pos])) Entry{val.first, val.second};
            for (size_t i = pos; i < _size; ++i) {
                ::new (static_cast<void*>(&new_data[i + 1])) Entry(std::move(_data[i]));
                _data[i].~Entry();
            }
            if (_data) std::free(_data);
            _data = new_data;
            _capacity = new_cap;
        } else {
            for (size_t i = _size; i > pos; --i) {
                _data[i].key = std::move(_data[i - 1].key);
                _data[i].value = std::move(_data[i - 1].value);
            }
            ::new (static_cast<void*>(&_data[pos])) Entry{val.first, val.second};
        }
        ++_size;
        return { iterator(&_data[pos]), true };
    }

    size_t erase(const K& key) {
        size_t pos = _lower_bound(key);
        if (pos < _size && !(_data[pos].key < key) && !(key < _data[pos].key)) {
            _data[pos].~Entry();
            for (size_t i = pos + 1; i < _size; ++i) {
                ::new (static_cast<void*>(&_data[i - 1])) Entry(std::move(_data[i]));
                _data[i].~Entry();
            }
            --_size;
            return 1;
        }
        return 0;
    }

    iterator erase(iterator it) {
        if (it == end()) return end();
        size_t pos = _lower_bound(it->first);
        if (pos >= _size) return end();
        _data[pos].~Entry();
        for (size_t i = pos + 1; i < _size; ++i) {
            ::new (static_cast<void*>(&_data[i - 1])) Entry(std::move(_data[i]));
            _data[i].~Entry();
        }
        --_size;
        return pos < _size ? iterator(&_data[pos]) : end();
    }

    size_t count(const K& key) const {
        return find(key) != end() ? 1 : 0;
    }

    void clear() {
        for (size_t i = 0; i < _size; ++i) _data[i].~Entry();
        _size = 0;
    }

    size_t size() const { return _size; }
    size_t capacity() const { return _capacity; }
    bool empty() const { return _size == 0; }

    iterator begin() { return iterator(_data); }
    const_iterator begin() const { return const_iterator(_data); }
    iterator end() { return iterator(_data + _size); }
    const_iterator end() const { return const_iterator(_data + _size); }

    iterator lower_bound(const K& key) {
        return iterator(_data + _lower_bound(key));
    }

    const_iterator lower_bound(const K& key) const {
        return const_iterator(_data + _lower_bound(key));
    }

    iterator upper_bound(const K& key) {
        size_t lo = 0, hi = _size;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (!(key < _data[mid].key))
                lo = mid + 1;
            else
                hi = mid;
        }
        return iterator(_data + lo);
    }

    const_iterator upper_bound(const K& key) const {
        size_t lo = 0, hi = _size;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (!(key < _data[mid].key))
                lo = mid + 1;
            else
                hi = mid;
        }
        return const_iterator(_data + lo);
    }

    void swap(map& other) noexcept {
        Entry* tmp_data = _data;
        size_t tmp_size = _size;
        size_t tmp_cap = _capacity;
        _data = other._data;
        _size = other._size;
        _capacity = other._capacity;
        other._data = tmp_data;
        other._size = tmp_size;
        other._capacity = tmp_cap;
    }
};

} // namespace tm_stl

#endif // TM_CACHE_MAP_H
