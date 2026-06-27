// ── C FFI entry points ──────────────────────────────────────
// These functions are exported from the Rust binary and called by
// the C++ SimBackend (.so) via dlsym / normal C ABI.

use crate::live_app::LIVE_STATE;

fn with_state<F, R>(f: F) -> R
where
    F: FnOnce(&mut crate::live_app::sim_state::LiveSimState) -> R,
{
    let mut guard = LIVE_STATE.lock().unwrap();
    let state = guard.as_mut().expect("LiveSimState not initialised");
    f(state)
}

#[no_mangle]
pub extern "C" fn sim_tm_read(addr: u64, width: u8, thread_id: u32) -> u64 {
    with_state(|s| s.tm_read(addr, width, thread_id))
}

#[no_mangle]
pub extern "C" fn sim_tm_write(addr: u64, width: u8, val: u64, thread_id: u32) {
    with_state(|s| s.tm_write(addr, width, val, thread_id));
}

#[no_mangle]
pub extern "C" fn sim_tm_begin(thread_id: u32) {
    with_state(|s| s.tm_begin(thread_id));
}

#[no_mangle]
pub extern "C" fn sim_tm_end(thread_id: u32) -> u8 {
    with_state(|s| {
        if s.tm_end(thread_id) { 1 } else { 0 }
    })
}

#[no_mangle]
pub extern "C" fn sim_tm_abort(thread_id: u32) {
    with_state(|s| s.tm_abort(thread_id));
}

#[no_mangle]
pub extern "C" fn sim_tm_malloc(size: u64, thread_id: u32) -> u64 {
    with_state(|s| s.tm_malloc(size, thread_id))
}

#[no_mangle]
pub extern "C" fn sim_tm_free(addr: u64, thread_id: u32) {
    with_state(|s| s.tm_free(addr, thread_id));
}

#[no_mangle]
pub extern "C" fn sim_tm_set_jmpbuf(buf: u64, thread_id: u32) {
    with_state(|s| s.tm_set_jmpbuf(buf, thread_id));
}

#[no_mangle]
pub extern "C" fn sim_tm_get_env(thread_id: u32) -> *mut std::ffi::c_void {
    with_state(|s| s.tm_get_env(thread_id)) as *mut std::ffi::c_void
}

#[no_mangle]
pub extern "C" fn sim_tm_get_thread_state(thread_id: u32) -> *mut std::ffi::c_void {
    with_state(|s| s.tm_get_thread_state(thread_id)) as *mut std::ffi::c_void
}

/// Called by the application's main() to get the thread ID.
/// The SimBackend initialises thread IDs via tm_init_thread().
/// This is used when the Rust side needs to set a thread's initial state.
#[no_mangle]
pub extern "C" fn sim_tm_set_thread_id(tid: u32) {
    // For single-threaded Phase 1, this is a no-op.
    // Multi-threaded Phase 2 will use this to associate an OS thread
    // with a simulator thread context.
    let _ = tid;
}
