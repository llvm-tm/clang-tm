// ── Machine Profile ─────────────────────────────────────────
// Hardware characteristics loaded from a JSON file (collected
// once per machine by running the profiling benchmarks).
//
// This separates machine-specific costs from the simulation
// logic.  A profiling run on one machine produces a machine
// profile that another machine can consume without re-running
// the profiling scripts.

use std::path::Path;

/// TSX-specific hardware characteristics.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct TsxCharacteristics {
    /// Cycles for a successful _xbegin().
    pub xbegin_cycles: f64,
    /// Cycles for _xend() commit (store buffer flush).
    pub xend_cycles: f64,
    /// Cycles for _xabort() (architectural state restore).
    pub xabort_cycles: f64,
    /// Cycles for an L1-cached read within TSX.
    pub read_l1_cycles: f64,
    /// Cycles for an L1-cached write within TSX.
    pub write_l1_cycles: f64,
    /// Cycles for a bloom-filter lookup (approximate L1 tracking).
    pub bloom_check_cycles: f64,
    /// Cycles for a mutex lock acquisition.
    pub mutex_lock_cycles: f64,
    /// Cycles for a mutex unlock.
    pub mutex_unlock_cycles: f64,
    /// Additional penalty cycles for a conflict-induced abort.
    pub conflict_abort_penalty: f64,
    /// CPU cache line size in bytes.
    pub cache_line_size: u64,
    /// Maximum unique cache lines in the read-set before capacity abort.
    pub max_read_lines: usize,
    /// Maximum unique cache lines in the write-set before capacity abort.
    pub max_write_lines: usize,
}

impl Default for TsxCharacteristics {
    fn default() -> Self {
        TsxCharacteristics {
            xbegin_cycles: 20.0,
            xend_cycles: 80.0,
            xabort_cycles: 1500.0,
            read_l1_cycles: 4.0,
            write_l1_cycles: 5.0,
            bloom_check_cycles: 2.0,
            mutex_lock_cycles: 100.0,
            mutex_unlock_cycles: 100.0,
            conflict_abort_penalty: 2000.0,
            cache_line_size: 64,
            max_read_lines: 512,
            max_write_lines: 128,
        }
    }
}

/// Non-TSX memory hierarchy costs.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct MemoryCharacteristics {
    /// L1 cache hit (cycles).
    pub l1_hit_cycles: f64,
    /// L2 cache hit (cycles).
    pub l2_hit_cycles: f64,
    /// L3 cache hit (cycles).
    pub l3_hit_cycles: f64,
    /// RAM access (cycles).
    pub ram_cycles: f64,
}

impl Default for MemoryCharacteristics {
    fn default() -> Self {
        MemoryCharacteristics {
            l1_hit_cycles: 4.0,
            l2_hit_cycles: 12.0,
            l3_hit_cycles: 40.0,
            ram_cycles: 200.0,
        }
    }
}

/// Per-backend cost model characteristics.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct BackendCharacteristics {
    /// Name of the backend (e.g., "tsxsgl", "norec", "tinystm").
    pub backend: String,
    /// Transaction begin overhead (cycles).
    pub begin_overhead: f64,
    /// Transaction commit overhead (cycles).
    pub commit_overhead: f64,
    /// Transaction abort overhead (cycles).
    pub abort_overhead: f64,
    /// Per read operation overhead (cycles).
    pub read_overhead: f64,
    /// Per write operation overhead (cycles).
    pub write_overhead: f64,
    /// Validation cost per read-set entry (cycles).
    pub validation_entry_cost: f64,
    /// Lock acquisition cost (cycles).
    pub lock_acquire_cost: f64,
}

impl Default for BackendCharacteristics {
    fn default() -> Self {
        BackendCharacteristics {
            backend: "default".into(),
            begin_overhead: 50.0,
            commit_overhead: 40.0,
            abort_overhead: 100.0,
            read_overhead: 5.0,
            write_overhead: 6.0,
            validation_entry_cost: 3.0,
            lock_acquire_cost: 30.0,
        }
    }
}

/// Complete machine profile: hardware + per-backend costs.
///
/// Collected by running the profiling benchmarks on the target
/// hardware and saving the output as JSON.  The simulator loads
/// this file at startup and uses it to estimate event costs.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct MachineProfile {
    /// CPU model string.
    pub cpu: String,
    /// Nominal CPU frequency in GHz.
    pub freq_ghz: f64,
    /// TSX hardware characteristics.
    pub tsx: TsxCharacteristics,
    /// Memory hierarchy characteristics.
    pub memory: MemoryCharacteristics,
    /// Per-backend cost characteristics.
    pub backends: Vec<BackendCharacteristics>,

    // Metadata
    /// Date collected.
    pub collected: String,
    /// Description.
    pub description: String,
}

impl MachineProfile {
    /// Default Skylake profile — used when no profiling data is available.
    pub fn skylake_default() -> Self {
        MachineProfile {
            cpu: "Intel Skylake (estimated)".into(),
            freq_ghz: 3.0,
            tsx: TsxCharacteristics::default(),
            memory: MemoryCharacteristics::default(),
            backends: vec![BackendCharacteristics::default()],
            collected: "built-in".into(),
            description: "Default Skylake cycle costs (uncalibrated)".into(),
        }
    }

    /// Load from a JSON file.
    pub fn load(path: &Path) -> Result<Self, String> {
        let data = std::fs::read_to_string(path)
            .map_err(|e| format!("Cannot read machine profile {}: {}", path.display(), e))?;
        let profile: MachineProfile = serde_json::from_str(&data)
            .map_err(|e| format!("Cannot parse machine profile {}: {}", path.display(), e))?;
        Ok(profile)
    }

    /// Save to a JSON file.
    pub fn save(&self, path: &Path) -> Result<(), String> {
        let data = serde_json::to_string_pretty(self)
            .map_err(|e| format!("Cannot serialize machine profile: {}", e))?;
        std::fs::write(path, data)
            .map_err(|e| format!("Cannot write machine profile {}: {}", path.display(), e))?;
        Ok(())
    }

    /// Get characteristics for a specific backend, or return defaults.
    pub fn backend(&self, name: &str) -> BackendCharacteristics {
        self.backends
            .iter()
            .find(|b| b.backend == name || b.backend == "default")
            .cloned()
            .unwrap_or_default()
    }
}
