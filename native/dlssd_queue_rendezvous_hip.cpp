// Native HIP half of the opt-in DLSS-D queue-rendezvous diagnostic.
//
// The D3D12 side owns and shares one small buffer. This companion imports that
// buffer on the matching AMD adapter, waits for a D3D12-written sequence value,
// queues one caller-supplied workload on the same HIP stream, and publishes
// completion with system-scope atomics. Both waits are bounded; this DLL never
// handles image or vendor model data.

#include <hip/hip_runtime.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <new>
#include <thread>

#include "dlssd_queue_rendezvous_abi.h"

#define DLSSD_RENDEZVOUS_EXPORT

namespace
{
constexpr std::uint32_t kStatusInvalidArgument = 0xE0020001u;
constexpr std::uint32_t kStatusAdapterMismatch = 0xE0020002u;
constexpr std::uint32_t kStatusAllocationFailed = 0xE0020003u;
constexpr std::uint32_t kStatusInvalidToken = 0xE0020004u;
constexpr std::uint32_t kStatusKernelStartTimedOut = 0xE0020005u;
constexpr std::uint32_t kStatusWorkloadFailed = 0xE0020006u;
constexpr std::uint32_t kTokenMagic = 0x565A4452u; // RDZV
constexpr auto kKernelStartTimeout = std::chrono::milliseconds(1000);

constexpr std::uint32_t kReadyWord = 0;
constexpr std::uint32_t kDoneWord = 1;
constexpr std::uint32_t kHipStatusWord = 2;
constexpr std::uint32_t kPayloadWord = 4;
constexpr std::uint32_t kHipWaitIterationsWord = 6;
constexpr std::uint32_t kSequenceMirrorWord = 8;

thread_local std::uint32_t g_lastStatus = 0;
thread_local char g_lastError[512] = "";

struct Token
{
    std::uint32_t magic = kTokenMagic;
    hipExternalMemory_t externalMemory = nullptr;
    std::uint32_t* words = nullptr;
    std::uint32_t* armDevice = nullptr;
    hipEvent_t armEvent = nullptr;
    std::uint64_t size = 0;
    int device = -1;
    bool failed = false;
};

std::uint32_t set_error(std::uint32_t status, const char* message)
{
    g_lastStatus = status;
    std::snprintf(g_lastError, sizeof(g_lastError), "%s", message != nullptr ? message : "unknown error");
    return status;
}

std::uint32_t set_hip_error(const char* operation, hipError_t result)
{
    const char* text = hipGetErrorString(result);
    g_lastStatus = static_cast<std::uint32_t>(result);
    std::snprintf(g_lastError, sizeof(g_lastError), "%s: HIP %d (%s)", operation, static_cast<int>(result),
                  text != nullptr ? text : "unknown error");
    return g_lastStatus;
}

std::uint32_t success()
{
    g_lastStatus = 0;
    g_lastError[0] = '\0';
    return 0;
}

bool valid_handle(const void* handle)
{
    return handle != nullptr && handle != reinterpret_cast<const void*>(static_cast<std::intptr_t>(-1));
}

__device__ std::uint32_t make_payload(std::uint32_t sequence, std::uint32_t workIterations)
{
    std::uint32_t value = sequence ^ 0xA5A55A5Au;
    for (std::uint32_t i = 0; i < workIterations; ++i)
    {
        value = value * 1664525u + 1013904223u;
        value ^= value >> 16;
    }
    return value;
}

__global__ void arm_kernel(std::uint32_t* armedWord, std::uint32_t sequence)
{
    if (blockIdx.x == 0 && threadIdx.x == 0)
        *armedWord = sequence;
}

__global__ void wait_ready_kernel(std::uint32_t* words, std::uint32_t baseWord, std::uint32_t sequence,
                                  std::uint32_t maxWaitIterations)
{
    if (blockIdx.x != 0 || threadIdx.x != 0)
        return;

    std::uint32_t* slot = words + baseWord;
    std::uint32_t observed = 0;
    std::uint32_t waitIterations = 0;
    for (; waitIterations < maxWaitIterations; ++waitIterations)
    {
        observed = atomicAdd_system(slot + kReadyWord, 0u);
        if (observed == sequence)
            break;
    }

    atomicExch_system(slot + kHipWaitIterationsWord, waitIterations);
    atomicExch_system(slot + kSequenceMirrorWord, sequence);
    atomicExch_system(slot + kHipStatusWord,
                      observed == sequence ? DlssdQueueRendezvousAbi::HipSucceeded
                                           : DlssdQueueRendezvousAbi::HipReadyTimedOut);
}

__global__ void synthetic_payload_kernel(std::uint32_t* words, std::uint32_t baseWord, std::uint32_t sequence,
                                         std::uint32_t workIterations)
{
    if (blockIdx.x != 0 || threadIdx.x != 0)
        return;

    std::uint32_t* slot = words + baseWord;
    if (atomicAdd_system(slot + kHipStatusWord, 0u) == DlssdQueueRendezvousAbi::HipSucceeded)
        atomicExch_system(slot + kPayloadWord, make_payload(sequence, workIterations));
}

__global__ void complete_kernel(std::uint32_t* words, std::uint32_t baseWord, std::uint32_t sequence,
                                std::uint32_t workloadStatus)
{
    if (blockIdx.x != 0 || threadIdx.x != 0)
        return;

    std::uint32_t* slot = words + baseWord;
    if (workloadStatus != 0 &&
        atomicAdd_system(slot + kHipStatusWord, 0u) == DlssdQueueRendezvousAbi::HipSucceeded)
    {
        atomicExch_system(slot + kHipStatusWord, DlssdQueueRendezvousAbi::HipWorkloadFailed);
    }
    __threadfence_system();
    atomicExch_system(slot + kDoneWord, sequence);
}

struct SyntheticWorkload
{
    Token* token = nullptr;
    std::uint32_t baseWord = 0;
    std::uint32_t workIterations = 0;
};

std::uint32_t __cdecl enqueue_synthetic_workload(void* context, void* hipStream, std::uint32_t sequence)
{
    auto* workload = static_cast<SyntheticWorkload*>(context);
    if (workload == nullptr || workload->token == nullptr || workload->token->words == nullptr ||
        workload->workIterations == 0)
    {
        return kStatusInvalidArgument;
    }

    auto stream = static_cast<hipStream_t>(hipStream);
    synthetic_payload_kernel<<<1, 1, 0, stream>>>(workload->token->words, workload->baseWord, sequence,
                                                  workload->workIterations);
    return static_cast<std::uint32_t>(hipGetLastError());
}

std::uint32_t enqueue_workload(Token* token, std::uint32_t baseWord, std::uint32_t sequence,
                               std::uint32_t maxWaitIterations,
                               DlssdQueueRendezvousAbi::EnqueueWorkload workload,
                               void* workloadContext)
{
    if (token == nullptr || token->magic != kTokenMagic || token->words == nullptr || sequence == 0 ||
        maxWaitIterations == 0 || workload == nullptr ||
        (static_cast<std::uint64_t>(baseWord) + 16u) * sizeof(std::uint32_t) > token->size)
    {
        return set_error(kStatusInvalidToken, "invalid rendezvous workload token or bounds");
    }

    if (token->failed)
        return set_error(DlssdQueueRendezvousAbi::TokenFailed, "rendezvous token disabled after enqueue failure");
    // Any failure after this point can leave queued work. Never reuse that token.
    token->failed = true;
    hipError_t result = hipSetDevice(token->device);
    if (result != hipSuccess)
        return set_hip_error("hipSetDevice(execute)", result);
    hipStream_t stream = nullptr;
    arm_kernel<<<1, 1, 0, stream>>>(token->armDevice, sequence);
    result = hipGetLastError();
    if (result != hipSuccess)
        return set_hip_error("rendezvous arm kernel launch", result);
    result = hipEventRecord(token->armEvent, stream);
    if (result != hipSuccess)
        return set_hip_error("hipEventRecord(rendezvous arm)", result);

    wait_ready_kernel<<<1, 1, 0, stream>>>(token->words, baseWord, sequence, maxWaitIterations);
    result = hipGetLastError();
    if (result != hipSuccess)
        return set_hip_error("rendezvous ready-wait kernel launch", result);

    std::uint32_t workloadStatus = kStatusWorkloadFailed;
    try
    {
        workloadStatus = workload(workloadContext, static_cast<void*>(stream), sequence);
    }
    catch (...)
    {
        // Never unwind through the C ABI or omit completion after partial work.
        workloadStatus = kStatusWorkloadFailed;
    }
    // Preserve a callback's unconsumed launch error before enqueueing completion.
    result = hipGetLastError();
    if (workloadStatus == 0 && result != hipSuccess)
        workloadStatus = static_cast<std::uint32_t>(result);
    complete_kernel<<<1, 1, 0, stream>>>(token->words, baseWord, sequence, workloadStatus);
    result = hipGetLastError();
    if (result != hipSuccess)
        return set_hip_error("rendezvous completion kernel launch", result);

    const auto deadline = std::chrono::steady_clock::now() + kKernelStartTimeout;
    while ((result = hipEventQuery(token->armEvent)) == hipErrorNotReady &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (result == hipErrorNotReady)
        return set_error(kStatusKernelStartTimedOut, "rendezvous kernel did not arm before D3D12 submission");
    if (result != hipSuccess)
        return set_hip_error("hipEventQuery(rendezvous arm)", result);
    if (workloadStatus != 0)
    {
        char message[160] {};
        std::snprintf(message, sizeof(message), "rendezvous workload enqueue failed 0x%08X", workloadStatus);
        return set_error(kStatusWorkloadFailed, message);
    }
    token->failed = false;
    return success();
}

std::uint32_t warm_up_rendezvous_kernel()
{
    std::uint32_t* words = nullptr;
    hipError_t result = hipMalloc(&words, 16u * sizeof(std::uint32_t));
    if (result != hipSuccess || words == nullptr)
        return result != hipSuccess ? set_hip_error("hipMalloc(rendezvous warmup)", result)
                                    : set_error(kStatusAllocationFailed, "null rendezvous warmup allocation");
    result = hipMemset(words, 0, 16u * sizeof(std::uint32_t));
    const std::uint32_t ready = 1;
    if (result == hipSuccess)
        result = hipMemcpy(words + kReadyWord, &ready, sizeof(ready), hipMemcpyHostToDevice);
    if (result == hipSuccess)
    {
        wait_ready_kernel<<<1, 1>>>(words, 0, ready, 1);
        result = hipGetLastError();
    }
    if (result == hipSuccess)
    {
        synthetic_payload_kernel<<<1, 1>>>(words, 0, ready, 1);
        result = hipGetLastError();
    }
    if (result == hipSuccess)
    {
        complete_kernel<<<1, 1>>>(words, 0, ready, 0);
        result = hipGetLastError();
    }
    if (result == hipSuccess)
        result = hipDeviceSynchronize();
    const hipError_t freed = hipFree(words);
    if (result != hipSuccess)
        return set_hip_error("rendezvous kernel warmup", result);
    if (freed != hipSuccess)
        return set_hip_error("hipFree(rendezvous warmup)", freed);
    return success();
}
} // namespace

extern "C" DLSSD_RENDEZVOUS_EXPORT std::uint32_t __cdecl DLSSD_RENDEZVOUS_Initialize(
    std::uint32_t luidLowPart, std::int32_t luidHighPart, void* sharedHandle, std::uint64_t size, void** lifetimeToken)
{
    if (!valid_handle(sharedHandle) || size == 0 || lifetimeToken == nullptr)
        return set_error(kStatusInvalidArgument, "invalid rendezvous initialization arguments");
    *lifetimeToken = nullptr;

    hipError_t result = hipInit(0);
    if (result != hipSuccess)
        return set_hip_error("hipInit", result);

    int deviceCount = 0;
    result = hipGetDeviceCount(&deviceCount);
    if (result != hipSuccess)
        return set_hip_error("hipGetDeviceCount", result);

    char expectedLuid[8] {};
    std::memcpy(expectedLuid, &luidLowPart, sizeof(luidLowPart));
    std::memcpy(expectedLuid + sizeof(luidLowPart), &luidHighPart, sizeof(luidHighPart));
    int selectedDevice = -1;
    for (int device = 0; device < deviceCount; ++device)
    {
        hipDeviceProp_t properties {};
        result = hipGetDeviceProperties(&properties, device);
        if (result != hipSuccess)
            return set_hip_error("hipGetDeviceProperties", result);
        if (std::memcmp(properties.luid, expectedLuid, sizeof(expectedLuid)) == 0)
        {
            selectedDevice = device;
            break;
        }
    }
    if (selectedDevice < 0)
        return set_error(kStatusAdapterMismatch, "no HIP device matches the D3D12 adapter LUID");

    result = hipSetDevice(selectedDevice);
    if (result != hipSuccess)
        return set_hip_error("hipSetDevice", result);

    const std::uint32_t warmupResult = warm_up_rendezvous_kernel();
    if (warmupResult != 0)
        return warmupResult;

    hipExternalMemoryHandleDesc handleDescription {};
    handleDescription.type = hipExternalMemoryHandleTypeD3D12Resource;
    handleDescription.handle.win32.handle = sharedHandle;
    handleDescription.size = size;
    hipExternalMemory_t externalMemory = nullptr;
    result = hipImportExternalMemory(&externalMemory, &handleDescription);
    if (result != hipSuccess)
        return set_hip_error("hipImportExternalMemory", result);

    hipExternalMemoryBufferDesc bufferDescription {};
    bufferDescription.size = size;
    void* mapped = nullptr;
    result = hipExternalMemoryGetMappedBuffer(&mapped, externalMemory, &bufferDescription);
    if (result != hipSuccess || mapped == nullptr)
    {
        if (externalMemory != nullptr)
            (void) hipDestroyExternalMemory(externalMemory);
        return result != hipSuccess ? set_hip_error("hipExternalMemoryGetMappedBuffer", result)
                                    : set_error(kStatusAllocationFailed, "HIP returned a null buffer mapping");
    }

    auto* token = new (std::nothrow) Token {};
    if (token == nullptr)
    {
        (void) hipFree(mapped);
        (void) hipDestroyExternalMemory(externalMemory);
        return set_error(kStatusAllocationFailed, "unable to allocate rendezvous lifetime token");
    }
    token->externalMemory = externalMemory;
    token->words = static_cast<std::uint32_t*>(mapped);
    token->size = size;
    token->device = selectedDevice;

    // A newly allocated D3D12 buffer has no promised initial contents. The HIP
    // waiter is submitted first, so stale Ready == sequence could otherwise
    // complete before SignalReadyCS clears the slot. Initialize only while the
    // caller owns an idle, protocol-only buffer, before either queue uses it.
    result = hipMemset(token->words, 0, size);
    if (result == hipSuccess)
        result = hipDeviceSynchronize();
    if (result != hipSuccess)
    {
        (void) hipFree(token->words);
        (void) hipDestroyExternalMemory(token->externalMemory);
        delete token;
        return set_hip_error("rendezvous protocol initialization", result);
    }

    result = hipMalloc(&token->armDevice, sizeof(std::uint32_t));
    if (result != hipSuccess || token->armDevice == nullptr)
    {
        (void) hipFree(token->words);
        (void) hipDestroyExternalMemory(token->externalMemory);
        delete token;
        return result != hipSuccess ? set_hip_error("hipMalloc(rendezvous arm)", result)
                                    : set_error(kStatusAllocationFailed, "null rendezvous arm allocation");
    }
    result = hipEventCreateWithFlags(&token->armEvent, hipEventDisableTiming);
    if (result != hipSuccess || token->armEvent == nullptr)
    {
        (void) hipFree(token->armDevice);
        (void) hipFree(token->words);
        (void) hipDestroyExternalMemory(token->externalMemory);
        delete token;
        return result != hipSuccess ? set_hip_error("hipEventCreateWithFlags(rendezvous arm)", result)
                                    : set_error(kStatusAllocationFailed, "null rendezvous arm event");
    }
    *lifetimeToken = token;
    return success();
}

extern "C" DLSSD_RENDEZVOUS_EXPORT std::uint32_t __cdecl DLSSD_RENDEZVOUS_Execute(
    void* lifetimeToken, std::uint32_t baseWord, std::uint32_t sequence, std::uint32_t maxWaitIterations,
    std::uint32_t workIterations)
{
    auto* token = static_cast<Token*>(lifetimeToken);
    if (workIterations == 0)
        return set_error(kStatusInvalidArgument, "rendezvous synthetic workload requires nonzero work");
    SyntheticWorkload workload { token, baseWord, workIterations };
    return enqueue_workload(token, baseWord, sequence, maxWaitIterations, enqueue_synthetic_workload, &workload);
}

extern "C" DLSSD_RENDEZVOUS_EXPORT std::uint32_t __cdecl DLSSD_RENDEZVOUS_ExecuteWorkload(
    void* lifetimeToken, std::uint32_t baseWord, std::uint32_t sequence, std::uint32_t maxWaitIterations,
    DlssdQueueRendezvousAbi::EnqueueWorkload workload, void* workloadContext)
{
    return enqueue_workload(static_cast<Token*>(lifetimeToken), baseWord, sequence, maxWaitIterations, workload,
                            workloadContext);
}

extern "C" DLSSD_RENDEZVOUS_EXPORT std::uint32_t __cdecl DLSSD_RENDEZVOUS_Synchronize(void* lifetimeToken)
{
    auto* token = static_cast<Token*>(lifetimeToken);
    if (token == nullptr || token->magic != kTokenMagic)
        return set_error(kStatusInvalidToken, "invalid rendezvous synchronization token");

    hipError_t result = hipSetDevice(token->device);
    if (result == hipSuccess)
        result = hipDeviceSynchronize();
    if (result != hipSuccess)
    {
        token->failed = true;
        return set_hip_error("hipDeviceSynchronize", result);
    }
    return success();
}

extern "C" DLSSD_RENDEZVOUS_EXPORT std::uint32_t __cdecl DLSSD_RENDEZVOUS_Destroy(void* lifetimeToken)
{
    auto* token = static_cast<Token*>(lifetimeToken);
    if (token == nullptr || token->magic != kTokenMagic)
        return set_error(kStatusInvalidToken, "invalid rendezvous lifetime token");

    std::uint32_t finalStatus = 0;
    hipError_t result = hipSetDevice(token->device);
    if (result == hipSuccess)
        result = hipDeviceSynchronize();
    if (result != hipSuccess)
        finalStatus = set_hip_error("hipDeviceSynchronize(destroy)", result);
    if (token->words != nullptr)
    {
        result = hipFree(token->words);
        if (result != hipSuccess && finalStatus == 0)
            finalStatus = set_hip_error("hipFree(external mapping)", result);
    }
    if (token->armEvent != nullptr)
    {
        result = hipEventDestroy(token->armEvent);
        if (result != hipSuccess && finalStatus == 0)
            finalStatus = set_hip_error("hipEventDestroy(rendezvous arm)", result);
    }
    if (token->armDevice != nullptr)
    {
        result = hipFree(token->armDevice);
        if (result != hipSuccess && finalStatus == 0)
            finalStatus = set_hip_error("hipFree(rendezvous arm)", result);
    }
    if (token->externalMemory != nullptr)
    {
        result = hipDestroyExternalMemory(token->externalMemory);
        if (result != hipSuccess && finalStatus == 0)
            finalStatus = set_hip_error("hipDestroyExternalMemory", result);
    }
    token->magic = 0;
    delete token;
    return finalStatus == 0 ? success() : finalStatus;
}

extern "C" DLSSD_RENDEZVOUS_EXPORT std::uint32_t __cdecl DLSSD_RENDEZVOUS_GetLastErrorStatus()
{
    return g_lastStatus;
}

extern "C" DLSSD_RENDEZVOUS_EXPORT const char* __cdecl DLSSD_RENDEZVOUS_GetLastErrorText()
{
    return g_lastError;
}
