#pragma once

#include "task.hpp"
#include "scheduler.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tsr {

// ----------------------------------------------------------------------------
// DagNode
//
// A node in the task graph. Holds:
//   - the user's callable (wrapped in a Task)
//   - an atomic counter of remaining predecessor dependencies
//   - a list of successors (populated at build time, read-only at exec time)
//
// Lifecycle:
//   1. Build phase: TaskGraph::node(...) creates nodes; depends_on(...) wires
//      edges and increments deps_remaining on the successor. No concurrency
//      during this phase (single-threaded construction).
//   2. Execute phase: TaskGraph::execute(scheduler) seeds all zero-dep nodes
//      onto the scheduler. Each worker runs a node, then decrements every
//      successor's deps_remaining. Any successor whose counter reaches 0 is
//      submitted to the scheduler. deps_remaining is write-once in the build
//      phase, then monotonically decreasing in the exec phase — no ABA.
//
// Memory ordering:
//   - deps_remaining.fetch_sub uses acq_rel. The acquire half ensures the
//     decrementing thread sees all writes the predecessor made to shared
//     memory before it finished (release-acquire chain). The release half
//     ensures that if our decrement hits zero and we submit the successor,
//     the submitted task sees the full causal history of every predecessor
//     that contributed to this node becoming ready.
//   - remaining_nodes_ on the parent TaskGraph uses acq_rel for the same
//     reason: when it hits 0 the driver thread returns from execute(), and
//     it must observe all writes made by every completed task.
// ----------------------------------------------------------------------------

class TaskGraph;

class DagNode {
    friend class TaskGraph;
public:
    using Fn = std::function<void()>;

private:
    // Only TaskGraph constructs nodes.
    DagNode(Fn fn, TaskGraph* owner)
        : fn_(std::move(fn)), owner_(owner),
          deps_remaining_(0) {}

public:
    DagNode(const DagNode&) = delete;
    DagNode& operator=(const DagNode&) = delete;

    // Add a dependency: `this` will only run after `pred` completes.
    // Must be called during build phase only (before TaskGraph::execute()).
    DagNode& depends_on(DagNode& pred) {
        pred.successors_.push_back(this);
        // Relaxed: build phase is single-threaded; this value becomes the
        // initial counter that workers will later decrement with acq_rel.
        deps_remaining_.fetch_add(1, std::memory_order_relaxed);
        return *this;
    }

    // Variadic convenience: n.depends_on(a, b, c);
    template<typename... Preds>
    DagNode& depends_on(DagNode& p1, DagNode& p2, Preds&... rest) {
        depends_on(p1);
        depends_on(p2, rest...);
        return *this;
    }

private:
    Fn                    fn_;
    TaskGraph*            owner_;
    std::atomic<int32_t>  deps_remaining_;
    std::vector<DagNode*> successors_;  // read-only during execution
};

// ----------------------------------------------------------------------------
// TaskGraph
//
// Owns a set of DagNodes. Executed via execute(scheduler) which:
//   1. Captures the total node count as the completion counter.
//   2. Walks the node list once, submits every node with 0 deps as a ready
//      root task.
//   3. Each ready task, on completion, decrements successors and submits any
//      that become zero.
//   4. Returns (via scheduler.run) once remaining_nodes_ hits zero.
//
// Not copyable or movable — nodes hold back-pointers to the owning graph.
// A TaskGraph is single-use: once execute() has been called, the internal
// atomic counters are in a post-execution state. Constructing a new graph is
// cheap; there's no reason to reuse.
// ----------------------------------------------------------------------------

class TaskGraph {
public:
    TaskGraph() = default;
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // Create a node. Returned reference is stable — nodes are stored via
    // unique_ptr so vector reallocation doesn't invalidate it.
    template<typename F>
    DagNode& node(F&& fn) {
        assert(!executing_ && "cannot add nodes after execute() begins");
        nodes_.emplace_back(new DagNode(std::forward<F>(fn), this));
        return *nodes_.back();
    }

    // Run the graph to completion on `sched` using `num_workers` threads.
    // Blocks until every node has finished.
    void execute(IScheduler& sched, uint32_t num_workers) {
        executing_ = true;

        // Seed the completion counter with the total node count.
        remaining_nodes_.store(static_cast<int64_t>(nodes_.size()),
                               std::memory_order_relaxed);

        // Submit every zero-dep node as a root.
        for (auto& np : nodes_) {
            if (np->deps_remaining_.load(std::memory_order_relaxed) == 0) {
                submit_node(sched, np.get());
            }
        }

        // Drives all workers to completion. run() returns when the scheduler's
        // own pending counter hits 0, which happens after every submitted task
        // (root + reactively-submitted successors) finishes.
        sched.run(num_workers);

        // Sanity check: every node must have fired exactly once.
        assert(remaining_nodes_.load(std::memory_order_relaxed) == 0);
    }

    std::size_t size() const noexcept { return nodes_.size(); }

private:
    void submit_node(IScheduler& sched, DagNode* n) {
        sched.submit(make_task([this, n](IScheduler& s) {
            // 1. Run the user's code.
            n->fn_();

            // 2. For each successor, decrement its refcount.
            //    acq_rel: acquire lets the successor's eventual reader see
            //    everything we did; release publishes that we finished.
            for (DagNode* succ : n->successors_) {
                int32_t prev = succ->deps_remaining_.fetch_sub(
                    1, std::memory_order_acq_rel);
                if (prev == 1) {
                    // We were the last predecessor; the successor is ready.
                    submit_node(s, succ);
                }
            }

            // 3. Mark this node done globally.
            remaining_nodes_.fetch_sub(1, std::memory_order_acq_rel);
        }));
    }

    std::vector<std::unique_ptr<DagNode>> nodes_;
    std::atomic<int64_t>                  remaining_nodes_{0};
    bool                                  executing_ = false;
};

} // namespace tsr
