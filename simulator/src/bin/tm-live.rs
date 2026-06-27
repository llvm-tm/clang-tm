// ── tm-live: Live-app simulation ─────────────────────────────
// Loads a benchmark .so linked against the SimBackend and runs it
// inside the simulator, with every TM operation dispatched through
// the shadow-memory conflict detection engine.
//
// Usage:
//   tm-live --app ./social_tm.so -- -d 2000 -u 64 -t 1
//
// The SimBackend .so path defaults to simulator/live_app/libsim_backend.so.

use clap::Parser;
use std::path::PathBuf;

#[derive(Parser, Debug)]
#[command(name = "tm-live", about = "Live-app TM simulation")]
struct Cli {
    /// Path to the application .so to simulate.
    #[arg(short, long)]
    app: PathBuf,

    /// Path to the SimBackend .so (default: ../live_app/libsim_backend.so).
    #[arg(short = 'b', long, default_value = "../live_app/libsim_backend.so")]
    sim_backend: PathBuf,

    /// Maximum number of simulated threads.
    #[arg(short = 'm', long, default_value = "32")]
    max_threads: u32,

    /// Arguments passed to the application.
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    app_args: Vec<String>,
}

fn main() {
    let cli = Cli::parse();

    // Canonicalise paths relative to the binary's location.
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
        .unwrap_or_else(|| PathBuf::from("."));

    let sim_backend_path = if cli.sim_backend.is_relative() {
        exe_dir.join(&cli.sim_backend)
    } else {
        cli.sim_backend.clone()
    };

    let app_path = if cli.app.is_relative() {
        std::env::current_dir().unwrap_or_default().join(&cli.app)
    } else {
        cli.app.clone()
    };

    eprintln!("tm-live: SimBackend={:?} App={:?}", sim_backend_path, app_path);
    eprintln!("tm-live: args={:?}", cli.app_args);

    // Initialise the live simulation state.
    tm_des::live_app::init(cli.max_threads);

    // Run the application.
    match tm_des::live_app::run_app(
        sim_backend_path.to_str().unwrap(),
        app_path.to_str().unwrap(),
        &cli.app_args,
    ) {
        Ok(exit_code) => {
            // Print final report.
            tm_des::live_app::print_report();
            std::process::exit(exit_code);
        }
        Err(e) => {
            eprintln!("tm-live error: {}", e);
            std::process::exit(1);
        }
    }
}
