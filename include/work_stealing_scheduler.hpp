#pragma once

#include "scheduler.hpp"
#include "task.hpp"
#include "chase_lev_deque.hpp"

#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <random>
#include <string>
#include <cstdint>
#include <cassert>

namespace tsr {

// ----------------------------------------------------------------------------
// WorkStealingScheduler
//
// Each worker thread owns a Chase-Lev deque.  The owner pushes and pops from
// the bottom (LIFO for the local deque — locality-friendly for DFS task
// graphs).  Idle workers steal from the top of a randomly chosen victim's
// deque (FIFO steal — fair, avoids starvation of old tasks).
//
// Architecture
// ============
//
//   ┌─────────────────────────────────────────────────────┐
//   │  WorkStealingScheduler                              │
//   │  ┌──────────┐  ┌──────────┐  ┌──────────┐          │
//   │  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  ...     │
//   │  │ deque    │  │ deque    │  │ deque    │          │
//   │  │ [bottom] │  │ [bottom] │  │ [bottom] │          │
//   │  └────┬─────┘  └────┬─────┘  └────┬─────┘          │
//   │       │  steal ◄────┘  steal ◄────┘                │
//   │       └────────────────────────────────────────►   │
//   └─────────────────────────────────────────────────────┘
//
// Submission from external threads
// ---------------------------------
// Tasks submitted before run() are buffered in an injection queue (a simple
// mutex-protected std::vector used only at startup — zero contention during
// execution).  Tasks submitted during execution (spawned children) are pushed
// directly to the calling worker's deque via thread-local state, avoiding
// any shared structure at all.
//
// Thread-local context
// --------------------
// `tl_worker_id_` stores the current worker's index (or -1 for non-worker
// threads).  This lets submit() push to the local deque when called from
// within a task, and fall back to the injection queue otherwise.
//
// Termination detection
// ---------------------
// A two-phase protocol:
//   pending_   counts submitted-but-not-completed tasks (including spawned
//              children).  When it reaches 0 the main thread wakes up.
//   idle_      counts workers in the steal-retry loop.  Used only for
//              waking up workers after external submission; not used for
//              termination.
//
// Memory ordering
// ---------------
// See chase_lev_deque.hpp for deque-internal ordering.  At the scheduler
// level:
//   - pending_ uses acq_rel on decrement so the store of 0 is visible
//     before the waiting thread's load returns 0.
//   - deques_[] is set up before any thread is created, so no ordering
//     is needed on the pointer reads inside workers.
// ----------------------------------------------------------------------------

// Per-worker context pinned to one cache line boundary.
struct alignas(64) WorkerCtx {
    ChaseLevDeque deque;
    uint32_t      id{0};
    // Pad to avoid false sharing between WorkerCtx objects.
    // ChaseLevDeque itself is already 64-byte aligned internally.
};

class WorkStealingScheduler final : public IScheduler {
public:
    explicit WorkStealingScheduler() : pending_(0), shutdown_(false) {}

    ~WorkStealingScheduler() override = default;

    // submit() is safe from any thread.
    void submit(TaskPtr task) override {
        int wid = tl_worker_id_;
        if (wid >= 0 && wid < static_cast<int>(workers_.size())) {
            // Called from inside a task: push to owner's deque.
            pending_.fetch_add(1, std::memory_order_relaxed);
            workers_[static_cast<std::size_t>(wid)]->deque.push_bottom(std::move(task));
        } else {
            // External submission: push to the injection queue.
            std::lock_guard lk(inject_mu_);
            pending_.fetch_add(1, std::memory_order_relaxed);
            inject_queue_.push_back(std::move(task));
        }
    }

    void run(uint32_t num_workers) override {
        // Build worker contexts.
        workers_.clear();
        workers_.resize(num_workers);
        for (uint32_t i = 0; i < num_workers; ++i) {
            workers_[i] = std::make_unique<WorkerCtx>();
            workers_[i]->id = i;
        }

        shutdown_.store(false, std::memory_order_relaxed);

        // Drain injection queue into worker 0's deque to start execution.
        {
            std::lock_guard lk(inject_mu_);
            for (auto& t : inject_queue_)
                workers_[0]->deque.push_bottom(std::move(t));
            inject_queue_.clear();
        }

        // Launch worker threads.
        std::vector<std::thread> threads;
        threads.reserve(num_workers);
        for (uint32_t i = 0; i < num_workers; ++i)
            threads.emplace_back([this, i] { worker_loop(i); });

        // Spin-wait for all tasks (including dynamically spawned) to finish.
        while (pending_.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();

        shutdown_.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();

        // Clean up for potential re-use.
        workers_.clear();
    }

    std::string name() const override { return "WorkStealing"; }

private:
    void worker_loop(uint32_t id) {
        tl_worker_id_ = static_cast<int>(id);
        WorkerCtx& self = *workers_[id];

        // Exponential backoff state for the steal loop.
        uint32_t backoff = 1;
        std::mt19937 rng(id * 6364136223846793005ULL + 1442695040888963407ULL);

        while (true) {
            // 1. Try local pop.
            if (TaskPtr task = self.deque.pop_bottom()) {
                task->run(*this);
                if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    return;  // We completed the last task.
                }
                backoff = 1;
                continue;
            }

            // 2. Check termination.
            if (pending_.load(std::memory_order_acquire) == 0 ||
                shutdown_.load(std::memory_order_acquire)) {
                return;
            }

            // 3. Try to steal from a random victim.
            bool found = false;
            if (workers_.size() > 1) {
                uint32_t n = static_cast<uint32_t>(workers_.size());
                // Pick a random starting victim (not ourselves).
                uint32_t start = rng() % (n - 1);
                if (start >= id) ++start;

                for (uint32_t attempt = 0; attempt < n - 1; ++attempt) {
                    uint32_t victim = (start + attempt) % n;
                    if (victim == id) { victim = (victim + 1) % n; }

                    auto result = workers_[victim]->deque.steal();
                    if (result.task) {
                        result.task->run(*this);
                        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                            return;
                        }
                        backoff = 1;
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                // Nothing to steal; back off to reduce bus traffic.
                for (uint32_t i = 0; i < backoff; ++i)
                    std::this_thread::yield();
                backoff = std::min(backoff * 2, 128u);
            }
        }
    }

    // Worker contexts.  Stored as unique_ptr so the deque address is stable.
    std::vector<std::unique_ptr<WorkerCtx>> workers_;

    // Injection queue for pre-run() submissions.
    std::mutex            inject_mu_;
    std::vector<TaskPtr>  inject_queue_;

    // Completion counter.
    alignas(64) std::atomic<int64_t> pending_;
    alignas(64) std::atomic<bool>    shutdown_;

    // Thread-local worker ID: -1 for external threads.
    static thread_local int tl_worker_id_;
};

// Definition of the thread-local (one per TU that includes this header).
inline thread_local int WorkStealingScheduler::tl_worker_id_ = -1;

} // namespace tsr
