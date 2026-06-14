// ── Verifier ──────────────────────────────────────────────
// Shadow memory + correctness checks that run alongside the
// backend simulation.
//
// Maintains a shadow of the TM address space:
//   - Tracks allocations (Alloc events) and frees (Free events)
//   - Detects double-free, free-of-unallocated, use-after-free
//   Tracks committed values for money conservation checks.

use std::collections::HashMap;

/// An allocation in the shadow memory.
#[derive(Debug, Clone)]
pub struct Allocation {
    pub size: u64,
    pub is_freed: bool,
}

/// A committed value record (for money conservation checks).
#[derive(Debug, Clone)]
pub struct CommittedValue {
    pub initial: u64,
    pub final_val: u64,
}

/// Simulation verifier.
#[derive(Debug, Clone)]
pub struct Verifier {
    /// Shadow memory: addr → Allocation
    pub shadow: HashMap<u64, Allocation>,

    /// Committed values at tracked addresses.
    /// Set from initial_values in trace or from Write events.
    pub values: HashMap<u64, u64>,

    /// Per-thread transaction depth (for nesting checks).
    pub tx_depth: HashMap<u64, u32>,

    /// Violations detected.
    pub violations: Vec<String>,

    /// Per-thread out-of-TX counters.
    pub reads_outside: u64,
    pub writes_outside: u64,

    /// Abort counter.
    pub aborts: u64,
    pub commits: u64,
}

impl Verifier {
    pub fn new() -> Self {
        Verifier {
            shadow: HashMap::new(),
            values: HashMap::new(),
            tx_depth: HashMap::new(),
            violations: Vec::new(),
            reads_outside: 0,
            writes_outside: 0,
            aborts: 0,
            commits: 0,
        }
    }

    /// Record the initial value of an address (from trace initial-values).
    pub fn set_initial_value(&mut self, addr: u64, val: u64) {
        self.values.entry(addr).or_insert(val);
    }

    /// Check a read operation. Returns true if in-transaction, false otherwise.
    pub fn check_read(&mut self, tid: u64, addr: u64) -> bool {
        let depth = self.tx_depth.get(&tid).copied().unwrap_or(0);
        if depth == 0 {
            self.reads_outside += 1;
            return false;
        }

        // Use-after-free check
        if let Some(alloc) = self.shadow.get(&addr) {
            if alloc.is_freed {
                self.violations.push(format!(
                    "USE-AFTER-FREE tid={} addr=0x{:x} (was freed, size={})",
                    tid, addr, alloc.size
                ));
            }
        }
        true
    }

    /// Check a write operation. Returns true if in-transaction.
    pub fn check_write(&mut self, tid: u64, addr: u64, _val: u64) -> bool {
        let depth = self.tx_depth.get(&tid).copied().unwrap_or(0);
        if depth == 0 {
            self.writes_outside += 1;
            return false;
        }

        // Use-after-free check
        if let Some(alloc) = self.shadow.get(&addr) {
            if alloc.is_freed {
                self.violations.push(format!(
                    "USE-AFTER-FREE tid={} addr=0x{:x} (was freed, size={})",
                    tid, addr, alloc.size
                ));
            }
        }
        true
    }

    /// Track an allocation.
    pub fn alloc(&mut self, addr: u64, size: u64) {
        if self.shadow.contains_key(&addr) {
            self.violations.push(format!(
                "RE-ALLOC of address 0x{:x} (already allocated)",
                addr
            ));
        }
        self.shadow.insert(addr, Allocation { size, is_freed: false });
    }

    /// Track a free, checking for double-free or free-of-unallocated.
    pub fn free(&mut self, addr: u64) {
        match self.shadow.get_mut(&addr) {
            None => {
                self.violations.push(format!(
                    "FREE of unallocated address 0x{:x}",
                    addr
                ));
            }
            Some(a) if a.is_freed => {
                self.violations.push(format!(
                    "DOUBLE-FREE of address 0x{:x}",
                    addr
                ));
            }
            Some(a) => {
                a.is_freed = true;
            }
        }
    }

    /// Called on TxBegin.
    pub fn tx_begin(&mut self, tid: u64) {
        *self.tx_depth.entry(tid).or_insert(0) += 1;
    }

    /// Called on successful commit (returns true).
    pub fn tx_commit(&mut self, tid: u64) {
        let d = self.tx_depth.get_mut(&tid).expect("commit without begin");
        *d = d.saturating_sub(1);
        self.commits += 1;
    }

    /// Called on abort.
    pub fn tx_abort(&mut self, tid: u64) {
        let d = self.tx_depth.get_mut(&tid).expect("abort without begin");
        *d = d.saturating_sub(1);
        self.aborts += 1;
    }

    /// Record a committed value.
    pub fn record_value(&mut self, addr: u64, val: u64) {
        self.values.insert(addr, val);
    }

    /// Compute money conservation: sum of all tracked values.
    pub fn total_value(&self) -> u64 {
        self.values.values().sum()
    }

    /// Reset per-scenario state (counters survive across scenarios).
    pub fn reset(&mut self) {
        self.shadow.clear();
        self.values.clear();
        self.tx_depth.clear();
        self.violations.clear();
        self.reads_outside = 0;
        self.writes_outside = 0;
    }

    /// Generate a final report.
    pub fn report(&self, initial_total: u64) -> Vec<String> {
        let mut lines = Vec::new();

        lines.push(format!("Commits: {}  Aborts: {}  Abort rate: {:.1}%",
            self.commits,
            self.aborts,
            if self.commits + self.aborts > 0 {
                100.0 * self.aborts as f64 / (self.commits + self.aborts) as f64
            } else { 0.0 },
        ));

        if self.violations.is_empty() {
            lines.push("NO MEMORY VIOLATIONS".into());
        } else {
            lines.push(format!("⚠ {} MEMORY VIOLATION(S):", self.violations.len()));
            for v in &self.violations {
                lines.push(format!("  ❌ {}", v));
            }
        }

        if self.reads_outside > 0 {
            lines.push(format!("⚠ {} read(s) outside transaction", self.reads_outside));
        }
        if self.writes_outside > 0 {
            lines.push(format!("⚠ {} write(s) outside transaction", self.writes_outside));
        }

        let final_total = self.total_value();
        if initial_total > 0 {
            lines.push(format!("Total value: {}  (initial: {})  Δ={}",
                final_total, initial_total,
                final_total as i64 - initial_total as i64));
            if final_total == initial_total {
                lines.push("MONEY CONSERVED ✓".into());
            } else {
                lines.push("⚠ MONEY NOT CONSERVED".into());
            }
        }

        // Per-thread nesting warnings
        for (&tid, &d) in &self.tx_depth {
            if d > 0 {
                lines.push(format!("⚠ Thread {} still in transaction (depth={}) at end", tid, d));
            }
        }

        lines
    }
}
