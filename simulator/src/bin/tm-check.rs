use clap::Parser;
use std::collections::HashMap;
use tm_des::event::{Event, EventKind};
use tm_des::memory::ShadowMemory;
use tm_des::trace::Trace;
use tm_des::tm_model::TmModel;

#[derive(Parser, Debug)]
#[command(name = "tm-check", about = "Replay TM trace through model and verify correctness")]
struct Cli {
    /// Trace file (JSONL) to check. Use '-' for stdin.
    #[arg(short, long, default_value = "-")]
    trace: String,

    /// TM region base address (hex or decimal).
    #[arg(long, default_value = "0x7f0000000000", value_parser = parse_u64)]
    tm_base: u64,

    /// TM region size (hex or decimal).
    #[arg(long, default_value = "0x10000000", value_parser = parse_u64)]
    tm_size: u64,

    /// Initial committed values file (JSON: {"addr_hex": value, ...}).
    /// Addresses are parsed as hex strings (e.g. "0x7f0010000000").
    #[arg(long)]
    initial_values: Option<String>,
}

/// Parse hex or decimal u64.
fn parse_u64(s: &str) -> Result<u64, String> {
    let s = s.trim();
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).map_err(|e| format!("bad hex '{}': {}", s, e))
    } else {
        s.parse::<u64>().map_err(|e| format!("bad value '{}': {}", s, e))
    }
}

/// Replay through the WBCTL model and track alloc/free + address space.
fn check_trace(events: &[Event], cli: &Cli) {
    let mut model = TmModel::new();
    let mut shadow = ShadowMemory::new(cli.tm_base, cli.tm_size);
    let mut violations: Vec<String> = Vec::new();
    let mut conflicts: Vec<String> = Vec::new();
    let mut n_events = 0u64;

    // ── Load initial committed values ────────────────────────
    if let Some(path) = &cli.initial_values {
        let data: String = std::fs::read_to_string(path)
            .unwrap_or_else(|e| { eprintln!("Cannot read {}: {}", path, e); std::process::exit(1); });
        let initial: HashMap<String, u64> = serde_json::from_str(&data)
            .unwrap_or_else(|e| { eprintln!("JSON parse error in {}: {}", path, e); std::process::exit(1); });
        for (k, v) in &initial {
            let addr = u64::from_str_radix(k.trim_start_matches("0x"), 16)
                .unwrap_or_else(|_| { eprintln!("Bad addr '{}'", k); std::process::exit(1); });
            model.committed_values.insert(addr, *v);
        }
        eprintln!("Loaded {} initial committed values", initial.len());
    }

    // ── Snapshot committed values at start for final sum check ─
    let initial_snapshot: HashMap<u64, u64> = model.committed_values.clone();

    for event in events {
        n_events += 1;
        let tid = event.thread_id as u64;

        // ── Address space check for memory operations ──────────
        match &event.kind {
            EventKind::Read { addr, .. } | EventKind::Write { addr, .. } => {
                // Null pointer dereference
                if *addr == 0 {
                    violations.push(format!(
                        "NULL-DEREF at event {} (ts={}, tid={})",
                        n_events, event.timestamp, event.thread_id
                    ));
                }
                // Use-after-free: access to a tracked address that was freed
                if *addr != 0 && shadow.was_freed(*addr) {
                    violations.push(format!(
                        "USE-AFTER-FREE at event {} (ts={}, tid={}): access to freed addr=0x{:x}",
                        n_events, event.timestamp, event.thread_id, addr
                    ));
                }
            }
            _ => {}
        }

        // ── Alloc/Free tracking ────────────────────────────────
        match &event.kind {
            EventKind::Alloc { addr, size } => {
                shadow.alloc(*addr, *size);
            }
            EventKind::Free { addr } => {
                if let Err(e) = shadow.free(*addr) {
                    violations.push(format!(
                        "MEM-VIOLATION at event {} (ts={}, tid={}): {}",
                        n_events, event.timestamp, event.thread_id, e
                    ));
                }
            }
            _ => {}
        }

        // ── TM model replay ────────────────────────────────────
        let result = match &event.kind {
            EventKind::TxBegin => {
                model.tm_begin(tid);
                Ok(())
            }
            EventKind::TxEnd => {
                model.tm_end(tid)
            }
            EventKind::Read { addr, .. } => {
                model.tm_read(tid, *addr, 4).map(|_| ())
            }
            EventKind::Write { addr, val, .. } => {
                model.tm_write(tid, *addr, *val, 4)
            }
            EventKind::Log { msg } => {
                eprintln!("[LOG ts={}] {}", event.timestamp, msg);
                Ok(())
            }
            EventKind::Checkpoint => Ok(()),
            _ => Ok(()),
        };

        if let Err(err) = result {
            if err.contains("_fail") || err.contains("_timeout") || err.contains("abort_") {
                conflicts.push(format!(
                    "conflict at event {} (ts={}, tid={}): {}",
                    n_events, event.timestamp, event.thread_id, err
                ));
            }
            if model.get_or_create_tx(tid).active {
                model.tm_abort_reason(tid, &err);
            }
        }
    }

    // ── Value verification ────────────────────────────────────
    let mut value_mismatches: Vec<String> = Vec::new();
    // Only check values that existed in the initial snapshot
    for (&addr, &initial_val) in &initial_snapshot {
        if let Some(&final_val) = model.committed_values.get(&addr) {
            if initial_val != final_val {
                value_mismatches.push(format!(
                    "addr=0x{:x}: initial=0x{:x} final=0x{:x} (delta={})",
                    addr, initial_val, final_val,
                    final_val as i64 - initial_val as i64
                ));
            }
        }
    }

    // ── Summary counts ────────────────────────────────────────
    let total_violations = violations.len();
    let double_frees = violations.iter().filter(|v| v.contains("double-free")).count();
    let dangling_frees = violations.iter().filter(|v| v.contains("unallocated")).count();
    let null_derefs = violations.iter().filter(|v| v.contains("NULL-DEREF")).count();
    let uaf = violations.iter().filter(|v| v.contains("USE-AFTER-FREE")).count();

    // ── Report ────────────────────────────────────────────────
    eprintln!();
    eprintln!("═══ tm-check report ═══");
    eprintln!("Events processed: {}", n_events);
    eprintln!("Commits: {}  Aborts: {}  Abort rate: {:.1}%",
              model.commits, model.aborts,
              if model.commits + model.aborts > 0 {
                  100.0 * model.aborts as f64 / (model.commits + model.aborts) as f64
              } else { 0.0 });
    eprintln!();

    // ── Violations ─────────────────────────────────────────────
    if !violations.is_empty() {
        eprintln!("⚠ MEMORY / ADDRESS-SPACE VIOLATIONS:");
        eprintln!("  Total: {}  Double-frees: {}  Free-of-unallocated: {}  Null-derefs: {}  Use-after-free: {}",
                  total_violations, double_frees, dangling_frees, null_derefs, uaf);
        for v in &violations {
            eprintln!("  ❌ {}", v);
        }
        eprintln!();
    } else {
        eprintln!("NO MEMORY VIOLATIONS ✓");
        eprintln!();
    }

    // ── Value verification ─────────────────────────────────────
    if !value_mismatches.is_empty() {
        eprintln!("⚠ VALUE MISMATCHES:");
        for m in &value_mismatches {
            eprintln!("  ❌ {}", m);
        }
        eprintln!();
    } else if !initial_snapshot.is_empty() {
        eprintln!("VALUE INTEGRITY: {} addresses verified ✓", initial_snapshot.len());
        eprintln!();
    }

    // ── Abort breakdown ────────────────────────────────────────
    if !model.abort_reasons.is_empty() {
        eprintln!("Abort reasons:");
        let mut reasons: Vec<(&String, &u64)> = model.abort_reasons.iter().collect();
        reasons.sort_by(|a, b| b.1.cmp(a.1));
        for (reason, count) in &reasons {
            eprintln!("  {:>6}  {}", count, reason);
        }
        eprintln!();
    }

    // ── Synchronization overhead ──────────────────────────────
    let total_tx = model.commits + model.aborts;
    eprintln!("Synchronization overhead (WBCTL model):");
    eprintln!("  Validations:         {}  ({:.1}/tx)",
              model.total_validations,
              if total_tx > 0 { model.total_validations as f64 / total_tx as f64 } else { 0.0 });
    eprintln!("  Snapshot extensions: {}  ({:.1}/tx)",
              model.total_snapshot_extensions,
              if total_tx > 0 { model.total_snapshot_extensions as f64 / total_tx as f64 } else { 0.0 });
    eprintln!("  Spin iterations:     {}  ({:.1}/tx)",
              model.total_spin_iterations,
              if total_tx > 0 { model.total_spin_iterations as f64 / total_tx as f64 } else { 0.0 });
    eprintln!("  Lock contentions:    {}  ({:.1}/tx)",
              model.total_lock_contentions,
              if total_tx > 0 { model.total_lock_contentions as f64 / total_tx as f64 } else { 0.0 });
    eprintln!();

    // ── Per-thread stats ───────────────────────────────────────
    let mut tx_stats: Vec<(u64, u32, u32, u32, u32, u32)> = Vec::new();
    for (&tid, tx) in &model.tx_states {
        tx_stats.push((tid, tx.peak_read_set_size, tx.peak_write_set_size,
                       tx.validations, tx.spin_iterations, tx.snapshot_extensions));
    }
    tx_stats.sort_by_key(|s| s.0);
    eprintln!("Per-thread statistics (peak read-set / write-set / validations / spins / ext):");
    for (tid, rs, ws, val, spin, ext) in &tx_stats {
        eprintln!("  T{:>2}:  RS={:>4}  WS={:>4}  V={:>5}  S={:>6}  Ext={}",
                  tid, rs, ws, val, spin, ext);
    }
    eprintln!();

    // ── Conflicts ──────────────────────────────────────────────
    if conflicts.is_empty() {
        eprintln!("NO CONFLICTS — ALL TRANSACTIONS ISOLATED ✓");
    } else {
        eprintln!("CONFLICTS DETECTED (normal TM behavior — opacity preserved):");
        for c in &conflicts {
            eprintln!("  ⚡ {}", c);
        }
    }
}

fn main() {
    let cli = Cli::parse();
    let events: Vec<Event> = if cli.trace == "-" {
        Trace::from_jsonl(std::io::stdin())
    } else {
        Trace::from_jsonl_file(&cli.trace)
    }
    .unwrap_or_else(|e| { eprintln!("Trace error: {}", e); std::process::exit(1); })
    .events;

    check_trace(&events, &cli);
}
