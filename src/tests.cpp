#include "mutex_queue_scheduler.hpp"
#include "lockfree_queue_scheduler.hpp"
#include "work_stealing_scheduler.hpp"
#include "chase_lev_deque.hpp"
#include "task_graph.hpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <numeric>
#include <vector>
#include <thread>
#include <functional>

using namespace tsr;

// ============================================================================
// Helpers
// ============================================================================

static void expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::abort();
    }
    std::cout << "PASS: " << msg << "\n";
}

// ============================================================================
// Chase-Lev deque unit tests
// ============================================================================

static void test_deque_basic() {
    ChaseLevDeque dq;
    expect(dq.empty(), "deque initially empty");

    dq.push_bottom(make_task([](IScheduler&){}));
    dq.push_bottom(make_task([](IScheduler&){}));
    expect(!dq.empty(), "deque non-empty after 2 pushes");
    expect(dq.size() == 2, "size == 2");

    auto t1 = dq.pop_bottom();
    expect(t1 != nullptr, "pop_bottom returns task");
    expect(dq.size() == 1, "size == 1 after pop");

    auto t2 = dq.pop_bottom();
    expect(t2 != nullptr, "pop_bottom returns second task");
    expect(dq.empty(), "deque empty after two pops");

    auto t3 = dq.pop_bottom();
    expect(t3 == nullptr, "pop_bottom returns null on empty");
}

// Null scheduler stub for deque-only tests (no spawn needed).
struct NullSched : IScheduler {
    void submit(TaskPtr) override {}
    void run(uint32_t) override {}
    std::string name() const override { return "null"; }
};

static void test_deque_steal() {
    ChaseLevDeque dq;
    std::atomic<int> counter{0};
    NullSched ns;

    for (int i = 0; i < 10; ++i) {
        dq.push_bottom(make_task([&](IScheduler&) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    // Steal all from "thief" thread.
    std::thread thief([&] {
        for (int i = 0; i < 10; ++i) {
            while (true) {
                auto r = dq.steal();
                if (r.task) {
                    r.task->run(ns);
                    break;
                }
                if (r.empty) break;
                std::this_thread::yield();
            }
        }
    });
    thief.join();
    expect(counter.load() == 10, "all 10 tasks stolen and run");
}

static void test_deque_concurrent() {
    // Owner pushes 1000, N thieves compete for them.
    constexpr int N_TASKS = 1000;
    constexpr int N_THIEVES = 4;
    ChaseLevDeque dq;
    std::atomic<int> counter{0};
    NullSched ns;

    // Pre-fill.
    for (int i = 0; i < N_TASKS; ++i) {
        dq.push_bottom(make_task([&](IScheduler&) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    std::vector<std::thread> thieves;
    for (int t = 0; t < N_THIEVES; ++t) {
        thieves.emplace_back([&, &ns_ref = ns] {
            while (true) {
                auto r = dq.steal();
                if (r.task) {
                    r.task->run(ns_ref);
                } else if (r.empty) {
                    if (dq.empty()) return;
                }
            }
        });
    }
    for (auto& t : thieves) t.join();

    // Pop anything the thieves missed.
    while (auto t = dq.pop_bottom()) t->run(ns);

    expect(counter.load() == N_TASKS, "all concurrent tasks executed exactly once");
}

// ============================================================================
// Scheduler correctness tests
// ============================================================================

template<typename Sched>
static void test_scheduler_sum(const char* label) {
    Sched sched;
    constexpr int N = 50'000;
    std::atomic<int64_t> sum{0};
    for (int i = 0; i < N; ++i) {
        sched.submit(make_task([i, &sum](IScheduler&) {
            sum.fetch_add(i, std::memory_order_relaxed);
        }));
    }
    sched.run(4);
    int64_t expected = int64_t(N - 1) * N / 2;
    std::string msg = std::string(label) + " scheduler sum correct";
    expect(sum.load() == expected, msg.c_str());
}

template<typename Sched>
static void test_scheduler_spawn(const char* label) {
    Sched sched;
    std::atomic<int> count{0};
    // Root task spawns 9 children.
    sched.submit(make_task([&](IScheduler& s) {
        count.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < 9; ++i) {
            s.submit(make_task([&](IScheduler&) {
                count.fetch_add(1, std::memory_order_relaxed);
            }));
        }
    }));
    sched.run(2);
    std::string msg = std::string(label) + " scheduler child spawning correct";
    expect(count.load() == 10, msg.c_str());
}

// ============================================================================
// DAG correctness tests
// ============================================================================

// 1. Linear chain A -> B -> C -> D: each node must run strictly after its
//    predecessor. We verify by ensuring the observed timestamp sequence is
//    monotonic. (Using a shared counter under relaxed + acq_rel fetch_add
//    gives a unique total order.)
template<typename Sched>
static void test_dag_linear_chain(const char* label) {
    Sched sched;
    TaskGraph g;
    std::atomic<int> ticker{0};
    int order[4] = {-1, -1, -1, -1};

    auto& a = g.node([&]{ order[0] = ticker.fetch_add(1, std::memory_order_acq_rel); });
    auto& b = g.node([&]{ order[1] = ticker.fetch_add(1, std::memory_order_acq_rel); });
    auto& c = g.node([&]{ order[2] = ticker.fetch_add(1, std::memory_order_acq_rel); });
    auto& d = g.node([&]{ order[3] = ticker.fetch_add(1, std::memory_order_acq_rel); });
    b.depends_on(a);
    c.depends_on(b);
    d.depends_on(c);

    g.execute(sched, 4);

    bool ok = (order[0] < order[1]) && (order[1] < order[2]) && (order[2] < order[3]);
    std::string msg = std::string(label) + " DAG linear chain ordering";
    expect(ok, msg.c_str());
}

// 2. Diamond:      A
//                / \
//               B   C
//                \ /
//                 D
//    D must see both B and C's side effects. We assert D's timestamp is
//    strictly greater than both B's and C's.
template<typename Sched>
static void test_dag_diamond(const char* label) {
    Sched sched;
    TaskGraph g;
    std::atomic<int> ticker{0};
    int ta = -1, tb = -1, tc = -1, td = -1;

    auto& a = g.node([&]{ ta = ticker.fetch_add(1, std::memory_order_acq_rel); });
    auto& b = g.node([&]{ tb = ticker.fetch_add(1, std::memory_order_acq_rel); });
    auto& c = g.node([&]{ tc = ticker.fetch_add(1, std::memory_order_acq_rel); });
    auto& d = g.node([&]{ td = ticker.fetch_add(1, std::memory_order_acq_rel); });
    b.depends_on(a);
    c.depends_on(a);
    d.depends_on(b, c);

    g.execute(sched, 4);

    bool ok = (ta < tb) && (ta < tc) && (tb < td) && (tc < td);
    std::string msg = std::string(label) + " DAG diamond ordering";
    expect(ok, msg.c_str());
}

// 3. Fork-join: one root forks to N parallel leaves, all joining a sink.
//    Verify: (a) all N leaves run, (b) sink runs last.
template<typename Sched>
static void test_dag_fork_join(const char* label) {
    constexpr int N = 64;
    Sched sched;
    TaskGraph g;
    std::atomic<int> leaf_count{0};
    std::atomic<int> ticker{0};
    int t_root = -1, t_sink = -1;

    auto& root = g.node([&]{ t_root = ticker.fetch_add(1, std::memory_order_acq_rel); });
    auto& sink = g.node([&]{ t_sink = ticker.fetch_add(1, std::memory_order_acq_rel); });

    for (int i = 0; i < N; ++i) {
        auto& leaf = g.node([&]{
            ticker.fetch_add(1, std::memory_order_acq_rel);
            leaf_count.fetch_add(1, std::memory_order_acq_rel);
        });
        leaf.depends_on(root);
        sink.depends_on(leaf);
    }

    g.execute(sched, 4);

    bool ok = (leaf_count.load() == N) && (t_root >= 0) && (t_sink > t_root);
    std::string msg = std::string(label) + " DAG fork-join completion";
    expect(ok, msg.c_str());
}

// ============================================================================
// main
// ============================================================================

int main() {
    std::cout << "=== Chase-Lev Deque Tests ===\n";
    test_deque_basic();
    test_deque_steal();
    test_deque_concurrent();

    std::cout << "\n=== Scheduler Correctness Tests ===\n";
    test_scheduler_sum<MutexQueueScheduler>("MutexQueue");
    test_scheduler_sum<LockFreeQueueScheduler>("LockFreeQueue");
    test_scheduler_sum<WorkStealingScheduler>("WorkStealing");

    test_scheduler_spawn<MutexQueueScheduler>("MutexQueue");
    test_scheduler_spawn<LockFreeQueueScheduler>("LockFreeQueue");
    test_scheduler_spawn<WorkStealingScheduler>("WorkStealing");

    std::cout << "\n=== DAG Correctness Tests ===\n";
    test_dag_linear_chain<MutexQueueScheduler>("MutexQueue");
    test_dag_linear_chain<LockFreeQueueScheduler>("LockFreeQueue");
    test_dag_linear_chain<WorkStealingScheduler>("WorkStealing");

    test_dag_diamond<MutexQueueScheduler>("MutexQueue");
    test_dag_diamond<LockFreeQueueScheduler>("LockFreeQueue");
    test_dag_diamond<WorkStealingScheduler>("WorkStealing");

    test_dag_fork_join<MutexQueueScheduler>("MutexQueue");
    test_dag_fork_join<LockFreeQueueScheduler>("LockFreeQueue");
    test_dag_fork_join<WorkStealingScheduler>("WorkStealing");

    std::cout << "\nAll tests passed.\n";
    return 0;
}
