// ── Backend abstraction ─────────────────────────────────
// Runtime dispatching between TM backends in simulation mode.
// Each registered backend exports the same simulation API.

/// Available TM backends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    Norec,
    Tl2,
}

impl Backend {
    pub fn from_name(name: &str) -> Option<Self> {
        match name {
            "norec" | "no-rec" => Some(Backend::Norec),
            "tl2" | "TL2" => Some(Backend::Tl2),
            _ => None,
        }
    }

    pub fn name(&self) -> &'static str {
        match self {
            Backend::Norec => "norec",
            Backend::Tl2 => "tl2",
        }
    }

    /// Initialize the TM runtime.
    pub fn init(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_init(),
            Backend::Tl2 => runtime_tl2::tm_init(),
        }
    }

    /// Initialize a (simulated) thread.
    pub fn init_thread(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_init_thread(),
            Backend::Tl2 => runtime_tl2::tm_init_thread(),
        }
    }

    /// Begin a transaction.
    pub fn begin(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_begin(),
            Backend::Tl2 => runtime_tl2::tm_begin(),
        }
    }

    /// Commit a transaction. Returns true on success.
    pub fn commit(&self) -> bool {
        match self {
            Backend::Norec => runtime_norec::tm_commit(),
            Backend::Tl2 => runtime_tl2::tm_commit(),
        }
    }

    /// Abort the current transaction.
    pub fn abort(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_abort(),
            Backend::Tl2 => runtime_tl2::tm_abort(),
        }
    }

    /// Set the simulated thread ID.
    pub fn sim_set_thread_id(&self, id: u64) {
        match self {
            Backend::Norec => runtime_norec::sim::set_thread_id(id),
            Backend::Tl2 => runtime_tl2::sim::set_thread_id(id),
        }
    }

    /// Clear the simulated thread ID.
    pub fn sim_clear_thread_id(&self) {
        match self {
            Backend::Norec => runtime_norec::sim::clear_thread_id(),
            Backend::Tl2 => runtime_tl2::sim::clear_thread_id(),
        }
    }

    /// Reset all simulated thread state.
    pub fn sim_reset(&self) {
        match self {
            Backend::Norec => runtime_norec::sim::reset(),
            Backend::Tl2 => runtime_tl2::sim::reset(),
        }
    }

    /// Typed transactional reads.
    pub fn read_u8(&self, addr: *mut u8) -> u8 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u8(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u8(addr),
        }
    }
    pub fn read_u16(&self, addr: *mut u16) -> u16 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u16(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u16(addr),
        }
    }
    pub fn read_u32(&self, addr: *mut u32) -> u32 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u32(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u32(addr),
        }
    }
    pub fn read_u64(&self, addr: *mut u64) -> u64 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u64(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u64(addr),
        }
    }

    /// Typed transactional writes.
    pub fn write_u8(&self, addr: *mut u8, val: u8) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u8(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u8(addr, val),
        }
    }
    pub fn write_u16(&self, addr: *mut u16, val: u16) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u16(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u16(addr, val),
        }
    }
    pub fn write_u32(&self, addr: *mut u32, val: u32) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u32(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u32(addr, val),
        }
    }
    pub fn write_u64(&self, addr: *mut u64, val: u64) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u64(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u64(addr, val),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::OnceLock;

    fn mmap_tm_region() {
        static MMAP: OnceLock<()> = OnceLock::new();
        MMAP.get_or_init(|| {
            unsafe {
                let addr = 0x7f00_0000_0000 as *mut libc::c_void;
                let result = libc::mmap(
                    addr,
                    256 * 1024 * 1024,
                    libc::PROT_READ | libc::PROT_WRITE,
                    libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED,
                    -1,
                    0,
                );
                if result == libc::MAP_FAILED {
                    panic!("mmap failed: {}", std::io::Error::last_os_error());
                }
            }
        });
    }

    fn run_simple_tx(b: Backend) {
        mmap_tm_region();
        b.init();
        b.sim_set_thread_id(0);
        b.init_thread();

        let addr = 0x7f00_0000_8000 as *mut u64;

        // Write outside TX (raw write)
        unsafe { addr.write(0); }

        b.begin();
        let v = b.read_u64(addr);
        assert_eq!(v, 0);
        b.write_u64(addr, 42);
        assert!(b.commit(), "commit should succeed");
        b.sim_clear_thread_id();

        // Verify value persisted
        b.sim_set_thread_id(0);
        b.begin();
        let v = b.read_u64(addr);
        assert_eq!(v, 42, "value should persist after commit");
        b.commit();
        b.sim_clear_thread_id();
    }

    // ── Backend parsing ────────────────────────────────────

    #[test]
    fn test_backend_from_name_norec() {
        assert_eq!(Backend::from_name("norec"), Some(Backend::Norec));
        assert_eq!(Backend::from_name("no-rec"), Some(Backend::Norec));
    }

    #[test]
    fn test_backend_from_name_tl2() {
        assert_eq!(Backend::from_name("tl2"), Some(Backend::Tl2));
        assert_eq!(Backend::from_name("TL2"), Some(Backend::Tl2));
    }

    #[test]
    fn test_backend_from_name_invalid() {
        assert_eq!(Backend::from_name("tinystm"), None);
        assert_eq!(Backend::from_name(""), None);
    }

    #[test]
    fn test_backend_name() {
        assert_eq!(Backend::Norec.name(), "norec");
        assert_eq!(Backend::Tl2.name(), "tl2");
    }

    // ── NOrec backend simulation ──────────────────────────

    #[test]
    fn test_norec_simple_tx() {
        run_simple_tx(Backend::Norec);
    }

    #[test]
    fn test_norec_commit_without_tx() {
        mmap_tm_region();
        Backend::Norec.init();
        Backend::Norec.sim_set_thread_id(0);
        Backend::Norec.init_thread();
        assert!(Backend::Norec.commit());
        Backend::Norec.sim_clear_thread_id();
    }

    #[test]
    fn test_norec_abort_without_tx() {
        mmap_tm_region();
        Backend::Norec.init();
        Backend::Norec.sim_set_thread_id(0);
        Backend::Norec.init_thread();
        Backend::Norec.abort();
        Backend::Norec.sim_clear_thread_id();
    }

    #[test]
    fn test_norec_read_write_u8() {
        mmap_tm_region();
        let b = Backend::Norec;
        b.init();
        b.sim_set_thread_id(0);
        b.init_thread();
        let addr = 0x7f00_0000_9000 as *mut u8;
        unsafe { addr.write(0); }
        b.begin();
        b.write_u8(addr, 255);
        let v = b.read_u8(addr);
        assert_eq!(v, 255, "write-then-read within same tx should see value");
        assert!(b.commit());
        b.sim_clear_thread_id();
    }

    // ── TL2 backend simulation ─────────────────────────────

    #[test]
    fn test_tl2_simple_tx() {
        run_simple_tx(Backend::Tl2);
    }

    #[test]
    fn test_tl2_commit_without_tx() {
        mmap_tm_region();
        Backend::Tl2.init();
        Backend::Tl2.sim_set_thread_id(0);
        Backend::Tl2.init_thread();
        assert!(Backend::Tl2.commit());
        Backend::Tl2.sim_clear_thread_id();
    }

    #[test]
    fn test_tl2_abort_without_tx() {
        mmap_tm_region();
        Backend::Tl2.init();
        Backend::Tl2.sim_set_thread_id(0);
        Backend::Tl2.init_thread();
        Backend::Tl2.abort();
        Backend::Tl2.sim_clear_thread_id();
    }

    #[test]
    fn test_tl2_read_write_u16() {
        mmap_tm_region();
        let b = Backend::Tl2;
        b.init();
        b.sim_set_thread_id(0);
        b.init_thread();
        let addr = 0x7f00_0000_A000 as *mut u16;
        unsafe { addr.write(0); }
        b.begin();
        b.write_u16(addr, 0xCAFE);
        let v = b.read_u16(addr);
        assert_eq!(v, 0xCAFE);
        assert!(b.commit());
        b.sim_clear_thread_id();
    }

    // ── Cross-backend consistency ─────────────────────────

    #[test]
    fn test_norec_and_tl2_produce_identical_commits() {
        mmap_tm_region();
        for b in [Backend::Norec, Backend::Tl2] {
            b.init();
            b.sim_set_thread_id(0);
            b.init_thread();
            let addr = 0x7f00_0000_B000 as *mut u64;
            unsafe { addr.write(0); }
            b.begin();
            b.write_u64(addr, 100);
            assert!(b.commit(), "{} commit should succeed", b.name());
            b.sim_clear_thread_id();
        }
    }

    // ── Simulation thread isolation ───────────────────────

    #[test]
    fn test_thread_isolation() {
        mmap_tm_region();
        for b in [Backend::Norec, Backend::Tl2] {
            b.init();
            let addr = 0x7f00_0000_C000 as *mut u64;
            unsafe { addr.write(0); }

            // Thread 0
            b.sim_set_thread_id(0);
            b.init_thread();
            b.begin();
            b.write_u64(addr, 10);
            assert!(b.commit());
            b.sim_clear_thread_id();

            // Thread 1 (starts after T0 committed, so sees 10)
            b.sim_set_thread_id(1);
            b.init_thread();
            b.begin();
            let v = b.read_u64(addr);
            assert_eq!(v, 10, "{}: thread 1 should see thread 0's committed value", b.name());
            b.sim_clear_thread_id();
        }
    }

    // ── Sim reset ─────────────────────────────────────────

    #[test]
    fn test_sim_reset_clears_state() {
        mmap_tm_region();
        for b in [Backend::Norec, Backend::Tl2] {
            b.init();
            b.sim_set_thread_id(0);
            b.init_thread();
            b.begin();
            assert!(b.commit(), "{}: first commit", b.name());
            b.sim_clear_thread_id();
            // After sim_reset, thread 0's state should be gone
            b.sim_reset();
            b.sim_set_thread_id(0);
            // After reset, need to re-init
            b.init_thread();
            b.begin();
            assert!(b.commit(), "{}: commit after reset", b.name());
            b.sim_clear_thread_id();
        }
    }
}
