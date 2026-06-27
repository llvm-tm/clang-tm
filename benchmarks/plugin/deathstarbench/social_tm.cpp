/**
 * DeathStarBench Social TM Benchmark
 *
 * Transactional-memory adaptation of the DeathStarBench social network
 * microservices benchmark [ASPLOS'19].
 *
 * Reference: Gan et al., "An Open-Source Benchmark Suite for Microservices
 * and Their Hardware-Software Implications for Cloud and Edge Systems",
 * ASPLOS 2019.  https://doi.org/10.1145/3297858.3304013
 *
 * ORIGINAL ARCHITECTURE (microservice-based):
 *   The original DeathStarBench social network is a broadcast-style social
 *   network with uni-directional follow relationships.  It comprises 13
 *   microservices (nginx, php-fpm, ComposePostService, HomeTimelineService,
 *   UserTimelineService, PostStorageService, SocialGraphService,
 *   UniqueIdService, TextService, UserMentionService, MediaService,
 *   UrlShortenService, UserService) communicating via Thrift RPCs, with
 *   Memcached, Redis, and MongoDB for storage.
 *
 * TM ADAPTATION:
 *   This benchmark models the same workload patterns (compose-post fan-out,
 *   home-timeline read, user-timeline read) as shared-memory transactions
 *   on TM-annotated globals.  The follower graph is pre-built at
 *   initialisation and is read-only during the benchmark (no concurrent
 *   follow/unfollow), matching the original mixed-workload mode where
 *   follow/unfollow are separate specialised scripts.
 *
 * SIMPLIFICATIONS vs the original (each annotated with [SIMPL]):
 *   1. Data stored as integer counters, not rich Post objects with text,
 *      media, user mentions, URLs.  [SIMPL: content model]
 *   2. Home timeline modelled as "posts available from followed users"
 *      (sum of followees' post_count), not fan-out Redis sorted sets.
 *      [SIMPL: timeline model]
 *   3. Fan-out writes increment per-follower timeline_count counters
 *      rather than inserting post_ids into Redis zsets.  [SIMPL: fan-out]
 *   4. No Memcached/MongoDB/Redis multi-layer caching hierarchy.
 *      [SIMPL: storage]
 *   5. No user registration, login, search, or media services.
 *      [SIMPL: services omitted]
 *   6. Follower graph is fixed at init (no concurrent mutations).
 *      [SIMPL: static graph]
 *   7. No inter-service RPC latency or network contention modelling.
 *      [SIMPL: no network]
 *   8. Only PostType::POST (no REPOST, REPLY, DM).  [SIMPL: post types]
 *   9. Unique IDs are sequential int64_t counters, not Snowflake-style
 *      timestamp+node+counter hex-encoded IDs.  [SIMPL: ID generation]
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("shared"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

/* =================================================================
 * Configuration defaults
 *
 * These match the original DeathStarBench's default 962-user dataset
 * ([SIMPL] reduced to 256 for faster TM benchmarking).
 * ================================================================= */
constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_USERS = 256;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int MIN_FOLLOWEES = 3;   /* [SIMPL] original graph has power-law
                                      degree distribution, not uniform */
constexpr int MAX_FOLLOWEES = 12;

/* =================================================================
 * Data model
 *
 * [SIMPL] The original stores full Post objects (text, media array,
 * user_mentions array, URLs, creator) in MongoDB with Memcached
 * caching.  Here we only track integer counters — sufficient to
 * exercise the same contention patterns.
 * ================================================================= */
struct SocialNode {
    TM int64_t post_count;        // number of posts authored by this user
    TM int64_t follower_count;    // how many users follow this user
    TM int64_t following_count;   // how many users this user follows
    TM int64_t timeline_count;    // incremented on compose-post fan-out
                                  // [SIMPL: replaces Redis zset push to
                                  //  each follower's home timeline]
};

/* TM-tracked globals — the LLVM instrumentation pass intercepts all
 * loads/stores to these addresses. */
TM int64_t      g_next_post_id;   // global post-ID counter (UniqueIdService
                                  // equivalent, [SIMPL] not Snowflake)
TM SocialNode  *g_nodes;          // array of SocialNode (one per user)
TM int64_t      g_node_count;     // number of users

/* Follower graph — NOT TM-tracked because it is constructed at init and
 * never mutated during the benchmark.  Pre-computed adjacency lists let
 * compose-post iterate followers for fan-out, and read-home-timeline
 * iterate followees for post-count aggregation.
 *
 * Original: SocialGraphService stores follower/followee edges in MongoDB
 * (collection "social-graph") + Redis sorted sets (key "{user_id}:followers"
 * and "{user_id}:followees"), replicated for sharding.
 *
 * [SIMPL] Static in-memory vectors, no concurrent modifications. */
std::vector<std::vector<int>> g_followees;  // g_followees[u] = users that u follows
std::vector<std::vector<int>> g_followers;  // g_followers[u] = users that follow u

/* =================================================================
 * Barrier — thread synchronisation at start
 * ================================================================= */
class Barrier {
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int count_;
    int num_threads_;
    int crossing_;

public:
    explicit Barrier(int n)
        : count_(n), num_threads_(n), crossing_(0) {}
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        crossing_++;
        if (crossing_ < num_threads_) {
            cv_.wait(lock);
        } else {
            crossing_ = 0;
            cv_.notify_all();
        }
    }
};

std::atomic<bool> stop_workers(false);

struct ThreadData {
    Barrier *barrier;
    std::atomic<uint64_t> ops{0};
    unsigned int seed;
    int thread_id;
    int nb_threads;
};

/* =================================================================
 * Transactional operations
 *
 * Each TX function corresponds to an endpoint in the original
 * DeathStarBench social network (see references in comments).
 * ================================================================= */

/**
 * Compose Post  —  original endpoint: POST /wrk2-api/post/compose
 *                  handler: ComposePostHandler::ComposePost()
 *
 * Original chain (7 Thrift RPC calls):
 *   1. UniqueIdService  → snowflake ID
 *   2. TextService      → parse @mentions, URLs
 *   3. UserService      → get Creator info
 *   4. MediaService     → process media attachments
 *   5. PostStorageService → StorePost (MongoDB + Memcached)
 *   6. UserTimelineService → WriteUserTimeline (MongoDB + Redis)
 *   7. HomeTimelineService  → WriteHomeTimeline fan-out:
 *        a. SocialGraphService::GetFollowers
 *        b. Redis ZADD to each follower's home-timeline zset
 *
 * [SIMPL] We skip steps 2–5 (text/media/storage) and model only the
 * ID allocation + post_count increment + fan-out.  The fan-out iterates
 * pre-computed g_followers[author] and increments each follower's
 * timeline_count instead of doing per-follower Redis ZADD.
 *
 * Contention pattern: writes to g_next_post_id (global hot-spot),
 * g_nodes[author].post_count, and g_nodes[follower].timeline_count
 * for every follower — this is the key write-amplification pattern
 * that distinguishes compose-post from a simple counter increment. */
TX void tx_compose_post(int64_t author_id) {
    // [SIMPL] UniqueIdService returns timestamp-based Snowflake ID;
    // we use a simple sequential counter.
    int64_t pid = g_next_post_id;
    g_next_post_id = pid + 1;

    // User's own post count — original: UserTimelineService tracks this
    g_nodes[author_id].post_count = g_nodes[author_id].post_count + 1;

    // Fan-out write to all followers — [SIMPL] original does Redis ZADD
    // for every follower via WriteHomeTimelineService.
    for (size_t i = 0; i < g_followers[author_id].size(); ++i) {
        int follower_id = g_followers[author_id][i];
        g_nodes[follower_id].timeline_count =
            g_nodes[follower_id].timeline_count + 1;
    }
}

/**
 * Read Home Timeline  —  original: GET /wrk2-api/home-timeline/read
 *                         handler: HomeTimelineHandler::ReadHomeTimeline()
 *
 * Original chain:
 *   1. Redis ZREVRANGE user_id start stop → post_ids (from home-timeline
 *      zset populated by fan-out on compose)
 *   2. PostStorageService::ReadPosts(post_ids) → Post objects (Memcached
 *      + MongoDB)
 *
 * Here we substitute: sum post_count of every user that 'user_id' follows.
 * This preserves the "read data written by followed users" semantic and
 * exercises the same read-set size (3–12 nodes, matching followee count).
 *
 * [SIMPL] No pagination (start/stop params), no Post object deserialisation,
 * no Redis read.  The original uses a fan-out inbox model where the home
 * timeline is pre-populated at compose time; we compute it on read. */
TX int64_t tx_read_home_timeline(int64_t user_id) {
    int64_t total = 0;
    // Read each followee's post_count — [SIMPL] original reads post_ids
    // from Redis then fetches full Post objects
    for (size_t i = 0; i < g_followees[user_id].size(); ++i) {
        int followee_id = g_followees[user_id][i];
        total += g_nodes[followee_id].post_count;
    }
    return total;
}

/**
 * Read User Timeline  —  original: GET /wrk2-api/user-timeline/read
 *                         handler: UserTimelineHandler::ReadUserTimeline()
 *
 * Original chain:
 *   1. Redis ZREVRANGE user_id start stop → post_ids (from user's own
 *      timeline zset, populated at compose time)
 *   2. PostStorageService::ReadPosts(post_ids) → Post objects
 *
 * [SIMPL] We read a single post_count integer instead of fetching
 * a paginated list of Post objects. */
TX int64_t tx_read_user_timeline(int64_t user_id) {
    return g_nodes[user_id].post_count;
}

/* =================================================================
 * Follow / Unfollow  —  original endpoint:
 *   POST /wrk2-api/user/follow  (--script follow-user.lua)
 *   POST /wrk2-api/user/unfollow (--script unfollow-user.lua)
 *                         handler: UserHandler::Follow() / Unfollow()
 *
 * Original chain:
 *   1. SocialGraphService::Follow(a, b) → insert edge in MongoDB
 *      "social-graph" collection + Redis zsets for user 'a' following 'b'
 *      and user 'b' followed-by 'a'
 *   2. UserService::getUser(a) / getUser(b) → update follower/following
 *      counts in MongoDB "user" collection
 *
 * [SIMPL] We only update the in-memory follower_count/following_count
 * counters on both SocialNodes (no persistent graph mutation, no Redis
 * zset maintenance).  Invariant: sum(follower_count) == sum(following_count)
 * after every complete follow/unfollow.
 *
 * Contention pattern: dual-write to two different SocialNode records.
 * Unlike compose-post (fan-out writes to MANY followers), follow/unfollow
 * touches exactly two nodes — a more targeted contention pattern. */
TX void tx_follow(int64_t follower_id, int64_t followee_id) {
    g_nodes[followee_id].follower_count =
        g_nodes[followee_id].follower_count + 1;
    g_nodes[follower_id].following_count =
        g_nodes[follower_id].following_count + 1;
}

TX void tx_unfollow(int64_t follower_id, int64_t followee_id) {
    g_nodes[followee_id].follower_count =
        g_nodes[followee_id].follower_count - 1;
    g_nodes[follower_id].following_count =
        g_nodes[follower_id].following_count - 1;
}

/* =================================================================
 * Workload dispatcher
 *
 * Matches the original DeathStarBench mixed-workload.lua ratios:
 *   60% Read Home Timeline
 *   30% Read User Timeline
 *   10% Compose Post
 *
 * The original mixed-workload.lua (lines 89–98) defines:
 *   local read_home_timeline_ratio = 0.60
 *   local read_user_timeline_ratio = 0.30
 *   local compose_post_ratio       = 0.10
 *
 * Follow/Unfollow are NOT part of the mixed workload in the original.
 * They are run as separate wrk2 scripts (--script follow-user.lua
 * / --script unfollow-user.lua).  Enable with --test follow-unfollow. */
void do_transaction_work(std::mt19937 &rng, int nb_users,
                         std::atomic<uint64_t> *ops) {
    double roll = std::uniform_real_distribution<double>(0.0, 100.0)(rng);

    if (roll < 60.0) {
        int user = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
        tx_read_home_timeline(user);
    } else if (roll < 90.0) {
        int user = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
        tx_read_user_timeline(user);
    } else {
        int author = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
        tx_compose_post(author);
    }
    ops->fetch_add(1);
}

/* Follow-unfollow workload (--test follow-unfollow).
 * 50% follow, 50% unfollow, always targeting different users. */
void do_follow_unfollow_work(std::mt19937 &rng, int nb_users,
                             std::atomic<uint64_t> *ops) {
    int a = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
    int b = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
    if (a == b) { b = (b + 1) % nb_users; }  // avoid self-follow

    if (std::uniform_int_distribution<int>(0, 1)(rng) == 0) {
        tx_follow(a, b);
    } else {
        tx_unfollow(a, b);
    }
    ops->fetch_add(1);
}

THREAD void worker_thread(ThreadData &data) {
    auto rng = std::mt19937(data.seed);
    data.barrier->wait();
    while (!stop_workers.load(std::memory_order_relaxed)) {
        do_transaction_work(rng, g_node_count, &data.ops);
    }
}

THREAD void follow_worker_thread(ThreadData &data) {
    auto rng = std::mt19937(data.seed);
    data.barrier->wait();
    while (!stop_workers.load(std::memory_order_relaxed)) {
        do_follow_unfollow_work(rng, g_node_count, &data.ops);
    }
}

/* =================================================================
 * Invariant checks
 *
 * Key invariant: g_next_post_id == sum(g_nodes[].post_count)
 *
 * Every tx_compose_post atomically increments both the global post-ID
 * counter (one per call) and exactly one user's post_count.  If the TM
 * system loses or duplicates an update, the invariant fails.
 *
 * CHECKED IN A SINGLE TRANSACTION — this ensures we read a consistent
 * snapshot, unlike two separate TX calls which might see different
 * points in time.
 *
 * NOTE: The original invariant was total_followers == total_following,
 * which is less meaningful here because the follower graph is static.
 * ================================================================= */
TX bool tx_invariant_holds() {
    int64_t total_posts = 0;
    for (int64_t i = 0; i < g_node_count; ++i) {
        total_posts += g_nodes[i].post_count;
    }
    return total_posts == g_next_post_id;
}

TX int64_t tx_social_posts() {
    int64_t total = 0;
    for (int64_t i = 0; i < g_node_count; ++i) {
        total += g_nodes[i].post_count;
    }
    return total;
}

TX int64_t tx_expected_posts() {
    return g_next_post_id;
}

/* Follow/unfollow invariant: sum(follower_count) == sum(following_count).
 * Every follow increments both, every unfollow decrements both. */
TX bool tx_follow_invariant_holds() {
    int64_t total_followers = 0;
    int64_t total_following = 0;
    for (int64_t i = 0; i < g_node_count; ++i) {
        total_followers += g_nodes[i].follower_count;
        total_following += g_nodes[i].following_count;
    }
    return total_followers == total_following;
}

/* Non-transactional snapshot — for informational output only
 * (may see inconsistent state due to concurrent modifications). */
int64_t nontx_total_posts() {
    int64_t total = 0;
    for (int64_t i = 0; i < g_node_count; ++i) {
        total += g_nodes[i].post_count;
    }
    return total;
}

/* =================================================================
 * Initialisation and main loop
 * ================================================================= */
MAIN int main(int argc, char *argv[]) {
    int duration_ms = DEFAULT_DURATION_MS;
    int nb_users = DEFAULT_NB_USERS;
    int nb_threads = DEFAULT_NB_THREADS;
    bool follow_unfollow_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "-d" && i + 1 < argc) {
            duration_ms = std::stoi(argv[++i]);
        } else if (arg == "-u" && i + 1 < argc) {
            nb_users = std::stoi(argv[++i]);
        } else if (arg == "-t" && i + 1 < argc) {
            nb_threads = std::stoi(argv[++i]);
        } else if (arg == "--test" && i + 1 < argc) {
            std::string mode(argv[++i]);
            if (mode == "follow-unfollow") {
                follow_unfollow_mode = true;
            } else {
                std::cerr << "Unknown test mode: " << mode << "\n"
                          << "Available: follow-unfollow\n";
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "DeathStarBench Social TM Benchmark\n"
                      << "Reference: Gan et al., ASPLOS 2019\n"
                      << "https://doi.org/10.1145/3297858.3304013\n"
                      << "\n"
                      << "Usage: social_tm [options]\n"
                      << "  -d <ms>       Duration (default: 10000)\n"
                      << "  -u <n>        Users (default: 256)\n"
                      << "  -t <n>        Threads (default: 4)\n"
                      << "  --test <mode> Run specialised workload:\n"
                      << "                  follow-unfollow  50/50 follow/unfollow\n"
                      << "  -h, --help    Show help\n"
                      << "\n"
                      << "Workload mix (default, DeathStarBench mixed-workload.lua):\n"
                      << "  60% Read Home Timeline\n"
                      << "  30% Read User Timeline\n"
                      << "  10% Compose Post (with fan-out)\n";
            return 0;
        }
    }

    std::cout << "DeathStarBench Social TM Benchmark\n"
              << "Reference: Gan et al., ASPLOS 2019\n"
              << "====================================\n"
              << "Duration:  " << duration_ms << " ms\n"
              << "Users:     " << nb_users << "\n"
              << "Threads:   " << nb_threads << "\n"
              << std::endl;

    /* ---- Initialise TM globals ---- */
    g_node_count = nb_users;
    g_next_post_id = 0;
    g_nodes = new SocialNode[nb_users]();

    /* ---- Build follower graph ---- *
     *
     * Original: SocialGraphService stores follower/followee relationships
     * in MongoDB.  A loader script populates the graph before the
     * benchmark runs.  Here we generate a random directed graph with
     * MIN_FOLLOWEES–MAX_FOLLOWEES outgoing edges per user.
     *
     * [SIMPL] Uniform random degree (original graph follows a power-law
     * distribution typical of social networks).  The graph is fixed once
     * built — no concurrent follow/unfollow during the benchmark. */
    g_followees.resize(nb_users);
    g_followers.resize(nb_users);
    {
        std::mt19937 graph_rng(42);
        for (int i = 0; i < nb_users; ++i) {
            int num_followees = std::uniform_int_distribution<int>(
                MIN_FOLLOWEES, MAX_FOLLOWEES)(graph_rng);
            // Build candidate list (all users except self)
            std::vector<int> candidates;
            candidates.reserve(static_cast<size_t>(nb_users) - 1);
            for (int j = 0; j < nb_users; ++j) {
                if (j != i) candidates.push_back(j);
            }
            std::shuffle(candidates.begin(), candidates.end(), graph_rng);
            g_followees[i].assign(
                candidates.begin(),
                candidates.begin() + num_followees);
            // Reverse: build follower lists
            for (int f : g_followees[i]) {
                g_followers[f].push_back(i);
            }
            g_nodes[i].follower_count =
                static_cast<int64_t>(g_followers[i].size());
            g_nodes[i].following_count =
                static_cast<int64_t>(g_followees[i].size());
            g_nodes[i].post_count = 0;
            g_nodes[i].timeline_count = 0;
        }
    }

    /* ---- Print graph stats (mixed mode only) ---- */
    if (!follow_unfollow_mode) {
        size_t total_edges = 0;
        for (auto &fl : g_followees) total_edges += fl.size();
        std::cout << "Graph:       " << total_edges
                  << " follow-edges ("
                  << (total_edges / static_cast<double>(nb_users))
                  << " avg followees/user)\n";
    }
    std::cout << std::endl;

    /* ---- Launch worker threads ---- */
    Barrier barrier(nb_threads);
    std::vector<std::unique_ptr<ThreadData>> thread_data;
    std::vector<std::thread> threads;

    for (int i = 0; i < nb_threads; ++i) {
        auto data = std::make_unique<ThreadData>();
        data->barrier = &barrier;
        data->seed = i + 5678;
        data->thread_id = i;
        data->nb_threads = nb_threads;
        thread_data.push_back(std::move(data));
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < nb_threads; ++i) {
        if (follow_unfollow_mode) {
            threads.emplace_back(follow_worker_thread, std::ref(*thread_data[i]));
        } else {
            threads.emplace_back(worker_thread, std::ref(*thread_data[i]));
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    stop_workers.store(true, std::memory_order_release);
    for (auto &t : threads) {
        t.join();
    }

    /* ---- Collect results ---- */
    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    uint64_t total_ops = 0;
    for (const auto &data : thread_data) {
        total_ops += data->ops.load();
    }

    if (follow_unfollow_mode) {
        bool invariant_ok = tx_follow_invariant_holds();

        std::cout << "\nDeathStarBench Social TM — Follow/Unfollow Results\n"
                  << "==================================================\n"
                  << "Elapsed:       " << elapsed_ms << " ms\n"
                  << "Total ops:     " << total_ops << "\n"
                  << "Ops/sec:       "
                  << (total_ops * 1000.0 / elapsed_ms) << "\n"
                  << std::endl;

        delete[] g_nodes;

        if (invariant_ok) {
            std::cout << "PASS: sum(follower_count) == sum(following_count)\n";
            return 0;
        } else {
            std::cerr << "FAIL: sum(follower_count) != sum(following_count)\n"
                      << "      Atomicity violation detected.\n";
            return 1;
        }
    } else {
        bool invariant_ok = tx_invariant_holds();
        int64_t final_tx_posts = tx_social_posts();
        int64_t final_tx_expected = tx_expected_posts();
        int64_t final_posts = nontx_total_posts();

        std::cout << "\nDeathStarBench Social TM Results\n"
                  << "==================================\n"
                  << "Elapsed:       " << elapsed_ms << " ms\n"
                  << "Total ops:     " << total_ops << "\n"
                  << "Ops/sec:       "
                  << (total_ops * 1000.0 / elapsed_ms) << "\n"
                  << "Posts (TX):    " << final_tx_posts << "\n"
                  << "Expected (TX): " << final_tx_expected << "\n"
                  << "Posts (NT):    " << final_posts << "\n"
                  << std::endl;

        delete[] g_nodes;

        /* ---- Invariant verification ---- *
         *
         * Invariant: total post_count across all users == g_next_post_id.
         * Every tx_compose_post increments both atomically.  A TM
         * correctness bug (lost update, phantom read, write skew, etc.)
         * will cause them to diverge. */
        if (invariant_ok) {
            std::cout << "PASS: sum(post_count) == g_next_post_id  ["
                      << final_tx_posts << " == " << final_tx_expected
                      << "]\n"
                      << "      Invariant verified in single atomic snapshot.\n";
            return 0;
        } else {
            std::cerr << "FAIL: sum(post_count) != g_next_post_id  ["
                      << final_tx_posts << " != " << final_tx_expected << "]\n"
                      << "      Atomicity violation detected.\n";
            return 1;
        }
    }
}
