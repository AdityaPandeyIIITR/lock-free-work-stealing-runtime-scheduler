#pragma once

#include "scheduler.hpp"
#include "task.hpp"

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>

namespace tsr {

// ----------------------------------------------------------------------------
// LockFreeQueueScheduler
//
// Implements a lock-free MPMC queue using the Dmitry Vyukov bounded ring-
// buffer algorithm, adapted to carry std::unique_ptr<Task> payloads safely.
//
// WHY NOT MICHAEL-SCOTT QUEUE
// ============================
// The classic M&S linked-list queue requires safe memory reclamation
// (hazard pointers or epoch GC) when unique_ptr payloads are used in a true
// multi-consumer setting.  Without reclamation, a losing dequeuer may access
// a node that a winning dequeuer has already freed.  Since the spec explicitly
// excludes hazard pointers and epoch GC, we instead use a ring buffer:
//
//   - Slots are pre-allocated; their addresses are fixed and never freed.
//   - Producers and consumers synchronise through monotonically increasing
//     64-bit sequence numbers stored per-slot.
//   - No external pointer to a slot escapes beyond a single critical section;
//     there is nothing to reclaim.
//
// ABA ANALYSIS
// ============
// ABA cannot occur in this design because:
//   a) No CAS operates on heap-allocated pointers that could be freed and
//      reallocated to the same address.
//   b) The only CAS targets are tail_ (a 64-bit monotonic counter) and the
//      slot sequence (also a 64-bit monotonic counter).  Both only increase,
//      so a thread stalled between a load and a CAS cannot observe the "old"
//      value again.
//
// MEMORY ORDERING
// ===============
// Producer enqueue:
//   1. head_ fetch_add(acq_rel)  — atomically reserves a slot index.
//   2. Spin-wait on slot.sequence(acquire) until it equals pos (our generation).
//   3. slot.payload store(relaxed) — private until step 4.
//   4. slot.sequence store(release, pos+1) — publishes the filled slot.
//
// Consumer dequeue:
//   1. tail_ load / CAS(acq_rel) — atomically claims a slot index.
//   2. slot.sequence load(acquire) — observes the producer's release.
//   3. slot.payload exchange(nullptr, acq_rel) — atomically takes ownership.
//   4. slot.sequence store(release, pos+kRingSize) — returns slot to producer.
//
// Seq_cst is NOT used anywhere — all orderings are acquire/release pairs that
// form correct happens-before edges without the full bus-lock cost of seq_cst.
// ----------------------------------------------------------------------------

class LockFreeQueueScheduler final : public IScheduler {
    // Ring size: 2^17 = 131072 slots.  Benchmarks use at most 200k tasks but
    // they are consumed concurrently so the high-water mark is much lower.
    static constexpr uint64_t kLog2Ring = 17;
    static constexpr uint64_t kRingSize = uint64_t(1) << kLog2Ring;
    static constexpr uint64_t kMask     = kRingSize - 1;

    // Each slot occupies exactly one cache line (64 bytes) to eliminate
    // false sharing between adjacent producers/consumers.
    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence;
        std::atomic<Task*>    payload;

        Slot() : sequence(0), payload(nullptr) {}
    };
    static_assert(sizeof(Slot) <= 64, "Slot must fit in one cache line");

public:
    LockFreeQueueScheduler()
        : pending_(0), shutdown_(false), head_(0), tail_(0)
    {
        ring_ = new Slot[kRingSize];
        // Initialise sequence[i] = i so producer at position i sees diff == 0.
        for (uint64_t i = 0; i < kRingSize; ++i)
            ring_[i].sequence.store(i, std::memory_order_relaxed);
    }

    ~LockFreeQueueScheduler() override {
        // Drain residual tasks (should be none after run() returns).
        while (TaskPtr t = dequeue()) { /* unique_ptr destructs */ }
        delete[] ring_;
    }

    void submit(TaskPtr task) override {
        pending_.fetch_add(1, std::memory_order_relaxed);
        enqueue(std::move(task));
    }

    void run(uint32_t num_workers) override {
        shutdown_.store(false, std::memory_order_release);

        std::vector<std::thread> workers;
        workers.reserve(num_workers);
        for (uint32_t i = 0; i < num_workers; ++i)
            workers.emplace_back([this] { worker_loop(); });

        while (pending_.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();

        shutdown_.store(true, std::memory_order_release);
        for (auto& w : workers) w.join();
    }

    std::string name() const override { return "LockFreeQueue"; }

private:
    void enqueue(TaskPtr task) {
        // Reserve a producer slot.
        uint64_t pos = head_.fetch_add(1, std::memory_order_acq_rel);
        Slot& slot   = ring_[pos & kMask];

        // Wait until the consumer has returned this slot (sequence == pos).
        while (slot.sequence.load(std::memory_order_acquire) != pos)
            std::this_thread::yield();

        // Store payload (relaxed; sequence release below is the fence).
        slot.payload.store(task.release(), std::memory_order_relaxed);

        // Publish: sequence = pos + 1 signals consumers.
        slot.sequence.store(pos + 1, std::memory_order_release);
    }

    TaskPtr dequeue() {
        uint64_t pos = tail_.load(std::memory_order_relaxed);
        while (true) {
            Slot& slot   = ring_[pos & kMask];
            uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            int64_t  diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);

            if (diff == 0) {
                // Slot is filled and ready.  Race to claim it.
                if (tail_.compare_exchange_weak(pos, pos + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    // Won the race: take ownership of the payload.
                    Task* raw = slot.payload.exchange(nullptr, std::memory_order_acq_rel);
                    // Return slot to the producer for its next generation.
                    slot.sequence.store(pos + kRingSize, std::memory_order_release);
                    return TaskPtr(raw);
                }
                // Lost the CAS; pos was updated by compare_exchange_weak.
            } else if (diff < 0) {
                // Queue appears empty.
                return nullptr;
            } else {
                // Producer has reserved this slot but hasn't filled it yet.
                // Reload tail and retry.
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    void worker_loop() {
        while (true) {
            if (TaskPtr task = dequeue()) {
                task->run(*this);
                pending_.fetch_sub(1, std::memory_order_acq_rel);
            } else {
                if (shutdown_.load(std::memory_order_acquire)) return;
                std::this_thread::yield();
            }
        }
    }

    alignas(64) std::atomic<int64_t>  pending_;
    alignas(64) std::atomic<bool>     shutdown_;
    alignas(64) std::atomic<uint64_t> head_;  // producer reservation cursor
    alignas(64) std::atomic<uint64_t> tail_;  // consumer claim cursor
    Slot* ring_;  // heap-allocated ring; avoids huge stack frame
};

} // namespace tsr
