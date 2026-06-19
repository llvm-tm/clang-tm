pub mod kmeans;
pub mod labyrinth;
pub mod vacation;
pub mod genome;
pub mod intruder;
pub mod ssca2;
pub mod bayes;
pub mod yada;

#[derive(Clone)]
pub struct Config {
    pub threads: usize,
    pub duration: usize,
    pub bench: Bench,
    pub points: usize,
    pub clusters: usize,
    pub dims: usize,
    pub grid_x: usize,
    pub grid_y: usize,
    pub grid_z: usize,
    pub num_rooms: usize,
    pub num_customers: usize,
    pub num_items: usize,
    pub gene_length: usize,
    pub segment_length: usize,
    pub num_segments: usize,
    pub num_atoms: usize,
    pub max_length: usize,
    pub num_packets: usize,
    pub percent_attack: usize,
    pub seed: u64,
    pub scale: usize,
    pub initiator_prob: f64,
    pub update_prob: f64,
    pub min_degree: usize,
    pub max_degree: usize,
    pub threshold: f64,
    pub jitter: f64,
    pub angle: f64,
    pub prob_unidirectional: f64,
    pub max_paral_edges: usize,
    pub subgr_edge_length: usize,
    pub iterations: usize,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            threads: 4, duration: 10000, bench: Bench::All,
            points: 10000, clusters: 5, dims: 3,
            grid_x: 5, grid_y: 5, grid_z: 5,
            num_rooms: 10, num_customers: 100, num_items: 10,
            gene_length: 16384, segment_length: 64, num_segments: 16777216,
            num_atoms: 10, max_length: 128, num_packets: 1048576,
            percent_attack: 10, seed: 1,
            scale: 20, initiator_prob: 1.0, update_prob: 1.0,
            min_degree: 3, max_degree: 3,
            threshold: 0.00001, jitter: 0.5, angle: 20.0,
            prob_unidirectional: 0.5, max_paral_edges: 3,
            subgr_edge_length: 3, iterations: 10,
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
pub enum Bench { All, KMeans, Labyrinth, Vacation, Genome, Intruder, SSCA2, Bayes, Yada }

pub fn run_test(bench: Bench) -> i32 {
    match bench {
        Bench::KMeans => kmeans::test(),
        Bench::Labyrinth => labyrinth::test(),
        Bench::Vacation => vacation::test(),
        Bench::Genome => genome::test(),
        Bench::Intruder => intruder::test(),
        Bench::SSCA2 => ssca2::test(),
        Bench::Bayes => bayes::test(),
        Bench::Yada => yada::test(),
        Bench::All => {
            let mut f = 0;
            f += kmeans::test(); f += labyrinth::test();
            f += vacation::test(); f += genome::test();
            f += intruder::test(); f += ssca2::test();
            f += bayes::test(); f += yada::test();
            f
        }
    }
}
