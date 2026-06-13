// norec_wrapper.cpp
//
// Compiles NOrec_runtime.cpp with renamed symbols so it can coexist
// with TinySTM in the same binary for runtime swap testing.
//
// Each renamed symbol gets a norec_ prefix to avoid linker conflicts.

// ── NOrec operator new/delete (skip in wrapper mode) ─────────────
#define NOREC_AS_WRAPPER

// ── TLS variables (shared in tm_hooks.cpp, but per-backend state
//    like g_in_tx still needs renaming)
#define g_in_tx                 norec_g_in_tx
#define g_deferred_frees        norec_g_deferred_frees
#define g_deferred_frees_set    norec_g_deferred_frees_set
#define g_spec_allocs           norec_g_spec_allocs

// ── Init / exit ─────────────────────────────────────────────────
#define tm_init                 norec_tm_init
#define tm_exit                 norec_tm_exit
#define tm_init_thread          norec_tm_init_thread
#define tm_exit_thread          norec_tm_exit_thread

// ── Infrastructure (serialisation, setjmp, env, thread state) ───
#define tm_serialize_lock        norec_tm_serialize_lock
#define tm_serialize_unlock      norec_tm_serialize_unlock
#define tm_serialize_unlock_all  norec_tm_serialize_unlock_all
#define tm_setjmp                norec_tm_setjmp
#define tm_set_jmpbuf            norec_tm_set_jmpbuf
#define tm_get_env               norec_tm_get_env
#define tm_set_env               norec_tm_set_env
#define tm_get_thread_state      norec_tm_get_thread_state

// ── Plugin‑specific runtime functions ───────────────────────────
#define tm_read_i16              norec_tm_read_i16
#define tm_read_i32              norec_tm_read_i32
#define tm_read_i64              norec_tm_read_i64
#define tm_read_z                norec_tm_read_z
#define tm_write_i16             norec_tm_write_i16
#define tm_write_i32             norec_tm_write_i32
#define tm_write_i64             norec_tm_write_i64
#define tm_write_z               norec_tm_write_z
#define tm_memset                norec_tm_memset
#define tm_load_symbols          norec_tm_load_symbols
#define consume_ptr              norec_consume_ptr

#include "../../backends/tm_impl/norec/NOrec_runtime.cpp"
