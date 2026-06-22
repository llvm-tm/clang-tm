use clap::Parser;
use std::path::Path;
use tm_des::{
    Cli,
    calibration::{self, calibration_to_machine_profile},
    cost_model::BackendProfile,
    engine::{ClockMode, SimState},
    machine_profile::MachineProfile,
    trace::Trace,
    ser,
};

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
    state.retry_cost_multiplier = cli.retry_cost_multiplier;
    if let Some(freq) = cli.effective_freq {
        state.effective_freq_ghz = freq;
        eprintln!("Effective frequency: {:.2} GHz (manual override)", freq);
    }

    // Apply calibration or machine profile from CLI (calibration takes precedence)
    if let Some(ref cal_path) = cli.calibration {
        match calibration::load_calibration(Path::new(cal_path)) {
            Ok(records) => {
                let profile = calibration_to_machine_profile(&records, "calibrated", 0.0,
                    &format!("Auto-generated from {}", cal_path));
                // Compute effective frequency if records contain depth_cycles info
                if let Some(avg) = calibration::average_records(&records) {
                    if avg.depth_cycles > 0.0 && avg.samples > 0 {
                        // depth_cycles = total tx cycles from profiling
                        // freq = cycles / time — but we don't have wall time here.
                        // Store the nominal freq from the profile, effective freq
                        // can be provided via --effective-freq.
                        state.effective_freq_ghz = profile.freq_ghz;
                    }
                }
                eprintln!("Loaded calibration from '{}': {} records, {} backends",
                          cal_path, records.len(), profile.backends.len());
                state.set_machine_profile(profile);
            }
            Err(e) => {
                eprintln!("Warning: failed to load calibration '{}': {}; using defaults", cal_path, e);
            }
        }
    } else if let Some(ref mp_path) = cli.machine_profile {
        match MachineProfile::load(Path::new(mp_path)) {
            Ok(profile) => {
                eprintln!("Loaded machine profile from '{}': {} @ {:.1} GHz",
                          mp_path, profile.cpu, profile.freq_ghz);
                state.set_machine_profile(profile);
            }
            Err(e) => {
                eprintln!("Warning: failed to load machine profile '{}': {}; using defaults", mp_path, e);
            }
        }
    }

    // Apply backend profile from CLI
    if cli.backend != "default" {
        let backend = BackendProfile::from_name(&cli.backend);
        eprintln!("Using backend profile: {:?} (from '{}')", backend, cli.backend);
        state.set_backend_profile(backend);
    }

    // Apply clock mode from CLI
    let clock_mode = match cli.clock_mode.to_lowercase().as_str() {
        "cost" | "costs" => {
            eprintln!("Clock mode: cost (accumulating estimated cycle costs)");
            ClockMode::Cost
        }
        _ => {
            eprintln!("Clock mode: timestamp (matching trace timestamps)");
            ClockMode::Timestamp
        }
    };
    state.set_clock_mode(clock_mode);

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
    eprintln!();
    state.print_summary();

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
