#ifndef TM_CACHE_SET_H
#define TM_CACHE_SET_H

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace tm_stl {

template<typename T>
class set {
    T* _data = nullptr;
    size_t _size = 0;
    size_t _capacity = 0;

    size_t lower_bound(const T& val) const {
        size_t lo = 0, hi = _size;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (_data[mid] < val)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

public:
    using iterator = T*;
    using const_iterator = const T*;
    using value_type = T;

    set() = default;

    ~set() {
        for (size_t i = 0; i < _size; ++i)
            _data[i].~T();
        if (_data) std::free(_data);
    }

    set(const set& other) {
        if (other._size > 0) {
            _data = static_cast<T*>(std::malloc(other._size * sizeof(T)));
            for (size_t i = 0; i < other._size; ++i)
                ::new (static_cast<void*>(&_data[i])) T(other._data[i]);
            _size = other._size;
            _capacity = other._size;
        }
    }

    set(set&& other) noexcept
        : _data(other._data), _size(other._size), _capacity(other._capacity) {
        other._data = nullptr;
        other._size = 0;
        other._capacity = 0;
    }

    set& operator=(const set& other) {
        if (this != &other) {
            for (size_t i = 0; i < _size; ++i) _data[i].~T();
            if (_data) std::free(_data);
            _data = nullptr;
            _size = 0;
            _capacity = 0;
            if (other._size > 0) {
                _data = static_cast<T*>(std::malloc(other._size * sizeof(T)));
                for (size_t i = 0; i < other._size; ++i)
                    ::new (static_cast<void*>(&_data[i])) T(other._data[i]);
                _size = other._size;
                _capacity = other._size;
            }
        }
        return *this;
    }

    set& operator=(set&& other) noexcept {
        if (this != &other) {
            for (size_t i = 0; i < _size; ++i) _data[i].~T();
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

    std::pair<iterator, bool> insert(const T& val) {
        size_t pos = lower_bound(val);
        if (pos < _size && !(_data[pos] < val) && !(val < _data[pos]))
            return { &_data[pos], false };
        if (_size >= _capacity) {
            size_t new_cap = _capacity == 0 ? 1 : _capacity * 2;
            T* new_data = static_cast<T*>(std::malloc(new_cap * sizeof(T)));
            for (size_t i = 0; i < pos; ++i) {
                ::new (static_cast<void*>(&new_data[i])) T(_data[i]);
                _data[i].~T();
            }
            ::new (static_cast<void*>(&new_data[pos])) T(val);
            for (size_t i = pos; i < _size; ++i) {
                ::new (static_cast<void*>(&new_data[i + 1])) T(_data[i]);
                _data[i].~T();
            }
            if (_data) std::free(_data);
            _data = new_data;
            _capacity = new_cap;
        } else {
            for (size_t i = _size; i > pos; --i) {
                _data[i] = std::move(_data[i - 1]);
            }
            ::new (static_cast<void*>(&_data[pos])) T(val);
        }
        ++_size;
        return { &_data[pos], true };
    }

    std::pair<iterator, bool> insert(T&& val) {
        size_t pos = lower_bound(val);
        if (pos < _size && !(_data[pos] < val) && !(val < _data[pos]))
            return { &_data[pos], false };
        if (_size >= _capacity) {
            size_t new_cap = _capacity == 0 ? 1 : _capacity * 2;
            T* new_data = static_cast<T*>(std::malloc(new_cap * sizeof(T)));
            for (size_t i = 0; i < pos; ++i) {
                ::new (static_cast<void*>(&new_data[i])) T(std::move(_data[i]));
                _data[i].~T();
            }
            ::new (static_cast<void*>(&new_data[pos])) T(std::move(val));
            for (size_t i = pos; i < _size; ++i) {
                ::new (static_cast<void*>(&new_data[i + 1])) T(std::move(_data[i]));
                _data[i].~T();
            }
            if (_data) std::free(_data);
            _data = new_data;
            _capacity = new_cap;
        } else {
            for (size_t i = _size; i > pos; --i) {
                _data[i] = std::move(_data[i - 1]);
            }
            ::new (static_cast<void*>(&_data[pos])) T(std::move(val));
        }
        ++_size;
        return { &_data[pos], true };
    }

    iterator find(const T& val) {
        size_t pos = lower_bound(val);
        if (pos < _size && !(_data[pos] < val) && !(val < _data[pos]))
            return &_data[pos];
        return end();
    }

    const_iterator find(const T& val) const {
        size_t pos = lower_bound(val);
        if (pos < _size && !(_data[pos] < val) && !(val < _data[pos]))
            return &_data[pos];
        return end();
    }

    size_t count(const T& val) const {
        return find(val) != end() ? 1 : 0;
    }

    size_t erase(const T& val) {
        size_t pos = lower_bound(val);
        if (pos < _size && !(_data[pos] < val) && !(val < _data[pos])) {
            _data[pos].~T();
            for (size_t i = pos + 1; i < _size; ++i) {
                ::new (static_cast<void*>(&_data[i - 1])) T(std::move(_data[i]));
                _data[i].~T();
            }
            --_size;
            return 1;
        }
        return 0;
    }

    void clear() {
        for (size_t i = 0; i < _size; ++i) _data[i].~T();
        _size = 0;
    }

    size_t size() const { return _size; }
    size_t capacity() const { return _capacity; }
    bool empty() const { return _size == 0; }

    iterator begin() { return _data; }
    const_iterator begin() const { return _data; }
    iterator end() { return _data + _size; }
    const_iterator end() const { return _data + _size; }

    void swap(set& other) noexcept {
        T* tmp_data = _data;
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

#endif // TM_CACHE_SET_H
