fn main() {
    // Export all symbols from the Rust binary so that dlopen'd .so files
    // (SimBackend, application) can resolve their extern "C" references
    // (sim_tm_read, etc.) to the Rust implementations.
    //
    // macOS: -rdynamic is accepted by clang and translated to -export_dynamic.
    // Linux: -rdynamic is the ld flag.
    println!("cargo:rustc-link-arg=-rdynamic");
}
