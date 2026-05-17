// Stubs for uninstrumented build — no-op serialization lock
extern "C" {
void tm_serialize_lock() {}
void tm_serialize_unlock() {}
}
