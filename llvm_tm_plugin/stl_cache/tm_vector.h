#ifndef TM_CACHE_VECTOR_H
#define TM_CACHE_VECTOR_H

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

namespace tm_stl {

template<typename T>
class vector {
    T* _data = nullptr;
    size_t _size = 0;
    size_t _capacity = 0;

public:
    using iterator = T*;
    using const_iterator = const T*;
    using value_type = T;

    vector() = default;

    explicit vector(size_t n) {
        if (n > 0) {
            _data = static_cast<T*>(std::malloc(n * sizeof(T)));
            for (size_t i = 0; i < n; ++i)
                ::new (static_cast<void*>(&_data[i])) T();
            _size = n;
            _capacity = n;
        }
    }

    ~vector() {
        for (size_t i = 0; i < _size; ++i)
            _data[i].~T();
        if (_data) std::free(_data);
    }

    vector(const vector& other) {
        if (other._size > 0) {
            _data = static_cast<T*>(std::malloc(other._size * sizeof(T)));
            for (size_t i = 0; i < other._size; ++i)
                ::new (static_cast<void*>(&_data[i])) T(other._data[i]);
            _size = other._size;
            _capacity = other._size;
        }
    }

    vector(vector&& other) noexcept
        : _data(other._data), _size(other._size), _capacity(other._capacity) {
        other._data = nullptr;
        other._size = 0;
        other._capacity = 0;
    }

    vector& operator=(const vector& other) {
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

    vector& operator=(vector&& other) noexcept {
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

    void push_back(const T& val) {
        if (_size >= _capacity) {
            size_t new_cap = _capacity == 0 ? 1 : _capacity * 2;
            T* new_data = static_cast<T*>(std::malloc(new_cap * sizeof(T)));
            for (size_t i = 0; i < _size; ++i) {
                ::new (static_cast<void*>(&new_data[i])) T(_data[i]);
                _data[i].~T();
            }
            if (_data) std::free(_data);
            _data = new_data;
            _capacity = new_cap;
        }
        ::new (static_cast<void*>(&_data[_size])) T(val);
        ++_size;
    }

    void push_back(T&& val) {
        if (_size >= _capacity) {
            size_t new_cap = _capacity == 0 ? 1 : _capacity * 2;
            T* new_data = static_cast<T*>(std::malloc(new_cap * sizeof(T)));
            for (size_t i = 0; i < _size; ++i) {
                ::new (static_cast<void*>(&new_data[i])) T(std::move(_data[i]));
                _data[i].~T();
            }
            if (_data) std::free(_data);
            _data = new_data;
            _capacity = new_cap;
        }
        ::new (static_cast<void*>(&_data[_size])) T(std::move(val));
        ++_size;
    }

    template<typename... Args>
    void emplace_back(Args&&... args) {
        if (_size >= _capacity) {
            size_t new_cap = _capacity == 0 ? 1 : _capacity * 2;
            T* new_data = static_cast<T*>(std::malloc(new_cap * sizeof(T)));
            for (size_t i = 0; i < _size; ++i) {
                ::new (static_cast<void*>(&new_data[i])) T(std::move(_data[i]));
                _data[i].~T();
            }
            if (_data) std::free(_data);
            _data = new_data;
            _capacity = new_cap;
        }
        ::new (static_cast<void*>(&_data[_size])) T(std::forward<Args>(args)...);
        ++_size;
    }

    void pop_back() {
        --_size;
        _data[_size].~T();
    }

    void reserve(size_t n) {
        if (n > _capacity) {
            T* new_data = static_cast<T*>(std::malloc(n * sizeof(T)));
            for (size_t i = 0; i < _size; ++i) {
                ::new (static_cast<void*>(&new_data[i])) T(_data[i]);
                _data[i].~T();
            }
            if (_data) std::free(_data);
            _data = new_data;
            _capacity = n;
        }
    }

    void resize(size_t n) {
        if (n < _size) {
            for (size_t i = n; i < _size; ++i) _data[i].~T();
        } else if (n > _size) {
            if (n > _capacity) {
                T* new_data = static_cast<T*>(std::malloc(n * sizeof(T)));
                for (size_t i = 0; i < _size; ++i) {
                    ::new (static_cast<void*>(&new_data[i])) T(std::move(_data[i]));
                    _data[i].~T();
                }
                if (_data) std::free(_data);
                _data = new_data;
                _capacity = n;
            }
            for (size_t i = _size; i < n; ++i)
                ::new (static_cast<void*>(&_data[i])) T();
        }
        _size = n;
    }

    size_t size() const { return _size; }
    size_t capacity() const { return _capacity; }
    bool empty() const { return _size == 0; }

    T& operator[](size_t i) { return _data[i]; }
    const T& operator[](size_t i) const { return _data[i]; }

    T& at(size_t i) { return _data[i]; }
    const T& at(size_t i) const { return _data[i]; }

    T& front() { return _data[0]; }
    const T& front() const { return _data[0]; }

    T& back() { return _data[_size - 1]; }
    const T& back() const { return _data[_size - 1]; }

    T* data() { return _data; }
    const T* data() const { return _data; }

    iterator begin() { return _data; }
    const_iterator begin() const { return _data; }
    iterator end() { return _data + _size; }
    const_iterator end() const { return _data + _size; }

    void clear() {
        for (size_t i = 0; i < _size; ++i) _data[i].~T();
        _size = 0;
    }

    void swap(vector& other) noexcept {
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

#endif // TM_CACHE_VECTOR_H
