#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace DlssdSubmissionGate
{
// The registry intentionally keeps gates for the lifetime of the process. A
// queued call may still hold a shared_ptr after a queue is no longer active;
// retiring and recreating its mutex would make serialization unsafe.
class Registry
{
public:
    static constexpr std::size_t kMaxQueues = 128;
    using Gate = std::shared_ptr<std::recursive_mutex>;

    Gate Get(void* queue)
    {
        if (!queue) return nullptr;
        std::lock_guard<std::mutex> registryLock(mutex_);
        const auto existing = gates_.find(queue);
        if (existing != gates_.end()) return existing->second;
        if (gates_.size() >= kMaxQueues) return nullptr;
        const Gate gate = std::make_shared<std::recursive_mutex>();
        gates_.emplace(queue, gate);
        return gate;
    }

    std::size_t Count() const
    {
        std::lock_guard<std::mutex> registryLock(mutex_);
        return gates_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<void*, Gate> gates_;
};
} // namespace DlssdSubmissionGate
