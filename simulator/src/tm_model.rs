use serde::{Deserialize, Serialize};
use std::collections::HashMap;

// ── Constants matching TinySTM ─────────────────────────────────────────
pub const LOCK_TABLE_BITS: u64 = 14;
pub const LOCK_TABLE_SIZE: u64 = 1 << LOCK_TABLE_BITS; // 16384
pub const OWNED_BITS: u64 = 2;
pub const INCARNATION_BITS: u64 = 3;
pub const THREAD_BITS: u64 = 13;
pub const LOCK_BITS: u64 = OWNED_BITS + INCARNATION_BITS;
pub const META_BITS: u64 = LOCK_BITS + THREAD_BITS;
pub const VERSION_MASK: u64 = (1u64 << (64 - META_BITS)) - 1;
pub const SPIN_ITERATIONS: u32 = 5000;

/// A lock word matching TinySTM's bit layout.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LockWord(u64);

impl LockWord {
    pub fn new(version: u64) -> Self {
        LockWord(version << META_BITS)
    }

    pub fn is_locked(&self) -> bool {
        self.0 & 0x3 != 0
    }

    pub fn is_write_locked(&self) -> bool {
        self.0 & 0x1 != 0
    }

    pub fn owner(&self) -> u64 {
        (self.0 >> LOCK_BITS) & ((1u64 << THREAD_BITS) - 1)
    }

    pub fn version(&self) -> u64 {
        (self.0 >> META_BITS) & VERSION_MASK
    }

    pub fn set_version(&mut self, v: u64) {
        let mask = VERSION_MASK << META_BITS;
        self.0 = (self.0 & !mask) | (v << META_BITS);
    }

    pub fn try_lock(&mut self, tid: u64) -> bool {
        if self.is_locked() {
            return false;
        }
        self.0 |= 0x1;
        let owner_mask = ((1u64 << THREAD_BITS) - 1) << LOCK_BITS;
        self.0 = (self.0 & !owner_mask) | (tid << LOCK_BITS);
        true
    }

    pub fn unlock_owner(&mut self, tid: u64) {
        if self.owner() != tid { return; }
        let clear = 0x3 | (((1u64 << THREAD_BITS) - 1) << LOCK_BITS);
        self.0 &= !clear;
    }

    pub fn unlock_with_version(&mut self, tid: u64, version: u64) {
        self.set_version(version);
        self.unlock_owner(tid);
    }

    pub fn raw(&self) -> u64 {
        self.0
    }
}

pub fn addr_to_lock_idx(addr: u64) -> usize {
    ((addr >> 3) & (LOCK_TABLE_SIZE - 1)) as usize
}

// ── Per-transaction state ──────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReadEntry {
    pub addr: u64,
    pub observed_version: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WriteEntry {
    pub addr: u64,
    pub new_val: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TxState {
    pub tid: u64,
    pub active: bool,
    pub read_only: bool,
    pub start_version: u64,
    pub end_version: u64,
    pub read_set: Vec<ReadEntry>,
    pub write_set: Vec<WriteEntry>,
    pub locks_held: Vec<usize>,
    pub retry_count: u32,
    // Statistics
    pub peak_read_set_size: u32,
    pub peak_write_set_size: u32,
    pub validations: u32,
    pub spin_iterations: u32,
    pub snapshot_extensions: u32,
}

impl TxState {
    pub fn new(tid: u64) -> Self {
        TxState {
            tid,
            active: false,
            read_only: true,
            start_version: 0,
            end_version: 0,
            read_set: Vec::new(),
            write_set: Vec::new(),
            locks_held: Vec::new(),
            retry_count: 0,
            peak_read_set_size: 0,
            peak_write_set_size: 0,
            validations: 0,
            spin_iterations: 0,
            snapshot_extensions: 0,
        }
    }

    pub fn reset(&mut self) {
        self.read_set.clear();
        self.write_set.clear();
        self.locks_held.clear();
        self.active = false;
        self.read_only = true;
    }
}

// ── TM Model ───────────────────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TmModel {
    pub global_clock: u64,
    pub locks: Vec<LockWord>,
    pub tx_states: HashMap<u64, TxState>,
    pub aborts: u64,
    pub commits: u64,
    /// Committed values at tracked addresses (for value verification).
    pub committed_values: HashMap<u64, u64>,
    // Global statistics
    pub total_validations: u64,
    pub total_snapshot_extensions: u64,
    pub total_spin_iterations: u64,
    pub total_lock_contentions: u64,
    pub abort_reasons: HashMap<String, u64>,
}

impl TmModel {
    pub fn new() -> Self {
        TmModel {
            global_clock: 1,
            locks: vec![LockWord::new(0); LOCK_TABLE_SIZE as usize],
            tx_states: HashMap::new(),
            aborts: 0,
            commits: 0,
            committed_values: HashMap::new(),
            total_validations: 0,
            total_snapshot_extensions: 0,
            total_spin_iterations: 0,
            total_lock_contentions: 0,
            abort_reasons: HashMap::new(),
        }
    }

    pub fn committed_value(&self, addr: u64) -> Option<u64> {
        self.committed_values.get(&addr).copied()
    }

    pub fn get_or_create_tx(&mut self, tid: u64) -> &mut TxState {
        self.tx_states.entry(tid).or_insert_with(|| TxState::new(tid))
    }

    // ── Helpers that take state by value to avoid borrow conflicts ────

    #[allow(dead_code)]
    fn read_lock_version(&self, lock_idx: usize) -> u64 {
        (self.locks[lock_idx].0 >> META_BITS) & VERSION_MASK
    }

    #[allow(dead_code)]
    fn is_lock_locked(&self, lock_idx: usize) -> bool {
        self.locks[lock_idx].0 & 0x1 != 0
    }

    #[allow(dead_code)]
    fn lock_owner(&self, lock_idx: usize) -> u64 {
        (self.locks[lock_idx].0 >> LOCK_BITS) & ((1u64 << THREAD_BITS) - 1)
    }

    fn lock_raw(&self, lock_idx: usize) -> u64 {
        self.locks[lock_idx].0
    }

    fn validate_read_set(&self, tid: u64) -> bool {
        let tx = match self.tx_states.get(&tid) {
            Some(tx) => tx,
            None => return true,
        };
        for r in &tx.read_set {
            let li = addr_to_lock_idx(r.addr);
            let l = self.locks[li].0;
            let locked = l & 0x1 != 0;
            let owner = (l >> LOCK_BITS) & ((1u64 << THREAD_BITS) - 1);
            let ver = (l >> META_BITS) & VERSION_MASK;
            if (locked && owner != tid) || ver > r.observed_version {
                return false;
            }
        }
        true
    }

    // ── TM operations ─────────────────────────────────────────────────

    pub fn tm_begin(&mut self, tid: u64) {
        let sv = self.global_clock;
        let tx = self.get_or_create_tx(tid);
        tx.active = true;
        tx.read_only = true;
        tx.start_version = sv;
        tx.end_version = sv;
        tx.read_set.clear();
        tx.write_set.clear();
        tx.locks_held.clear();
    }

    pub fn tm_read(&mut self, tid: u64, addr: u64, _width: u8) -> Result<u64, String> {
        let li = addr_to_lock_idx(addr);

        // Check write-set first (read-after-write)
        {
            let tx = self.get_or_create_tx(tid);
            if !tx.active {
                return Err("read outside transaction".into());
            }
            for w in &tx.write_set {
                if w.addr == addr {
                    return Ok(w.new_val);
                }
            }
        }

        // Double-check protocol
        let mut spins: u32 = 0;
        for _ in 0..SPIN_ITERATIONS {
            spins += 1;
            let l1 = self.lock_raw(li);
            let locked = l1 & 0x1 != 0;

            if locked {
                self.total_lock_contentions += 1;
                continue;
            }

            let version = (l1 >> META_BITS) & VERSION_MASK;
            let value = version;

            // Double-check
            let l2 = self.lock_raw(li);
            if l1 != l2 {
                continue;
            }

            // Check if we need to extend snapshot
            let end_version = self.tx_states.get(&tid).map(|t| t.end_version).unwrap_or(0);
            if version > end_version {
                let last_version = self.global_clock;
                self.total_validations += 1;
                if !self.validate_read_set(tid) {
                    if let Some(tx) = self.tx_states.get_mut(&tid) {
                        self.total_spin_iterations += spins as u64;
                tx.spin_iterations += spins;
                    }
                    return Err("read_validate_fail".into());
                }
                self.total_snapshot_extensions += 1;
                // Update end_version
                if let Some(tx) = self.tx_states.get_mut(&tid) {
                    tx.snapshot_extensions += 1;
                    tx.end_version = last_version;
                }
                continue;
            }

            // Record in read-set
            if let Some(tx) = self.tx_states.get_mut(&tid) {
                self.total_spin_iterations += spins as u64;
                tx.spin_iterations += spins;
                tx.read_set.push(ReadEntry { addr, observed_version: version });
                tx.peak_read_set_size = tx.peak_read_set_size.max(tx.read_set.len() as u32);
            }
            return Ok(value);
        }

        if let Some(tx) = self.tx_states.get_mut(&tid) {
            self.total_spin_iterations += spins as u64;
                tx.spin_iterations += spins;
        }
        Err("read_spin_timeout".into())
    }

    pub fn tm_write(&mut self, tid: u64, addr: u64, val: u64, _width: u8) -> Result<(), String> {
        let li = addr_to_lock_idx(addr);

        // Update existing write-set entry
        {
            let tx = self.get_or_create_tx(tid);
            if !tx.active {
                return Err("write outside transaction".into());
            }
            tx.read_only = false;
            for w in &mut tx.write_set {
                if w.addr == addr {
                    w.new_val = val;
                    return Ok(());
                }
            }
        }

        // Double-check protocol for writes (WBCTL-style: no lock at write time)
        let mut spins: u32 = 0;
        for _ in 0..SPIN_ITERATIONS {
            spins += 1;
            let l1 = self.lock_raw(li);
            let locked = l1 & 0x1 != 0;
            let owner = (l1 >> LOCK_BITS) & ((1u64 << THREAD_BITS) - 1);

            if locked && owner != tid {
                self.total_lock_contentions += 1;
                self.total_validations += 1;
                if let Some(tx) = self.tx_states.get_mut(&tid) {
                    tx.validations += 1;
                }
                if !self.validate_read_set(tid) {
                    if let Some(tx) = self.tx_states.get_mut(&tid) {
                        self.total_spin_iterations += spins as u64;
                tx.spin_iterations += spins;
                    }
                    return Err("write_lock_validate_fail".into());
                }
                continue;
            }

            let l2 = self.lock_raw(li);
            if l1 != l2 {
                continue;
            }

            let version = (l2 >> META_BITS) & VERSION_MASK;
            let end_version = self.tx_states.get(&tid).map(|t| t.end_version).unwrap_or(0);

            if version > end_version {
                let last_version = self.global_clock;
                self.total_validations += 1;
                if let Some(tx) = self.tx_states.get_mut(&tid) {
                    tx.validations += 1;
                }
                if !self.validate_read_set(tid) {
                    if let Some(tx) = self.tx_states.get_mut(&tid) {
                        self.total_spin_iterations += spins as u64;
                tx.spin_iterations += spins;
                    }
                    return Err("write_version_extension_fail".into());
                }
                self.total_snapshot_extensions += 1;
                if let Some(tx) = self.tx_states.get_mut(&tid) {
                    tx.snapshot_extensions += 1;
                    tx.end_version = last_version;
                }
                continue;
            }

            // Record write and also track in read-set (TinySTM mirrors writes in read-set)
            if let Some(tx) = self.tx_states.get_mut(&tid) {
                self.total_spin_iterations += spins as u64;
                tx.spin_iterations += spins;
                tx.write_set.push(WriteEntry { addr, new_val: val });
                tx.read_set.push(ReadEntry { addr, observed_version: version });
                tx.peak_read_set_size = tx.peak_read_set_size.max(tx.read_set.len() as u32);
                tx.peak_write_set_size = tx.peak_write_set_size.max(tx.write_set.len() as u32);
            }
            return Ok(());
        }

        if let Some(tx) = self.tx_states.get_mut(&tid) {
            self.total_spin_iterations += spins as u64;
                tx.spin_iterations += spins;
        }
        Err("write_spin_timeout".into())
    }

    pub fn tm_commit(&mut self, tid: u64) -> Result<(), String> {
        if !self.tx_states.get(&tid).map(|t| t.active).unwrap_or(false) {
            return Err("commit without active tx".into());
        }

        let is_read_only = self.tx_states.get(&tid).map(|t| t.read_only).unwrap_or(true);

        if !is_read_only {
            let write_addrs: Vec<u64>;
            {
                let tx = self.tx_states.get(&tid).unwrap();
                write_addrs = tx.write_set.iter().map(|w| w.addr).collect();
            }

            // Phase 1: Lock acquisition (sorted for deadlock freedom)
            let mut sorted = write_addrs.clone();
            sorted.sort();
            sorted.dedup();

            let mut held: Vec<usize> = Vec::new();

            for &addr in &sorted {
                let li = addr_to_lock_idx(addr);
                let mut acquired = false;
                for _ in 0..SPIN_ITERATIONS {
                    if !self.locks[li].is_locked() {
                        if self.locks[li].try_lock(tid) {
                            acquired = true;
                            break;
                        }
                    }
                    self.total_lock_contentions += 1;
                    self.total_validations += 1;
                    if let Some(tx) = self.tx_states.get_mut(&tid) {
                        tx.validations += 1;
                    }
                    if !self.validate_read_set(tid) {
                        // Abort — release any locks we hold
                        for &hli in &held {
                            self.locks[hli].unlock_owner(tid);
                        }
                        self.tm_abort_reason(tid, "commit_lock_abort");
                        return Err("commit_lock_abort".into());
                    }
                }
                if !acquired {
                    for &hli in &held {
                        self.locks[hli].unlock_owner(tid);
                    }
                    self.tm_abort_reason(tid, "commit_lock_timeout");
                    return Err("commit_lock_timeout".into());
                }
                held.push(li);
            }

            // Update tx's locks_held
            if let Some(tx) = self.tx_states.get_mut(&tid) {
                tx.locks_held = held.clone();
            }

            // Phase 2: Clock bump
            self.global_clock += 1;
            let commit_version = self.global_clock;

            // Phase 3: Final validation
            self.total_validations += 1;
            if let Some(tx) = self.tx_states.get_mut(&tid) {
                tx.validations += 1;
            }
            if !self.validate_read_set(tid) {
                for &hli in &held {
                    self.locks[hli].unlock_owner(tid);
                }
                self.tm_abort_reason(tid, "commit_validate_fail");
                return Err("commit_validate_fail".into());
            }

            // Phase 4: Write-back (record committed values + unlock)
            {
                let tx = self.tx_states.get(&tid).unwrap();
                for w in &tx.write_set {
                    self.committed_values.insert(w.addr, w.new_val);
                }
            }
            for &hli in &held {
                self.locks[hli].unlock_with_version(tid, commit_version);
            }
        }

        if let Some(tx) = self.tx_states.get_mut(&tid) {
            tx.active = false;
            tx.read_set.clear();
            tx.write_set.clear();
            tx.locks_held.clear();
        }
        self.commits += 1;
        Ok(())
    }

    pub fn tm_abort(&mut self, tid: u64) {
        self.tm_abort_reason(tid, "unspecified")
    }

    pub fn tm_abort_reason(&mut self, tid: u64, reason: &str) {
        *self.abort_reasons.entry(reason.to_string()).or_insert(0) += 1;
        if let Some(tx) = self.tx_states.get_mut(&tid) {
            for &li in &tx.locks_held {
                self.locks[li].unlock_owner(tid);
            }
            tx.read_set.clear();
            tx.write_set.clear();
            tx.locks_held.clear();
            tx.active = false;
            tx.retry_count += 1;
        }
        self.aborts += 1;
    }

    pub fn tm_end(&mut self, tid: u64) -> Result<(), String> {
        self.total_validations += 1;
        if let Some(tx) = self.tx_states.get_mut(&tid) {
            tx.validations += 1;
        }
        if !self.validate_read_set(tid) {
            self.tm_abort_reason(tid, "end_validate_fail");
            return Err("abort_conflict".into());
        }
        self.tm_commit(tid)
    }
}
