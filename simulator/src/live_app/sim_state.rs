use std::collections::HashMap;

/// Per-thread transaction state.
#[derive(Debug, Clone)]
pub struct PerThreadState {
    pub in_tx: bool,
    pub aborted: bool,
    pub read_set: Vec<ReadEntry>,
    pub write_set: Vec<WriteEntry>,
    pub jmpbuf: u64,
}

impl PerThreadState {
    fn new() -> Self {
        PerThreadState {
            in_tx: false,
            aborted: false,
            read_set: Vec::new(),
            write_set: Vec::new(),
            jmpbuf: 0,
        }
    }

    fn reset(&mut self) {
        self.in_tx = false;
        self.aborted = false;
        self.read_set.clear();
        self.write_set.clear();
    }
}

#[derive(Debug, Clone, Copy)]
pub struct ReadEntry {
    pub addr: u64,
    pub width: u8,
    pub value: u64,
}

#[derive(Debug, Clone, Copy)]
pub struct WriteEntry {
    pub addr: u64,
    pub width: u8,
    pub value: u64,
}

/// Statistics collected during the live simulation.
#[derive(Debug, Clone, Default)]
pub struct LiveStats {
    pub commits: u64,
    pub aborts: u64,
    pub reads: u64,
    pub writes: u64,
    pub total_cycles: u64,
}

/// The core simulation state for live-app mode.
pub struct LiveSimState {
    shadow: HashMap<u64, u64>,
    threads: Vec<PerThreadState>,
    clock: u64,
    pub stats: LiveStats,
}

impl LiveSimState {
    pub fn new(max_threads: u32) -> Self {
        LiveSimState {
            shadow: HashMap::new(),
            threads: (0..max_threads).map(|_| PerThreadState::new()).collect(),
            clock: 0,
            stats: LiveStats::default(),
        }
    }

    pub fn thread_state(&mut self, tid: u32) -> &mut PerThreadState {
        let idx = tid as usize;
        if idx >= self.threads.len() {
            self.threads.resize_with(idx + 1, PerThreadState::new);
        }
        &mut self.threads[idx]
    }

    // ── TM operations ────────────────────────────────────────

    pub fn tm_begin(&mut self, tid: u32) {
        let t = self.thread_state(tid);
        t.read_set.clear();
        t.write_set.clear();
        t.in_tx = true;
        t.aborted = false;
        self.clock += 60;
    }

    pub fn tm_read(&mut self, addr: u64, width: u8, tid: u32) -> u64 {
        // Do shadow lookup first — no thread_state borrow needed.
        let shadow_val = self.shadow.get(&addr).copied().unwrap_or(0);

        let t = self.thread_state(tid);
        if !t.in_tx || t.aborted {
            return shadow_val;
        }

        // Own writes visible.
        for w in t.write_set.iter() {
            if w.addr == addr {
                return w.value;
            }
        }

        t.read_set.push(ReadEntry { addr, width, value: shadow_val });
        self.stats.reads += 1;
        self.clock += 5;
        shadow_val
    }

    pub fn tm_write(&mut self, addr: u64, width: u8, val: u64, tid: u32) {
        let t = self.thread_state(tid);
        if !t.in_tx || t.aborted {
            return;
        }

        if let Some(w) = t.write_set.iter_mut().find(|w| w.addr == addr) {
            w.value = val;
        } else {
            t.write_set.push(WriteEntry { addr, width, value: val });
        }
        self.stats.writes += 1;
        self.clock += 6;
    }

    pub fn tm_end(&mut self, tid: u32) -> bool {
        let t = self.thread_state(tid);
        if !t.in_tx || t.aborted {
            t.reset();
            return false;
        }

        // Take ownership of write-set entries so we can drop the borrow.
        let ws: Vec<WriteEntry> = t.write_set.drain(..).collect();
        t.reset();
        // t borrow ends here

        // Write to shadow memory (no overlapping borrow).
        for w in &ws {
            self.shadow.insert(w.addr, w.value);
        }
        self.stats.commits += 1;
        self.clock += 25;
        true
    }

    pub fn tm_abort(&mut self, tid: u32) {
        let t = self.thread_state(tid);
        t.reset();
        self.stats.aborts += 1;
        self.clock += 60;
    }

    pub fn tm_malloc(&mut self, size: u64, _tid: u32) -> u64 {
        static mut NEXT_ADDR: u64 = 0x7000_0000_0000;
        let addr = unsafe {
            let a = NEXT_ADDR;
            NEXT_ADDR += size;
            a
        };
        for i in 0..size {
            self.shadow.insert(addr + i, 0);
        }
        self.clock += 20;
        addr
    }

    pub fn tm_free(&mut self, _addr: u64, _tid: u32) {
        self.clock += 10;
    }

    pub fn tm_set_jmpbuf(&mut self, buf: u64, tid: u32) {
        let t = self.thread_state(tid);
        t.jmpbuf = buf;
    }

    pub fn tm_get_env(&mut self, _tid: u32) -> u64 {
        0
    }

    pub fn tm_get_thread_state(&mut self, _tid: u32) -> u64 {
        0
    }

    pub fn report(&self) {
        eprintln!(
            "═══ live sim report ═══\n\
             Commits: {}  Aborts: {}  Reads: {}  Writes: {}\n\
             TM cycles: {}",
            self.stats.commits,
            self.stats.aborts,
            self.stats.reads,
            self.stats.writes,
            self.clock,
        );
    }
}
