#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("shared"), noinline))
#define THREAD __attribute__((annotate("thread"), noinline))
#define MAIN __attribute__((annotate("main"), noinline))

constexpr int DEFAULT_DURATION_MS = 10000;
constexpr int DEFAULT_NB_USERS = 256;
constexpr int DEFAULT_NB_THREADS = 4;
constexpr int INITIAL_BALANCE = 1000;
constexpr int MAX_POSTS = 1000000;

struct Post {
	int64_t post_id;
	int64_t author_id;
	int64_t timestamp;
};

struct SocialNode {
	TM int64_t post_count;
	TM int64_t follower_count;
	TM int64_t following_count;
};

struct FollowEdge {
	int follower;
	int followee;
};

TM int64_t g_next_post_id;
TM SocialNode *g_nodes;
TM int64_t g_node_count;
int64_t g_edge_count;
std::vector<FollowEdge> g_edges;

class Barrier
{
private:
	std::mutex mutex_;
	std::condition_variable cv_;
	int count_;
	int num_threads_;
	int crossing_;

public:
	explicit Barrier(int n)
	    : count_(n), num_threads_(n), crossing_(0) {}
	void wait()
	{
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

TX void tx_create_post(int64_t author_id)
{
	int64_t pid = g_next_post_id;
	g_next_post_id = pid + 1;
	g_nodes[author_id].post_count = g_nodes[author_id].post_count + 1;
}

TX void tx_follow(int follower, int followee)
{
	g_nodes[follower].following_count =
	    g_nodes[follower].following_count + 1;
	g_nodes[followee].follower_count =
	    g_nodes[followee].follower_count + 1;
}

TX void tx_unfollow(int follower, int followee)
{
	g_nodes[follower].following_count =
	    g_nodes[follower].following_count - 1;
	g_nodes[followee].follower_count =
	    g_nodes[followee].follower_count - 1;
}

TX int64_t tx_read_timeline(int user_id, int nb_users)
{
	int64_t total = 0;
	for (int i = 0; i < nb_users; ++i) {
		total += g_nodes[i].post_count;
	}
	return total;
}

TX void tx_transfer_post(int64_t from_author, int64_t to_author)
{
	g_nodes[from_author].post_count =
	    g_nodes[from_author].post_count - 1;
	g_nodes[to_author].post_count =
	    g_nodes[to_author].post_count + 1;
}

void do_transaction_work(std::mt19937 &rng, int nb_users,
                         std::atomic<uint64_t> *ops)
{
	double roll = std::uniform_real_distribution<double>(0.0, 100.0)(rng);

	if (roll < 50.0) {
		int author = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		tx_create_post(author);
	} else if (roll < 80.0) {
		int user = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		tx_read_timeline(user, nb_users);
	} else if (roll < 90.0) {
		int follower = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		int followee = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		if (follower != followee) {
			tx_follow(follower, followee);
		}
	} else if (roll < 95.0) {
		int follower = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		int followee = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		if (follower != followee) {
			tx_unfollow(follower, followee);
		}
	} else {
		int src = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		int dst = std::uniform_int_distribution<int>(0, nb_users - 1)(rng);
		if (src != dst) {
			tx_transfer_post(src, dst);
		}
	}
	ops->fetch_add(1);
}

THREAD void worker_thread(ThreadData &data)
{
	auto rng = std::mt19937(data.seed);
	data.barrier->wait();
	while (!stop_workers.load(std::memory_order_relaxed)) {
		do_transaction_work(rng, g_node_count, &data.ops);
	}
}

TX int64_t tx_total_social_posts()
{
	int64_t total = 0;
	for (int64_t i = 0; i < g_node_count; ++i) {
		total += g_nodes[i].post_count;
	}
	return total;
}

TX int64_t tx_total_followers()
{
	int64_t total = 0;
	for (int64_t i = 0; i < g_node_count; ++i) {
		total += g_nodes[i].follower_count;
	}
	return total;
}

TX int64_t tx_total_following()
{
	int64_t total = 0;
	for (int64_t i = 0; i < g_node_count; ++i) {
		total += g_nodes[i].following_count;
	}
	return total;
}

int64_t nontx_total_posts()
{
	int64_t total = 0;
	for (int64_t i = 0; i < g_node_count; ++i) {
		total += g_nodes[i].post_count;
	}
	return total;
}

int64_t nontx_total_followers()
{
	int64_t total = 0;
	for (int64_t i = 0; i < g_node_count; ++i) {
		total += g_nodes[i].follower_count;
	}
	return total;
}

int64_t nontx_total_following()
{
	int64_t total = 0;
	for (int64_t i = 0; i < g_node_count; ++i) {
		total += g_nodes[i].following_count;
	}
	return total;
}

MAIN int main(int argc, char *argv[])
{
	int duration_ms = DEFAULT_DURATION_MS;
	int nb_users = DEFAULT_NB_USERS;
	int nb_threads = DEFAULT_NB_THREADS;

	for (int i = 1; i < argc; ++i) {
		std::string arg(argv[i]);
		if (arg == "-d" && i + 1 < argc) {
			duration_ms = std::stoi(argv[++i]);
		} else if (arg == "-u" && i + 1 < argc) {
			nb_users = std::stoi(argv[++i]);
		} else if (arg == "-t" && i + 1 < argc) {
			nb_threads = std::stoi(argv[++i]);
		} else if (arg == "-h" || arg == "--help") {
			std::cout << "DeathStarBench Social TM Benchmark\n"
			          << "Usage: social_tm [options]\n"
			          << "  -d <ms>       Duration (default: 10000)\n"
			          << "  -u <n>        Users (default: 256)\n"
			          << "  -t <n>        Threads (default: 4)\n"
			          << "  -h, --help    Show help\n";
			return 0;
		}
	}

	std::cout << "DeathStarBench Social TM Benchmark\n"
	          << "=====================================\n"
	          << "Duration:  " << duration_ms << " ms\n"
	          << "Users:     " << nb_users << "\n"
	          << "Threads:   " << nb_threads << "\n"
	          << std::endl;

	g_node_count = nb_users;
	g_next_post_id = 0;
	g_nodes = new SocialNode[nb_users]();

	for (int i = 0; i < nb_users; ++i) {
		g_nodes[i].post_count = 0;
		g_nodes[i].follower_count = 0;
		g_nodes[i].following_count = 0;
	}

	std::cout << "Initial total posts: " << nontx_total_posts() << std::endl;

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
		threads.emplace_back(worker_thread, std::ref(*thread_data[i]));
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

	stop_workers.store(true, std::memory_order_release);
	for (auto &t : threads) {
		t.join();
	}

	int64_t final_tx_posts = tx_total_social_posts();
	int64_t final_tx_followers = tx_total_followers();
	int64_t final_tx_following = tx_total_following();
	int64_t final_posts = nontx_total_posts();
	int64_t final_followers = nontx_total_followers();
	int64_t final_following = nontx_total_following();
	auto end_time = std::chrono::high_resolution_clock::now();
	auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    end_time - start_time).count();

	uint64_t total_ops = 0;
	for (const auto &data : thread_data) {
		total_ops += data->ops.load();
	}

	std::cout << "\nDeathStarBench Social TM Results\n"
	          << "==================================\n"
	          << "Elapsed:    " << elapsed_ms << " ms\n"
	          << "Total ops:  " << total_ops << "\n"
	          << "Ops/sec:    " << (total_ops * 1000.0 / elapsed_ms) << "\n"
	          << "Posts (TX): " << final_tx_posts << "\n"
	          << "Posts (NT): " << final_posts << "\n"
	          << "Followers:  " << final_followers << "\n"
	          << "Following:  " << final_following << "\n"
	          << std::endl;

	delete[] g_nodes;

	if (final_followers == final_following) {
		std::cout << "PASS: follower == following invariant holds\n";
		return 0;
	} else {
		std::cerr << "FAIL: follower (" << final_followers
		          << ") != following (" << final_following << ")\n";
		return 1;
	}
}
