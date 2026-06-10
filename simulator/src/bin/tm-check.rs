use clap::Parser;
use tm_des::event::{Event, EventKind};
use tm_des::trace::Trace;
use tm_des::tm_model::TmModel;

#[derive(Parser, Debug)]
#[command(name = "tm-check", about = "Replay TM trace through model and verify correctness")]
struct Cli {
    /// Trace file (JSONL) to check.
    trace: String,
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

    let mut model = TmModel::new();
    let mut conflicts: Vec<String> = Vec::new();
    let mut n_events = 0u64;

    for event in &events {
        n_events += 1;
        let tid = event.thread_id as u64;

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
            if matches!(event.kind, EventKind::TxEnd) {
                model.tm_begin(tid);
            }
        }
    }

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
