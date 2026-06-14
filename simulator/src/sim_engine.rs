// ── Simulation engine ────────────────────────────────────
// Drives the actual NOrec (and eventually other) Rust backend
// via the `simulation` feature.  All simulated threads run on
// the same OS thread; the sim_thread_id tells the backend which
// simulated thread's state to access.

use crate::event::{Event, EventKind};

/// Statistics collected during replay.
#[derive(Debug, Clone, Default)]
pub struct ReplayStats {
    pub total_events: u64,
    pub commits: u64,
    pub aborts: u64,
    pub aborts_by_reason: Vec<(String, u64)>,
    pub reads_outside_tx: u64,
    pub writes_outside_tx: u64,
}

impl ReplayStats {
    pub fn abort_rate(&self) -> f64 {
        let total = self.commits + self.aborts;
        if total == 0 {
            0.0
        } else {
            100.0 * self.aborts as f64 / total as f64
        }
    }
}

/// The simulation engine.
pub struct SimEngine {
    pub stats: ReplayStats,
    in_tx: std::collections::HashMap<u64, bool>,
    seen_threads: std::collections::HashSet<u64>,
}

impl SimEngine {
    pub fn new() -> Self {
        SimEngine {
            stats: ReplayStats::default(),
            in_tx: std::collections::HashMap::new(),
            seen_threads: std::collections::HashSet::new(),
        }
    }

    /// Initialize the backend and back the TM address space.
    pub fn init(&mut self) {
        // Map a 256 MB region at the trace's base address (0x7F0000000000)
        // so the NOrec backend can dereference trace addresses safely.
        unsafe {
            let addr = 0x7f00_0000_0000 as *mut libc::c_void;
            let size: usize = 256 * 1024 * 1024; // 256 MB
            let result = libc::mmap(
                addr,
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_FIXED_NOREPLACE,
                -1,
                0,
            );
            if result == libc::MAP_FAILED {
                panic!("mmap of TM address space at 0x7f0000000000 failed: {}", std::io::Error::last_os_error());
            }
        }

        runtime_norec::tm_init();
        runtime_norec::sim::set_thread_id(0);
        runtime_norec::tm_init_thread();
        runtime_norec::sim::clear_thread_id();
        self.seen_threads.insert(0);
    }

    /// Ensure a simulated thread has been initialized in the backend.
    fn ensure_thread(&mut self, tid: u64) {
        if self.seen_threads.insert(tid) {
            runtime_norec::sim::set_thread_id(tid);
            runtime_norec::tm_init_thread();
            runtime_norec::sim::clear_thread_id();
        }
    }

    /// Process a single event.
    pub fn process_event(&mut self, event: &Event) {
        self.stats.total_events += 1;
        let tid = event.thread_id as u64;

        self.ensure_thread(tid);
        runtime_norec::sim::set_thread_id(tid);
        let result = self.dispatch_event(event);
        runtime_norec::sim::clear_thread_id();

        if let Err(why) = result {
            eprintln!(
                "  [ts={}] tid={}: {}",
                event.timestamp, event.thread_id, why
            );
        }
    }

    fn dispatch_event(&mut self, event: &Event) -> Result<(), String> {
        let tid = event.thread_id as u64;

        match &event.kind {
            EventKind::TxBegin => {
                runtime_norec::tm_begin();
                self.in_tx.insert(tid, true);
                Ok(())
            }
            EventKind::TxEnd => {
                let ok = runtime_norec::tm_commit();
                self.in_tx.insert(tid, false);
                if ok {
                    self.stats.commits += 1;
                    Ok(())
                } else {
                    self.stats.aborts += 1;
                    runtime_norec::tm_abort();
                    Err("commit failed".into())
                }
            }
            EventKind::Read { addr, width } => {
                if !self.in_tx.get(&tid).copied().unwrap_or(false) {
                    self.stats.reads_outside_tx += 1;
                    return Err("read outside transaction".into());
                }
                let _val: u64 = match width {
                    1 => runtime_norec::tm_read_u8(*addr as *mut u8) as u64,
                    2 => runtime_norec::tm_read_u16(*addr as *mut u16) as u64,
                    4 => runtime_norec::tm_read_u32(*addr as *mut u32) as u64,
                    8 => runtime_norec::tm_read_u64(*addr as *mut u64),
                    w => return Err(format!("unsupported read width {}", w)),
                };
                Ok(())
            }
            EventKind::Write { addr, width, val } => {
                if !self.in_tx.get(&tid).copied().unwrap_or(false) {
                    self.stats.writes_outside_tx += 1;
                    return Err("write outside transaction".into());
                }
                match width {
                    1 => runtime_norec::tm_write_u8(*addr as *mut u8, *val as u8),
                    2 => runtime_norec::tm_write_u16(*addr as *mut u16, *val as u16),
                    4 => runtime_norec::tm_write_u32(*addr as *mut u32, *val as u32),
                    8 => runtime_norec::tm_write_u64(*addr as *mut u64, *val),
                    w => return Err(format!("unsupported write width {}", w)),
                }
                Ok(())
            }
            EventKind::ThreadSpawn(child_id) => {
                self.ensure_thread(*child_id as u64);
                Ok(())
            }
            EventKind::ThreadJoin(_)
            | EventKind::Alloc { .. }
            | EventKind::Free { .. }
            | EventKind::Checkpoint
            | EventKind::Assert { .. }
            | EventKind::Log { .. } => {
                // These are tracked by the verifier, not the backend.
                Ok(())
            }
        }
    }

    /// Reset state between scenarios.
    pub fn reset(&mut self) {
        runtime_norec::sim::reset();
        self.in_tx.clear();
        self.seen_threads.clear();
    }
}
