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
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct Allocation {
    pub size: u64,
    pub is_freed: bool,
}

/// Simulation verifier.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct Verifier {
    pub shadow: HashMap<u64, Allocation>,
    pub values: HashMap<u64, u64>,
    pub tx_depth: HashMap<u64, u32>,
    pub violations: Vec<String>,
    pub reads_outside: u64,
    pub writes_outside: u64,
    pub aborts: u64,
    pub commits: u64,
    pub initial_value_sum: u64,
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
            initial_value_sum: 0,
        }
    }

    pub fn set_initial_value(&mut self, addr: u64, val: u64) {
        self.values.entry(addr).or_insert(val);
    }

    pub fn check_read(&mut self, tid: u64, addr: u64) -> bool {
        let depth = self.tx_depth.get(&tid).copied().unwrap_or(0);
        if depth == 0 {
            self.reads_outside += 1;
            return false;
        }
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

    pub fn check_write(&mut self, tid: u64, addr: u64, _val: u64) -> bool {
        let depth = self.tx_depth.get(&tid).copied().unwrap_or(0);
        if depth == 0 {
            self.writes_outside += 1;
            return false;
        }
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

    pub fn alloc(&mut self, addr: u64, size: u64) {
        match self.shadow.get(&addr) {
            Some(a) if !a.is_freed => {
                self.violations.push(format!(
                    "RE-ALLOC of address 0x{:x} (still live, size={})",
                    addr, a.size
                ));
                return;
            }
            _ => {}
        }
        self.shadow.insert(addr, Allocation { size, is_freed: false });
    }

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

    /// Handle a TxBegin event.
    ///
    /// Simply increments the nesting depth.  If the caller knows that the
    /// previous transaction was implicitly aborted (e.g. by C++ siglongjmp
    /// retry), it must call `tx_abort` before `tx_begin`.
    pub fn tx_begin(&mut self, tid: u64) {
        let depth = self.tx_depth.entry(tid).or_insert(0);
        if *depth > 0 {
            self.violations.push(format!(
                "DOUBLE-TX-BEGIN tid={} (already in transaction, depth={})",
                tid, *depth
            ));
        }
        *depth += 1;
    }

    pub fn tx_commit(&mut self, tid: u64) {
        let d = match self.tx_depth.get_mut(&tid) {
            Some(d) if *d > 0 => d,
            _ => {
                self.violations.push(format!(
                    "COMMIT-WITHOUT-BEGIN tid={}",
                    tid
                ));
                return;
            }
        };
        *d = d.saturating_sub(1);
        self.commits += 1;
    }

    pub fn tx_abort(&mut self, tid: u64) {
        match self.tx_depth.get_mut(&tid) {
            Some(d) if *d > 0 => {
                *d = d.saturating_sub(1);
                self.aborts += 1;
            }
            // Thread never began a transaction — this is a real abort-without-begin.
            None => self.violations.push(format!(
                "ABORT-WITHOUT-BEGIN tid={}",
                tid
            )),
            // Depth already 0 — this can happen when a panic-based backend
            // (e.g. NOrec, TL2) throws TmxAbort on a read that follows the
            // abort point in a C++ siglongjmp trace.  The previous abort event
            // (from the first TmxAbort catch in dispatch_event) already closed
            // the transaction.  Silently ignore — the transaction is already
            // closed.
            _ => {}
        };
    }

    pub fn record_value(&mut self, addr: u64, val: u64) {
        self.values.insert(addr, val);
    }

    pub fn total_value(&self) -> u64 {
        self.values.values().sum()
    }

    pub fn reset(&mut self) {
        self.shadow.clear();
        self.values.clear();
        self.tx_depth.clear();
        self.violations.clear();
        self.reads_outside = 0;
        self.writes_outside = 0;
    }

    pub fn report(&self) -> Vec<String> {
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
        if self.initial_value_sum > 0 {
            lines.push(format!("Total value: {}  (initial: {})  Δ={}",
                final_total, self.initial_value_sum,
                final_total as i64 - self.initial_value_sum as i64));
            if final_total == self.initial_value_sum {
                lines.push("MONEY CONSERVED ✓".into());
            } else {
                lines.push("⚠ MONEY NOT CONSERVED".into());
            }
        }

        for (&tid, &d) in &self.tx_depth {
            if d > 0 {
                lines.push(format!("⚠ Thread {} still in transaction (depth={}) at end", tid, d));
            }
        }

        lines
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Transaction depth ──────────────────────────────────

    #[test]
    fn test_begin_commit_tracks_depth() {
        let mut v = Verifier::new();
        v.tx_begin(0);
        assert_eq!(v.tx_depth.get(&0), Some(&1));
        v.tx_commit(0);
        assert_eq!(v.tx_depth.get(&0), Some(&0));
        assert_eq!(v.commits, 1);
    }

    #[test]
    fn test_begin_abort_tracks_depth() {
        let mut v = Verifier::new();
        v.tx_begin(0);
        assert_eq!(v.tx_depth.get(&0), Some(&1));
        v.tx_abort(0);
        assert_eq!(v.tx_depth.get(&0), Some(&0));
        assert_eq!(v.aborts, 1);
    }

    #[test]
    fn test_multiple_threads_independent_depth() {
        let mut v = Verifier::new();
        v.tx_begin(0);
        v.tx_begin(1);
        v.tx_begin(1);
        assert_eq!(v.tx_depth.get(&0), Some(&1));
        assert_eq!(v.tx_depth.get(&1), Some(&2));
        v.tx_commit(0);
        v.tx_commit(1);
        assert_eq!(v.tx_depth.get(&0), Some(&0));
        assert_eq!(v.tx_depth.get(&1), Some(&1));
        v.tx_commit(1);
        assert_eq!(v.tx_depth.get(&1), Some(&0));
    }

    // ── Out-of-transaction detection ───────────────────────

    #[test]
    fn test_read_outside_tx_counted() {
        let mut v = Verifier::new();
        let in_tx = v.check_read(0, 0x1000);
        assert!(!in_tx);
        assert_eq!(v.reads_outside, 1);
    }

    #[test]
    fn test_read_inside_tx_not_counted() {
        let mut v = Verifier::new();
        v.tx_begin(0);
        let in_tx = v.check_read(0, 0x1000);
        assert!(in_tx);
        assert_eq!(v.reads_outside, 0);
    }

    #[test]
    fn test_write_outside_tx_counted() {
        let mut v = Verifier::new();
        let in_tx = v.check_write(0, 0x1000, 42);
        assert!(!in_tx);
        assert_eq!(v.writes_outside, 1);
    }

    #[test]
    fn test_write_inside_tx_not_counted() {
        let mut v = Verifier::new();
        v.tx_begin(0);
        let in_tx = v.check_write(0, 0x1000, 42);
        assert!(in_tx);
        assert_eq!(v.writes_outside, 0);
    }

    // ── Memory violation detection ─────────────────────────

    #[test]
    fn test_double_free_detected() {
        let mut v = Verifier::new();
        v.alloc(0x1000, 64);
        v.free(0x1000);
        assert!(v.violations.is_empty(), "first free should succeed");
        v.free(0x1000); // second free — double free
        assert_eq!(v.violations.len(), 1);
        assert!(v.violations[0].contains("DOUBLE-FREE"));
    }

    #[test]
    fn test_free_unallocated_detected() {
        let mut v = Verifier::new();
        v.free(0xDEAD);
        assert_eq!(v.violations.len(), 1);
        assert!(v.violations[0].contains("FREE of unallocated"));
    }

    #[test]
    fn test_use_after_free_detected_on_read() {
        let mut v = Verifier::new();
        v.alloc(0x2000, 32);
        v.free(0x2000);
        v.tx_begin(0);
        v.check_read(0, 0x2000);
        assert_eq!(v.violations.len(), 1);
        assert!(v.violations[0].contains("USE-AFTER-FREE"));
    }

    #[test]
    fn test_use_after_free_detected_on_write() {
        let mut v = Verifier::new();
        v.alloc(0x2000, 32);
        v.free(0x2000);
        v.tx_begin(0);
        v.check_write(0, 0x2000, 99);
        assert_eq!(v.violations.len(), 1);
        assert!(v.violations[0].contains("USE-AFTER-FREE"));
    }

    #[test]
    fn test_re_alloc_detected() {
        let mut v = Verifier::new();
        v.alloc(0x3000, 64);
        v.alloc(0x3000, 128); // same address, still live
        assert_eq!(v.violations.len(), 1);
        assert!(v.violations[0].contains("RE-ALLOC"));
    }

    #[test]
    fn test_alloc_free_alloc_works() {
        let mut v = Verifier::new();
        v.alloc(0x4000, 64);
        v.free(0x4000);
        assert!(v.violations.is_empty());
        v.alloc(0x4000, 128); // re-alloc after free — allowed
        assert!(v.violations.is_empty(), "re-alloc after free is OK");
    }

    // ── Value / money conservation ─────────────────────────

    #[test]
    fn test_record_value_tracks_values() {
        let mut v = Verifier::new();
        v.record_value(0x1000, 100);
        v.record_value(0x2000, 200);
        assert_eq!(v.total_value(), 300);
    }

    #[test]
    fn test_record_value_overwrites_same_addr() {
        let mut v = Verifier::new();
        v.record_value(0x1000, 100);
        v.record_value(0x1000, 150);
        assert_eq!(v.total_value(), 150);
    }

    #[test]
    fn test_empty_verifier_total_zero() {
        let v = Verifier::new();
        assert_eq!(v.total_value(), 0);
    }

    #[test]
    fn test_set_initial_value_only_first() {
        let mut v = Verifier::new();
        v.set_initial_value(0x1000, 50);
        v.set_initial_value(0x1000, 99); // ignored — or_insert
        assert_eq!(v.total_value(), 50);
    }

    // ── Reset ──────────────────────────────────────────────

    #[test]
    fn test_reset_clears_scenario_state() {
        let mut v = Verifier::new();
        v.tx_begin(0);
        v.check_read(0, 0x1000);
        v.record_value(0x1000, 42);
        v.alloc(0x2000, 64);
        v.free(0xDEAD); // violation
        v.tx_commit(0);

        assert_eq!(v.commits, 1);
        assert_eq!(v.reads_outside, 0);
        assert!(!v.violations.is_empty());
        assert!(!v.values.is_empty());

        v.reset();

        // Counters survive reset
        assert_eq!(v.commits, 1);
        // Scenario state cleared
        assert!(v.shadow.is_empty());
        assert!(v.values.is_empty());
        assert!(v.tx_depth.is_empty());
        assert!(v.violations.is_empty());
        assert_eq!(v.reads_outside, 0);
    }

    // ── Report ─────────────────────────────────────────────

    #[test]
    fn test_report_empty() {
        let v = Verifier::new();
        let lines = v.report();
        assert!(lines.iter().any(|l| l.contains("Commits: 0")));
        assert!(lines.iter().any(|l| l.contains("NO MEMORY VIOLATIONS")));
    }

    #[test]
    fn test_report_with_aborts() {
        let mut v = Verifier::new();
        v.commits = 9;
        v.aborts = 1;
        let lines = v.report();
        assert!(lines.iter().any(|l| l.contains("10.0%")));
    }

    #[test]
    fn test_report_with_money_conservation() {
        let mut v = Verifier::new();
        v.record_value(0x1000, 100);
        v.record_value(0x2000, 200);
        v.initial_value_sum = 300;
        let lines = v.report();
        assert!(lines.iter().any(|l| l.contains("MONEY CONSERVED")));
        assert!(lines.iter().any(|l| l.contains("Δ=0")));
    }

    #[test]
    fn test_report_money_not_conserved() {
        let mut v = Verifier::new();
        v.record_value(0x1000, 100);
        v.initial_value_sum = 200;
        let lines = v.report();
        assert!(lines.iter().any(|l| l.contains("MONEY NOT CONSERVED")));
        assert!(lines.iter().any(|l| l.contains("Δ=-100")));
    }

    #[test]
    fn test_report_violations() {
        let mut v = Verifier::new();
        v.free(0xBEEF); // unallocated
        let lines = v.report();
        assert!(lines.iter().any(|l| l.contains("1 MEMORY VIOLATION")));
    }

    #[test]
    fn test_report_outside_accesses() {
        let mut v = Verifier::new();
        v.reads_outside = 3;
        v.writes_outside = 1;
        let lines = v.report();
        assert!(lines.iter().any(|l| l.contains("3 read(s) outside")));
        assert!(lines.iter().any(|l| l.contains("1 write(s) outside")));
    }

    #[test]
    fn test_commit_without_begin_recorded() {
        let mut v = Verifier::new();
        v.tx_commit(0);
        assert!(v.violations.iter().any(|l| l.contains("COMMIT-WITHOUT-BEGIN")));
    }

    #[test]
    fn test_abort_without_begin_recorded() {
        let mut v = Verifier::new();
        v.tx_abort(0);
        assert!(v.violations.iter().any(|l| l.contains("ABORT-WITHOUT-BEGIN")));
    }
}

