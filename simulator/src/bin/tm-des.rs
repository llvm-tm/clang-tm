use clap::Parser;
use tm_des::{Cli, engine::SimState, trace::Trace, ser};

fn main() {
    let cli = Cli::parse();

    // Optional checkpoint restore
    let mut state = if let Some(cp_path) = &cli.checkpoint {
        match ser::load_checkpoint(cp_path) {
            Ok(s) => {
                eprintln!("Restored from checkpoint '{}'", cp_path);
                s
            }
            Err(e) => {
                eprintln!("Failed to load checkpoint: {}; starting fresh", e);
                SimState::new(1, 0x7f00_0000_0000, 0x0000_0000_1000_0000)
            }
        }
    } else {
        SimState::new(1, 0x7f00_0000_0000, 0x0000_0000_1000_0000)
    };
    state.checker.livelock_threshold = cli.livelock_threshold;

    // Load trace
    let events = if cli.trace == "-" {
        Trace::from_jsonl(std::io::stdin())
    } else {
        Trace::from_jsonl_file(&cli.trace)
    };
    let events = match events {
        Ok(t) => {
            eprintln!("Loaded {} events from trace", t.events.len());
            t.events
        }
        Err(e) => {
            eprintln!("Trace error: {}", e);
            std::process::exit(1);
        }
    };
    state.load_events(events);

    // Run simulation
    let initial_count = state.events_processed;
    let processed = state.run(cli.max_events);

    // Periodic checkpoint
    if cli.checkpoint_every > 0 && !processed.is_empty() {
        let n = state.events_processed - initial_count;
        if n >= cli.checkpoint_every {
            let cp_path = format!("tm-des.checkpoint.{}", n);
            if let Err(e) = ser::save_checkpoint(&state, &cp_path) {
                eprintln!("Checkpoint error: {}", e);
            } else {
                eprintln!("Checkpoint saved to '{}'", cp_path);
            }
        }
    }

    // Final report
    eprintln!("Processed {} events ({} new)", state.events_processed, processed.len());
    eprintln!("Clock: {}", state.clock);

    let warnings = state.checker.finalize();
    if warnings.is_empty() {
        eprintln!("All TM checks PASSED");
    } else {
        eprintln!("TM checks:");
        for w in &warnings {
            eprintln!("  WARNING: {}", w);
        }
    }

    // Write processed events to output file
    if !processed.is_empty() {
        let out_path = cli.output.as_deref().unwrap_or("tm-des.output.jsonl");
        if let Err(e) = (tm_des::trace::Trace { events: processed }).to_jsonl(
            &mut std::fs::File::create(out_path).unwrap()
        ) {
            eprintln!("Output error: {}", e);
        } else {
            eprintln!("Output written to '{}'", out_path);
        }
    }
}
