#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>

namespace tsr {

class IScheduler;  // forward declaration

// ----------------------------------------------------------------------------
// Task — a moveable unit of work.
// The callable receives a reference to the scheduler so it can spawn children.
// ----------------------------------------------------------------------------
struct Task {
    using Fn = std::function<void(IScheduler&)>;
    explicit Task(Fn fn) : fn_(std::move(fn)) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) = default;
    Task& operator=(Task&&) = default;
    void run(IScheduler& sched) { fn_(sched); }
private:
    Fn fn_;
};

using TaskPtr = std::unique_ptr<Task>;

template<typename F>
TaskPtr make_task(F&& fn) {
    return std::make_unique<Task>(std::forward<F>(fn));
}

} // namespace tsr
