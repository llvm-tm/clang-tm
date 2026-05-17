#!/usr/bin/env python3
"""
gen_stl_cache.py — TM-safe STL Wrapper Generator

Generates C++ headers with TM-safe STL container wrappers and
verifies them through the LLVM TM plugin pipeline.

The wrappers provide container implementations with all operations
inline (no opaque calls), so the LLVM TM plugin can fully trace
and instrument them without errors.

Usage:
    python gen_stl_cache.py [--out DIR] [--plugin PATH] [--run]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


# ---------------------------------------------------------------------------
# Wrapper template source
# ---------------------------------------------------------------------------

TM_VECTOR_H = """\
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
"""

TM_SET_H = """\
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
"""

TM_UNORDERED_MAP_H = """\
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
"""


# ---------------------------------------------------------------------------
# Test source
# ---------------------------------------------------------------------------

TEST_SOURCE_VECTOR = r"""
#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

#include "tm_vector.h"
#include <cstdio>

TM tm_stl::vector<int> g_vec_int;

TX void test() {
    g_vec_int.push_back(1);
    g_vec_int.push_back(2);
    g_vec_int.push_back(3);
    int s = 0;
    for (size_t i = 0; i < g_vec_int.size(); ++i) s += g_vec_int[i];
    g_vec_int[0] = s;
}

MAIN int main() {
    printf("TM STL Wrapper Test\n");
    test();
    printf("vector: size=%zu, [0]=%d\n", g_vec_int.size(), g_vec_int[0]);
    int ok = (g_vec_int[0] == 6);
    printf("\n%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
"""

TEST_SOURCE_SET = r"""
#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))

#include "tm_set.h"

TM tm_stl::set<int> g_set_int;

TX void test() {
    g_set_int.insert(3);
    g_set_int.insert(1);
    g_set_int.insert(2);
}
"""

TEST_SOURCE_UNORDERED_MAP = r"""
#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))

#include "tm_unordered_map.h"

TM tm_stl::unordered_map<int, int> g_map_int;

TX void test() {
    g_map_int[1] = 10;
    g_map_int[2] = 20;
    g_map_int.erase(1);
    (void)g_map_int.count(2);
    (void)g_map_int.size();
}
"""


# ---------------------------------------------------------------------------
# Pipeline helpers
# ---------------------------------------------------------------------------

def find_llvm_config():
    for name in ["llvm-config-22", "llvm-config"]:
        path = shutil.which(name)
        if path:
            return path
    return None


def run(cmd, **kwargs):
    cmd_str = [str(c) for c in cmd]
    print(f"  + {' '.join(cmd_str)}", file=sys.stderr)
    return subprocess.run(cmd_str, capture_output=True, text=True, **kwargs)


def generate_headers(out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, content in [("tm_vector.h", TM_VECTOR_H),
                          ("tm_set.h", TM_SET_H),
                          ("tm_unordered_map.h", TM_UNORDERED_MAP_H)]:
        path = out_dir / name
        path.write_text(content.lstrip("\n"))
        print(f"  wrote {path}")
    return out_dir


def verify_pipeline(header_dir: Path, plugin: str, out_dir: Path):
    llvm_config = find_llvm_config()
    if not llvm_config:
        print("error: llvm-config not found", file=sys.stderr)
        return False

    bindir = Path(subprocess.check_output([llvm_config, "--bindir"]).decode().strip())
    cxx = bindir / "clang++"
    opt = bindir / "opt"
    llvm_link = bindir / "llvm-link"

    project_dir = Path(__file__).resolve().parent.parent
    plugin_path = Path(plugin) if plugin else project_dir / "bin" / "libTMInstrument.so"
    backends_dir = project_dir.parent / "backends"
    runtime_src = backends_dir / "runtimes" / "TinySTM_runtime.cpp"
    tinystm_inc = f"-I{backends_dir} -I{backends_dir / 'TinySTM'}"

    if not plugin_path.exists():
        print(f"error: plugin not found at {plugin_path}", file=sys.stderr)
        return False
    if not runtime_src.exists():
        print(f"warning: runtime not found at {runtime_src}, skipping link+run", file=sys.stderr)
        runtime_src = None

    _flags = "-O1 -fno-inline -fno-vectorize -fno-slp-vectorize -fno-unroll-loops -fno-stack-protector -pthread"

    def compile_and_link(src: str, label: str, link: bool = True, flags_extra: str = ""):
        import tempfile as _tf
        wd = Path(_tf.mkdtemp())

        def w(fn):
            return str(wd / fn)

        fl = _flags + " " + flags_extra
        src_path = wd / "test.cpp"
        src_path.write_text(src)

        r = run([cxx, "-std=c++20"] + fl.split() + ["-emit-llvm", "-c",
                str(src_path), "-o", w("test.bc"), f"-I{header_dir}"])
        if r.returncode != 0:
            return False, f"compile:\n{r.stderr}"

        r = run([opt, f"-load-pass-plugin={plugin_path}", "-passes=tm-instrument",
                 w("test.bc"), "-o", w("test.instr.bc")])
        if r.returncode != 0:
            return False, f"instrument:\n{r.stderr}"

        r = run([opt, "-O3", w("test.instr.bc"), "-o", w("test.opt.bc")])
        if r.returncode != 0:
            return False, f"optimize:\n{r.stderr}"

        if not link:
            return True, f"{label}: OK (compile+instrument)"

        r = run([cxx, "-std=c++20", "-O1", "-emit-llvm", "-c",
                 str(runtime_src), "-o", w("rt.bc")] +
                tinystm_inc.split() + ["-fno-stack-protector", "-pthread",
                                        "-DDESIGN_WBCTL"])
        if r.returncode != 0:
            return False, f"runtime:\n{r.stderr}"

        r = run([llvm_link, w("test.opt.bc"), w("rt.bc"), "-o", w("merged.bc")])
        if r.returncode != 0:
            return False, f"link:\n{r.stderr}"

        r = run([opt, "-O3", w("merged.bc"), "-o", w("merged.opt.bc")])
        if r.returncode != 0:
            return False, f"final-opt:\n{r.stderr}"

        r = run([cxx, "-std=c++20", "-O1", w("merged.opt.bc"),
                 "-o", w("test"), "-pthread", "-DDESIGN_WBCTL"])
        if r.returncode != 0:
            return False, f"final-link:\n{r.stderr}"

        r = run([w("test")])
        if r.returncode != 0:
            return False, f"run exit={r.returncode}\n{r.stdout}\n{r.stderr}"

        return True, f"PASS\n{r.stdout}"

    all_ok = True

    results = []
    for name, src, link in [
        ("tm_stl::vector (runtime)", TEST_SOURCE_VECTOR, True),

        ("tm_stl::set (compile+instr)", TEST_SOURCE_SET, False),

        ("tm_stl::unordered_map (compile+instr)", TEST_SOURCE_UNORDERED_MAP, False),
    ]:
        print(f"\n--- {name} ---")
        ok, out = compile_and_link(src, name, link=link)
        lines = out.strip().split("\n")
        print(f"  {'PASS' if ok else 'FAIL'}: {lines[-1] if lines else '(no output)'}")
        if ok and link and len(lines) > 1:
            for l in lines[-3:-1]:
                print(f"    {l}")
        if not ok and len(lines) > 1:
            for l in lines[-5:]:
                print(f"    {l}")
        results.append(ok)

    return all(results)

    return all_ok


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="TM STL Wrapper Generator")
    ap.add_argument("--out", default=None, help="output directory for headers")
    ap.add_argument("--plugin", default=None, help="path to libTMInstrument.so")
    ap.add_argument("--skip-verify", action="store_true", help="skip pipeline verification")
    ap.add_argument("--install", action="store_true", help="install headers to --out")
    args = ap.parse_args()

    script_dir = Path(__file__).resolve().parent
    project_dir = script_dir.parent
    default_out = project_dir / "stl_cache"

    out_dir = Path(args.out) if args.out else default_out

    print("=" * 52)
    print("  TM STL Wrapper Generator")
    print("=" * 52)
    print(f"  Output:  {out_dir}")

    if args.install or args.out:
        generate_headers(out_dir)
        print("\nHeaders generated successfully.")
    else:
        print("  (use --install to write headers)")
    print()

    if not args.skip_verify:
        ok = verify_pipeline(out_dir, args.plugin, out_dir)
        if not ok:
            print("\nVerification FAILED")
            sys.exit(1)
        print("Verification PASSED")

    if not args.install and not args.out:
        print(f"\nRun with --install to write headers to {default_out}")


if __name__ == "__main__":
    main()
