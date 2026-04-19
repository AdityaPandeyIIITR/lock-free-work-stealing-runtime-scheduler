#include "benchmark.hpp"
#include "mutex_queue_scheduler.hpp"
#include "lockfree_queue_scheduler.hpp"
#include "work_stealing_scheduler.hpp"

#include <iostream>
#include <vector>
#include <thread>
#include <memory>
#include <iomanip>
#include <algorithm>

int main() {
    using namespace tsr;
    using namespace tsr::bench;

    const uint32_t hw = std::thread::hardware_concurrency();
    std::cout << "Hardware concurrency: " << hw << " logical cores\n\n";

    // Thread counts to sweep.
    std::vector<uint32_t> thread_counts;
    for (uint32_t n = 1; n <= hw; n = (n < 4 ? n + 1 : n * 2))
        thread_counts.push_back(std::min(n, hw));
    thread_counts.erase(
        std::unique(thread_counts.begin(), thread_counts.end()),
        thread_counts.end());

    MutexQueueScheduler   mutex_sched;
    LockFreeQueueScheduler lf_sched;
    WorkStealingScheduler  ws_sched;

    std::vector<IScheduler*> schedulers = {
        &mutex_sched, &lf_sched, &ws_sched
    };

    std::vector<BenchResult> all_results;

    for (IScheduler* sched : schedulers) {
        std::cout << "\n=== " << sched->name() << " ===\n";
        print_header();
        auto results = run_suite(*sched, thread_counts);
        for (auto& r : results) {
            r.print();
            all_results.push_back(r);
        }
        std::cout << std::flush;
    }

    // Speedup table.
    std::cout << "\n\n=== SPEEDUP: WorkStealing vs MutexQueue at "
              << thread_counts.back() << " threads ===\n";
    std::cout << std::left
              << std::setw(26) << "Benchmark"
              << std::setw(14) << "Mutex(t/s)"
              << std::setw(14) << "WS(t/s)"
              << std::setw(10) << "Speedup"
              << "\n"
              << std::string(64, '-') << "\n";

    auto find_tp = [&](const std::string& sname,
                        const std::string& bname,
                        uint32_t nw) -> double {
        for (auto& r : all_results)
            if (r.scheduler_name == sname && r.benchmark_name == bname && r.num_workers == nw)
                return r.throughput;
        return 1.0;
    };

    std::vector<std::string> bench_names = {
        "cpu_bound_50k", "recursive_d12", "tiny_tasks_100k"
    };
    for (auto& bn : bench_names) {
        double mt = find_tp("MutexQueue",   bn, thread_counts.back());
        double ws = find_tp("WorkStealing", bn, thread_counts.back());
        std::cout << std::left
                  << std::setw(26) << bn
                  << std::fixed << std::setprecision(0)
                  << std::setw(14) << mt
                  << std::setw(14) << ws
                  << std::setprecision(2)
                  << std::setw(10) << (ws / mt)
                  << "\n";
    }

    return 0;
}
