Task Scheduler Runtime
A C++20 task scheduling runtime comparing three scheduler strategies with a DAG execution layer.
Built as a portfolio project for systems/runtime engineering, with a focus on correctness and measurable performance.

What it does
Runs tasks across multiple worker threads using three different scheduling strategies.
Tasks can spawn child tasks at runtime. Tasks can also be wired into a dependency graph (DAG),
where a task only executes after all its predecessors have finished.

Three schedulers
SchedulerHow it worksWhen it winsMutexQueueSingle shared queue, protected by a mutexSingle-threaded, low-frequency workloadsLockFreeQueueVyukov MPMC ring buffer, no locksBalanced multi-producer multi-consumerWorkStealingPer-thread Chase-Lev deque, idle workers stealRecursive spawning, irregular workloads

Project structure
task_scheduler/
├── include/                         # Header-only library (include these)
│   ├── task.hpp                     # Task type
│   ├── scheduler.hpp                # IScheduler interface
│   ├── task_graph.hpp               # DAG execution layer
│   ├── mutex_queue_scheduler.hpp
│   ├── lockfree_queue_scheduler.hpp
│   ├── chase_lev_deque.hpp
│   ├── work_stealing_scheduler.hpp
│   └── benchmark.hpp
│
├── src/
│   └── tests.cpp                    # Correctness tests
│
├── bench/
│   ├── main.cpp                     # Throughput benchmark
│   └── scaling.cpp                  # 1 / 2 / 4 / 8 thread scaling sweep
│
├── docs/
│   └── ENGINEERING.md               # Design decisions, memory ordering, results
│
└── CMakeLists.txt

Build
bash# Configure and build everything
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
This produces four binaries inside build/:
BinaryWhat it runstest_runnerAll correctness teststest_asanSame tests under AddressSanitizer + UBSanitizertest_tsanSame tests under ThreadSanitizerbench_runnerThroughput benchmarkbench_scalingScaling sweep across 1 / 2 / 4 / 8 threads
Or build manually without CMake:
bash# Tests under sanitizers
g++ -std=c++20 -O1 -g -Iinclude -fsanitize=address,undefined src/tests.cpp -o test_asan -pthread
g++ -std=c++20 -O1 -g -Iinclude -fsanitize=thread            src/tests.cpp -o test_tsan -pthread

# Benchmarks
g++ -std=c++20 -O3 -march=native -Iinclude bench/main.cpp    -o bench_runner  -pthread
g++ -std=c++20 -O3 -march=native -Iinclude bench/scaling.cpp -o bench_scaling -pthread

Usage
Flat tasks
cpp#include "work_stealing_scheduler.hpp"

tsr::WorkStealingScheduler sched;

for (int i = 0; i < 10000; ++i) {
    sched.submit(tsr::make_task([](tsr::IScheduler&) {
        // your work here
    }));
}

sched.run(4); // 4 worker threads
Tasks that spawn children
cppsched.submit(tsr::make_task([](tsr::IScheduler& s) {
    s.submit(tsr::make_task([](tsr::IScheduler&) {
        // child task
    }));
}));

sched.run(4);
DAG execution
cpp#include "task_graph.hpp"
#include "work_stealing_scheduler.hpp"

tsr::WorkStealingScheduler sched;
tsr::TaskGraph g;

//      A
//     / \
//    B   C
//     \ /
//      D

auto& a = g.node([] { /* ... */ });
auto& b = g.node([] { /* ... */ });
auto& c = g.node([] { /* ... */ });
auto& d = g.node([] { /* ... */ });

b.depends_on(a);
c.depends_on(a);
d.depends_on(b, c);   // D runs only after both B and C finish

g.execute(sched, 4);  // 4 worker threads

Benchmark results (2-core machine)
Flat workload — 60k tasks, throughput vs thread count
Scheduler        1T            2T            4T            8T
--------------------------------------------------------------
MutexQueue       4,638,463     627,025       609,524       690,702
LockFreeQueue      767,804   3,738,964     3,898,292       911,362
WorkStealing       801,333   4,225,321     4,554,518     4,408,246
DAG workload — 32-wide × 60-deep pipeline, throughput vs thread count
Scheduler        1T            2T            4T            8T
--------------------------------------------------------------
MutexQueue       1,802,289     523,803       141,160       127,796
LockFreeQueue    1,546,903     849,398       793,207       579,146
WorkStealing     1,455,867     831,044       786,257       676,545
Key observations:

MutexQueue peaks at 1 thread (uncontended futex) then collapses — adding a second worker causes a 7.4× regression
WorkStealing degrades only ~10% from 2T to 8T under oversubscription; the per-thread deque avoids shared state on the fast path
LockFreeQueue is stable through 4T but cliffs at 8T due to contention on the shared consumer cursor
DAG workloads scale worse than flat because the critical path depth (60 stages) is a hard limit no amount of threads can shorten


Correctness
25 tests covering:

Chase-Lev deque: empty/push/pop, single-thief steal, concurrent multi-thief steal
Schedulers: parallel sum, child task spawning — all three strategies
DAG: linear chain ordering, diamond (multiple predecessors), fork-join (1 root → 64 leaves → 1 sink)

All pass under AddressSanitizer, UBSanitizer, and ThreadSanitizer with zero reported errors.

Requirements

GCC 13+ or Clang 16+
C++20
POSIX threads
CMake 3.20+ (optional)
