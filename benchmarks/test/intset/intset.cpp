/**
 * Integer Set Benchmark - Modern C++17 Version
 *
 * Tests transactional memory with concurrent set operations.
 * Implements a simple ordered set with add, remove, and contains operations.
 *
 * Compiler: C++17
 * Uses: C++ Standard Library threads, std::atomic
 */

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <optional>
#include <array>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int DEFAULT_INITIAL_SIZE = 1000;
constexpr int DEFAULT_RANGE_MAX = 10000;
constexpr int DEFAULT_READ_PCT = 80;

struct TMNode {
    int key;
    TM TMNode* left;
    TM TMNode* right;
    bool red;
    int size;
};

struct IntSet {
    TM TMNode* root = nullptr;

    TMNode* rotateLeft(TMNode* h) {
        TMNode* x = h->right;
        h->right = x->left;
        x->left = h;
        x->red = h->red;
        h->red = true;
        return x;
    }

    TMNode* rotateRight(TMNode* h) {
        TMNode* x = h->left;
        h->left = x->right;
        x->right = h;
        x->red = h->red;
        h->red = true;
        return x;
    }

    void flipColors(TMNode* h) {
        h->red = !h->red;
        h->left->red = !h->left->red;
        h->right->red = !h->right->red;
    }

    TMNode* insert(TMNode* h, int key) {
        if (h == nullptr) {
            TMNode* n = new TMNode();
            n->key = key;
            n->left = n->right = nullptr;
            n->red = true;
            n->size = 1;
            return n;
        }

        if (key < h->key) {
            h->left = insert(h->left, key);
        } else if (key > h->key) {
            h->right = insert(h->right, key);
        }

        if (h->right && h->right->red && !h->left->red) {
            h = rotateLeft(h);
        }
        if (h->left && h->left->red && h->left->left && h->left->left->red) {
            h = rotateRight(h);
        }
        if (h->left && h->left->red && h->right && h->right->red) {
            flipColors(h);
        }

        return h;
    }

    TMNode* fixUp(TMNode* h) {
        if (h->right && h->right->red && !h->left->red) {
            h = rotateLeft(h);
        }
        if (h->left && h->left->red && h->left->left && h->left->left->red) {
            h = rotateRight(h);
        }
        if (h->left && h->left->red && h->right && h->right->red) {
            flipColors(h);
        }
        return h;
    }

    TMNode* moveRedLeft(TMNode* h) {
        flipColors(h);
        if (h->right && h->right->left && h->right->left->red) {
            h->right = rotateRight(h->right);
            h = rotateLeft(h);
            flipColors(h);
        }
        return h;
    }

    TMNode* moveRedRight(TMNode* h) {
        flipColors(h);
        if (h->left && h->left->left && h->left->left->red) {
            h = rotateRight(h);
            flipColors(h);
        }
        return h;
    }

    TMNode* eraseMin(TMNode* h) {
        if (h->left == nullptr) {
            delete h;
            return nullptr;
        }
        if (!h->left->red && !h->left->left->red) {
            h = moveRedLeft(h);
        }
        h->left = eraseMin(h->left);
        return fixUp(h);
    }

    TMNode* min(TMNode* h) {
        while (h->left) h = h->left;
        return h;
    }

    TMNode* erase(TMNode* h, int key) {
        if (key < h->key) {
            if (!h->left->red && !h->left->left->red) {
                h = moveRedLeft(h);
            }
            h->left = erase(h->left, key);
        } else {
            if (h->left->red) {
                h = rotateRight(h);
            }
            if (key == h->key && h->right == nullptr) {
                delete h;
                return nullptr;
            }
            if (!h->right->red && !h->right->left->red) {
                h = moveRedRight(h);
            }
            if (key == h->key) {
                TMNode* m = min(h->right);
                h->key = m->key;
                h->right = eraseMin(h->right);
            } else {
                h->right = erase(h->right, key);
            }
        }
        return fixUp(h);
    }

    bool contains(TMNode* h, int key) const {
        while (h) {
            if (key < h->key) h = h->left;
            else if (key > h->key) h = h->right;
            else return true;
        }
        return false;
    }

    int size(TMNode* h) const {
        if (!h) return 0;
        return size(h->left) + size(h->right) + 1;
    }

public:
    void add(int key) {
        root = insert(root, key);
        root->red = false;
    }

    bool remove(int key) {
        if (!contains(root, key)) return false;
        root->red = true;
        root = erase(root, key);
        if (root) root->red = false;
        return true;
    }

    bool contains(int key) const {
        return contains(root, key);
    }

    int size() const {
        return size(root);
    }
};

struct Barrier {
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_ = 0;
    int num_threads_ = 0;
    int crossing_ = 0;

    explicit Barrier(int n) : num_threads_(n), count_(n) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        crossing_++;
        if (crossing_ < count_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

std::atomic<bool> stop_workers(false);

struct ThreadData {
    IntSet* set;
    Barrier* barrier;
    std::atomic<uint64_t> nb_add{0};
    std::atomic<uint64_t> nb_remove{0};
    std::atomic<uint64_t> nb_contains{0};
    unsigned int seed;
    int thread_id;
    int nb_threads;
    int read_pct;
    int range_max;
};

THREAD void worker_thread(ThreadData& data) {
    auto rng = std::mt19937(data.seed);
    std::uniform_int_distribution<> dist(0, data.range_max - 1);

    data.barrier->wait();

    while (!stop_workers.load(std::memory_order_relaxed)) {
        int key = dist(rng);
        int op = dist(rng) % 100;

        if (op < data.read_pct) {
            data.set->contains(key);
            data.nb_contains++;
        } else if (op < data.read_pct + 5) {
            data.set->remove(key);
            data.nb_remove++;
        } else {
            data.set->add(key);
            data.nb_add++;
        }
    }
}

MAIN int main(int argc, char* argv[]) {
    int duration_ms = DEFAULT_DURATION_MS;
    int nb_threads = DEFAULT_NB_THREADS;
    int initial_size = DEFAULT_INITIAL_SIZE;
    int range_max = DEFAULT_RANGE_MAX;
    int read_pct = DEFAULT_READ_PCT;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-d" && i + 1 < argc) {
            duration_ms = std::stoi(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            nb_threads = std::stoi(argv[++i]);
        } else if (arg == "-i" && i + 1 < argc) {
            initial_size = std::stoi(argv[++i]);
        } else if (arg == "-r" && i + 1 < argc) {
            range_max = std::stoi(argv[++i]);
        } else if (arg == "-p" && i + 1 < argc) {
            read_pct = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "IntSet Benchmark Usage:\n"
                      << "  -d <ms>       Duration in ms (default: " << DEFAULT_DURATION_MS << ")\n"
                      << "  -t <n>        Number of threads (default: " << DEFAULT_NB_THREADS << ")\n"
                      << "  -i <n>        Initial set size (default: " << DEFAULT_INITIAL_SIZE << ")\n"
                      << "  -r <n>        Key range (default: " << DEFAULT_RANGE_MAX << ")\n"
                      << "  -p <pct>      Read percentage (default: " << DEFAULT_READ_PCT << ")\n";
            return 0;
        }
    }

    std::cout << "IntSet Benchmark - Modern C++17 Version\n"
              << "========================================\n"
              << "Duration:  " << duration_ms << " ms\n"
              << "Threads:   " << nb_threads << "\n"
              << "Initial:   " << initial_size << " elements\n"
              << "Range:     [0, " << range_max << ")\n"
              << "Read %:    " << read_pct << "%\n"
              << std::endl;

    IntSet set;
    for (int i = 0; i < initial_size; ++i) {
        set.add(i);
    }

    std::cout << "Initial set size: " << set.size() << std::endl;

    Barrier barrier(nb_threads);
    std::vector<std::unique_ptr<ThreadData>> thread_data;
    std::vector<std::thread> threads;

    for (int i = 0; i < nb_threads; ++i) {
        auto data = std::make_unique<ThreadData>();
        data->set = &set;
        data->barrier = &barrier;
        data->seed = i + 1234;
        data->thread_id = i;
        data->nb_threads = nb_threads;
        data->read_pct = read_pct;
        data->range_max = range_max;
        thread_data.push_back(std::move(data));
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < nb_threads; ++i) {
        threads.emplace_back(worker_thread, std::ref(*thread_data[i]));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    stop_workers.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    uint64_t total_add = 0, total_remove = 0, total_contains = 0;
    for (const auto& data : thread_data) {
        total_add += data->nb_add.load();
        total_remove += data->nb_remove.load();
        total_contains += data->nb_contains.load();
    }

    uint64_t total_ops = total_add + total_remove + total_contains;

    std::cout << "\nResults\n"
              << "=======\n"
              << "Elapsed time:    " << elapsed_ms << " ms\n"
              << "Final set size: " << set.size() << "\n"
              << "Total adds:      " << total_add << "\n"
              << "Total removes:   " << total_remove << "\n"
              << "Total contains:  " << total_contains << "\n"
              << "Total ops:       " << total_ops << "\n"
              << "Ops/sec:         " << (total_ops * 1000.0 / elapsed_ms) << "\n"
              << std::endl;

    return 0;
}
