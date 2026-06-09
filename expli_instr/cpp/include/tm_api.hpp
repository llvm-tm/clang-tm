#pragma once

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// All backends use this thread-local nesting counter.
// The LLVM plugin increments it before calling tm_begin/tm_end.
// In the explicit API we set it manually.
extern __thread int32_t tm_nested_call_counter;
extern __thread int32_t tm_longjmp_ret;
extern __thread sigjmp_buf tm_jmpbuf;

extern "C" {
void     tm_init();
void     tm_exit();
void     tm_init_thread();
void     tm_exit_thread();
void     tm_begin();
void     tm_end();
void    *tm_malloc(size_t size);
void    *tm_calloc(size_t nmemb, size_t size);
void    *tm_realloc(void *ptr, size_t size);
void     tm_free(void *ptr);

uint8_t  tm_read_i1(uint8_t *addr);
uint16_t tm_read_i2(uint16_t *addr);
uint32_t tm_read_i4(uint32_t *addr);
uint64_t tm_read_i8(uint64_t *addr);
float    tm_read_f4(float *addr);
double   tm_read_f8(double *addr);
void    *tm_read_ptr(void **addr);

void tm_write_i1(uint8_t *addr, uint8_t val);
void tm_write_i2(uint16_t *addr, uint16_t val);
void tm_write_i4(uint32_t *addr, uint32_t val);
void tm_write_i8(uint64_t *addr, int64_t val);
void tm_write_f4(float *addr, float val);
void tm_write_f8(double *addr, double val);
void tm_write_ptr(void **addr, void *val);
}

namespace expli {

// ── Type traits for TM read/write dispatch ─────────────────
template<typename T> struct tm_type_traits;

template<> struct tm_type_traits<uint8_t> {
    static uint8_t  read(void *a) { return tm_read_i1((uint8_t*)a); }
    static void write(void *a, uint8_t v) { tm_write_i1((uint8_t*)a, v); }
    static constexpr size_t size = 1;
};
template<> struct tm_type_traits<int8_t>  : tm_type_traits<uint8_t> {};
template<> struct tm_type_traits<uint16_t> {
    static uint16_t read(void *a) { return tm_read_i2((uint16_t*)a); }
    static void write(void *a, uint16_t v) { tm_write_i2((uint16_t*)a, v); }
    static constexpr size_t size = 2;
};
template<> struct tm_type_traits<int16_t> : tm_type_traits<uint16_t> {};
template<> struct tm_type_traits<uint32_t> {
    static uint32_t read(void *a) { return tm_read_i4((uint32_t*)a); }
    static void write(void *a, uint32_t v) { tm_write_i4((uint32_t*)a, v); }
    static constexpr size_t size = 4;
};
template<> struct tm_type_traits<int32_t> : tm_type_traits<uint32_t> {};
template<> struct tm_type_traits<uint64_t> {
    static uint64_t read(void *a) { return tm_read_i8((uint64_t*)a); }
    static void write(void *a, uint64_t v) { tm_write_i8((uint64_t*)a, (int64_t)v); }
    static constexpr size_t size = 8;
};
template<> struct tm_type_traits<int64_t>  : tm_type_traits<uint64_t> {};
template<> struct tm_type_traits<float> {
    static float  read(void *a) { return tm_read_f4((float*)a); }
    static void write(void *a, float v) { tm_write_f4((float*)a, v); }
    static constexpr size_t size = 4;
};
template<> struct tm_type_traits<double> {
    static double read(void *a) { return tm_read_f8((double*)a); }
    static void   write(void *a, double v) { tm_write_f8((double*)a, v); }
    static constexpr size_t size = 8;
};

// ── TM<T> — Transactional Memory wrapper ─────────────────────
// TM<T> stores its value in the TM address space (allocated via tm_malloc).
// This ensures isTMAddress() assertions in the backend pass and eliminates
// the need for stack-address write-back in the commit phase.
template<typename T>
class TM {
    T *value_ = nullptr;

    T* ptr() {
        if (!value_) {
            value_ = (T*)tm_malloc(sizeof(T));
        }
        return value_;
    }
    const T* ptr() const {
        if (!value_) {
            const_cast<TM*>(this)->value_ = (T*)tm_malloc(sizeof(T));
        }
        return value_;
    }
public:
    TM() {}
    ~TM() {}  // tm_region memory: no per-object free

    // ── Lifecycle ──
    // Nesting protocol: the plugin normally increments tm_nested_call_counter
    // before calling tm_begin/tm_end.  We replicate that here so the runtime
    // sees counter==1 on the outermost tx and 0 after it commits.
    //
    // ⚠  IMPORTANT: begin() and end() do NOT call sigsetjmp/siglongjmp.
    //    Those calls must live in a frame that stays alive across the entire
    //    TX.  See transaction() below for a safe retry loop.
    //
    static void begin() {
        if (tm_nested_call_counter == 0) {
            tm_nested_call_counter = 1;
            tm_begin();
        } else {
            tm_nested_call_counter++;
        }
    }
    static void end() {
        if (tm_nested_call_counter == 1) {
            tm_end();                       // may siglongjmp to caller's sigsetjmp
            tm_nested_call_counter = 0;     // reached only on successful commit
        } else {
            tm_nested_call_counter--;
        }
    }

    // ── Safe retry loop — sigsetjmp lives in this frame ──
    // The caller provides a lambda/function with the TX body.
    // sigsetjmp is called from this frame, which does NOT return
    // between sigsetjmp and siglongjmp (undoing the UB in the old
    // begin() approach).
    template<typename F>
    static void transaction(F&& body) {
        volatile bool done = false;
        while (!done) {
            sigsetjmp(tm_jmpbuf, 0);
            tm_nested_call_counter = 1;
            tm_begin();
            body();
            tm_end();
            done = true;  // commit succeeded
        }
        tm_nested_call_counter = 0;
    }
    static void init()         { tm_init(); }
    static void exit()         { tm_exit(); }
    static void thread_init()  { tm_init_thread(); }
    static void thread_exit()  { tm_exit_thread(); }

    // ── Memory allocation (TM-tracked) ──
    static void *malloc(size_t s)          { return tm_malloc(s); }
    static void *calloc(size_t n, size_t s){ return tm_calloc(n, s); }
    static void *realloc(void *p, size_t s){ return tm_realloc(p, s); }
    static void  free(void *p)             { tm_free(p); }

    // ── Read / Write (TM operations) ──
    T read() const {
        return tm_type_traits<T>::read(const_cast<T*>(ptr()));
    }
    void write(const T &v) {
        tm_type_traits<T>::write(ptr(), v);
    }

    // ── Static read/write with explicit address ──
    static T read_at(const T *addr) {
        return tm_type_traits<T>::read(const_cast<T*>(addr));
    }
    static void write_at(T *addr, const T &v) {
        tm_type_traits<T>::write(addr, v);
    }

    // ── Peek / Poke (non-TM direct access — use only outside any TX) ──
    T peek() const { return value_ ? *value_ : T{}; }
    void poke(const T &v) { if (ptr()) *value_ = v; }
};

// ── TM<T*> specialization for pointers ──────────────────────
// The pointer itself is stored in TM address space; the pointed-to buffer
// is managed separately via alloc/free_ptr (heap, not TM-tracked).
template<typename T>
class TM<T*> {
    T **value_ = nullptr;

    T** ptr() {
        if (!value_) {
            value_ = (T**)tm_malloc(sizeof(T*));
            *value_ = nullptr;
        }
        return value_;
    }

public:
    TM() {}
    ~TM() {}

    static void begin()        { tm_nested_call_counter = 1; tm_begin(); }
    static void end()          { tm_end(); tm_nested_call_counter = 0; }

    template<typename F>
    static void transaction(F&& body) {
        volatile bool done = false;
        while (!done) {
            sigsetjmp(tm_jmpbuf, 0);
            tm_nested_call_counter = 1;
            tm_begin();
            body();
            tm_end();
            done = true;
        }
        tm_nested_call_counter = 0;
    }
    static void init()         { tm_init(); }
    static void exit()         { tm_exit(); }
    static void thread_init()  { tm_init_thread(); }
    static void thread_exit()  { tm_exit_thread(); }
    static void *malloc(size_t s)          { return tm_malloc(s); }
    static void *calloc(size_t n, size_t s){ return tm_calloc(n, s); }
    static void *realloc(void *p, size_t s){ return tm_realloc(p, s); }
    static void  free(void *p)             { tm_free(p); }

    // Allocate / free the pointed-to buffer
    // Uses ::operator new/delete directly (NOT tm_malloc/tm_free)
    // because the buffer address is tracked via tm_write_ptr and the buffer
    // itself does NOT need spec_alloc tracking.  Using tm_malloc would add
    // the buffer to g_spec_allocs, and tm_begin()'s tm_clear_spec_allocs()
    // frees ALL spec-alloc entries — including buffers allocated OUTSIDE
    // the current TX — causing use-after-free on the first TX begin().
    T *alloc(size_t n) {
        *ptr() = static_cast<T*>(::operator new(n * sizeof(T)));
        return *value_;
    }
    void free_ptr() {
        if (value_ && *value_) { ::operator delete(*value_); *value_ = nullptr; }
    }

    T *read() const {
        if (!value_) return nullptr;
        return (T*)tm_read_ptr((void**)value_);
    }
    T *read() {
        return (T*)tm_read_ptr((void**)ptr());
    }
    void write(T *v) {
        tm_write_ptr((void**)ptr(), (void*)v);
    }

    // Indexed element access inside a TX.
    // When T = TM<U>, this returns TM<U>&, so buf[i].write(v) and
    // buf[i].read() work directly.  Does NOT compile for TM<void*>
    // (void& is ill-formed).
    T& operator[](size_t i) { return read()[i]; }
    const T& operator[](size_t i) const { return const_cast<TM*>(this)->read()[i]; }

    // Direct access (use only outside TX)
    T *peek() const { return value_ ? *value_ : nullptr; }
    void poke(T *v) {
        if (value_) *value_ = v;
        else { T** p = ptr(); *p = v; }
    }

    // Static element read/write for raw pointers
    static T read_at(const T *addr) {
        return tm_type_traits<T>::read(const_cast<T*>(addr));
    }
    static void write_at(T *addr, const T &v) {
        tm_type_traits<T>::write(addr, v);
    }
};

// ── my::pair (std::pair replacement) ────────────────────────
template<typename T1, typename T2>
struct pair {
    T1 first;
    T2 second;
    pair() : first(), second() {}
    pair(const T1 &a, const T2 &b) : first(a), second(b) {}
    template<typename U1, typename U2>
    pair(const pair<U1,U2> &o) : first(o.first), second(o.second) {}
};

// ── my::make_pair ───────────────────────────────────────────
template<typename T1, typename T2>
pair<T1,T2> make_pair(const T1 &a, const T2 &b) { return pair<T1,T2>(a,b); }

// ── my::vector<T> (std::vector replacement) ─────────────────
template<typename T>
class vector {
    T      *m_data;
    size_t  m_size;
    size_t  m_cap;

    void grow(size_t min_cap) {
        size_t nc = m_cap ? m_cap : 4;
        while (nc < min_cap) nc *= 2;
        T *nd = static_cast<T *>(::operator new(nc * sizeof(T)));
        for (size_t i = 0; i < m_size; i++) {
            new (&nd[i]) T(m_data[i]);
            m_data[i].~T();
        }
        ::operator delete(m_data);
        m_data = nd;
        m_cap = nc;
    }

public:
    using value_type      = T;
    using reference       = T&;
    using const_reference = const T&;
    using iterator        = T*;
    using const_iterator  = const T*;

    vector() : m_data(nullptr), m_size(0), m_cap(0) {}
    explicit vector(size_t n) : m_data(nullptr), m_size(0), m_cap(0) { resize(n); }
    vector(const vector &o) : m_data(nullptr), m_size(0), m_cap(0) { reserve(o.m_size); for (size_t i=0;i<o.m_size;i++) new (&m_data[i]) T(o.m_data[i]); m_size=o.m_size; }
    vector &operator=(const vector &o) { if (this!=&o) { clear(); reserve(o.m_size); for (size_t i=0;i<o.m_size;i++) new (&m_data[i]) T(o.m_data[i]); m_size=o.m_size; } return *this; }
    ~vector() { clear(); ::operator delete(m_data); }

    void reserve(size_t n) { if (n > m_cap) grow(n); }
    void resize(size_t n) {
        if (n < m_size) { for (size_t i=n;i<m_size;i++) m_data[i].~T(); }
        else if (n > m_size) { reserve(n); for (size_t i=m_size;i<n;i++) new (&m_data[i]) T(); }
        m_size = n;
    }
    void clear() { for (size_t i=0;i<m_size;i++) m_data[i].~T(); m_size=0; }

    void push_back(const T &v) {
        if (m_size >= m_cap) grow(m_size + 1);
        new (&m_data[m_size]) T(v);
        m_size++;
    }
    template<typename... Args>
    void emplace_back(Args&&... args) {
        if (m_size >= m_cap) grow(m_size + 1);
        new (&m_data[m_size]) T(static_cast<Args&&>(args)...);
        m_size++;
    }
    void pop_back() { if (m_size) { m_size--; m_data[m_size].~T(); } }
    iterator erase(iterator pos) {
        if (pos < m_data || pos >= m_data + m_size) return m_data + m_size;
        size_t idx = pos - m_data;
        m_data[idx].~T();
        for (size_t i = idx + 1; i < m_size; i++) {
            new (&m_data[i-1]) T(static_cast<T&&>(m_data[i]));
            m_data[i].~T();
        }
        m_size--;
        return m_data + idx;
    }

    T &operator[](size_t i)       { return m_data[i]; }
    const T &operator[](size_t i) const { return m_data[i]; }

    T &front()       { return m_data[0]; }
    const T &front() const { return m_data[0]; }
    T &back()        { return m_data[m_size-1]; }
    const T &back() const  { return m_data[m_size-1]; }

    size_t size()     const { return m_size; }
    size_t capacity() const { return m_cap; }
    bool   empty()    const { return m_size == 0; }

    T *data()       { return m_data; }
    const T *data() const { return m_data; }

    iterator       begin()       { return m_data; }
    const_iterator begin() const { return m_data; }
    iterator       end()         { return m_data + m_size; }
    const_iterator end()   const { return m_data + m_size; }
};

// ── my::string (char array wrapper) ─────────────────────────
class string {
    char *m_data;
    size_t m_len;
public:
    string() : m_data(nullptr), m_len(0) {}
    string(const char *s) : m_data(nullptr), m_len(0) { *this = s; }
    string(const string &o) : m_data(nullptr), m_len(0) { *this = o.c_str(); }
    string &operator=(const char *s) {
        ::operator delete(m_data);
        m_len = s ? std::strlen(s) : 0;
        m_data = static_cast<char *>(::operator new(m_len + 1));
        if (s) std::memcpy(m_data, s, m_len + 1);
        else m_data[0] = 0;
        return *this;
    }
    string &operator=(const string &o) { return *this = o.c_str(); }
    ~string() { ::operator delete(m_data); }

    const char *c_str() const { return m_data ? m_data : ""; }
    size_t size() const { return m_len; }
    bool empty() const { return m_len == 0; }

    char &operator[](size_t i) { return m_data[i]; }
    const char &operator[](size_t i) const { return m_data[i]; }
};

} // namespace expli
