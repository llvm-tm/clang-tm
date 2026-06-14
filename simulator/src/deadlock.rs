use std::collections::{HashMap, HashSet};

#[derive(Debug, Clone)]
pub struct WaitEntry {
    pub tid: u64,
    pub conflicts: HashSet<u64>,
    pub retries: u64,
}

#[derive(Debug, Clone)]
pub struct DeadlockReport {
    pub cycle: Vec<u64>,
    pub conflicting_addrs: Vec<u64>,
    pub retries: u64,
}

#[derive(Debug, Clone)]
pub struct DeadlockDetector {
    retries: HashMap<u64, u64>,
    thread_write_sets: HashMap<u64, Vec<u64>>,
    last_abort_write_sets: HashMap<u64, Vec<u64>>,
    max_retries: u64,
    reports: Vec<DeadlockReport>,
}

impl DeadlockDetector {
    pub fn new(max_retries: u64) -> Self {
        DeadlockDetector {
            retries: HashMap::new(),
            thread_write_sets: HashMap::new(),
            last_abort_write_sets: HashMap::new(),
            max_retries,
            reports: Vec::new(),
        }
    }

    pub fn record_commit(&mut self, tid: u64, write_set: &[u64]) {
        self.retries.remove(&tid);
        self.thread_write_sets.insert(tid, write_set.to_vec());
        self.last_abort_write_sets.remove(&tid);
    }

    pub fn record_abort(&mut self, tid: u64, write_set: &[u64]) {
        let retries = self.retries.entry(tid).or_insert(0);
        *retries += 1;
        self.last_abort_write_sets.insert(tid, write_set.to_vec());
    }

    pub fn check(&mut self) -> Vec<DeadlockReport> {
        let mut reports = Vec::new();

        for (&tid, &retries) in &self.retries {
            if retries < self.max_retries {
                continue;
            }

            if let Some(cycle) = self.detect_cycle(tid) {
                let addrs = self.collect_addrs_in_cycle(&cycle);
                reports.push(DeadlockReport {
                    cycle,
                    conflicting_addrs: addrs,
                    retries,
                });
            }
        }

        for r in &reports {
            self.reports.push(r.clone());
        }

        reports
    }

    fn detect_cycle(&self, start_tid: u64) -> Option<Vec<u64>> {
        let mut visited = HashSet::new();
        let mut stack = Vec::new();
        let mut path = Vec::new();

        if self.dfs(start_tid, start_tid, &mut visited, &mut stack, &mut path) {
            let cycle_start = path.iter().position(|&t| t == start_tid).unwrap_or(0);
            let cycle: Vec<u64> = path[cycle_start..].to_vec();
            if cycle.len() >= 2 {
                return Some(cycle);
            }
        }

        None
    }

    fn get_conflict_set(&self, tid: u64) -> HashSet<u64> {
        let mut conflicts = HashSet::new();

        let my_addrs: HashSet<u64> = self
            .last_abort_write_sets
            .get(&tid)
            .map(|ws| ws.iter().copied().collect())
            .unwrap_or_default();

        if my_addrs.is_empty() {
            return conflicts;
        }

        for (&other_tid, write_set) in &self.last_abort_write_sets {
            if other_tid == tid {
                continue;
            }
            if write_set.iter().any(|addr| my_addrs.contains(addr)) {
                conflicts.insert(other_tid);
            }
        }

        for (&other_tid, write_set) in &self.thread_write_sets {
            if other_tid == tid {
                continue;
            }
            if write_set.iter().any(|addr| my_addrs.contains(addr)) {
                conflicts.insert(other_tid);
            }
        }

        conflicts
    }

    fn dfs(
        &self,
        current: u64,
        target: u64,
        visited: &mut HashSet<u64>,
        stack: &mut Vec<u64>,
        path: &mut Vec<u64>,
    ) -> bool {
        if !visited.insert(current) {
            return false;
        }

        stack.push(current);
        path.push(current);

        let conflicts = self.get_conflict_set(current);

        for &next in &conflicts {
            if next == target && stack.len() >= 2 {
                path.push(next);
                return true;
            }
            if self.dfs(next, target, visited, stack, path) {
                return true;
            }
        }

        stack.pop();
        path.pop();
        false
    }

    fn collect_addrs_in_cycle(&self, cycle: &[u64]) -> Vec<u64> {
        let mut addrs = HashSet::new();
        for &tid in cycle {
            if let Some(ws) = self.last_abort_write_sets.get(&tid) {
                for &a in ws {
                    addrs.insert(a);
                }
            }
            if let Some(ws) = self.thread_write_sets.get(&tid) {
                for &a in ws {
                    addrs.insert(a);
                }
            }
        }
        let mut sorted: Vec<u64> = addrs.into_iter().collect();
        sorted.sort();
        sorted
    }

    pub fn reset(&mut self) {
        self.retries.clear();
        self.thread_write_sets.clear();
        self.last_abort_write_sets.clear();
        self.reports.clear();
    }

    pub fn report(&self) -> Vec<String> {
        let mut lines = Vec::new();
        if self.reports.is_empty() {
            return lines;
        }
        lines.push(format!("⚠ {} DEADLOCK/LIVELOCK DETECTION(S):", self.reports.len()));
        for (i, r) in self.reports.iter().enumerate() {
            lines.push(format!(
                "  ⚠ Cycle {}: threads [{}] — {} retries each",
                i + 1,
                r.cycle.iter().map(|t| t.to_string()).collect::<Vec<_>>().join(", "),
                r.retries
            ));
            if !r.conflicting_addrs.is_empty() {
                lines.push(format!(
                    "     Conflicting addresses: [{}]",
                    r.conflicting_addrs.iter().map(|a| format!("0x{:x}", a)).collect::<Vec<_>>().join(", ")
                ));
            }
        }
        lines
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_no_deadlock_no_aborts() {
        let mut d = DeadlockDetector::new(5);
        d.record_commit(0, &[0x1000]);
        d.record_commit(1, &[0x2000]);
        let reports = d.check();
        assert!(reports.is_empty());
    }

    #[test]
    fn test_single_abort_below_threshold() {
        let mut d = DeadlockDetector::new(5);
        d.record_abort(0, &[0x1000]);
        let reports = d.check();
        assert!(reports.is_empty(), "1 abort < threshold 5");
    }

    #[test]
    fn test_cycle_detected_after_threshold() {
        let mut d = DeadlockDetector::new(2);
        d.record_commit(0, &[0x1000]);
        d.record_commit(1, &[0x1000]);
        for _ in 0..3 {
            d.record_abort(0, &[0x1000]);
            d.record_abort(1, &[0x1000]);
        }
        let reports = d.check();
        assert!(!reports.is_empty(), "should detect cycle after 3 aborts (threshold 2)");
    }

    #[test]
    fn test_commit_clears_retries() {
        let mut d = DeadlockDetector::new(3);
        d.record_abort(0, &[0x1000]);
        d.record_abort(0, &[0x1000]);
        d.record_commit(0, &[0x1000]);
        let reports = d.check();
        assert!(reports.is_empty(), "commit should clear retries");
    }

    #[test]
    fn test_different_addresses_no_conflict() {
        let mut d = DeadlockDetector::new(2);
        d.record_commit(0, &[0x1000]);
        d.record_commit(1, &[0x2000]);
        for _ in 0..5 {
            d.record_abort(0, &[0x1000]);
            d.record_abort(1, &[0x2000]);
        }
        let reports = d.check();
        assert!(reports.is_empty(), "different addresses should not conflict");
    }

    #[test]
    fn test_reset_clears_all_state() {
        let mut d = DeadlockDetector::new(2);
        d.record_abort(0, &[0x1000]);
        d.record_abort(1, &[0x1000]);
        d.reset();
        let reports = d.check();
        assert!(reports.is_empty());
    }

    #[test]
    fn test_report_format() {
        let mut d = DeadlockDetector::new(2);
        d.record_commit(0, &[0x1000]);
        d.record_commit(1, &[0x1000]);
        for _ in 0..3 {
            d.record_abort(0, &[0x1000]);
            d.record_abort(1, &[0x1000]);
        }
        d.check();
        let lines = d.report();
        assert!(lines.len() >= 2);
        assert!(lines[0].contains("DEADLOCK"));
    }
}
