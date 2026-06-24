pub mod ffi;
pub mod sim_state;

use sim_state::LiveSimState;
use std::sync::Mutex;
use std::ffi::{CString, CStr};
use std::os::raw::c_char;

/// Global live simulation state, accessed by the C FFI functions.
static LIVE_STATE: Mutex<Option<LiveSimState>> = Mutex::new(None);

/// Initialise the live simulation state.
pub fn init(max_threads: u32) {
    let mut state = LIVE_STATE.lock().unwrap();
    *state = Some(LiveSimState::new(max_threads));
}

/// Print the final simulation report.
pub fn print_report() {
    let guard = LIVE_STATE.lock().unwrap();
    if let Some(ref state) = *guard {
        state.report();
    }
}

/// Run the application loaded from a .so.
///
/// Loads the SimBackend .so first (with RTLD_GLOBAL so its symbols are
/// visible to the app), then loads the application .so (which resolves its
/// TM hook references to the SimBackend definitions).
pub fn run_app(sim_backend_path: &str, app_path: &str, app_args: &[String]) -> Result<i32, String> {
    // Load SimBackend with RTLD_GLOBAL so its symbols are visible to the app.
    let sim_cpath = CString::new(sim_backend_path).map_err(|_| "null in sim_backend_path")?;
    let _sim = unsafe {
        let h = libc::dlopen(sim_cpath.as_ptr(), libc::RTLD_LAZY | libc::RTLD_GLOBAL);
        if h.is_null() {
            let err = CStr::from_ptr(libc::dlerror());
            return Err(format!("load SimBackend: {}", err.to_string_lossy()));
        }
        h
    };

    // Load the application .so.
    let app_cpath = CString::new(app_path).map_err(|_| "null in app_path")?;
    let app_lib = unsafe {
        let h = libc::dlopen(app_cpath.as_ptr(), libc::RTLD_LAZY | libc::RTLD_LOCAL);
        if h.is_null() {
            let err = CStr::from_ptr(libc::dlerror());
            return Err(format!("load app: {}", err.to_string_lossy()));
        }
        h
    };

    // Find entry point: try "run_benchmark" first, then "main".
    let entry_name = b"run_benchmark\0";
    let mut main_ptr = unsafe { libc::dlsym(app_lib, entry_name.as_ptr() as *const c_char) };
    if main_ptr.is_null() {
        let entry_name2 = b"main\0";
        main_ptr = unsafe { libc::dlsym(app_lib, entry_name2.as_ptr() as *const c_char) };
        if main_ptr.is_null() {
            let err = unsafe { CStr::from_ptr(libc::dlerror()) };
            return Err(format!("find entry point: {}", err.to_string_lossy()));
        }
    }
    let main_fn: unsafe extern "C" fn(i32, *mut *mut u8) -> i32 =
        unsafe { std::mem::transmute(main_ptr) };

    // Build argc/argv for the app.
    let mut c_args: Vec<*mut u8> = Vec::new();
    for arg in app_args {
        let c_arg = CString::new(arg.as_bytes()).map_err(|_| "null byte in arg")?;
        c_args.push(c_arg.into_raw() as *mut u8);
    }
    c_args.push(std::ptr::null_mut()); // argv[argc] == NULL per convention

    let argc = app_args.len() as i32;
    let argv = c_args.as_mut_ptr();

    eprintln!("[tm-live] calling app entry point...");
    let ret = unsafe { main_fn(argc, argv) };
    eprintln!("[tm-live] app returned: {}", ret);

    // Free the CStrings we created.
    for (i, _) in app_args.iter().enumerate() {
        let _ = unsafe { CString::from_raw(c_args[i] as *mut c_char) };
    }

    Ok(ret)
}
