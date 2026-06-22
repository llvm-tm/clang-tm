// ── Backend abstraction ─────────────────────────────────
// Runtime dispatching between TM backends in simulation mode.
// Each registered backend exports the same simulation API.

use std::collections::HashMap;

/// Available TM backends.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    Norec,
    Tl2,
    Tinystm,
    Romulus,
    Swisstm,
    TsxSim,
}

impl Backend {
    pub fn from_name(name: &str) -> Option<Self> {
        match name {
            "norec" | "no-rec" => Some(Backend::Norec),
            "tl2" | "TL2" => Some(Backend::Tl2),
            "tinystm" | "tiny-stm" => Some(Backend::Tinystm),
            "romulus" => Some(Backend::Romulus),
            "swisstm" => Some(Backend::Swisstm),
            "tsx-sim" | "tsx_sim" => Some(Backend::TsxSim),
            _ => None,
        }
    }

    pub fn name(&self) -> &'static str {
        match self {
            Backend::Norec => "norec",
            Backend::Tl2 => "tl2",
            Backend::Tinystm => "tinystm",
            Backend::Romulus => "romulus",
            Backend::Swisstm => "swisstm",
            Backend::TsxSim => "tsx-sim",
        }
    }

    pub fn init(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_init(),
            Backend::Tl2 => runtime_tl2::tm_init(),
            Backend::Tinystm => runtime_tinystm::tm_init(),
            Backend::Romulus => runtime_romulus::tm_init(),
            Backend::Swisstm => runtime_swisstm::tm_init(),
            Backend::TsxSim => runtime_tsx_sim::tm_init(),
        }
    }

    pub fn init_thread(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_init_thread(),
            Backend::Tl2 => runtime_tl2::tm_init_thread(),
            Backend::Tinystm => runtime_tinystm::tm_init_thread(),
            Backend::Romulus => runtime_romulus::tm_init_thread(),
            Backend::Swisstm => runtime_swisstm::tm_init_thread(),
            Backend::TsxSim => runtime_tsx_sim::tm_init_thread(),
        }
    }

    pub fn begin(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_begin(),
            Backend::Tl2 => runtime_tl2::tm_begin(),
            Backend::Tinystm => runtime_tinystm::tm_begin(),
            Backend::Romulus => runtime_romulus::tm_begin(),
            Backend::Swisstm => runtime_swisstm::tm_begin(),
            Backend::TsxSim => runtime_tsx_sim::tm_begin(),
        }
    }

    /// Attempt to begin a transaction.  Returns `true` if TSX was entered
    /// successfully, `false` if the xbegin failed (LOCK_BUSY, conflict,
    /// capacity, etc.).  Default (non-TSX backends) always returns true.
    ///
    /// The SimEngine uses this to drive the retry loop externally, allowing
    /// other threads' events to interleave between retry attempts.  Only
    /// the `tsx-sim` backend has a meaningful implementation.
    pub fn try_begin(&self) -> bool {
        match self {
            Backend::TsxSim => runtime_tsx_sim::sim::try_begin(),
            _ => true,
        }
    }

    /// Force SGL fallback for the current thread's transaction.  Called by
    /// the SimEngine after `max_retries` consecutive `try_begin()` failures.
    /// No-op for non-TSX backends.
    pub fn force_sgl(&self) {
        match self {
            Backend::TsxSim => runtime_tsx_sim::sim::force_sgl(),
            _ => {}
        }
    }

    pub fn commit(&self) -> bool {
        match self {
            Backend::Norec => runtime_norec::tm_commit(),
            Backend::Tl2 => runtime_tl2::tm_commit(),
            Backend::Tinystm => runtime_tinystm::tm_commit(),
            Backend::Romulus => runtime_romulus::tm_commit(),
            Backend::Swisstm => runtime_swisstm::tm_commit(),
            Backend::TsxSim => runtime_tsx_sim::tm_commit(),
        }
    }

    pub fn abort(&self) {
        match self {
            Backend::Norec => runtime_norec::tm_abort(),
            Backend::Tl2 => runtime_tl2::tm_abort(),
            Backend::Tinystm => runtime_tinystm::tm_abort(),
            Backend::Romulus => runtime_romulus::tm_abort(),
            Backend::Swisstm => runtime_swisstm::tm_abort(),
            Backend::TsxSim => runtime_tsx_sim::tm_abort(),
        }
    }

    pub fn sim_set_thread_id(&self, id: u64) {
        match self {
            Backend::Norec => runtime_norec::sim::set_thread_id(id),
            Backend::Tl2 => runtime_tl2::sim::set_thread_id(id),
            Backend::Tinystm => runtime_tinystm::sim::set_thread_id(id),
            Backend::Romulus => runtime_romulus::sim::set_thread_id(id),
            Backend::Swisstm => runtime_swisstm::sim::set_thread_id(id),
            Backend::TsxSim => runtime_tsx_sim::sim::set_thread_id(id),
        }
    }

    pub fn sim_clear_thread_id(&self) {
        match self {
            Backend::Norec => runtime_norec::sim::clear_thread_id(),
            Backend::Tl2 => runtime_tl2::sim::clear_thread_id(),
            Backend::Tinystm => runtime_tinystm::sim::clear_thread_id(),
            Backend::Romulus => runtime_romulus::sim::clear_thread_id(),
            Backend::Swisstm => runtime_swisstm::sim::clear_thread_id(),
            Backend::TsxSim => runtime_tsx_sim::sim::clear_thread_id(),
        }
    }

    pub fn sim_reset(&self) {
        match self {
            Backend::Norec => runtime_norec::sim::reset(),
            Backend::Tl2 => runtime_tl2::sim::reset(),
            Backend::Tinystm => runtime_tinystm::sim::reset(),
            Backend::Romulus => runtime_romulus::sim::reset(),
            Backend::Swisstm => runtime_swisstm::sim::reset(),
            Backend::TsxSim => runtime_tsx_sim::sim::reset(),
        }
    }

    pub fn read_u8(&self, addr: *mut u8) -> u8 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u8(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u8(addr),
            Backend::Tinystm => runtime_tinystm::tm_read_u8(addr),
            Backend::Romulus => runtime_romulus::tm_read_u8(addr),
            Backend::Swisstm => runtime_swisstm::tm_read_u8(addr),
            Backend::TsxSim => runtime_tsx_sim::tm_read_u8(addr),
        }
    }
    pub fn read_u16(&self, addr: *mut u16) -> u16 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u16(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u16(addr),
            Backend::Tinystm => runtime_tinystm::tm_read_u16(addr),
            Backend::Romulus => runtime_romulus::tm_read_u16(addr),
            Backend::Swisstm => runtime_swisstm::tm_read_u16(addr),
            Backend::TsxSim => runtime_tsx_sim::tm_read_u16(addr),
        }
    }
    pub fn read_u32(&self, addr: *mut u32) -> u32 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u32(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u32(addr),
            Backend::Tinystm => runtime_tinystm::tm_read_u32(addr),
            Backend::Romulus => runtime_romulus::tm_read_u32(addr),
            Backend::Swisstm => runtime_swisstm::tm_read_u32(addr),
            Backend::TsxSim => runtime_tsx_sim::tm_read_u32(addr),
        }
    }
    pub fn read_u64(&self, addr: *mut u64) -> u64 {
        match self {
            Backend::Norec => runtime_norec::tm_read_u64(addr),
            Backend::Tl2 => runtime_tl2::tm_read_u64(addr),
            Backend::Tinystm => runtime_tinystm::tm_read_u64(addr),
            Backend::Romulus => runtime_romulus::tm_read_u64(addr),
            Backend::Swisstm => runtime_swisstm::tm_read_u64(addr),
            Backend::TsxSim => runtime_tsx_sim::tm_read_u64(addr),
        }
    }

    pub fn write_u8(&self, addr: *mut u8, val: u8) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u8(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u8(addr, val),
            Backend::Tinystm => runtime_tinystm::tm_write_u8(addr, val),
            Backend::Romulus => runtime_romulus::tm_write_u8(addr, val),
            Backend::Swisstm => runtime_swisstm::tm_write_u8(addr, val),
            Backend::TsxSim => runtime_tsx_sim::tm_write_u8(addr, val),
        }
    }
    pub fn write_u16(&self, addr: *mut u16, val: u16) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u16(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u16(addr, val),
            Backend::Tinystm => runtime_tinystm::tm_write_u16(addr, val),
            Backend::Romulus => runtime_romulus::tm_write_u16(addr, val),
            Backend::Swisstm => runtime_swisstm::tm_write_u16(addr, val),
            Backend::TsxSim => runtime_tsx_sim::tm_write_u16(addr, val),
        }
    }
    pub fn write_u32(&self, addr: *mut u32, val: u32) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u32(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u32(addr, val),
            Backend::Tinystm => runtime_tinystm::tm_write_u32(addr, val),
            Backend::Romulus => runtime_romulus::tm_write_u32(addr, val),
            Backend::Swisstm => runtime_swisstm::tm_write_u32(addr, val),
            Backend::TsxSim => runtime_tsx_sim::tm_write_u32(addr, val),
        }
    }
    pub fn write_u64(&self, addr: *mut u64, val: u64) {
        match self {
            Backend::Norec => runtime_norec::tm_write_u64(addr, val),
            Backend::Tl2 => runtime_tl2::tm_write_u64(addr, val),
            Backend::Tinystm => runtime_tinystm::tm_write_u64(addr, val),
            Backend::Romulus => runtime_romulus::tm_write_u64(addr, val),
            Backend::Swisstm => runtime_swisstm::tm_write_u64(addr, val),
            Backend::TsxSim => runtime_tsx_sim::tm_write_u64(addr, val),
        }
    }

    /// Serialize the backend's per-thread state to a byte blob.
    pub fn sim_snapshot_bytes(&self) -> Vec<u8> {
        match self {
            Backend::Norec => {
                let states = runtime_norec::sim::snapshot_states();
                bincode::serialize(&states).unwrap_or_default()
            }
            Backend::Tl2 => {
                let states = runtime_tl2::sim::snapshot_states();
                bincode::serialize(&states).unwrap_or_default()
            }
            Backend::Tinystm => {
                let states = runtime_tinystm::sim::snapshot_states();
                bincode::serialize(&states).unwrap_or_default()
            }
            Backend::Romulus => {
                let states = runtime_romulus::sim::snapshot_states();
                bincode::serialize(&states).unwrap_or_default()
            }
            Backend::Swisstm => {
                let states = runtime_swisstm::sim::snapshot_states();
                bincode::serialize(&states).unwrap_or_default()
            }
            Backend::TsxSim => {
                let states = runtime_tsx_sim::sim::snapshot_states();
                bincode::serialize(&states).unwrap_or_default()
            }
        }
    }

    /// Deserialize and restore the backend's per-thread state from a byte blob.
    pub fn sim_restore_bytes(&self, data: &[u8]) -> Result<(), String> {
        match self {
            Backend::Norec => {
                let states: HashMap<u64, Option<Box<runtime_norec::TxState>>> =
                    bincode::deserialize(data)
                        .map_err(|e| format!("deserialize norec state: {}", e))?;
                runtime_norec::sim::restore_states(states);
                Ok(())
            }
            Backend::Tl2 => {
                let states: HashMap<u64, Option<Box<runtime_tl2::TxState>>> =
                    bincode::deserialize(data)
                        .map_err(|e| format!("deserialize tl2 state: {}", e))?;
                runtime_tl2::sim::restore_states(states);
                Ok(())
            }
            Backend::Tinystm => {
                let states: HashMap<u64, Option<Box<runtime_tinystm::TxState>>> =
                    bincode::deserialize(data)
                        .map_err(|e| format!("deserialize tinystm state: {}", e))?;
                runtime_tinystm::sim::restore_states(states);
                Ok(())
            }
            Backend::Romulus => {
                let states: HashMap<u64, Option<Box<runtime_romulus::TxState>>> =
                    bincode::deserialize(data)
                        .map_err(|e| format!("deserialize romulus state: {}", e))?;
                runtime_romulus::sim::restore_states(states);
                Ok(())
            }
            Backend::Swisstm => {
                let states: HashMap<u64, Option<Box<runtime_swisstm::TxState>>> =
                    bincode::deserialize(data)
                        .map_err(|e| format!("deserialize swisstm state: {}", e))?;
                runtime_swisstm::sim::restore_states(states);
                Ok(())
            }
            Backend::TsxSim => {
                let states: HashMap<u64, Option<Box<runtime_tsx_sim::TsxThreadState>>> =
                    bincode::deserialize(data)
                        .map_err(|e| format!("deserialize tsx_sim state: {}", e))?;
                runtime_tsx_sim::sim::restore_states(states);
                Ok(())
            }
        }
    }

    pub fn is_norec(&self) -> bool { matches!(self, Backend::Norec) }
    pub fn is_tl2(&self) -> bool { matches!(self, Backend::Tl2) }
    pub fn is_tinystm(&self) -> bool { matches!(self, Backend::Tinystm) }
    pub fn is_tsx_sim(&self) -> bool { matches!(self, Backend::TsxSim) }

    /// Take snapshot of internal sync counters and reset them.
    pub fn take_stats(&self) -> runtime_core::SyncCounters {
        match self {
            Backend::Norec => runtime_norec::sim::take_stats(),
            Backend::Tl2 => runtime_tl2::sim::take_stats(),
            Backend::Tinystm => runtime_tinystm::sim::take_stats(),
            Backend::Romulus => runtime_romulus::sim::take_stats(),
            Backend::Swisstm => runtime_swisstm::sim::take_stats(),
            Backend::TsxSim => runtime_tsx_sim::sim::take_stats(),
        }
    }

    /// Print internal sync counters to stderr.
    pub fn print_stats(&self, s: &runtime_core::SyncCounters) {
        match self {
            Backend::Norec => runtime_norec::sim::print_stats(s),
            Backend::Tl2 => runtime_tl2::sim::print_stats(s),
            Backend::Tinystm => runtime_tinystm::sim::print_stats(s),
            Backend::Romulus => runtime_romulus::sim::print_stats(s),
            Backend::Swisstm => runtime_swisstm::sim::print_stats(s),
            Backend::TsxSim => runtime_tsx_sim::sim::print_stats(s),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::OnceLock;
    use std::sync::atomic::AtomicU64;

    fn alloc_tid() -> u64 {
        static NEXT: AtomicU64 = AtomicU64::new(1000);
        NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    }

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
        let tid = alloc_tid();
        mmap_tm_region();
        b.init();
        b.sim_set_thread_id(tid);
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
        b.sim_set_thread_id(tid);
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
    fn test_backend_from_name_tinystm() {
        assert_eq!(Backend::from_name("tinystm"), Some(Backend::Tinystm));
    }

    #[test]
    fn test_backend_from_name_romulus() {
        assert_eq!(Backend::from_name("romulus"), Some(Backend::Romulus));
    }

    #[test]
    fn test_backend_from_name_swisstm() {
        assert_eq!(Backend::from_name("swisstm"), Some(Backend::Swisstm));
        assert_eq!(Backend::from_name(""), None);
    }

    #[test]
    fn test_backend_name() {
        assert_eq!(Backend::Norec.name(), "norec");
        assert_eq!(Backend::Tl2.name(), "tl2");
        assert_eq!(Backend::Romulus.name(), "romulus");
        assert_eq!(Backend::Swisstm.name(), "swisstm");
    }

    // ── NOrec backend simulation ──────────────────────────

    #[test]
    fn test_norec_simple_tx() {
        run_simple_tx(Backend::Norec);
    }

    #[test]
    fn test_norec_commit_without_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Norec.init();
        Backend::Norec.sim_set_thread_id(tid);
        Backend::Norec.init_thread();
        assert!(Backend::Norec.commit());
        Backend::Norec.sim_clear_thread_id();
    }

    #[test]
    fn test_norec_abort_without_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Norec.init();
        Backend::Norec.sim_set_thread_id(tid);
        Backend::Norec.init_thread();
        Backend::Norec.abort();
        Backend::Norec.sim_clear_thread_id();
    }

    #[test]
    fn test_norec_read_write_u8() {
        let tid = alloc_tid();
        mmap_tm_region();
        let b = Backend::Norec;
        b.init();
        b.sim_set_thread_id(tid);
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
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Tl2.init();
        Backend::Tl2.sim_set_thread_id(tid);
        Backend::Tl2.init_thread();
        assert!(Backend::Tl2.commit());
        Backend::Tl2.sim_clear_thread_id();
    }

    #[test]
    fn test_tl2_abort_without_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Tl2.init();
        Backend::Tl2.sim_set_thread_id(tid);
        Backend::Tl2.init_thread();
        Backend::Tl2.abort();
        Backend::Tl2.sim_clear_thread_id();
    }

    #[test]
    fn test_tl2_read_write_u16() {
        let tid = alloc_tid();
        mmap_tm_region();
        let b = Backend::Tl2;
        b.init();
        b.sim_set_thread_id(tid);
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

    // ── TinySTM backend simulation ────────────────────────

    #[test]
    fn test_tinystm_simple_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        let b = Backend::Tinystm;
        b.init();
        b.sim_set_thread_id(tid);
        b.init_thread();
        let addr = 0x7f00_0000_D000 as *mut u64;
        unsafe { addr.write(0); }
        b.begin();
        let v = b.read_u64(addr);
        assert_eq!(v, 0);
        b.write_u64(addr, 77);
        assert!(b.commit(), "tinystm commit should succeed");
        b.sim_clear_thread_id();

        // Read back
        b.sim_set_thread_id(tid);
        b.begin();
        let v = b.read_u64(addr);
        assert_eq!(v, 77, "value should persist after commit");
        b.commit();
        b.sim_clear_thread_id();
    }

    // ── ROMULUS backend simulation ────────────────────────

    #[test]
    fn test_romulus_simple_tx() {
        run_simple_tx(Backend::Romulus);
    }

    #[test]
    fn test_romulus_commit_without_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Romulus.init();
        Backend::Romulus.sim_set_thread_id(tid);
        Backend::Romulus.init_thread();
        assert!(Backend::Romulus.commit());
        Backend::Romulus.sim_clear_thread_id();
    }

    #[test]
    fn test_romulus_abort_without_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Romulus.init();
        Backend::Romulus.sim_set_thread_id(tid);
        Backend::Romulus.init_thread();
        Backend::Romulus.abort();
        Backend::Romulus.sim_clear_thread_id();
    }

    #[test]
    fn test_romulus_read_write_u32() {
        let tid = alloc_tid();
        mmap_tm_region();
        let b = Backend::Romulus;
        b.init();
        b.sim_set_thread_id(tid);
        b.init_thread();
        let addr = 0x7f00_0000_E000 as *mut u32;
        unsafe { addr.write(0); }
        b.begin();
        b.write_u32(addr, 0xDEAD);
        let v = b.read_u32(addr);
        assert_eq!(v, 0xDEAD);
        assert!(b.commit());
        b.sim_clear_thread_id();
    }

    // ── SwissTM backend simulation ────────────────────────

    #[test]
    fn test_swisstm_simple_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        let b = Backend::Swisstm;
        b.init();
        b.sim_set_thread_id(tid);
        b.init_thread();
        let addr = 0x7f00_0000_F000 as *mut u64;
        unsafe { addr.write(0); }
        b.begin();
        let v = b.read_u64(addr);
        assert_eq!(v, 0);
        b.write_u64(addr, 99);
        assert!(b.commit(), "swisstm commit should succeed");
        b.sim_clear_thread_id();

        // Read back
        b.sim_set_thread_id(tid);
        b.begin();
        let v = b.read_u64(addr);
        assert_eq!(v, 99, "value should persist after commit");
        b.commit();
        b.sim_clear_thread_id();
    }

    #[test]
    fn test_swisstm_commit_without_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Swisstm.init();
        Backend::Swisstm.sim_set_thread_id(tid);
        Backend::Swisstm.init_thread();
        assert!(Backend::Swisstm.commit());
        Backend::Swisstm.sim_clear_thread_id();
    }

    #[test]
    fn test_swisstm_abort_without_tx() {
        let tid = alloc_tid();
        mmap_tm_region();
        Backend::Swisstm.init();
        Backend::Swisstm.sim_set_thread_id(tid);
        Backend::Swisstm.init_thread();
        Backend::Swisstm.abort();
        Backend::Swisstm.sim_clear_thread_id();
    }

    #[test]
    fn test_swisstm_read_write_u8() {
        let tid = alloc_tid();
        mmap_tm_region();
        let b = Backend::Swisstm;
        b.init();
        b.sim_set_thread_id(tid);
        b.init_thread();
        let addr = 0x7f00_0000_F100 as *mut u8;
        unsafe { addr.write(0); }
        b.begin();
        b.write_u8(addr, 0xAB);
        let v = b.read_u8(addr);
        assert_eq!(v, 0xAB);
        assert!(b.commit());
        b.sim_clear_thread_id();
    }

    // ── Cross-backend consistency ─────────────────────────

    #[test]
    fn test_all_backends_produce_identical_commits() {
        mmap_tm_region();
        for b in [Backend::Norec, Backend::Tl2, Backend::Tinystm, Backend::Romulus, Backend::Swisstm] {
            let tid = alloc_tid();
            b.init();
            b.sim_set_thread_id(tid);
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
        for b in [Backend::Norec, Backend::Tl2, Backend::Tinystm, Backend::Romulus, Backend::Swisstm] {
            let tid0 = alloc_tid();
            let tid1 = alloc_tid();
            b.init();
            let addr = 0x7f00_0000_C000 as *mut u64;
            unsafe { addr.write(0); }

            // Thread 0
            b.sim_set_thread_id(tid0);
            b.init_thread();
            b.begin();
            b.write_u64(addr, 10);
            assert!(b.commit());
            b.sim_clear_thread_id();

            // Thread 1 (starts after T0 committed, so sees 10)
            b.sim_set_thread_id(tid1);
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
        for b in [Backend::Norec, Backend::Tl2, Backend::Tinystm, Backend::Romulus, Backend::Swisstm] {
            let tid = alloc_tid();
            b.init();
            b.sim_set_thread_id(tid);
            b.init_thread();
            b.begin();
            assert!(b.commit(), "{}: first commit", b.name());
            // After sim_reset, thread 0's state should be gone
            b.sim_reset();
            b.sim_clear_thread_id();
            b.sim_set_thread_id(tid);
            // After reset, need to re-init
            b.init_thread();
            b.begin();
            assert!(b.commit(), "{}: commit after reset", b.name());
            b.sim_clear_thread_id();
        }
    }

    // ── Checkpoint/restore ────────────────────────────────

    #[test]
    fn test_romulus_checkpoint_restore() {
        mmap_tm_region();
        let b = Backend::Romulus;
        let tid = alloc_tid();
        b.init();
        b.sim_set_thread_id(tid);
        b.init_thread();
        let addr = 0x7f00_0000_2800 as *mut u64;
        unsafe { addr.write(0); }

        b.begin();
        b.write_u64(addr, 42);
        // Snapshot mid-transaction
        let bytes = b.sim_snapshot_bytes();
        assert!(!bytes.is_empty(), "snapshot should have data");

        // Restore mid-transaction (should lose the write, but tx still valid)
        b.sim_restore_bytes(&bytes).expect("restore should succeed");
        assert!(b.commit());
        b.sim_clear_thread_id();
    }
}
