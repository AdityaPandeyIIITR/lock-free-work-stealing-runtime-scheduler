#pragma once

#include "task.hpp"
#include <cstdint>
#include <string>

namespace tsr {

// IScheduler — pure interface. Tasks use this to spawn children.
class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void submit(TaskPtr task) = 0;
    virtual void run(uint32_t num_workers) = 0;
    virtual std::string name() const = 0;
};

} // namespace tsr
