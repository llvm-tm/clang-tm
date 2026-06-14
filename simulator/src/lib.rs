pub mod event;
pub mod queue;
pub mod engine;
pub mod trace;
pub mod lp;
pub mod memory;
pub mod checker;
pub mod rng;
pub mod ser;
pub mod tm_model;
pub mod bank_sim;
pub mod sim_engine;

use clap::Parser;

#[derive(Parser, Debug)]
#[command(name = "tm-des", about = "TM Discrete Event Simulator")]
pub struct Cli {
    /// Path to trace file (JSONL). Use '-' for stdin.
    #[arg(short, long, default_value = "-")]
    pub trace: String,

    /// Restore from checkpoint file instead of starting fresh.
    #[arg(long)]
    pub checkpoint: Option<String>,

    /// Save checkpoint every N events.
    #[arg(long, default_value = "10000")]
    pub checkpoint_every: u64,

    /// Global PRNG seed.
    #[arg(short, long, default_value = "42")]
    pub seed: u64,

    /// Stop after processing M events (0 = unlimited).
    #[arg(long, default_value = "0")]
    pub max_events: u64,

    /// Retries before livelock warning.
    #[arg(long, default_value = "1000")]
    pub livelock_threshold: u32,
}
