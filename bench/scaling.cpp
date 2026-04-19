// ============================================================================
// Scaling benchmark
//
// For each scheduler, runs the same workload at 1, 2, 4, 8 threads and reports
// throughput and speedup vs the single-threaded baseline. Answers "does it
// scale" directly and exposes the plateau where contention or oversubscription
// dominates.
//
// Each data point is the median of 3 runs to reduce timing noise. Warm-up pass
// is discarded.
// ============================================================================

#include "benchmark.hpp"
#include "mutex_queue_scheduler.hpp"
#include "lockfree_queue_scheduler.hpp"
#include "work_stealing_scheduler.hpp"
#include "task_graph.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace tsr;
using namespace tsr::bench;

// ---------------------------------------------------------------------------
// Workload definitions
// ---------------------------------------------------------------------------

// Small-but-nonzero CPU burn. Tuned so single-threaded run takes ~100ms —
// big enough to dwarf measurement noise, small enough to run 12 configs
// quickly.
static constexpr uint64_t kWorkIters = 400;
static constexpr uint64_t kNumTasks  = 60'000;

// Flat workload: N independent CPU-bound tasks, submitted before run().
// Measures pure queue throughput + per-task overhead.
static double time_flat_workload(IScheduler& sched, uint32_t num_workers) {
    for (uint64_t i = 0; i < kNumTasks; ++i) {
        sched.submit(make_task([](IScheduler&) {
            volatile uint64_t acc = 0;
            for (uint64_t k = 0; k < kWorkIters; ++k) acc += k;
            (void)acc;
        }));
    }
    Timer t;
    sched.run(num_workers);
    return t.elapsed_sec();
}

// DAG workload: a 4-wide fork-join pipeline of depth D. Each "stage" has W
// parallel nodes that depend on all nodes from the previous stage. Simulates
// a simple ML-style operator graph or instruction pipeline.
//
//   stage 0 (W roots)
//       |  (all-to-all)
//   stage 1 (W nodes)
//       |
//   stage 2 (W nodes)
//       ...
//
// Total nodes = W * D. Per-task work is identical to the flat case so the two
// workloads are directly comparable.
static constexpr uint32_t kDagWidth = 32;
static constexpr uint32_t kDagDepth = 60;  // 1920 nodes total — enough work
                                            //  to see the parallelism
                                            //  amortise over graph overhead

static double time_dag_workload(IScheduler& sched, uint32_t num_workers) {
    TaskGraph g;

    // Build stage-by-stage, wiring each new stage to depend on all predecessors.
    std::vector<DagNode*> prev_stage;
    std::vector<DagNode*> cur_stage;
    cur_stage.reserve(kDagWidth);
    prev_stage.reserve(kDagWidth);

    for (uint32_t d = 0; d < kDagDepth; ++d) {
        cur_stage.clear();
        for (uint32_t w = 0; w < kDagWidth; ++w) {
            auto& n = g.node([]{
                volatile uint64_t acc = 0;
                for (uint64_t k = 0; k < kWorkIters; ++k) acc += k;
                (void)acc;
            });
            for (DagNode* p : prev_stage) n.depends_on(*p);
            cur_stage.push_back(&n);
        }
        prev_stage.swap(cur_stage);
    }

    Timer t;
    g.execute(sched, num_workers);
    return t.elapsed_sec();
}

// ---------------------------------------------------------------------------
// Median-of-N runner
// ---------------------------------------------------------------------------

static double median_of(std::vector<double> xs) {
    std::sort(xs.begin(), xs.end());
    return xs[xs.size() / 2];
}

template<typename TimedFn>
static double run_median(IScheduler& sched, uint32_t num_workers, TimedFn fn) {
    // Warm-up (discarded).
    fn(sched, num_workers);

    std::vector<double> samples;
    samples.reserve(3);
    for (int i = 0; i < 3; ++i) samples.push_back(fn(sched, num_workers));
    return median_of(std::move(samples));
}

// ---------------------------------------------------------------------------
// Print helpers
// ---------------------------------------------------------------------------

struct ScalingRow {
    std::string scheduler;
    std::string workload;
    uint32_t    threads;
    uint64_t    tasks;
    double      wall_sec;
    double      throughput;
    double      speedup;   // vs. 1-thread of the same scheduler/workload
    double      efficiency;// speedup / threads
};

static void print_scaling_header() {
    std::cout
        << std::left
        << std::setw(16) << "Scheduler"
        << std::setw(10) << "Workload"
        << std::setw(9)  << "Threads"
        << std::setw(12) << "Wall(s)"
        << std::setw(14) << "Tasks/s"
        << std::setw(10) << "Speedup"
        << std::setw(10) << "Efficiency"
        << "\n" << std::string(81, '-') << "\n";
}

static void print_row(const ScalingRow& r) {
    std::cout
        << std::left
        << std::setw(16) << r.scheduler
        << std::setw(10) << r.workload
        << std::setw(9)  << r.threads
        << std::fixed << std::setprecision(4)
        << std::setw(12) << r.wall_sec
        << std::setprecision(0)
        << std::setw(14) << r.throughput
        << std::setprecision(2)
        << std::setw(10) << r.speedup
        << std::setw(10) << r.efficiency
        << "\n";
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

static std::unique_ptr<IScheduler> make_scheduler(const std::string& name) {
    if (name == "MutexQueue")    return std::make_unique<MutexQueueScheduler>();
    if (name == "LockFreeQueue") return std::make_unique<LockFreeQueueScheduler>();
    if (name == "WorkStealing")  return std::make_unique<WorkStealingScheduler>();
    std::abort();
}

int main() {
    const uint32_t hw = std::thread::hardware_concurrency();
    std::cout << "Hardware concurrency: " << hw << " logical cores\n";
    std::cout << "Workload: " << kNumTasks << " flat tasks, or "
              << (kDagWidth * kDagDepth) << "-node DAG ("
              << kDagWidth << "w x " << kDagDepth << "d); "
              << kWorkIters << " iters per task\n\n";

    const std::vector<uint32_t>    thread_counts = {1, 2, 4, 8};
    const std::vector<std::string> sched_names   = {
        "MutexQueue", "LockFreeQueue", "WorkStealing"
    };

    std::vector<ScalingRow> all_rows;

    // ----- Flat workload ----------------------------------------------------
    std::cout << "=== Scaling: flat CPU-bound workload ===\n";
    print_scaling_header();
    for (const auto& sn : sched_names) {
        double baseline = -1.0;
        for (uint32_t n : thread_counts) {
            auto sched = make_scheduler(sn);
            double wall = run_median(*sched, n, time_flat_workload);
            double tps  = static_cast<double>(kNumTasks) / wall;
            if (n == 1) baseline = tps;
            ScalingRow r{
                sn, "flat", n, kNumTasks, wall, tps,
                (baseline > 0 ? tps / baseline : 1.0),
                (baseline > 0 ? (tps / baseline) / n : 1.0)
            };
            print_row(r);
            all_rows.push_back(r);
        }
    }

    // ----- DAG workload -----------------------------------------------------
    std::cout << "\n=== Scaling: DAG workload (" << kDagWidth << "w x "
              << kDagDepth << "d pipeline) ===\n";
    print_scaling_header();
    const uint64_t dag_nodes = kDagWidth * kDagDepth;
    for (const auto& sn : sched_names) {
        double baseline = -1.0;
        for (uint32_t n : thread_counts) {
            auto sched = make_scheduler(sn);
            double wall = run_median(*sched, n, time_dag_workload);
            double tps  = static_cast<double>(dag_nodes) / wall;
            if (n == 1) baseline = tps;
            ScalingRow r{
                sn, "dag", n, dag_nodes, wall, tps,
                (baseline > 0 ? tps / baseline : 1.0),
                (baseline > 0 ? (tps / baseline) / n : 1.0)
            };
            print_row(r);
            all_rows.push_back(r);
        }
    }

    // ----- Summary table: thread-count pivot --------------------------------
    std::cout << "\n=== Throughput summary (tasks/sec) ===\n";
    std::cout << std::left << std::setw(16) << "Scheduler"
              << std::setw(10) << "Workload";
    for (uint32_t n : thread_counts)
        std::cout << std::setw(14) << (std::to_string(n) + "T");
    std::cout << "\n" << std::string(16 + 10 + 14 * thread_counts.size(), '-') << "\n";

    for (const auto& sn : sched_names) {
        for (const char* wl : {"flat", "dag"}) {
            std::cout << std::left << std::setw(16) << sn
                      << std::setw(10) << wl;
            for (uint32_t n : thread_counts) {
                for (const auto& r : all_rows) {
                    if (r.scheduler == sn && r.workload == wl && r.threads == n) {
                        std::cout << std::setw(14) << std::llround(r.throughput);
                        break;
                    }
                }
            }
            std::cout << "\n";
        }
    }

    return 0;
}
