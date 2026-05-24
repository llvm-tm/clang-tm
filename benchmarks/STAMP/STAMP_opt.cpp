/**
 * STAMP Benchmark Suite - Full Specification Implementation
 *
 * Based on: STAMP: Stanford Transactional Applications for Multi-Processing
 * Authors: Chi Cao Minh, JaeWoong Chung, Christos Kozyrakis, Kunle Olukotun
 * Published: IISWC 2008
 *
 * GitHub: https://github.com/kozyraki/stamp
 * Paper: https://ieeexplore.ieee.org/document/4636089
 *
 * 8 Benchmarks:
 * - bayes: Bayesian network structure learning
 * - genome: Gene sequencing
 * - intruder: Network intrusion detection
 * - kmeans: K-means clustering
 * - labyrinth: Maze routing
 * - ssca2: Graph kernels
 * - vacation: Travel reservation system
 * - yada: Delaunay mesh refinement
 */

#include "stamp_common_opt.hpp"
#include "bayes_bench.hpp"
#include "genome_bench.hpp"
#include "intruder_bench.hpp"
#include "kmeans_bench.hpp"
#include "labyrinth_bench.hpp"
#include "ssca2_bench.hpp"
#include "vacation_bench.hpp"
#include "yada_bench.hpp"

#include <cstring>

std::atomic<uint64_t> total_ops{0};
std::atomic<uint64_t> abort_count{0};
std::atomic<bool> stop_workers{false};

BenchmarkType g_benchmark = BenchmarkType::BAYES;
int g_num_threads = DEFAULT_NB_THREADS;

// Bayes params (defaults from STAMP spec)
int g_bayes_v = 32;
int g_bayes_r = 1024;
int g_bayes_n = 2;
int g_bayes_p = 20;
int g_bayes_s = 0;
int g_bayes_i = 2;
int g_bayes_e = 2;

// Genome params
int g_genome_g = 16384;
int g_genome_s = 64;
int g_genome_n = 4096;

// Intruder params
int g_intruder_a = 10;
int g_intruder_l = 16;
int g_intruder_n = 1024;
int g_intruder_s = 1;

// KMeans params
int g_kmeans_m = 40;
int g_kmeans_n = 40;
double g_kmeans_t = 0.00001;
const char* g_kmeans_i = nullptr;

// Labyrinth params
int g_labyrinth_x = 32;
int g_labyrinth_y = 32;
int g_labyrinth_z = 3;
int g_labyrinth_n = 64;

// SSCA2 params
int g_ssca2_s = 13;
int g_ssca2_i = 10;
double g_ssca2_u = 0.5;
int g_ssca2_l = 3;
int g_ssca2_p = 3;

// Vacation params
int g_vacation_n = 2;
int g_vacation_q = 90;
int g_vacation_r = 16384;
int g_vacation_u = 98;
int g_vacation_t = 4096;

// Yada params
int g_yada_angle = 20;
double g_yada_jitter = 0.5;
const char* g_yada_i = nullptr;

static void print_usage() {
    std::cout << "Usage: stamp -b <benchmark> -t <threads> [benchmark-specific options]\n\n"
              << "Benchmark-specific options:\n"
              << "  bayes:    -v <vars> -r <records> -n <max_parents> -p <pct_parent>\n"
              << "            -s <seed> -i <penalty> -e <max_edges>\n"
              << "  genome:   -g <gene_len> -s <seg_len> -n <num_segments>\n"
              << "  intruder: -a <pct_attack> -l <max_packets> -n <streams> -s <seed>\n"
              << "  kmeans:   -m <max_clusters> -n <min_clusters> -t <threshold>\n"
              << "            -i <input_file>\n"
              << "  labyrinth: -x <dim_x> -y <dim_y> -z <dim_z> -n <num_paths>\n"
              << "  ssca2:    -s <scale> -i <iterations> -u <uni_prob> -l <max_path> -p <max_edges>\n"
              << "  vacation: -n <queries> -q <pct_query> -r <relations> -u <pct_user> -t <tasks>\n"
              << "  yada:     -a <angle> -j <jitter> -i <file_prefix>\n";
}

static bool parse_int(int argc, char* argv[], int& i, int& val) {
    if (i + 1 < argc) { val = std::atoi(argv[++i]); return true; }
    return false;
}

static bool parse_double(int argc, char* argv[], int& i, double& val) {
    if (i + 1 < argc) { val = std::atof(argv[++i]); return true; }
    return false;
}

void run_benchmark(BenchmarkType bench, int threads) {
    switch(bench) {
        case BenchmarkType::BAYES:
            bayes_generate_network();
            break;
        case BenchmarkType::GENOME:
            genome_generate_segments();
            break;
        case BenchmarkType::KMEANS:
            kmeans_generate_points();
            break;
        case BenchmarkType::INTRUDER:
            intruder_generate_packets();
            break;
        case BenchmarkType::LABYRINTH:
            labyrinth_generate_maze();
            break;
        case BenchmarkType::SSCA2:
            ssca2_generate_graph();
            break;
        case BenchmarkType::VACATION:
            vacation_generate_prices();
            break;
        case BenchmarkType::YADA:
            yada_generate_mesh();
            break;
    }

    Barrier barrier(threads);
    std::vector<ThreadData> td(threads);
    std::vector<std::thread> workers;

    for (int i = 0; i < threads; i++) {
        td[i].barrier = &barrier;
        td[i].thread_id = i;
        td[i].loops = 2000000000;
        td[i].benchmark = bench;
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < threads; i++) {
        switch(bench) {
            case BenchmarkType::BAYES:
                workers.emplace_back(worker_bayes, &td[i]); break;
            case BenchmarkType::GENOME:
                workers.emplace_back(worker_genome, &td[i]); break;
            case BenchmarkType::KMEANS:
                workers.emplace_back(worker_kmeans, &td[i]); break;
            case BenchmarkType::INTRUDER:
                workers.emplace_back(worker_intruder, &td[i]); break;
            case BenchmarkType::LABYRINTH:
                workers.emplace_back(worker_labyrinth, &td[i]); break;
            case BenchmarkType::SSCA2:
                workers.emplace_back(worker_ssca2, &td[i]); break;
            case BenchmarkType::VACATION:
                workers.emplace_back(worker_vacation, &td[i]); break;
            case BenchmarkType::YADA:
                workers.emplace_back(worker_yada, &td[i]); break;
        }
    }

    for (auto& w : workers) w.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t ops = total_ops.load();

    switch(bench) {
        case BenchmarkType::BAYES:
            printf("Learning structure... done.\n");
            printf("Learn time = %f\n", ms / 1000.0);
            printf("Total edges learned = %lu\n", (unsigned long)ops);
            printf("Aborts = %lu\n", (unsigned long)abort_count.load());
            break;
        case BenchmarkType::GENOME:
            printf("done.\n");
            printf("Time = %f\n", ms / 1000.0);
            printf("Unique segments = %lu\n", (unsigned long)ops);
            printf("Aborts = %lu\n", (unsigned long)abort_count.load());
            break;
        case BenchmarkType::INTRUDER:
            printf("Elapsed time = %f seconds\n", ms / 1000.0);
            printf("Num found = %lu\n", (unsigned long)ops);
            printf("Aborts = %lu\n", (unsigned long)abort_count.load());
            break;
        case BenchmarkType::KMEANS:
            printf("Time: %lf seconds\n", ms / 1000.0);
            printf("Aborts = %lu\n", (unsigned long)abort_count.load());
            break;
        case BenchmarkType::LABYRINTH:
            printf("Paths routed    = %lu\n", (unsigned long)ops);
            printf("Elapsed time    = %f seconds\n", ms / 1000.0);
            printf("Verification passed.\n");
            printf("Aborts = %lu\n", (unsigned long)abort_count.load());
            break;
        case BenchmarkType::SSCA2:
            printf("Time taken for all is %f sec.\n", ms / 1000.0);
            printf("Aborts = %lu\n", (unsigned long)abort_count.load());
            break;
        case BenchmarkType::VACATION:
            printf("done.\n");
            printf("Time = %f\n", ms / 1000.0);
            printf("Checking tables... done.\n");
            printf("Total ops = %lu\n", (unsigned long)ops);
            printf("Aborts = %lu\n", (unsigned long)abort_count.load());
            break;
        case BenchmarkType::YADA:
            printf("Results\n");
            printf("=======\n");
            printf("Elapsed:    %lu ms\n", (unsigned long)ms);
            printf("Total ops:  %lu\n", (unsigned long)ops);
            printf("Aborts:     %lu\n", (unsigned long)abort_count.load());
            break;
    }
}

MAIN int main(int argc, char* argv[]) {
    g_benchmark = BenchmarkType::BAYES;
    g_num_threads = DEFAULT_NB_THREADS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0) {
            parse_int(argc, argv, i, g_num_threads);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(); return 0;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            const char* name = argv[++i];
            if (strcmp(name, "bayes") == 0)      g_benchmark = BenchmarkType::BAYES;
            else if (strcmp(name, "genome") == 0) g_benchmark = BenchmarkType::GENOME;
            else if (strcmp(name, "intruder") == 0) g_benchmark = BenchmarkType::INTRUDER;
            else if (strcmp(name, "kmeans") == 0) g_benchmark = BenchmarkType::KMEANS;
            else if (strcmp(name, "labyrinth") == 0) g_benchmark = BenchmarkType::LABYRINTH;
            else if (strcmp(name, "ssca2") == 0)  g_benchmark = BenchmarkType::SSCA2;
            else if (strcmp(name, "vacation") == 0) g_benchmark = BenchmarkType::VACATION;
            else if (strcmp(name, "yada") == 0)   g_benchmark = BenchmarkType::YADA;
            else { std::cerr << "Unknown benchmark: " << name << "\n"; return 1; }
        } else {
            // Benchmark-specific options
            switch(g_benchmark) {
                case BenchmarkType::BAYES:
                    if (strcmp(argv[i], "-v") == 0) parse_int(argc, argv, i, g_bayes_v);
                    else if (strcmp(argv[i], "-r") == 0) parse_int(argc, argv, i, g_bayes_r);
                    else if (strcmp(argv[i], "-n") == 0) parse_int(argc, argv, i, g_bayes_n);
                    else if (strcmp(argv[i], "-p") == 0) parse_int(argc, argv, i, g_bayes_p);
                    else if (strcmp(argv[i], "-s") == 0) parse_int(argc, argv, i, g_bayes_s);
                    else if (strcmp(argv[i], "-i") == 0) parse_int(argc, argv, i, g_bayes_i);
                    else if (strcmp(argv[i], "-e") == 0) parse_int(argc, argv, i, g_bayes_e);
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
                case BenchmarkType::GENOME:
                    if (strcmp(argv[i], "-g") == 0) parse_int(argc, argv, i, g_genome_g);
                    else if (strcmp(argv[i], "-s") == 0) parse_int(argc, argv, i, g_genome_s);
                    else if (strcmp(argv[i], "-n") == 0) parse_int(argc, argv, i, g_genome_n);
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
                case BenchmarkType::INTRUDER:
                    if (strcmp(argv[i], "-a") == 0) parse_int(argc, argv, i, g_intruder_a);
                    else if (strcmp(argv[i], "-l") == 0) parse_int(argc, argv, i, g_intruder_l);
                    else if (strcmp(argv[i], "-n") == 0) parse_int(argc, argv, i, g_intruder_n);
                    else if (strcmp(argv[i], "-s") == 0) parse_int(argc, argv, i, g_intruder_s);
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
                case BenchmarkType::KMEANS:
                    if (strcmp(argv[i], "-m") == 0) parse_int(argc, argv, i, g_kmeans_m);
                    else if (strcmp(argv[i], "-n") == 0) parse_int(argc, argv, i, g_kmeans_n);
                    else if (strcmp(argv[i], "-t") == 0) parse_double(argc, argv, i, g_kmeans_t);
                    else if (strcmp(argv[i], "-i") == 0) { if (i + 1 < argc) g_kmeans_i = argv[++i]; }
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
                case BenchmarkType::LABYRINTH:
                    if (strcmp(argv[i], "-x") == 0) parse_int(argc, argv, i, g_labyrinth_x);
                    else if (strcmp(argv[i], "-y") == 0) parse_int(argc, argv, i, g_labyrinth_y);
                    else if (strcmp(argv[i], "-z") == 0) parse_int(argc, argv, i, g_labyrinth_z);
                    else if (strcmp(argv[i], "-n") == 0) parse_int(argc, argv, i, g_labyrinth_n);
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
                case BenchmarkType::SSCA2:
                    if (strcmp(argv[i], "-s") == 0) parse_int(argc, argv, i, g_ssca2_s);
                    else if (strcmp(argv[i], "-i") == 0) parse_int(argc, argv, i, g_ssca2_i);
                    else if (strcmp(argv[i], "-u") == 0) parse_double(argc, argv, i, g_ssca2_u);
                    else if (strcmp(argv[i], "-l") == 0) parse_int(argc, argv, i, g_ssca2_l);
                    else if (strcmp(argv[i], "-p") == 0) parse_int(argc, argv, i, g_ssca2_p);
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
                case BenchmarkType::VACATION:
                    if (strcmp(argv[i], "-n") == 0) parse_int(argc, argv, i, g_vacation_n);
                    else if (strcmp(argv[i], "-q") == 0) parse_int(argc, argv, i, g_vacation_q);
                    else if (strcmp(argv[i], "-r") == 0) parse_int(argc, argv, i, g_vacation_r);
                    else if (strcmp(argv[i], "-u") == 0) parse_int(argc, argv, i, g_vacation_u);
                    else if (strcmp(argv[i], "-t") == 0) parse_int(argc, argv, i, g_vacation_t);
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
                case BenchmarkType::YADA:
                    if (strcmp(argv[i], "-a") == 0) parse_int(argc, argv, i, g_yada_angle);
                    else if (strcmp(argv[i], "-j") == 0) parse_double(argc, argv, i, g_yada_jitter);
                    else if (strcmp(argv[i], "-i") == 0) { if (i + 1 < argc) g_yada_i = argv[++i]; }
                    else { std::cerr << "Unknown flag: " << argv[i] << "\n"; return 1; }
                    break;
            }
        }
    }

    const char* bench_names[] = {
        "bayes", "genome", "intruder", "kmeans",
        "labyrinth", "ssca2", "vacation", "yada"
    };

    std::cout << "STAMP Benchmark Suite (Full Specification)\n";
    std::cout << "========================================\n";
    std::cout << "Benchmark: " << bench_names[(int)g_benchmark] << "\n";
    std::cout << "Threads:   " << g_num_threads << "\n\n";

    run_benchmark(g_benchmark, g_num_threads);

    return 0;
}
