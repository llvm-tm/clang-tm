// ── Computation baseline profile ─────────────────────
// Records per-transaction wall times from an uninstrumented
// baseline run.  The baseline provides pure computation cost
// (no TM overhead), which the simulation adds TM-model cycles
// to for total wall-time estimation.
//
// File format (from tm_stub_runtime.cpp):
//   <thread_id> <seq> <begin_ns> <end_ns>
//   ...
//   TOTAL: <count> seqs <total_ns> ns

use std::fs::File;
use std::io::{BufRead, BufReader};
use std::path::Path;

/// Computation baseline profile loaded from a baseline timing file.
#[derive(Debug, Clone, Default)]
pub struct ComputationProfile {
    /// Total wall time of all transactions in nanoseconds.
    pub total_ns: u64,
    /// Number of transactions recorded.
    pub count: u64,
}

impl ComputationProfile {
    /// Load a computation profile from a baseline timing file.
    ///
    /// The file must contain a final line of the form:
    ///   TOTAL: <count> seqs <total_ns> ns
    pub fn load<P: AsRef<Path>>(path: P) -> Result<Self, String> {
        let file = File::open(path.as_ref())
            .map_err(|e| format!("Cannot open '{}': {}", path.as_ref().display(), e))?;
        let reader = BufReader::new(file);
        let mut profile = ComputationProfile::default();

        for line in reader.lines() {
            let line = line.map_err(|e| format!("Read error: {}", e))?;
            if line.starts_with("TOTAL:") {
                // Parse "TOTAL: 12392 seqs 3187000 ns"
                let parts: Vec<&str> = line.split_whitespace().collect();
                if parts.len() >= 4 {
                    profile.count = parts[1].parse().unwrap_or(0);
                    profile.total_ns = parts[3].parse().unwrap_or(0);
                    return Ok(profile);
                }
                return Err(format!("Malformed TOTAL line: '{}'", line));
            }
        }

        Err("No TOTAL line found in baseline profile".into())
    }

    /// Estimated computation time in seconds.
    pub fn seconds(&self) -> f64 {
        self.total_ns as f64 / 1e9
    }
}
