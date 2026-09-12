#include "dlssd_submission_gate.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
using DlssdSubmissionGate::Registry;

bool TestRecursion()
{
    Registry registry;
    int queue = 0;
    const auto gate = registry.Get(&queue);
    if (!gate) return false;
    std::lock_guard<std::recursive_mutex> outer(*gate);
    std::lock_guard<std::recursive_mutex> inner(*gate);
    return registry.Count() == 1;
}

bool TestSameQueueSerializationAndOtherQueueProgress()
{
    Registry registry;
    int queue = 0;
    int otherQueue = 0;
    const auto gate = registry.Get(&queue);
    const auto otherGate = registry.Get(&otherQueue);
    if (!gate || !otherGate || gate == otherGate) return false;

    std::mutex stateMutex;
    std::condition_variable stateChanged;
    bool firstHolding = false;
    bool secondAttempted = false;
    bool secondAcquired = false;
    bool allowFirstFinish = false;
    bool otherProgress = false;
    std::vector<int> order;

    std::thread first([&] {
        std::unique_lock<std::recursive_mutex> lock(*gate);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex);
            order.push_back(1);
            firstHolding = true;
        }
        stateChanged.notify_all();
        std::unique_lock<std::mutex> stateLock(stateMutex);
        stateChanged.wait(stateLock, [&] { return allowFirstFinish; });
        order.push_back(2);
    });

    std::thread second([&] {
        std::unique_lock<std::mutex> stateLock(stateMutex);
        stateChanged.wait(stateLock, [&] { return firstHolding; });
        secondAttempted = true;
        stateLock.unlock();
        std::lock_guard<std::recursive_mutex> lock(*gate);
        std::lock_guard<std::mutex> acquiredLock(stateMutex);
        secondAcquired = true;
        order.push_back(3);
        stateChanged.notify_all();
    });

    std::thread other([&] {
        std::unique_lock<std::mutex> stateLock(stateMutex);
        stateChanged.wait(stateLock, [&] { return firstHolding; });
        stateLock.unlock();
        std::lock_guard<std::recursive_mutex> lock(*otherGate);
        std::lock_guard<std::mutex> progressLock(stateMutex);
        otherProgress = true;
        stateChanged.notify_all();
    });

    {
        std::unique_lock<std::mutex> stateLock(stateMutex);
        stateChanged.wait(stateLock, [&] { return secondAttempted && otherProgress; });
        if (secondAcquired || order != std::vector<int>{1})
        {
            allowFirstFinish = true;
            stateLock.unlock();
            stateChanged.notify_all();
            first.join();
            second.join();
            other.join();
            return false;
        }
        allowFirstFinish = true;
    }
    stateChanged.notify_all();
    first.join();
    second.join();
    other.join();
    return secondAcquired && otherProgress && order == std::vector<int>({1, 2, 3});
}

bool TestNullCapAndPointerReuse()
{
    Registry registry;
    if (registry.Get(nullptr) || registry.Count() != 0) return false;
    std::array<std::uint8_t, Registry::kMaxQueues> queues{};
    Registry::Gate firstGate;
    for (std::size_t index = 0; index != queues.size(); ++index)
    {
        const auto gate = registry.Get(&queues[index]);
        if (!gate) return false;
        if (index == 0) firstGate = gate;
    }
    if (registry.Count() != Registry::kMaxQueues) return false;
    std::uint8_t beyond = 0;
    if (registry.Get(&beyond) || registry.Count() != Registry::kMaxQueues) return false;
    return registry.Get(&queues[0]) == firstGate && registry.Count() == Registry::kMaxQueues;
}
}

int main()
{
    const bool recursion = TestRecursion();
    const bool serialization = TestSameQueueSerializationAndOtherQueueProgress();
    const bool cap = TestNullCapAndPointerReuse();
    const bool passed = recursion && serialization && cap;
    std::printf("%s recursive gate, same-queue serialization, other-queue progress, null/cap and pointer reuse\n",
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
