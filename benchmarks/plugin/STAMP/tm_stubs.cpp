// Stubs for uninstrumented build — no-op serialization lock
#include <cstdlib>

extern "C" {
void tm_serialize_lock() {}
void tm_serialize_unlock() {}
void* tm_calloc(size_t nmemb, size_t size) { return std::calloc(nmemb, size); }
}
