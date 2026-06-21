// ── tm-sim: TM Simulator ─────────────────────────────────
// Runs a real TM backend through a trace file using the
// `simulation` feature flag for deterministic replay.
//
// Two clock modes:
//   Timestamp — events processed at trace timestamps (original)
//   Cost      — events processed while accumulating estimated
//               cycle costs; the backend still detects conflicts
//               and aborts naturally, so throughput varies with
//               thread count (unlike the abstract DES path)
//
// Usage:
//   tm-sim [--backend norec] [--trace trace.jsonl] [--clock-mode cost]

use clap::Parser;
use std::collections::HashMap;
use tm_des::backend::Backend;
use tm_des::cost_model::{BackendProfile, CalibratedCostModel};
use tm_des::event::{Event, EventKind};
use tm_des::machine_profile::MachineProfile;
use tm_des::sim_engine::{SimClockMode, SimEngine};
use tm_des::trace::Trace;

#[derive(Parser, Debug)]
#[command(name = "tm-sim", about = "Replay TM trace through the real backend",
          version = "0.1.0")]
struct Cli {
    /// Trace file (JSONL) to replay.
    #[arg(short, long, default_value = "-")]
    trace: String,

    /// Backend to use: norec, tl2, tinystm, romulus, swisstm, tsx-sim.
    #[arg(short, long, default_value = "norec")]
    backend: String,

    /// Clock mode: timestamp or cost.
    #[arg(long, default_value = "timestamp")]
    clock_mode: String,

    /// Machine profile JSON (e.g. machine_profiles/broadwell_ep_v4.json).
    #[arg(long)]
    machine_profile: Option<String>,

    /// CPU frequency in GHz (for wall-time estimation).
    #[arg(long)]
    freq_ghz: Option<f64>,

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
        eprintln!("Unknown backend '{}'. Available: norec, tl2, tinystm, romulus, swisstm, tsx-sim", cli.backend);
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

    // Build engine and initialize with addresses from the trace
    let mut engine = SimEngine::new(backend);
    engine.init_from_events(&events);

    // Set up cost mode
    match cli.clock_mode.as_str() {
        "timestamp" => {}
        "cost" => {
            let freq = cli.freq_ghz.unwrap_or(3.0);
            let model = if let Some(path) = &cli.machine_profile {
                let data: String = std::fs::read_to_string(path)
                    .unwrap_or_else(|e| { eprintln!("Cannot read {}: {}", path, e); std::process::exit(1); });
                let mp: MachineProfile = serde_json::from_str(&data)
                    .unwrap_or_else(|e| { eprintln!("JSON parse error in {}: {}", path, e); std::process::exit(1); });
                let bp = BackendProfile::from_name(&cli.backend);
                let cm = CalibratedCostModel::from_profile(&mp, bp);
                eprintln!("Cost mode: machine={} backend={:?} freq={} GHz",
                    mp.cpu, bp, freq);
                cm
            } else {
                eprintln!("Cost mode: defaults (no machine profile)");
                CalibratedCostModel::default()
            };
            engine.set_cost_mode(model, freq);
        }
        other => {
            eprintln!("Unknown clock mode '{}'. Use 'timestamp' or 'cost'", other);
            std::process::exit(1);
        }
    }

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

    // Report estimated time in cost mode
    if engine.sim_clock_mode == SimClockMode::Cost {
        let secs = engine.estimated_cycles as f64 / (engine.freq_ghz * 1e9);
        eprintln!(
            "═══ cost mode: {} cycles ≈ {:.4}s @ {} GHz ═══",
            engine.estimated_cycles, secs, engine.freq_ghz
        );
    }

    // Report internal sync counters
    let stats = engine.backend.take_stats();
    engine.backend.print_stats(&stats);
}
