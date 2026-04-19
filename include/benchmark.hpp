#pragma once

#include "scheduler.hpp"
#include "task.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>
#include <atomic>

namespace tsr::bench {

struct Timer {
    using Clock = std::chrono::steady_clock;
    using Dur   = std::chrono::duration<double>;
    Clock::time_point start_ = Clock::now();
    double elapsed_sec() const { return Dur(Clock::now() - start_).count(); }
};

struct BenchResult {
    std::string  scheduler_name;
    std::string  benchmark_name;
    uint32_t     num_workers;
    uint64_t     num_tasks;
    double       wall_sec;
    double       throughput;

    void print() const {
        std::cout
            << std::left
            << std::setw(18) << scheduler_name
            << std::setw(22) << benchmark_name
            << std::setw(8)  << num_workers
            << std::setw(12) << num_tasks
            << std::fixed << std::setprecision(3)
            << std::setw(10) << wall_sec
            << std::setw(14) << std::llround(throughput)
            << "\n";
    }
};

inline void print_header() {
    std::cout
        << std::left
        << std::setw(18) << "Scheduler"
        << std::setw(22) << "Benchmark"
        << std::setw(8)  << "Workers"
        << std::setw(12) << "Tasks"
        << std::setw(10) << "Wall(s)"
        << std::setw(14) << "Tasks/s"
        << "\n"
        << std::string(84, '-') << "\n";
}

inline BenchResult run_bench(
    IScheduler&                          sched,
    const std::string&                   bench_name,
    uint32_t                             num_workers,
    std::function<uint64_t(IScheduler&)> setup_fn)
{
    // Warm-up.
    { setup_fn(sched); sched.run(num_workers); }

    uint64_t total_tasks = setup_fn(sched);
    Timer t;
    sched.run(num_workers);
    double wall = t.elapsed_sec();

    BenchResult r;
    r.scheduler_name = sched.name();
    r.benchmark_name = bench_name;
    r.num_workers    = num_workers;
    r.num_tasks      = total_tasks;
    r.wall_sec       = wall;
    r.throughput     = static_cast<double>(total_tasks) / wall;
    return r;
}

// ============================================================================
// BENCHMARK SCENARIOS
// ============================================================================

// 1. CPU-bound tasks
inline uint64_t setup_cpu_bound(IScheduler& sched, uint64_t num_tasks, uint64_t work_iters) {
    for (uint64_t i = 0; i < num_tasks; ++i) {
        sched.submit(make_task([work_iters](IScheduler&) {
            volatile uint64_t acc = 0;
            for (uint64_t k = 0; k < work_iters; ++k) acc += k;
            (void)acc;
        }));
    }
    return num_tasks;
}

// 2. Recursive task spawning
inline std::atomic<uint64_t> g_task_count{0};

inline void recursive_spawn(IScheduler& sched, uint32_t depth) {
    g_task_count.fetch_add(1, std::memory_order_relaxed);
    if (depth == 0) return;
    sched.submit(make_task([depth](IScheduler& s) { recursive_spawn(s, depth - 1); }));
    sched.submit(make_task([depth](IScheduler& s) { recursive_spawn(s, depth - 1); }));
}

inline uint64_t setup_recursive(IScheduler& sched, uint32_t tree_depth) {
    g_task_count.store(0, std::memory_order_relaxed);
    sched.submit(make_task([tree_depth](IScheduler& s) { recursive_spawn(s, tree_depth); }));
    return (uint64_t(1) << (tree_depth + 1)) - 1;
}

// 3. Tiny tasks (contention-heavy)
inline uint64_t setup_tiny_tasks(IScheduler& sched, uint64_t num_tasks) {
    for (uint64_t i = 0; i < num_tasks; ++i) {
        sched.submit(make_task([](IScheduler&) {}));
    }
    return num_tasks;
}

// ============================================================================
// Suite runner
// ============================================================================
inline std::vector<BenchResult> run_suite(
    IScheduler&           sched,
    std::vector<uint32_t> thread_counts)
{
    std::vector<BenchResult> results;
    for (uint32_t nw : thread_counts) {
        results.push_back(run_bench(sched, "cpu_bound_50k", nw,
            [](IScheduler& s) { return setup_cpu_bound(s, 50'000, 500); }));

        results.push_back(run_bench(sched, "recursive_d12", nw,
            [](IScheduler& s) { return setup_recursive(s, 12); }));

        results.push_back(run_bench(sched, "tiny_tasks_100k", nw,
            [](IScheduler& s) { return setup_tiny_tasks(s, 100'000); }));
    }
    return results;
}

} // namespace tsr::bench
