// ── tm-sim: TM Simulator ─────────────────────────────────
// Runs a real TM backend through a trace file using the
// `simulation` feature flag for deterministic replay.
//
// Usage:
//   tm-sim [--backend norec] [--trace trace.jsonl]

use clap::Parser;
use std::collections::HashMap;
use tm_des::backend::Backend;
use tm_des::event::{Event, EventKind};
use tm_des::sim_engine::SimEngine;
use tm_des::trace::Trace;

#[derive(Parser, Debug)]
#[command(name = "tm-sim", about = "Replay TM trace through the real backend",
          version = "0.1.0")]
struct Cli {
    /// Trace file (JSONL) to replay.
    #[arg(short, long, default_value = "-")]
    trace: String,

    /// Backend to use: norec, tl2, tinystm.
    #[arg(short, long, default_value = "norec")]
    backend: String,

    /// Stop after N events (0 = unlimited).
    #[arg(long, default_value = "0")]
    max_events: u64,

    /// Initial committed values file (JSON: {"addr_hex": value, ...}).
    #[arg(long)]
    initial_values: Option<String>,
}

fn main() {
    let cli = Cli::parse();
    let Some(backend) = Backend::from_name(&cli.backend) else {
        eprintln!("Unknown backend '{}'. Available: norec, tl2, tinystm", cli.backend);
        std::process::exit(1);
    };

    eprintln!("TM Simulator — backend={}", backend.name());

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
    let mut engine = SimEngine::new(backend);
    engine.init();

    // Load initial committed values for money conservation check
    if let Some(path) = &cli.initial_values {
        let data: String = std::fs::read_to_string(path)
            .unwrap_or_else(|e| { eprintln!("Cannot read {}: {}", path, e); std::process::exit(1); });
        let initial: HashMap<String, u64> = serde_json::from_str(&data)
            .unwrap_or_else(|e| { eprintln!("JSON parse error in {}: {}", path, e); std::process::exit(1); });
        for (k, v) in &initial {
            let addr = u64::from_str_radix(k.trim_start_matches("0x"), 16)
                .unwrap_or_else(|_| { eprintln!("Bad addr '{}'", k); std::process::exit(1); });
            engine.verifier.set_initial_value(addr, *v);
        }
        engine.verifier.initial_value_sum = engine.verifier.total_value();
        eprintln!("Loaded {} initial committed values", initial.len());
    }

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

        if matches!(event.kind, EventKind::Checkpoint) {
            eprintln!("  Scenario {}: {} events processed", scenario, i - scenario_start);
            engine.reset();
            scenario += 1;
            scenario_start = i + 1;
            continue;
        }

        engine.process_event(event);
    }

    // Report via Verifier
    eprintln!();
    eprintln!("═══ tm-sim report ═══");
    for line in engine.verifier.report() {
        eprintln!("{}", line);
    }
}
