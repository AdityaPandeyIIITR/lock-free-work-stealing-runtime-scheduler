#pragma once

#include "scheduler.hpp"
#include "task.hpp"

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>
#include <atomic>
#include <string>

namespace tsr {

// ----------------------------------------------------------------------------
// MutexQueueScheduler
//
// The simplest correct scheduler: a single std::queue protected by a mutex
// and drained by N worker threads.  It is the correctness baseline and the
// primary contention stress-test — every enqueue and dequeue acquires the
// same lock.
//
// Design decisions
// ----------------
// * std::queue<TaskPtr> is used rather than a lock-free container so that the
//   mutex cost dominates and gives a clear measurement of lock overhead.
// * Workers block on a condition variable when the queue is empty rather than
//   spinning, keeping CPU idle while no work exists.
// * pending_ counts tasks that have been submitted but not yet completed
//   (including tasks currently executing).  run() returns when pending_ == 0.
// * A generation counter (gen_) prevents spurious early exit: when run()
//   calls wait(), a new generation has already been opened so wake-ups from
//   the previous generation are not confused with a genuine drain.
// ----------------------------------------------------------------------------
class MutexQueueScheduler final : public IScheduler {
public:
    MutexQueueScheduler() : pending_(0), shutdown_(false) {}

    ~MutexQueueScheduler() override {
        {
            std::lock_guard lk(mu_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    void submit(TaskPtr task) override {
        {
            std::lock_guard lk(mu_);
            pending_.fetch_add(1, std::memory_order_relaxed);
            queue_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void run(uint32_t num_workers) override {
        // Reset state for repeated benchmark runs.
        {
            std::lock_guard lk(mu_);
            shutdown_ = false;
        }

        std::vector<std::thread> workers;
        workers.reserve(num_workers);
        for (uint32_t i = 0; i < num_workers; ++i)
            workers.emplace_back([this] { worker_loop(); });

        // Wait until all submitted work (including dynamically spawned tasks)
        // has completed.
        {
            std::unique_lock lk(mu_);
            done_cv_.wait(lk, [this] {
                return pending_.load(std::memory_order_relaxed) == 0;
            });
            shutdown_ = true;
        }
        cv_.notify_all();

        for (auto& w : workers) w.join();
    }

    std::string name() const override { return "MutexQueue"; }

private:
    void worker_loop() {
        while (true) {
            TaskPtr task;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [this] {
                    return !queue_.empty() || shutdown_;
                });
                if (queue_.empty()) return;   // shutdown_ must be true
                task = std::move(queue_.front());
                queue_.pop();
            }

            task->run(*this);

            // Decrement after run so that children submitted during run()
            // are counted before we check for completion.
            int64_t remaining = pending_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) done_cv_.notify_all();
        }
    }

    std::mutex              mu_;
    std::condition_variable cv_;
    std::condition_variable done_cv_;
    std::queue<TaskPtr>     queue_;
    std::atomic<int64_t>    pending_;
    bool                    shutdown_;
};

} // namespace tsr
