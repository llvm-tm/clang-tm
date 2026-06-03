#include <cstdio>
#include <thread>
#include <vector>

#include "tm_test_common.hpp"

TM std::vector<int> g_queue;

TX void push_task() { g_queue.push_back(1); }

TX void pop_task() { if (!g_queue.empty()) g_queue.pop_back(); }

THREAD void worker() {
    for (int i = 0; i < 50000; i++) {
        push_task();
        pop_task();
    }
}

MAIN int main() {
    printf("task_queue_race: start\n"); fflush(stdout);
    std::thread t1(worker);
    std::thread t2(worker);
    std::thread t3(worker);
    std::thread t4(worker);
    t1.join(); t2.join(); t3.join(); t4.join();
    printf("task_queue_race: done (size=%zu)\n", g_queue.size()); fflush(stdout);
    return 0;
}
