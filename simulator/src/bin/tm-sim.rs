// ── tm-sim: TM Simulator ─────────────────────────────────
// Runs a real TM backend through a trace file using the
// `simulation` feature flag for deterministic replay.
//
// Usage:
//   tm-sim [--backend norec] [--trace trace.jsonl]

use clap::Parser;
use tm_des::event::{Event, EventKind};
use tm_des::sim_engine::SimEngine;
use tm_des::trace::Trace;

#[derive(Parser, Debug)]
#[command(name = "tm-sim", about = "Replay TM trace through the real backend")]
struct Cli {
    /// Trace file (JSONL) to replay.
    #[arg(short, long, default_value = "-")]
    trace: String,

    /// Backend to use (currently only "norec").
    #[arg(short, long, default_value = "norec")]
    backend: String,

    /// Stop after N events (0 = unlimited).
    #[arg(long, default_value = "0")]
    max_events: u64,
}

fn main() {
    let cli = Cli::parse();

    eprintln!("TM Simulator — backend={}", cli.backend);

    // Load trace
    let events: Vec<Event> = if cli.trace == "-" {
        Trace::from_jsonl(std::io::stdin())
    } else {
        Trace::from_jsonl_file(&cli.trace)
    }
    .unwrap_or_else(|e| {
        eprintln!("Trace error: {}", e);
        std::process::exit(1);
    })
    .events;

    eprintln!("Loaded {} events", events.len());

    // Build engine and run
    let mut engine = SimEngine::new();
    engine.init();

    let limit = if cli.max_events > 0 {
        cli.max_events as usize
    } else {
        events.len()
    };

    let mut scenario = 0u64;
    let mut scenario_start = 0usize;

    for (i, event) in events.iter().enumerate() {
        if i >= limit {
            break;
        }

        // Track scenario boundaries (Checkpoint events separate scenarios)
        if matches!(event.kind, EventKind::Checkpoint) {
            eprintln!("  Scenario {}: {} events processed", scenario, i - scenario_start);
            engine.reset();
            scenario += 1;
            scenario_start = i + 1;
            continue;
        }

        engine.process_event(event);
    }

    // Report
    let s = &engine.stats;
    eprintln!();
    eprintln!("═══ tm-sim report ═══");
    eprintln!("Events: {}  Commits: {}  Aborts: {}  Abort rate: {:.1}%",
              s.total_events, s.commits, s.aborts, s.abort_rate());
    if s.reads_outside_tx > 0 {
        eprintln!("⚠ {} reads outside transaction", s.reads_outside_tx);
    }
    if s.writes_outside_tx > 0 {
        eprintln!("⚠ {} writes outside transaction", s.writes_outside_tx);
    }

    // Value-based verification: sum all committed values
    // (NOrec writes back to real memory — we can't inspect from here directly.
    //  Full verification needs shadow memory, coming in Phase 4.)
    eprintln!();
    eprintln!("NOTE: Value verification requires shadow memory (Phase 4)");
}
