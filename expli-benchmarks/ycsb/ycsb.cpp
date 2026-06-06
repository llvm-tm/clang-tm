#include "expli_tm_api/tm_api.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "../tests/benchmark_test.hpp"

// ── Workload / Distribution ────────────────────────────────────────────
enum Workload { WL_A, WL_B, WL_C, WL_D, WL_E, WL_F };
enum Distribution { DIST_UNIFORM, DIST_ZIPFIAN, DIST_LATEST };

const char *wl_name(Workload w) {
    switch (w) { case WL_A: return "A"; case WL_B: return "B"; case WL_C: return "C";
                 case WL_D: return "D"; case WL_E: return "E"; case WL_F: return "F"; }
    return "?";
}
const char *dist_name(Distribution d) {
    switch (d) { case DIST_UNIFORM: return "uniform"; case DIST_ZIPFIAN: return "zipfian";
                 case DIST_LATEST: return "latest"; }
    return "?";
}

// ── Config ─────────────────────────────────────────────────────────────
struct Config {
    int threads = 4;
    int duration = 10000;
    Workload workload = WL_A;
    Distribution dist = DIST_ZIPFIAN;
    int key_range = 10000;
    int initial_records = 10000;
};

Config parse_args(int argc, char *argv[]) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        auto nxt = [&](){ return i+1 < argc ? atoi(argv[++i]) : 0; };
        if      (!strcmp(argv[i], "-t"))  c.threads = nxt();
        else if (!strcmp(argv[i], "-d"))  c.duration = nxt();
        else if (!strcmp(argv[i], "-k"))  c.key_range = nxt();
        else if (!strcmp(argv[i], "-i"))  c.initial_records = nxt();
        else if (!strcmp(argv[i], "-w") && i+1 < argc) {
            const char *wl = argv[++i];
            if      (!strcmp(wl, "a") || !strcmp(wl, "A")) c.workload = WL_A;
            else if (!strcmp(wl, "b") || !strcmp(wl, "B")) c.workload = WL_B;
            else if (!strcmp(wl, "c") || !strcmp(wl, "C")) c.workload = WL_C;
            else if (!strcmp(wl, "d") || !strcmp(wl, "D")) c.workload = WL_D;
            else if (!strcmp(wl, "e") || !strcmp(wl, "E")) c.workload = WL_E;
            else if (!strcmp(wl, "f") || !strcmp(wl, "F")) c.workload = WL_F;
        }
        else if (!strcmp(argv[i], "-dist") && i+1 < argc) {
            const char *d = argv[++i];
            if      (!strcmp(d, "u") || !strcmp(d, "uniform")) c.dist = DIST_UNIFORM;
            else if (!strcmp(d, "z") || !strcmp(d, "zipfian")) c.dist = DIST_ZIPFIAN;
            else if (!strcmp(d, "l") || !strcmp(d, "latest"))  c.dist = DIST_LATEST;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: ycsb [-t n] [-d ms] [-w a|b|c|d|e|f] [-k n] [-i n] [-dist u|z|l]\n");
            exit(0);
        }
    }
    return c;
}

// ── RNG ────────────────────────────────────────────────────────────────
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 33;
    }
};

// ── Zipfian ────────────────────────────────────────────────────────────
double zeta(int n, double theta) {
    double s = 0.0;
    for (int i = 0; i < n; ++i)
        s += 1.0 / pow(i + 1.0, theta);
    return s;
}

std::vector<double> build_zipfian_cdf(int n, double theta) {
    double z = zeta(n, theta);
    std::vector<double> cdf(n);
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += 1.0 / pow(i + 1.0, theta);
        cdf[i] = sum / z;
    }
    return cdf;
}

int zipfian_sample(const std::vector<double> &cdf, double r) {
    for (size_t i = 0; i < cdf.size(); ++i)
        if (r < cdf[i]) return (int)i;
    return (int)cdf.size() - 1;
}

// ── Record ─────────────────────────────────────────────────────────────
static const int NUM_FIELDS = 10;
static const int FIELD_SIZE = 100;
static const int RECORD_BYTES = NUM_FIELDS * FIELD_SIZE;  // 1000

struct Record {
    int64_t key;
    expli::TM<uint8_t> data[RECORD_BYTES];
    expli::TM<int64_t> timestamp;

    Record(int64_t k) : key(k) {
        const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; i < RECORD_BYTES; ++i)
            data[i].poke((uint8_t)chars[(key + i) % 62]);
        timestamp.poke(k);
    }
};

// ── Database ───────────────────────────────────────────────────────────
struct Database {
    int max_records;
    expli::TM<int64_t> *key_to_idx;  // key hash -> index, -1 = empty
    expli::TM<uint64_t> *records;    // TmCell<TmPtr<Record>>
    std::atomic<uint64_t> count{0};

    Database(int max) : max_records(max) {
        key_to_idx = new expli::TM<int64_t>[max];
        records = new expli::TM<uint64_t>[max];
        for (int i = 0; i < max; ++i) {
            key_to_idx[i].poke(-1);
            records[i].poke(0);
        }
    }
    ~Database() { delete[] key_to_idx; delete[] records; }

    int hash(int64_t key) const {
        uint64_t h = (uint64_t)key;
        return (int)((h * 0x9e3779b9ULL) >> 54) % max_records;
    }

    void insert(int64_t key) {
        if (count.load() >= (uint64_t)max_records) return;
        int h = hash(key);
        for (int off = 0; off < max_records; ++off) {
            int idx = (h + off) % max_records;
            int64_t stored = key_to_idx[idx].read();
            if (stored == -1) {
                Record *rec = new Record(key);
                key_to_idx[idx].write(key);
                records[idx].write((uint64_t)(uintptr_t)rec);
                count.fetch_add(1);
                return;
            }
            if (stored == key) return;
        }
    }

    Record *find(int64_t key) {
        int h = hash(key);
        for (int off = 0; off < max_records; ++off) {
            int idx = (h + off) % max_records;
            int64_t stored = key_to_idx[idx].read();
            if (stored == -1) return nullptr;
            if (stored == key)
                return (Record*)(uintptr_t)records[idx].read();
        }
        return nullptr;
    }

    void update_record(int64_t key) {
        Record *rec = find(key);
        if (!rec) return;
        const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; i < RECORD_BYTES; ++i)
            rec->data[i].write((uint8_t)chars[(key + i) % 62]);
        rec->timestamp.write(key);
    }

    void read_field0(int64_t key, uint8_t *out) {
        Record *rec = find(key);
        if (!rec) return;
        for (int i = 0; i < FIELD_SIZE; ++i)
            out[i] = rec->data[i].read();
    }
};

// ── Globals ────────────────────────────────────────────────────────────
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_total_ops{0};
Database *g_db = nullptr;

// ── Worker ─────────────────────────────────────────────────────────────
void run_worker(int tid, const Config &cfg, const std::vector<double> &cdf) {
    expli::TM<int64_t>::thread_init();
    Rng rng(tid * 12345ULL + 42);
    int64_t insert_counter = cfg.key_range + tid;

    while (!g_stop.load()) {
        double r = (double)rng.next() / (double)UINT64_MAX;
        int64_t key;
        if (cfg.dist == DIST_ZIPFIAN)
            key = zipfian_sample(cdf, r) % cfg.key_range;
        else if (cfg.dist == DIST_LATEST) {
            int64_t max = cfg.key_range + insert_counter;
            if (max > cfg.key_range * 2) max = cfg.key_range * 2;
            key = max - 1 - (int64_t)(rng.next() % 1000);
        } else
            key = (int64_t)(rng.next() % cfg.key_range);

        int op_r = (int)(r * 100.0);

        switch (cfg.workload) {
        case WL_A:
            if (op_r < 50) {
                uint8_t buf[FIELD_SIZE];
                expli::TM<int64_t>::transaction([&](){ g_db->read_field0(key, buf); });
            } else
                expli::TM<int64_t>::transaction([&](){ g_db->update_record(key); });
            break;
        case WL_B:
            if (op_r < 95) {
                uint8_t buf[FIELD_SIZE];
                expli::TM<int64_t>::transaction([&](){ g_db->read_field0(key, buf); });
            } else
                expli::TM<int64_t>::transaction([&](){ g_db->update_record(key); });
            break;
        case WL_C: {
            uint8_t buf[FIELD_SIZE];
            expli::TM<int64_t>::transaction([&](){ g_db->read_field0(key, buf); });
            break;
        }
        case WL_D:
            if (op_r < 95) {
                uint8_t buf[FIELD_SIZE];
                expli::TM<int64_t>::transaction([&](){ g_db->read_field0(key, buf); });
            } else {
                expli::TM<int64_t>::transaction([&](){ g_db->insert(insert_counter); });
                insert_counter += cfg.threads;
            }
            break;
        case WL_E:
            if (op_r < 95) {
                uint8_t buf[FIELD_SIZE];
                expli::TM<int64_t>::transaction([&](){ g_db->read_field0(key, buf); });
            } else {
                expli::TM<int64_t>::transaction([&](){
                    for (int i = 0; i < 10; ++i) g_db->find(key + i);
                });
            }
            break;
        case WL_F:
            if (op_r < 50) {
                expli::TM<int64_t>::transaction([&](){
                    for (int i = 0; i < 10; ++i) g_db->find(key + i);
                });
            } else {
                uint8_t buf[FIELD_SIZE];
                expli::TM<int64_t>::transaction([&](){
                    g_db->read_field0(key, buf);
                    g_db->update_record(key);
                });
            }
            break;
        }
        g_total_ops.fetch_add(1);
    }
    expli::TM<int64_t>::thread_exit();
}

static void test_cli_flags() {
    printf("  Testing CLI flags...\n");
    Config c;
    // Test defaults
    TEST_EQ(c.threads, 4, "default threads");
    TEST_EQ(c.duration, 10000, "default duration");
    TEST_EQ(c.workload, WL_A, "default workload");
    TEST_EQ(c.dist, DIST_ZIPFIAN, "default dist");
    TEST_EQ(c.key_range, 10000, "default key range");
    TEST_EQ(c.initial_records, 10000, "default initial records");

    // Override
    const char* test_args[] = {"prog", "-t", "2", "-d", "500", "-k", "100", "-i", "50",
                                "-w", "c", "-dist", "u"};
    Config c2 = parse_args(13, (char**)test_args);
    TEST_EQ(c2.threads, 2, "override threads");
    TEST_EQ(c2.duration, 500, "override duration");
    TEST_EQ(c2.key_range, 100, "override key range");
    TEST_EQ(c2.initial_records, 50, "override initial records");
    TEST_EQ(c2.workload, WL_C, "override workload");
    TEST_EQ(c2.dist, DIST_UNIFORM, "override dist");

    // Test another workload/dist
    const char* test_args2[] = {"prog", "-w", "f", "-dist", "l"};
    Config c3 = parse_args(5, (char**)test_args2);
    TEST_EQ(c3.workload, WL_F, "workload F");
    TEST_EQ(c3.dist, DIST_LATEST, "dist latest");

    if (test_result() != 0) exit(1);
}

static void test_rng() {
    printf("  Testing RNG determinism...\n");
    // Test custom Rng determinism
    Rng a(42), b(42);
    for (int i = 0; i < 1000; i++) {
        TEST_EQ(a.next(), b.next(), "Rng determinism");
    }
    if (test_result() != 0) exit(1);
}

static void test_logic() {
    printf("  Testing YCSB logic...\n");
    // Test zipfian CDF generation and sampling
    auto cdf = build_zipfian_cdf(100, 0.99);
    TEST_EQ((int)cdf.size(), 100, "zipfian CDF size");
    TEST_NEAR(cdf[99], 1.0, 1e-9, "zipfian CDF ends at 1.0");
    TEST_ASSERT(cdf[0] > 0, "zipfian first element > 0");
    // Test sampling
    int s = zipfian_sample(cdf, 0.5);
    TEST_ASSERT(s >= 0 && s < 100, "zipfian sample in range");

    // Test Rng-based operations determinism
    Rng r1(123), r2(123);
    for (int i = 0; i < 100; i++) {
        TEST_EQ(r1.next(), r2.next(), "Rng sequence");
    }

    if (test_result() != 0) exit(1);
}

// ── Main ───────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        printf("Running self-tests for ycsb...\n");
        test_cli_flags();
        test_rng();
        test_logic();
        printf("All tests passed.\n");
        return 0;
    }
    Config cfg = parse_args(argc, argv);

    printf("========= YCSB Benchmark =========\n");
    printf("===================================\n");
    printf("Workload:     %s\n", wl_name(cfg.workload));
    printf("Distribution: %s\n", dist_name(cfg.dist));
    printf("Threads:      %d\n", cfg.threads);
    printf("Duration:     %d ms\n", cfg.duration);
    printf("Key range:    %d\n", cfg.key_range);
    printf("Initial recs: %d\n\n", cfg.initial_records);

    std::vector<double> cdf;
    if (cfg.dist == DIST_ZIPFIAN)
        cdf = build_zipfian_cdf(10000, 0.99);

    expli::TM<int64_t>::init();

    g_db = new Database(100000);

    printf("Loading %d records...\n", cfg.initial_records);
    expli::TM<int64_t>::thread_init();
    for (int64_t key = 0; key < cfg.initial_records; ++key)
        expli::TM<int64_t>::transaction([&](){ g_db->insert(key); });
    expli::TM<int64_t>::thread_exit();
    printf("  Loaded: %llu\n\n", (unsigned long long)g_db->count.load());

    std::vector<std::thread> threads;
    for (int t = 0; t < cfg.threads; ++t)
        threads.emplace_back([t, &cfg, &cdf]() { run_worker(t, cfg, cdf); });

    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.duration));
    g_stop.store(true);
    for (auto &th : threads) th.join();

    uint64_t ops = g_total_ops.load();
    double secs = cfg.duration / 1000.0;

    printf("\nResults\n");
    printf("=======\n");
    printf("Elapsed:  %.1fs\n", secs);
    printf("Total ops: %llu\n", (unsigned long long)ops);
    printf("Throughput: %.0f ops/s\n", ops / secs);

    delete g_db;
    expli::TM<int64_t>::exit();
    return 0;
}
