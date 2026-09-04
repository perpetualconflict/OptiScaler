#pragma once

#include <cstdint>

// Stable C ABI shared by the native HIP companion and diagnostic callers.
// A workload callback must only enqueue work on the supplied HIP stream and
// return promptly; synchronization remains owned by the rendezvous helper.
namespace DlssdQueueRendezvousAbi
{
constexpr std::uint32_t Version = 1;

constexpr std::uint32_t HipSucceeded = 1;
constexpr std::uint32_t HipReadyTimedOut = 2;
constexpr std::uint32_t HipWorkloadFailed = 3;
constexpr std::uint32_t WorkloadEnqueueFailed = 0xE0020006u;
constexpr std::uint32_t TokenFailed = 0xE0020007u;

// Windows x64 C-linkage export; this C++ header supplies loader function types.
// Calls for all tokens must be externally serialized (as in the submission hook).
// Initialize clears the entire shared protocol buffer: it must be idle and
// contain no workload data. Use unique nonzero sequences until both APIs drain;
// reinitialize before sequence wrap/reuse. Slots belong to the caller's ring.
// The callback runs synchronously on the calling CPU thread, exactly once after
// the ready-wait is enqueued, NOT after readiness is observed by the GPU.
// The stream is the helper's legacy default HIP stream (nullptr is valid).
// Use the same HIP runtime/device and supplied stream for every enqueue. Never
// synchronize, switch devices, recurse into this API, or retain the stream/context.
// Preallocate and prewarm work before ExecuteWorkload. Callback context need only
// survive the call; GPU buffers/modules and async-copy host storage must survive
// Synchronize AND the caller's D3D12 completion fence, including on enqueue error.
// Nonzero return or a caught C++ exception poisons the token and queues status 3
// after any partially queued work. The caller MUST still submit the D3D12 tail,
// drain both APIs, discard the exchange, and disable further work. Errors are
// thread-local and overwritten by the next helper call; copy them before draining.
// No cancellation: queued work runs even after a ready timeout. This ABI is only
// for bounded scratch workloads safe in that case. It cannot bound arbitrary GPU
// kernels, catch access violations, or roll back partially queued writes. Real
// resources require a separately validated device-side readiness guard/recovery.

using EnqueueWorkload = std::uint32_t(__cdecl*)(void* context, void* hipStream, std::uint32_t sequence);
using ExecuteWorkload = std::uint32_t(__cdecl*)(void* lifetimeToken, std::uint32_t baseWord,
                                                std::uint32_t sequence, std::uint32_t maxWaitIterations,
                                                EnqueueWorkload enqueueWorkload, void* workloadContext);
} // namespace DlssdQueueRendezvousAbi
