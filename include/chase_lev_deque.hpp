#pragma once

#include "task.hpp"
#include <atomic>
#include <vector>
#include <memory>
#include <cassert>
#include <cstdint>
#include <stdexcept>

namespace tsr {

// ----------------------------------------------------------------------------
// CircularArray
//
// The backing store for the Chase-Lev deque.  It is a power-of-two sized
// ring buffer of raw pointers to Task.  The deque owns the Tasks, but the
// array itself is managed by the deque's growth protocol.
//
// GROWTH / ABA ANALYSIS
// =====================
// The Chase-Lev deque resizes by allocating a new array and copying live
// entries before installing the new pointer with a release store.  The old
// array is kept alive in a garbage list (`old_arrays_`) that is held by the
// owning deque.  We never reclaim old arrays during a run because:
//
//   - Thieves can be reading from an old array at the exact moment we resize.
//   - Without epoch-GC or hazard pointers the only safe option is to leak
//     the old array until we know no thief is inside it.
//
// In practice deque capacity never shrinks so growth is rare and bounded by
// the total number of tasks ever live concurrently.  For a benchmark harness
// this is perfectly acceptable.  A production runtime would add a quiescent
// epoch flush.
//
// ABA IS NOT A CONCERN HERE because:
//   - `top_` and `bot_` are strictly monotonically increasing indices, never
//     reused.  A CAS on `top_` cannot mis-fire because the index it read will
//     never wrap back to the same value within a reasonable program lifetime
//     (they are 64-bit).
//   - The array pointer CAS in take() compares a *freshly loaded* top with
//     the value we read earlier; since top only ever increases, a mismatch
//     means someone else already stole from that slot, which is correct.
// ----------------------------------------------------------------------------
class CircularArray {
public:
    explicit CircularArray(int64_t log2_cap)
        : log2_cap_(log2_cap)
        , mask_((int64_t(1) << log2_cap) - 1)
        , buf_(static_cast<std::size_t>(int64_t(1) << log2_cap))
    {}

    int64_t capacity() const noexcept { return int64_t(1) << log2_cap_; }

    // Raw pointer store/load — the deque enforces correct ownership.
    void put(int64_t idx, Task* p) noexcept {
        buf_[static_cast<std::size_t>(idx & mask_)].store(
            p, std::memory_order_relaxed);
    }

    Task* get(int64_t idx) const noexcept {
        return buf_[static_cast<std::size_t>(idx & mask_)].load(
            std::memory_order_relaxed);
    }

    // Grow: returns a new array twice as large, populated with live entries
    // in the range [bot, top).  Caller is responsible for keeping *this alive
    // until all possible readers have finished.
    CircularArray* grow(int64_t bot, int64_t top) const {
        auto* next = new CircularArray(log2_cap_ + 1);
        for (int64_t i = top; i < bot; ++i)
            next->put(i, get(i));
        return next;
    }

private:
    int64_t log2_cap_;
    int64_t mask_;
    std::vector<std::atomic<Task*>> buf_;
};

// ----------------------------------------------------------------------------
// ChaseLevDeque
//
// Single-owner, multi-thief work-stealing deque implementing the algorithm
// from "Dynamic circular work-stealing deque" (Chase & Lev, 2005) with the
// memory-ordering corrections from Lê et al., 2013 ("Correct and Efficient
// Work-Stealing for Weak Memory Models").
//
// Owner operations (push_bottom / pop_bottom):
//   Called only by the owning worker thread.  No contention with other owner
//   calls; only the steal path races with pop_bottom.
//
// Thief operation (steal):
//   Called by any worker except the owner.  Multiple thieves may race.
//
// Memory ordering summary (see ENGINEERING.md for full rationale):
//
//   push_bottom
//     - relaxed store to array slot  (no other thread reads it yet)
//     - release store to bot_        (publishes the new entry to thieves)
//
//   pop_bottom (owner)
//     - relaxed decrement of bot_    (only owner touches bot_)
//     - acquire fence before reading top_ (synchronises with steal's
//       release-CAS on top_)
//     - if empty: seq_cst CAS to win the race with a concurrent steal
//       (necessary to agree on who got the last element — this is the one
//        place where seq_cst is justified; cost is amortised O(1) over N pushes)
//
//   steal (thief)
//     - acquire load of top_
//     - acquire load of bot_ (needed to see entries published by push_bottom)
//     - relaxed load of the slot (safe: the release store of bot_ in push
//       ensures visibility by the time we see bot_ > top)
//     - release CAS on top_ (publishes the increment to other thieves)
//
// Alignment:
//   top_ and bot_ are on separate cache lines (alignas(64)) to eliminate
//   false sharing between the owner writing bot_ and thieves writing top_.
// ----------------------------------------------------------------------------

// Sentinel returned by steal() when the deque is empty or a race was lost.
struct StealResult {
    TaskPtr task;   // nullptr if nothing was stolen
    bool    empty;  // true  => deque was empty
                    // false => lost a race; retry
};

class alignas(64) ChaseLevDeque {
public:
    static constexpr int64_t kInitialLog2Cap = 8; // 256 slots

    ChaseLevDeque()
        : top_(0), bot_(0)
        , array_(new CircularArray(kInitialLog2Cap))
    {}

    ~ChaseLevDeque() {
        // Drain any remaining tasks to avoid leaks.
        int64_t b = bot_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        CircularArray* a = array_.load(std::memory_order_relaxed);
        for (int64_t i = t; i < b; ++i) {
            Task* p = a->get(i);
            delete p;
        }
        delete a;
        for (auto* old : old_arrays_) delete old;
    }

    // Not copyable or movable — the deque is pinned to its owning thread.
    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;

    // ------------------------------------------------------------------ owner
    // push_bottom — O(1) amortised; owner thread only.
    void push_bottom(TaskPtr task) {
        int64_t b = bot_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);
        CircularArray* a = array_.load(std::memory_order_relaxed);

        if (b - t >= a->capacity() - 1) {
            // Resize: grow before the slot fills up.
            CircularArray* next = a->grow(b, t);
            old_arrays_.push_back(a);          // keep old array alive
            array_.store(next, std::memory_order_relaxed);
            a = next;
        }

        a->put(b, task.release());             // transfer ownership to array
        std::atomic_thread_fence(std::memory_order_release);
        bot_.store(b + 1, std::memory_order_relaxed);
    }

    // pop_bottom — O(1); owner thread only.
    TaskPtr pop_bottom() {
        int64_t b = bot_.load(std::memory_order_relaxed) - 1;
        CircularArray* a = array_.load(std::memory_order_relaxed);
        bot_.store(b, std::memory_order_relaxed);

        // Full fence before reading top so we synchronise with any concurrent
        // steal that may have just bumped top_.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        int64_t t = top_.load(std::memory_order_relaxed);

        if (t > b) {
            // The deque was already empty before our decrement; restore.
            bot_.store(b + 1, std::memory_order_relaxed);
            return nullptr;
        }

        Task* raw = a->get(b);

        if (t < b) {
            // Not the last element; no race possible with thieves.
            return TaskPtr(raw);
        }

        // Last element: race with thieves for it.
        // We use a seq_cst CAS on top_.  This is the algorithm's only
        // seq_cst operation and is necessary to establish a total order
        // between a concurrent steal and this pop.
        bool won = top_.compare_exchange_strong(
            t, t + 1,
            std::memory_order_seq_cst,
            std::memory_order_relaxed);

        bot_.store(b + 1, std::memory_order_relaxed);
        return won ? TaskPtr(raw) : nullptr;
    }

    // ----------------------------------------------------------------- thief
    // steal — may be called by any thread except the owner.
    StealResult steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bot_.load(std::memory_order_acquire);

        if (t >= b) return {nullptr, true};   // empty

        CircularArray* a = array_.load(std::memory_order_consume);
        Task* raw = a->get(t);

        // Race with other thieves (and the owner's pop_bottom for the last
        // element).  Release ordering publishes the top_ increment.
        int64_t expected = t;
        if (!top_.compare_exchange_strong(
                expected, t + 1,
                std::memory_order_seq_cst,
                std::memory_order_relaxed)) {
            return {nullptr, false};           // lost the race; retry
        }

        return {TaskPtr(raw), false};          // successfully stolen
    }

    bool empty() const noexcept {
        int64_t b = bot_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return b <= t;
    }

    int64_t size() const noexcept {
        int64_t b = bot_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return std::max(int64_t(0), b - t);
    }

private:
    // top_ and bot_ on separate cache lines to avoid false sharing.
    alignas(64) std::atomic<int64_t> top_;
    alignas(64) std::atomic<int64_t> bot_;

    // The live array; wrapped in atomic so grow() is visible to thieves.
    alignas(64) std::atomic<CircularArray*> array_;

    // Old arrays kept alive to prevent use-after-free by concurrent thieves.
    // Freed in the destructor once we know no other thread is accessing us.
    std::vector<CircularArray*> old_arrays_;
};

} // namespace tsr
