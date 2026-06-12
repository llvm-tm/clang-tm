#include <cstdio>
#include <cstdint>
#include <thread>
#include <random>
#include "tm_vector.hpp"

extern "C" {
extern void tm_init();
extern void tm_init_thread();
extern void tm_exit_thread();
extern void tm_begin();
extern void tm_end();
}

struct Account { uint64_t balance; };
TMSafeVector<Account> accounts(1024, Account{1000});

#define TX __attribute__((annotate("transaction"), noinline))

TX void do_tx(int src, int dst, uint64_t amt) {
    tm_begin();
    uint64_t s = accounts[src].balance;
    uint64_t d = accounts[dst].balance;
    accounts[src].balance = s - amt;
    accounts[dst].balance = d + amt;
    tm_end();
}

void worker(int id, int iterations) {
    tm_init_thread();
    std::mt19937 rng(id);
    std::uniform_int_distribution<int> dist(0, 1023);
    std::uniform_int_distribution<uint64_t> amt(1, 100);
    for (int i = 0; i < iterations; i++) {
        int src = dist(rng);
        int dst = dist(rng);
        while (dst == src) dst = dist(rng);
        uint64_t amount = amt(rng);
        do_tx(src, dst, amount);
    }
    tm_exit_thread();
}

int main(int argc, char** argv) {
    int threads = 1;
    int iters = 100;
    if (argc > 1) threads = atoi(argv[1]);
    if (argc > 2) iters = atoi(argv[2]);
    
    tm_init();
    
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; i++)
        workers.emplace_back(worker, i, iters);
    for (auto& w : workers) w.join();
    
    uint64_t total = 0;
    for (auto& a : accounts) total += a.balance;
    fprintf(stderr, "total=%llu (expected %llu)\n",
            (unsigned long long)total, (unsigned long long)1024*1000);
    return total == 1024*1000 ? 0 : 1;
}
