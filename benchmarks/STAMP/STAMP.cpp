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

#include "stamp_common.hpp"
#include "bayes_bench.hpp"
#include "genome_bench.hpp"
#include "intruder_bench.hpp"
#include "kmeans_bench.hpp"
#include "labyrinth_bench.hpp"
#include "ssca2_bench.hpp"
#include "vacation_bench.hpp"
#include "yada_bench.hpp"

std::atomic<bool> stop_workers{false};
std::atomic<uint64_t> total_ops{0};
std::atomic<uint64_t> abort_count{0};

BenchmarkType g_benchmark = BenchmarkType::BAYES;
int g_num_threads = DEFAULT_NB_THREADS;
int g_duration = DEFAULT_DURATION_MS;

void run_benchmark(BenchmarkType bench, int threads, int duration_ms) {
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

    int loops = duration_ms / 10;
    if (loops < 10) loops = 10;

    Barrier barrier(threads);
    std::vector<ThreadData> td(threads);
    std::vector<std::thread> workers;

    for (int i = 0; i < threads; i++) {
        td[i].barrier = &barrier;
        td[i].thread_id = i;
        td[i].loops = loops;
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

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    stop_workers = true;

    for (auto& w : workers) w.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    uint64_t ops = total_ops.load();

    std::cout << "Results\n";
    std::cout << "=======\n";
    std::cout << "Elapsed:    " << ms << " ms\n";
    std::cout << "Total ops: " << ops << "\n";
    std::cout << "Ops/sec:   " << (ops * 1000.0 / ms) << "\n";
    std::cout << "Aborts:    " << abort_count.load() << "\n";
}

MAIN int main(int argc, char* argv[]) {
    g_benchmark = BenchmarkType::BAYES;
    g_num_threads = DEFAULT_NB_THREADS;
    g_duration = DEFAULT_DURATION_MS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            g_num_threads = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            g_duration = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            char b = argv[++i][0];
            switch(b) {
                case 'b': case 'B': g_benchmark = BenchmarkType::BAYES; break;
                case 'g': case 'G': g_benchmark = BenchmarkType::GENOME; break;
                case 'i': case 'I': g_benchmark = BenchmarkType::INTRUDER; break;
                case 'k': case 'K': g_benchmark = BenchmarkType::KMEANS; break;
                case 'l': case 'L': g_benchmark = BenchmarkType::LABYRINTH; break;
                case 's': case 'S': g_benchmark = BenchmarkType::SSCA2; break;
                case 'v': case 'V': g_benchmark = BenchmarkType::VACATION; break;
                case 'y': case 'Y': g_benchmark = BenchmarkType::YADA; break;
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
    std::cout << "Threads:   " << g_num_threads << "\n";
    std::cout << "Duration:  " << g_duration << " ms\n\n";

    run_benchmark(g_benchmark, g_num_threads, g_duration);

    return 0;
}
