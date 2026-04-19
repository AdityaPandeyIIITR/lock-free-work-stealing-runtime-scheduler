# Task Scheduler Runtime

A C++20 task scheduling runtime comparing three scheduling strategies under real contention and dependency-aware execution.

---

## What This Is

A from-scratch runtime implementing three schedulers, a lock-free work-stealing deque, and a DAG execution layer — benchmarked against each other across thread counts to measure throughput, scaling, and contention behavior.

Built as a portfolio project targeting systems/runtime roles and PhD research in hardware-software co-design.

---

## Schedulers

| Strategy | Description |
|---|---|
| `MutexQueueScheduler` | Single global queue protected by `std::mutex` |
| `LockFreeQueueScheduler` | Vyukov bounded MPMC ring buffer, no locks |
| `WorkStealingScheduler` | Per-worker Chase-Lev deques with randomized stealing |

---

## Project Structure

include/          # Header-only library (all algorithms live here)
task.hpp
scheduler.hpp
mutex_queue_scheduler.hpp
lockfree_queue_scheduler.hpp
chase_lev_deque.hpp
work_stealing_scheduler.hpp
task_graph.hpp  # DAG execution layer
benchmark.hpp
src/
tests.cpp       # 25 correctness tests
bench/
main.cpp        # Throughput benchmark
scaling.cpp     # 1 / 2 / 4 / 8 thread scaling sweep
docs/
ENGINEERING.md  # Design decisions, memory ordering, ABA analysis


---

## Key Features

- **Three scheduling strategies** with a shared `IScheduler` interface
- **Chase-Lev work-stealing deque** with explicit acquire/release memory ordering
- **DAG execution layer** — atomic refcount-based dependency resolution, supports linear chains, diamonds, and fork-join graphs
- **Scaling benchmark** — fixed workload across 1/2/4/8 threads, median of 3 runs
- **Clean under ASan, UBSan, and TSan** — 25/25 tests pass with zero race reports

---

## Build

```bash
# Optimized benchmarks
g++ -std=c++20 -O3 -march=native -Iinclude bench/main.cpp    -o bench_runner  -pthread
g++ -std=c++20 -O3 -march=native -Iinclude bench/scaling.cpp -o bench_scaling -pthread

# Tests with sanitizers
g++ -std=c++20 -O1 -g -Iinclude -fsanitize=address,undefined src/tests.cpp -o test_asan -pthread
g++ -std=c++20 -O1 -g -Iinclude -fsanitize=thread            src/tests.cpp -o test_tsan -pthread
```

Or with CMake:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest
```

---

## Benchmark Results (2-core VM)

### Flat workload — 60k independent CPU-bound tasks

| Scheduler | 1 Thread | 2 Threads | 4 Threads | 8 Threads |
|---|---|---|---|---|
| MutexQueue | 4,638,463 | 627,025 | 609,524 | 690,702 |
| LockFreeQueue | 767,804 | 3,738,964 | 3,898,292 | 911,362 |
| WorkStealing | 801,333 | 4,225,321 | 4,554,518 | 4,408,246 |

*Numbers are tasks/sec. Higher is better.*

### DAG workload — 1920-node pipeline (32 wide × 60 deep)

| Scheduler | 1 Thread | 2 Threads | 4 Threads | 8 Threads |
|---|---|---|---|---|
| MutexQueue | 1,802,289 | 523,803 | 141,160 | 127,796 |
| LockFreeQueue | 1,546,903 | 849,398 | 793,207 | 579,146 |
| WorkStealing | 1,455,867 | 831,044 | 786,257 | 676,545 |

### What the numbers show

- **MutexQueue** peaks at 4.6M/s single-threaded (uncontended futex), then collapses 7.4× the moment a second worker appears — classic lock convoy
- **WorkStealing** degrades only ~10% from 2 to 8 threads under oversubscription; per-worker deques avoid shared hot-path contention
- **LockFreeQueue** holds through 4 threads then cliffs at 8 — shared `tail_` CAS becomes the bottleneck under preemption
- **DAG workloads** scale worse than flat across all schedulers — depth-60 critical path cannot be parallelized regardless of core count

---

## Design Notes

See [`docs/ENGINEERING.md`](docs/ENGINEERING.md) for:

- Memory ordering decisions and why `seq_cst` was avoided
- ABA analysis for the deque and ring buffer
- Why hazard pointers were not needed
- DAG layer design and correctness argument
- Full scaling analysis
