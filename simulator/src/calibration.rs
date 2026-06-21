// ── Calibration: profiling data → MachineProfile ───────────
// Converts raw profiling output into a MachineProfile JSON file
// that can be loaded by the simulator on any machine.
//
// The profiling data is collected by running the TSX profiling
// patch on real hardware.  The output (TSX_STATS lines) is
// parsed and converted into a hardware-agnostic profile.

use crate::machine_profile::{BackendCharacteristics, MachineProfile, MemoryCharacteristics, TsxCharacteristics};
use std::collections::HashMap;
use std::path::Path;

/// A single calibration record from TSX profiling data.
///
/// Multiple records (from different benchmarks/variants) can be
/// combined to produce a single MachineProfile.
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CalibrationRecord {
    pub xbegin_ok_cycles: f64,
    pub xbegin_abort_cycles: f64,
    pub xend_cycles: f64,
    pub xabort_cycles: f64,
    pub read_cycles: f64,
    pub write_cycles: f64,
    pub sgl_begin_cycles: f64,
    pub sgl_end_cycles: f64,
    pub sgl_spin_cycles: f64,
    pub depth_cycles: f64,
    pub abort_rate_pct: f64,
    pub avg_reads_per_tx: f64,
    pub avg_writes_per_tx: f64,
    pub capacity_abort_pct: f64,
    pub conflict_abort_pct: f64,
    pub samples: usize,
}

/// Load calibration records from a JSON file.
pub fn load_calibration(path: &Path) -> Result<HashMap<String, CalibrationRecord>, String> {
    let data = std::fs::read_to_string(path)
        .map_err(|e| format!("Cannot read {}: {}", path.display(), e))?;
    let records: HashMap<String, CalibrationRecord> = serde_json::from_str(&data)
        .map_err(|e| format!("Cannot parse {}: {}", path.display(), e))?;
    Ok(records)
}

/// Compute a weighted average of calibration records.
pub fn average_records(records: &HashMap<String, CalibrationRecord>) -> Option<CalibrationRecord> {
    if records.is_empty() { return None; }
    let n = records.len() as f64;
    Some(CalibrationRecord {
        xbegin_ok_cycles: records.values().map(|r| r.xbegin_ok_cycles).sum::<f64>() / n,
        xbegin_abort_cycles: records.values().map(|r| r.xbegin_abort_cycles).sum::<f64>() / n,
        xend_cycles: records.values().map(|r| r.xend_cycles).sum::<f64>() / n,
        xabort_cycles: records.values().map(|r| r.xabort_cycles).sum::<f64>() / n,
        read_cycles: records.values().map(|r| r.read_cycles).sum::<f64>() / n,
        write_cycles: records.values().map(|r| r.write_cycles).sum::<f64>() / n,
        sgl_begin_cycles: records.values().map(|r| r.sgl_begin_cycles).sum::<f64>() / n,
        sgl_end_cycles: records.values().map(|r| r.sgl_end_cycles).sum::<f64>() / n,
        sgl_spin_cycles: records.values().map(|r| r.sgl_spin_cycles).sum::<f64>() / n,
        depth_cycles: records.values().map(|r| r.depth_cycles).sum::<f64>() / n,
        abort_rate_pct: records.values().map(|r| r.abort_rate_pct).sum::<f64>() / n,
        avg_reads_per_tx: records.values().map(|r| r.avg_reads_per_tx).sum::<f64>() / n,
        avg_writes_per_tx: records.values().map(|r| r.avg_writes_per_tx).sum::<f64>() / n,
        capacity_abort_pct: records.values().map(|r| r.capacity_abort_pct).sum::<f64>() / n,
        conflict_abort_pct: records.values().map(|r| r.conflict_abort_pct).sum::<f64>() / n,
        samples: records.values().map(|r| r.samples).sum(),
    })
}

/// Convert calibration records to a MachineProfile.
///
/// The profiling data provides measured cycle costs for TSX
/// operations.  We average across all available records to
/// produce a hardware profile.
pub fn calibration_to_machine_profile(
    records: &HashMap<String, CalibrationRecord>,
    cpu: &str,
    freq_ghz: f64,
    description: &str,
) -> MachineProfile {
    let avg = average_records(records).unwrap_or(CalibrationRecord {
        xbegin_ok_cycles: 20.0,
        xbegin_abort_cycles: 1500.0,
        xend_cycles: 80.0,
        xabort_cycles: 1500.0,
        read_cycles: 4.0,
        write_cycles: 5.0,
        sgl_begin_cycles: 100.0,
        sgl_end_cycles: 100.0,
        sgl_spin_cycles: 0.0,
        depth_cycles: 0.0,
        abort_rate_pct: 0.0,
        avg_reads_per_tx: 0.0,
        avg_writes_per_tx: 0.0,
        capacity_abort_pct: 0.0,
        conflict_abort_pct: 0.0,
        samples: 0,
    });

    MachineProfile {
        cpu: cpu.to_string(),
        freq_ghz,
        tsx: TsxCharacteristics {
            xbegin_cycles: avg.xbegin_ok_cycles,
            xend_cycles: avg.xend_cycles,
            xabort_cycles: avg.xabort_cycles,
            read_l1_cycles: avg.read_cycles,
            write_l1_cycles: avg.write_cycles,
            bloom_check_cycles: 2.0, // estimated; profiling can't isolate this
            mutex_lock_cycles: avg.sgl_begin_cycles,
            mutex_unlock_cycles: avg.sgl_end_cycles,
            conflict_abort_penalty: 2000.0, // estimated
            cache_line_size: 64,
            max_read_lines: 512,
            max_write_lines: 128,
        },
        memory: MemoryCharacteristics::default(),
        backends: vec![
            BackendCharacteristics {
                backend: "default".into(),
                begin_overhead: avg.xbegin_ok_cycles,
                commit_overhead: avg.xend_cycles,
                abort_overhead: avg.xabort_cycles,
                read_overhead: 2.0, // tracking overhead beyond L1
                write_overhead: 2.0,
                validation_entry_cost: 3.0,
                lock_acquire_cost: avg.sgl_begin_cycles,
            },
            BackendCharacteristics {
                backend: "tsxsgl".into(),
                begin_overhead: avg.xbegin_ok_cycles,
                commit_overhead: avg.xend_cycles,
                abort_overhead: avg.xabort_cycles,
                read_overhead: avg.read_cycles - avg.xbegin_ok_cycles,
                write_overhead: avg.write_cycles - avg.xbegin_ok_cycles,
                validation_entry_cost: 0.0, // TSX has no explicit validation
                lock_acquire_cost: avg.sgl_begin_cycles,
            },
        ],
        collected: "auto-generated".into(),
        description: description.to_string(),
    }
}

/// Build a MachineProfile directly from TSX_STATS output lines
/// (collected by running the profiling patch on the target machine).
pub fn machine_profile_from_tsx_stats(
    tsx_stats_lines: &[&str],
    cpu: &str,
    freq_ghz: f64,
) -> Result<MachineProfile, String> {
    // Parse each TSX_STATS line and average
    let mut xbegin_ok_acc = 0.0f64;
    let mut xend_acc = 0.0f64;
    let mut xabort_acc = 0.0f64;
    let mut read_acc = 0.0f64;
    let mut write_acc = 0.0f64;
    let mut count = 0u64;

    for line in tsx_stats_lines {
        if !line.trim().starts_with("TSX_STATS:") { continue; }
        let body = line.trim().strip_prefix("TSX_STATS:").unwrap_or("");
        for part in body.split_whitespace() {
            let kv: Vec<&str> = part.split('=').collect();
            if kv.len() != 2 { continue; }
            let (key, val) = (kv[0], kv[1]);
            let paren = val.find('(');
            let cnt: u64 = if let Some(p) = paren {
                val[..p].parse().unwrap_or(0)
            } else { 0 };
            let acc: f64 = if let Some(p) = paren {
                val[p+1..val.len()-1].parse().unwrap_or(0.0)
            } else { 0.0 };
            if cnt == 0 { continue; }
            let avg = acc / cnt as f64;

            match key {
                "xbegin_ok" => { xbegin_ok_acc += avg; count += 1; }
                "xend" => { xend_acc += avg; }
                "xabort" => { xabort_acc += avg; }
                "read" => { read_acc += avg; }
                "write" => { write_acc += avg; }
                _ => {}
            }
        }
    }

    if count == 0 {
        return Err("No valid TSX_STATS lines found".into());
    }

    let n = count as f64;
    Ok(MachineProfile {
        cpu: cpu.to_string(),
        freq_ghz,
        tsx: TsxCharacteristics {
            xbegin_cycles: xbegin_ok_acc / n,
            xend_cycles: xend_acc / n,
            xabort_cycles: xabort_acc / n,
            read_l1_cycles: read_acc / n,
            write_l1_cycles: write_acc / n,
            bloom_check_cycles: 2.0,
            mutex_lock_cycles: 100.0,
            mutex_unlock_cycles: 100.0,
            conflict_abort_penalty: 2000.0,
            cache_line_size: 64,
            max_read_lines: 512,
            max_write_lines: 128,
        },
        memory: MemoryCharacteristics::default(),
        backends: vec![BackendCharacteristics::default()],
        collected: "from TSX_STATS".into(),
        description: format!("Machine profile from {} TSX_STATS lines", count),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn test_load_calibration() {
        let json = r#"{
            "fuzz_counter": {
                "xbegin_ok_cycles": 18.5,
                "xbegin_abort_cycles": 1450.0,
                "xend_cycles": 75.2,
                "xabort_cycles": 1520.0,
                "read_cycles": 5.8,
                "write_cycles": 6.2,
                "sgl_begin_cycles": 95.0,
                "sgl_end_cycles": 85.0,
                "sgl_spin_cycles": 10.0,
                "depth_cycles": 1200.0,
                "abort_rate_pct": 12.3,
                "avg_reads_per_tx": 4.2,
                "avg_writes_per_tx": 2.1,
                "capacity_abort_pct": 5.0,
                "conflict_abort_pct": 70.0,
                "samples": 4
            }
        }"#;

        let dir = std::env::temp_dir();
        let path = dir.join("test_calib.json");
        {
            let mut f = std::fs::File::create(&path).unwrap();
            f.write_all(json.as_bytes()).unwrap();
        }
        let records = load_calibration(&path).unwrap();
        assert_eq!(records.len(), 1);

        let profile = calibration_to_machine_profile(&records, "TestCPU", 3.0, "test");
        assert!((profile.tsx.xbegin_cycles - 18.5).abs() < 0.1);
        assert!((profile.tsx.xend_cycles - 75.2).abs() < 0.1);
        assert!((profile.tsx.read_l1_cycles - 5.8).abs() < 0.1);

        let _ = std::fs::remove_file(&path);
    }
}
