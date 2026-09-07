#include "pch.h"

#include "DlssdExperimentalBackend.h"
#include "DlssdTranslatedSession.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <denoisers/RrInputRegistry.h>

#include "../../native/dlssd_translated_runtime.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace DlssdExperimentalBackend
{
namespace
{
using namespace RayReconstruction;
using Clock = std::chrono::steady_clock;

struct AcceptedExtent
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t outputWidth;
    uint32_t outputHeight;
    const char* name;
};

constexpr AcceptedExtent kAcceptedExtents[] = {
    { 1706, 960, 2560, 1440, "cp2077-quality" },
    { 1920, 1080, 3840, 2160, "fullhd" },
};

std::atomic<uint32_t> gEvaluated { 0 };
std::atomic<uint32_t> gRejectedExtent { 0 };
std::atomic<uint32_t> gRejectedCaptureOnly { 0 };
std::atomic<uint32_t> gRejectedInventory { 0 };
std::atomic<uint32_t> gRejectedRuntime { 0 };
std::atomic<uint32_t> gSkippedUnsafe { 0 };
std::atomic<uint32_t> gDispatched { 0 };
std::once_flag gLoggedContract;
std::atomic<bool> gRuntimePoisoned { false };
Clock::time_point gLastEvaluateExit {};
bool gHaveLastEvaluateExit = false;
Clock::time_point gLoadIdleAt {};
bool gSawLoadIdle = false;
Clock::time_point gSettleStarted {};
bool gSettling = false;
Clock::time_point gInWorldAt {};
bool gInWorld = false;
Clock::time_point gLastDispatchAt {};
bool gHaveLastDispatch = false;

// The 7 s NGX gap at save-load is the load START, not "world ready".
// Same-thread Init+Create on the Streamline evaluate caller never
// returned (2026-09-06 watchdog, 120 s after ngx create feature 13).
// A dedicated create thread also never returned; leftover waiter was
// Streamline tid 22664 in the NVAPI bridge / nvngx_dlssd. slEvaluateFeature
// is not thread-safe and must return quickly. Prepare (hooks/staging)
// stays on the evaluate caller and must not LoadLibrary nvcuda there.
// Init, CreateFeature, and sidecar EvaluateFeature run on one persistent
// owner so ZLUDA's CUDA TLS stack stays on that thread. Present only
// signals a submitted frame. Bound create at 8 s; abandon and leak if
// it hangs. Keep the 30 s floor, 10 s resource settle, and 60 s
// in-world plus a stable VRAM window. A dead VRAM query must not block
// HIP; a climbing window still delays prepare. Command-list pointer
// changes are not resource identity. Do not restart settle just because
// nothing has dispatched.
constexpr long long kDropMappingsIdleMs = 400;
constexpr long long kLoadIdleMs = 1000;
constexpr long long kLoadMinMs = 30000;
constexpr long long kCreateTimeoutMs = 8000;
constexpr long long kVramStableMs = 10000;
constexpr uint64_t kVramClimbBytes = 256ull * 1024ull * 1024ull;
constexpr long long kSettleStableMs = 10000;
constexpr long long kInWorldStableMs = 60000;
constexpr long long kExperimentalMinIntervalMs = 2000;
constexpr uint32_t kSkipLogEvery = 60;

struct EvaluateKey
{
    const void* color = nullptr;
    const void* output = nullptr;
    const void* depth = nullptr;
    const void* motion = nullptr;

    bool operator==(const EvaluateKey&) const = default;
};

EvaluateKey gLastKey {};
bool gHaveLastKey = false;

struct VramSample
{
    Clock::time_point at {};
    uint64_t bytes = 0;
};

std::deque<VramSample> gVramHistory;
uint64_t gVramLastBytes = 0;
uint64_t gVramWindowMin = 0;
uint64_t gVramWindowMax = 0;
uint64_t gVramSessionMin = 0;
uint64_t gVramSessionMax = 0;
long long gVramSpanMs = 0;
bool gVramQueryOk = false;
bool gVramSawClimb = false;
bool gLoggedDeadVramQuery = false;
const char* gLastSkipReason = "";
uint32_t gLastSkipReasonCount = 0;

enum class OwnerOp
{
    FinishAttach,
    Evaluate,
};

struct OwnerJob
{
    OwnerOp op = OwnerOp::Evaluate;
    InputSnapshot snapshot;
    uint32_t handleId = 0;
    const AcceptedExtent* extent = nullptr;
    ID3D12CommandQueue* queue = nullptr;
};

std::mutex gJobMutex;
std::condition_variable gJobCv;
std::mutex gThreadMutex;
std::thread gOwner;
bool gHaveJob = false;
std::atomic<bool> gOwnerBusy { false };
bool gShutdown = false;
OwnerJob gJob;
OwnerJob gPending;
bool gHavePending = false;
bool gReleaseAfterOwner = false;
bool gLoggedBusyDefer = false;
ID3D12Fence* gWaitFence = nullptr;
HANDLE gWaitEvent = nullptr;
UINT64 gWaitValue = 0;

void AddRefSnapshot(InputSnapshot& snapshot);
void ReleaseSnapshot(InputSnapshot& snapshot);
void AddRefJob(OwnerJob& job, ID3D12CommandQueue* queue);
void ReleaseJob(OwnerJob& job);
void EnsureOwner();
void StopOwner(bool abortIfCreating);
bool QueueFinishAttach(uint32_t handleId);
bool AbortHungCreateIfTimedOut(uint32_t handleId, const AcceptedExtent* extent);

void FlushExperimentalLog()
{
    if (auto logger = spdlog::default_logger())
        logger->flush();
}

void LogContractOnce()
{
    std::call_once(gLoggedContract,
                   []
                   {
                       LOG_INFO("DLSS-D experimental backend: accepted contracts are 1706x960 -> 2560x1440 "
                                "(Cyberpunk Quality) and isolated 1920x1080 -> 3840x2160 signed 310.7. "
                                "Prepare stays on the NGX evaluate call without loading nvcuda. "
                                "Init, CreateFeature, and sidecar EvaluateFeature run on a persistent "
                                "owner thread. Input copies stay on the caller list; Present only "
                                "signals a submitted frame. It is not same-command-list HIP insertion. "
                                "Intro skips this path. A long NGX idle keeps FSR-RR for 30 s plus a "
                                "10 s resource settle and 60 s in-world. Create is bounded at 8 s. "
                                "Failures keep FSR-RR.");
                   });
}

void LogCounters(uint32_t handleId, const char* reason)
{
    LOG_WARN("DLSS-D experimental backend handle={} {}: evaluated={} extent={} capture_only={} "
             "inventory={} runtime={} skipped={} dispatched={}",
             handleId, reason, gEvaluated.load(), gRejectedExtent.load(), gRejectedCaptureOnly.load(),
             gRejectedInventory.load(), gRejectedRuntime.load(), gSkippedUnsafe.load(), gDispatched.load());
}

const void* ResourcePointer(const InputSnapshot& snapshot, InputSemantic semantic)
{
    const auto* input = snapshot.Find(semantic);
    return input != nullptr ? input->resource : nullptr;
}

EvaluateKey MakeEvaluateKey(const InputSnapshot& snapshot, ID3D12GraphicsCommandList*)
{
    EvaluateKey key;
    key.color = ResourcePointer(snapshot, InputSemantic::Color);
    key.output = ResourcePointer(snapshot, InputSemantic::Output);
    key.depth = ResourcePointer(snapshot, InputSemantic::Depth);
    key.motion = ResourcePointer(snapshot, InputSemantic::MotionVectors);
    return key;
}

long long IdleSinceLastBoundaryMs()
{
    if (!gHaveLastEvaluateExit)
        return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - gLastEvaluateExit).count();
}

void NoteEvaluateBoundary()
{
    gLastEvaluateExit = Clock::now();
    gHaveLastEvaluateExit = true;
}

void DropTranslatedMappings()
{
    bool releaseNow = true;
    {
        std::unique_lock lock(gJobMutex);
        if (gHavePending)
        {
            ReleaseJob(gPending);
            gHavePending = false;
        }
        if (gHaveJob)
        {
            ReleaseJob(gJob);
            gHaveJob = false;
        }
        if (gOwnerBusy.load(std::memory_order_acquire))
        {
            gReleaseAfterOwner = true;
            releaseNow = false;
        }
    }
    gHaveLastKey = false;
    gHaveLastDispatch = false;
    gSettling = false;
    gInWorld = false;
    if (DlssdTranslatedSession::IsCreating())
    {
        LOG_INFO("DLSS-D experimental backend skipped translated release while NGX create is in flight");
        return;
    }
    if (releaseNow)
        DlssdTranslatedSession::Release();
    else
        LOG_INFO("DLSS-D experimental backend deferred translated release until the owner finishes");
}

bool AttachInFlight()
{
    return DlssdTranslatedSession::IsCreating() ||
           (DlssdTranslatedSession::IsPrepared() && !DlssdTranslatedSession::IsAttached());
}

void BeginLoadUnsafeWindow()
{
    DropTranslatedMappings();
    gSawLoadIdle = true;
    gLoadIdleAt = Clock::now();
    gSettling = false;
    gInWorld = false;
    gVramHistory.clear();
    gVramLastBytes = 0;
    gVramWindowMin = 0;
    gVramWindowMax = 0;
    gVramSessionMin = 0;
    gVramSessionMax = 0;
    gVramSpanMs = 0;
    gVramQueryOk = false;
    gVramSawClimb = false;
    gLoggedDeadVramQuery = false;
}

long long MsUntil(const Clock::time_point& start, long long durationMs)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (elapsed >= durationMs)
        return 0;
    return durationMs - elapsed;
}

Decision SkipUnsafe(uint32_t handleId, const char* reason, long long idleMs, uint32_t reset,
                    const AcceptedExtent* extent, long long remainingMs)
{
    const auto skipped = gSkippedUnsafe.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool reasonChanged = gLastSkipReason[0] == '\0' || std::strcmp(gLastSkipReason, reason) != 0;
    if (reasonChanged)
    {
        gLastSkipReason = reason;
        gLastSkipReasonCount = 1;
    }
    else
        ++gLastSkipReasonCount;
    if (reasonChanged || gLastSkipReasonCount <= 2 || (skipped % kSkipLogEvery) == 0)
    {
        LOG_INFO("DLSS-D experimental backend handle={} evaluate skip reason={} idle_ms={} "
                 "remaining_ms={} reset={} evaluated={} dispatched={} skipped={} extent={} "
                 "tid={} vram_mb={} vram_delta_mb={} vram_span_ms={} climb={} query={}",
                 handleId, reason, idleMs, remainingMs, reset, gEvaluated.load(), gDispatched.load(), skipped,
                 extent != nullptr ? extent->name : "none", GetCurrentThreadId(),
                 gVramLastBytes / (1024ull * 1024ull),
                 gVramWindowMax > gVramWindowMin ? (gVramWindowMax - gVramWindowMin) / (1024ull * 1024ull) : 0,
                 gVramSpanMs,
                 (gVramQueryOk && gVramWindowMax > gVramWindowMin &&
                  (gVramWindowMax - gVramWindowMin) >= kVramClimbBytes)
                     ? 1
                     : 0,
                 gVramQueryOk ? 1 : 0);
        if (reasonChanged)
            FlushExperimentalLog();
    }
    NoteEvaluateBoundary();
    return Decision::Rejected;
}

bool HasRequiredInventory(const InputSnapshot& snapshot)
{
    constexpr InputSemantic required[] = {
        InputSemantic::Color,          InputSemantic::Output,         InputSemantic::Depth,
        InputSemantic::MotionVectors,  InputSemantic::Normals,        InputSemantic::DiffuseAlbedo,
        InputSemantic::SpecularAlbedo, InputSemantic::SpecularHitDistance,
        InputSemantic::ColorBeforeParticles, InputSemantic::BiasMask,
        InputSemantic::ScreenSpaceSubsurfaceScatteringGuide,
    };
    for (const auto semantic : required)
    {
        const auto* input = snapshot.Find(semantic);
        if (!input || !input->IsPresent())
            return false;
    }
    return snapshot.hasWorldToView && snapshot.hasViewToClip;
}

const AcceptedExtent* FindAcceptedExtent(const InputSnapshot& snapshot)
{
    for (const auto& extent : kAcceptedExtents)
    {
        if (snapshot.renderWidth == extent.renderWidth && snapshot.renderHeight == extent.renderHeight &&
            snapshot.outputWidth == extent.outputWidth && snapshot.outputHeight == extent.outputHeight)
            return &extent;
    }
    return nullptr;
}

uint64_t QueryAdapterLocalUsage(IDXGIAdapter* adapter)
{
    if (adapter == nullptr)
        return 0;
    IDXGIAdapter3* adapter3 = nullptr;
    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) || adapter3 == nullptr)
        return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO info {};
    const HRESULT infoHr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    adapter3->Release();
    if (FAILED(infoHr))
        return 0;
    return info.CurrentUsage;
}

IDXGIAdapter3* gCachedVramAdapter = nullptr;
LUID gCachedVramLuid {};

void ReleaseCachedVramAdapter()
{
    if (gCachedVramAdapter != nullptr)
    {
        gCachedVramAdapter->Release();
        gCachedVramAdapter = nullptr;
    }
    gCachedVramLuid = {};
}

bool SameAdapterLuid(const LUID& left, const LUID& right)
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

void CacheVramAdapter(IDXGIAdapter* adapter, const LUID& luid)
{
    if (adapter == nullptr)
        return;
    IDXGIAdapter3* adapter3 = nullptr;
    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) || adapter3 == nullptr)
        return;
    ReleaseCachedVramAdapter();
    gCachedVramAdapter = adapter3;
    gCachedVramLuid = luid;
}

uint64_t QueryLocalVramBytes(ID3D12Device* device)
{
    if (device == nullptr)
        return 0;

    const LUID luid = device->GetAdapterLuid();
    if (gCachedVramAdapter != nullptr && SameAdapterLuid(gCachedVramLuid, luid))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info {};
        if (SUCCEEDED(gCachedVramAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return info.CurrentUsage;
        ReleaseCachedVramAdapter();
    }

    IDXGIFactory1* factory1 = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory1))) && factory1 != nullptr)
    {
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory4* factory4 = nullptr;
        if (SUCCEEDED(factory1->QueryInterface(IID_PPV_ARGS(&factory4))) && factory4 != nullptr)
        {
            factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
            factory4->Release();
        }
        if (adapter == nullptr)
        {
            for (UINT index = 0;; ++index)
            {
                IDXGIAdapter1* adapter1 = nullptr;
                if (FAILED(factory1->EnumAdapters1(index, &adapter1)) || adapter1 == nullptr)
                    break;
                DXGI_ADAPTER_DESC1 desc {};
                if (SUCCEEDED(adapter1->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
                    desc.AdapterLuid.HighPart == luid.HighPart)
                {
                    adapter = adapter1;
                    break;
                }
                adapter1->Release();
            }
        }
        factory1->Release();
        if (adapter != nullptr)
        {
            const uint64_t usage = QueryAdapterLocalUsage(adapter);
            if (usage > 0)
                CacheVramAdapter(adapter, luid);
            adapter->Release();
            if (usage > 0)
                return usage;
        }
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || dxgiDevice == nullptr)
        return 0;
    IDXGIAdapter* adapter = nullptr;
    const HRESULT adapterHr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(adapterHr) || adapter == nullptr)
        return 0;
    const uint64_t usage = QueryAdapterLocalUsage(adapter);
    if (usage > 0)
        CacheVramAdapter(adapter, luid);
    adapter->Release();
    return usage;
}

void NoteVram(uint64_t bytes)
{
    const auto now = Clock::now();
    gVramLastBytes = bytes;
    gVramQueryOk = bytes > 0;
    if (gVramQueryOk)
    {
        if (gVramSessionMin == 0 || bytes < gVramSessionMin)
            gVramSessionMin = bytes;
        if (bytes > gVramSessionMax)
            gVramSessionMax = bytes;
        if (gVramSessionMax - gVramSessionMin >= kVramClimbBytes)
            gVramSawClimb = true;
    }
    gVramHistory.push_back({ now, bytes });
    while (!gVramHistory.empty() &&
           std::chrono::duration_cast<std::chrono::milliseconds>(now - gVramHistory.front().at).count() > kVramStableMs)
        gVramHistory.pop_front();
    gVramWindowMin = 0;
    gVramWindowMax = 0;
    gVramSpanMs = 0;
    if (gVramHistory.empty())
        return;
    gVramWindowMin = gVramHistory.front().bytes;
    gVramWindowMax = gVramHistory.front().bytes;
    for (const auto& sample : gVramHistory)
    {
        gVramWindowMin = (std::min)(gVramWindowMin, sample.bytes);
        gVramWindowMax = (std::max)(gVramWindowMax, sample.bytes);
    }
    gVramSpanMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(gVramHistory.back().at - gVramHistory.front().at).count();
}

void AddRefSnapshot(InputSnapshot& snapshot)
{
    for (auto& input : snapshot.resources)
    {
        if (input.resource != nullptr)
            input.resource->AddRef();
    }
}

void ReleaseSnapshot(InputSnapshot& snapshot)
{
    for (auto& input : snapshot.resources)
    {
        if (input.resource != nullptr)
        {
            input.resource->Release();
            input.resource = nullptr;
        }
    }
}

void AddRefJob(OwnerJob& job, ID3D12CommandQueue* queue)
{
    AddRefSnapshot(job.snapshot);
    job.queue = queue;
    if (job.queue != nullptr)
        job.queue->AddRef();
}

void ReleaseJob(OwnerJob& job)
{
    ReleaseSnapshot(job.snapshot);
    if (job.queue != nullptr)
    {
        job.queue->Release();
        job.queue = nullptr;
    }
}

void ReleaseWaitFence()
{
    if (gWaitEvent != nullptr)
    {
        CloseHandle(gWaitEvent);
        gWaitEvent = nullptr;
    }
    if (gWaitFence != nullptr)
    {
        gWaitFence->Release();
        gWaitFence = nullptr;
    }
    gWaitValue = 0;
}

bool WaitGameQueue(ID3D12CommandQueue* queue, DWORD timeoutMs)
{
    if (queue == nullptr)
        queue = State::Instance().currentCommandQueue;
    if (queue == nullptr)
        return false;
    if (gWaitFence == nullptr)
    {
        ID3D12Device* device = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
            return false;
        const HRESULT fenceHr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gWaitFence));
        device->Release();
        if (FAILED(fenceHr) || gWaitFence == nullptr)
            return false;
        gWaitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (gWaitEvent == nullptr)
        {
            gWaitFence->Release();
            gWaitFence = nullptr;
            return false;
        }
    }
    const auto value = ++gWaitValue;
    if (FAILED(queue->Signal(gWaitFence, value)))
        return false;
    if (gWaitFence->GetCompletedValue() >= value)
        return true;
    if (FAILED(gWaitFence->SetEventOnCompletion(value, gWaitEvent)))
        return false;
    return WaitForSingleObject(gWaitEvent, timeoutMs) == WAIT_OBJECT_0;
}

void OwnerLoop()
{
    DlssdTranslatedSession::ScopedAttachLoadBypass bypass;
    DlssdTranslatedSession::BindCudaOnThisThread();
    LOG_INFO("DLSS-D experimental backend NGX owner tid={}", GetCurrentThreadId());
    FlushExperimentalLog();
    for (;;)
    {
        OwnerJob job;
        {
            std::unique_lock lock(gJobMutex);
            gJobCv.wait(lock, [] { return gShutdown || gHaveJob; });
            if (gShutdown && !gHaveJob)
                return;
            job = std::move(gJob);
            gHaveJob = false;
            gOwnerBusy.store(true, std::memory_order_release);
        }

        if (job.op == OwnerOp::FinishAttach)
        {
            const char* error = nullptr;
            const auto enter = Clock::now();
            const bool ok = DlssdTranslatedSession::FinishAttachOnOwnerThread(&error);
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - enter).count();
            if (!ok)
            {
                if (!DlssdTranslatedSession::IsCreating())
                    DlssdTranslatedSession::Release();
                gRuntimePoisoned.store(true, std::memory_order_relaxed);
                gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("DLSS-D experimental backend handle={} owner finish attach failed after {} ms: {}",
                         job.handleId, elapsedMs, error != nullptr ? error : "unknown");
                LogCounters(job.handleId, "translated runtime create failed");
                FlushExperimentalLog();
            }
            else
            {
                LOG_INFO("DLSS-D experimental backend handle={} finish attach ok ms={} tid={}", job.handleId,
                         elapsedMs, GetCurrentThreadId());
                FlushExperimentalLog();
            }
        }
        else
        {
            const char* error = nullptr;
            const auto enter = Clock::now();
            bool ok = WaitGameQueue(job.queue, 2000);
            const bool waitFailed = !ok;
            if (!ok)
                error = "game queue wait failed before unpublished evaluate";
            else
                ok = DlssdTranslatedSession::Evaluate(nullptr, job.snapshot, nullptr, false, &error,
                                                      DlssdRuntimeFrame_SkipInputCopy);
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - enter).count();
            ReleaseJob(job);

            if (!ok && waitFailed)
            {
                gSkippedUnsafe.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("DLSS-D experimental backend handle={} owner skipped after {} ms: {}", job.handleId, elapsedMs,
                         error);
                FlushExperimentalLog();
            }
            else if (!ok)
            {
                DlssdTranslatedSession::Release();
                gRuntimePoisoned.store(true, std::memory_order_relaxed);
                gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("DLSS-D experimental backend handle={} owner evaluate failed after {} ms: {}", job.handleId,
                         elapsedMs, error != nullptr ? error : "unknown");
                LogCounters(job.handleId, "translated runtime owner failed");
                FlushExperimentalLog();
            }
            else
            {
                const auto dispatched = gDispatched.fetch_add(1, std::memory_order_relaxed) + 1;
                LOG_INFO("DLSS-D experimental backend handle={} evaluate exit ok=1 publish=0 ms={} dispatched={} "
                         "extent={} tid={}",
                         job.handleId, elapsedMs, dispatched, job.extent != nullptr ? job.extent->name : "none",
                         GetCurrentThreadId());
                FlushExperimentalLog();
            }
        }

        bool releaseAfter = false;
        {
            std::lock_guard lock(gJobMutex);
            gOwnerBusy.store(false, std::memory_order_release);
            releaseAfter = gReleaseAfterOwner;
            gReleaseAfterOwner = false;
            gJobCv.notify_all();
        }
        if (releaseAfter)
            DlssdTranslatedSession::Release();
    }
}

void EnsureOwner()
{
    std::lock_guard lock(gThreadMutex);
    if (gOwner.joinable())
        return;
    gShutdown = false;
    gOwner = std::thread(OwnerLoop);
}

void StopOwner(bool abortIfCreating)
{
    const bool abort = abortIfCreating && DlssdTranslatedSession::IsCreating();
    if (abort)
    {
        std::lock_guard threadLock(gThreadMutex);
        if (gOwner.joinable())
        {
            TerminateThread(gOwner.native_handle(), 1);
            gOwner.detach();
        }
        std::unique_lock lock(gJobMutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            if (gHavePending)
                ReleaseJob(gPending);
            gHavePending = false;
            if (gHaveJob)
                ReleaseJob(gJob);
            gHaveJob = false;
            gOwnerBusy.store(false, std::memory_order_release);
            gReleaseAfterOwner = false;
            gShutdown = true;
        }
        else
        {
            gHaveJob = false;
            gHavePending = false;
            gOwnerBusy.store(false, std::memory_order_release);
            gShutdown = true;
        }
        DlssdTranslatedSession::AbandonHungCreate();
        ReleaseWaitFence();
        LOG_WARN("DLSS-D experimental backend aborted the hung NGX owner thread");
        FlushExperimentalLog();
        return;
    }

    {
        std::lock_guard lock(gJobMutex);
        gShutdown = true;
        gReleaseAfterOwner = false;
        if (gHavePending)
            ReleaseJob(gPending);
        gHavePending = false;
        if (gHaveJob)
            ReleaseJob(gJob);
        gHaveJob = false;
        gJobCv.notify_all();
    }
    std::lock_guard lock(gThreadMutex);
    if (gOwner.joinable())
        gOwner.join();
    ReleaseWaitFence();
}

bool QueueFinishAttach(uint32_t handleId)
{
    EnsureOwner();
    std::lock_guard lock(gJobMutex);
    if (gShutdown || gHaveJob || gOwnerBusy.load(std::memory_order_acquire))
        return false;
    gJob = {};
    gJob.op = OwnerOp::FinishAttach;
    gJob.handleId = handleId;
    gHaveJob = true;
    gJobCv.notify_one();
    return true;
}

bool QueueUnpublishedEvaluate(uint32_t handleId, const InputSnapshot& snapshot, const AcceptedExtent* extent,
                              ID3D12CommandQueue* queue)
{
    if (!DlssdTranslatedSession::IsAttached())
        return false;
    EnsureOwner();
    std::lock_guard lock(gJobMutex);
    if (gShutdown || gHaveJob || gOwnerBusy.load(std::memory_order_acquire))
        return false;
    gJob = {};
    gJob.op = OwnerOp::Evaluate;
    gJob.snapshot = snapshot;
    AddRefJob(gJob, queue);
    gJob.handleId = handleId;
    gJob.extent = extent;
    gHaveJob = true;
    gJobCv.notify_one();
    return true;
}

bool AbortHungCreateIfTimedOut(uint32_t handleId, const AcceptedExtent* extent)
{
    if (!DlssdTranslatedSession::IsCreating())
        return false;
    const auto elapsedMs = DlssdTranslatedSession::CreatingElapsedMs();
    if (elapsedMs < kCreateTimeoutMs)
        return false;
    LOG_WARN("DLSS-D experimental backend handle={} create timed out after {} ms extent={}", handleId, elapsedMs,
             extent != nullptr ? extent->name : "none");
    FlushExperimentalLog();
    gRuntimePoisoned.store(true, std::memory_order_relaxed);
    gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
    StopOwner(true);
    LogCounters(handleId, "translated runtime create timed out");
    FlushExperimentalLog();
    return true;
}

Decision RunPrepareCreateThenQueue(uint32_t handleId, const InputSnapshot& snapshot, ID3D12Device* device,
                                   ID3D12GraphicsCommandList* commandList, const AcceptedExtent* extent)
{
    if (device == nullptr)
    {
        gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
        if (gRejectedRuntime.load() == 1)
            LogCounters(handleId, "missing evaluate device");
        NoteEvaluateBoundary();
        return Decision::Rejected;
    }
    if (!gVramQueryOk && !gLoggedDeadVramQuery)
    {
        gLoggedDeadVramQuery = true;
        LOG_WARN("DLSS-D experimental backend handle={} local VRAM query returned 0; "
                 "continuing after the load window tid={} ",
                 handleId, GetCurrentThreadId());
    }

    if (AbortHungCreateIfTimedOut(handleId, extent))
    {
        NoteEvaluateBoundary();
        return Decision::Rejected;
    }

    if (DlssdTranslatedSession::IsCreating())
        return SkipUnsafe(handleId, "creating", IdleSinceLastBoundaryMs(), snapshot.reset, extent,
                          DlssdTranslatedSession::CreatingElapsedMs());

    if (!DlssdTranslatedSession::IsAttached())
    {
        const auto enter = Clock::now();
        const char* error = nullptr;
        const bool needPrepare = !DlssdTranslatedSession::IsPrepared();
        if (needPrepare)
        {
            LOG_INFO("DLSS-D experimental backend handle={} evaluate enter prepare=1 publish=0 evaluated={} "
                     "dispatched={} extent={} tid={} vram_mb={} climb={} query={}",
                     handleId, gEvaluated.load(), gDispatched.load(), extent != nullptr ? extent->name : "none",
                     GetCurrentThreadId(), gVramLastBytes / (1024ull * 1024ull), gVramSawClimb ? 1 : 0,
                     gVramQueryOk ? 1 : 0);
            FlushExperimentalLog();
            if (!DlssdTranslatedSession::EnsurePrepared(device, snapshot.renderWidth, snapshot.renderHeight,
                                                        snapshot.outputWidth, snapshot.outputHeight, &error))
            {
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - enter).count();
                DlssdTranslatedSession::Release();
                gRuntimePoisoned.store(true, std::memory_order_relaxed);
                gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("DLSS-D experimental backend handle={} prepare failed after {} ms: {}", handleId, elapsedMs,
                         error != nullptr ? error : "unknown");
                LogCounters(handleId, "translated runtime prepare failed");
                FlushExperimentalLog();
                NoteEvaluateBoundary();
                return Decision::Rejected;
            }
            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - enter).count();
            LOG_INFO("DLSS-D experimental backend handle={} prepare ok ms={} tid={}", handleId, elapsedMs,
                     GetCurrentThreadId());
            FlushExperimentalLog();
        }
        EnsureOwner();
        if (!DlssdTranslatedSession::StartFinishAttach(&error))
        {
            DlssdTranslatedSession::Release();
            gRuntimePoisoned.store(true, std::memory_order_relaxed);
            gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("DLSS-D experimental backend handle={} finish attach request failed: {}", handleId,
                     error != nullptr ? error : "unknown");
            LogCounters(handleId, "translated runtime create failed");
            FlushExperimentalLog();
            NoteEvaluateBoundary();
            return Decision::Rejected;
        }
        if (!DlssdTranslatedSession::IsAttached())
        {
            if (DlssdTranslatedSession::IsCreating() && !QueueFinishAttach(handleId))
            {
                gRuntimePoisoned.store(true, std::memory_order_relaxed);
                gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
                StopOwner(true);
                LOG_WARN("DLSS-D experimental backend handle={} could not queue owner finish attach", handleId);
                LogCounters(handleId, "translated runtime create queue failed");
                FlushExperimentalLog();
                NoteEvaluateBoundary();
                return Decision::Rejected;
            }
            return SkipUnsafe(handleId, "creating", IdleSinceLastBoundaryMs(), snapshot.reset, extent,
                              DlssdTranslatedSession::CreatingElapsedMs());
        }
    }

    if (commandList == nullptr)
        return SkipUnsafe(handleId, "missing_cmdlist", IdleSinceLastBoundaryMs(), snapshot.reset, extent, -1);

    const char* copyError = nullptr;
    if (!DlssdTranslatedSession::Evaluate(commandList, snapshot, nullptr, false, &copyError,
                                          DlssdRuntimeFrame_CopyOnCallerList))
    {
        if (copyError != nullptr && std::strcmp(copyError, "translated runtime is busy") == 0)
            return SkipUnsafe(handleId, "runtime_busy", IdleSinceLastBoundaryMs(), snapshot.reset, extent, -1);
        DlssdTranslatedSession::Release();
        gRuntimePoisoned.store(true, std::memory_order_relaxed);
        gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("DLSS-D experimental backend handle={} caller-list input copy failed: {}", handleId,
                 copyError != nullptr ? copyError : "unknown");
        LogCounters(handleId, "translated runtime input copy failed");
        FlushExperimentalLog();
        NoteEvaluateBoundary();
        return Decision::Rejected;
    }

    {
        std::lock_guard lock(gJobMutex);
        if (gHavePending)
            ReleaseJob(gPending);
        gPending = {};
        gPending.op = OwnerOp::Evaluate;
        gPending.snapshot = snapshot;
        AddRefJob(gPending, State::Instance().currentCommandQueue);
        gPending.handleId = handleId;
        gPending.extent = extent;
        gHavePending = true;
    }

    gLastDispatchAt = Clock::now();
    gHaveLastDispatch = true;
    LOG_INFO("DLSS-D experimental backend handle={} evaluate prepared publish=0 evaluated={} dispatched={} "
             "extent={} tid={}",
             handleId, gEvaluated.load(), gDispatched.load(), extent != nullptr ? extent->name : "none",
             GetCurrentThreadId());
    FlushExperimentalLog();
    NoteEvaluateBoundary();
    return Decision::Unpublished;
}
} // namespace

bool Enabled()
{
    const auto* config = Config::Instance();
    return config->FSRREnabled.value_or_default() && config->FSRRExperimentalDlssd.value_or_default();
}

Decision Evaluate(uint32_t handleId, const InputSnapshot& snapshot, ID3D12GraphicsCommandList* commandList,
                  NVSDK_NGX_Parameter* parameters)
{
    (void) parameters;
    if (!Enabled())
        return Decision::NotEnabled;

    LogContractOnce();
    gEvaluated.fetch_add(1, std::memory_order_relaxed);

    if (Config::Instance()->FSRRCaptureOnly.value_or_default())
    {
        gRejectedCaptureOnly.fetch_add(1, std::memory_order_relaxed);
        if (gRejectedCaptureOnly.load() == 1)
            LogCounters(handleId, "capture-only");
        return Decision::Rejected;
    }

    const auto* extent = FindAcceptedExtent(snapshot);
    if (extent == nullptr)
    {
        gRejectedExtent.fetch_add(1, std::memory_order_relaxed);
        if (gRejectedExtent.load() == 1)
        {
            LOG_WARN("DLSS-D experimental backend rejected unsupported extent {}x{} -> {}x{} "
                     "(accepted 1706x960 -> 2560x1440 or 1920x1080 -> 3840x2160)",
                     snapshot.renderWidth, snapshot.renderHeight, snapshot.outputWidth, snapshot.outputHeight);
            LogCounters(handleId, "unsupported extent");
        }
        return Decision::Rejected;
    }

    if (!HasRequiredInventory(snapshot))
    {
        gRejectedInventory.fetch_add(1, std::memory_order_relaxed);
        if (gRejectedInventory.load() == 1)
            LogCounters(handleId, "incomplete inventory");
        return Decision::Rejected;
    }

    if (AbortHungCreateIfTimedOut(handleId, extent))
        return Decision::Rejected;

    const long long idleMs = IdleSinceLastBoundaryMs();
    const auto key = MakeEvaluateKey(snapshot, commandList);

    ID3D12Device* device = nullptr;
    if (commandList != nullptr)
        commandList->GetDevice(IID_PPV_ARGS(&device));
    if (device != nullptr)
    {
        NoteVram(QueryLocalVramBytes(device));
        device->Release();
        device = nullptr;
    }

    if (idleMs >= kLoadIdleMs)
    {
        if (AttachInFlight())
            return SkipUnsafe(handleId, DlssdTranslatedSession::IsCreating() ? "creating" : "loading_prepared", idleMs,
                              snapshot.reset, extent,
                              DlssdTranslatedSession::IsCreating() ? DlssdTranslatedSession::CreatingElapsedMs()
                                                                   : kLoadMinMs);
        BeginLoadUnsafeWindow();
        return SkipUnsafe(handleId, "loading", idleMs, snapshot.reset, extent, kLoadMinMs);
    }

    if (idleMs >= kDropMappingsIdleMs)
    {
        if (AttachInFlight())
            return SkipUnsafe(handleId, DlssdTranslatedSession::IsCreating() ? "creating" : "idle_prepared", idleMs,
                              snapshot.reset, extent,
                              DlssdTranslatedSession::IsCreating() ? DlssdTranslatedSession::CreatingElapsedMs() : -1);
        DropTranslatedMappings();
        return SkipUnsafe(handleId, "idle", idleMs, snapshot.reset, extent, -1);
    }

    if (!gSawLoadIdle)
        return SkipUnsafe(handleId, "intro", idleMs, snapshot.reset, extent, -1);

    const long long loadRemainingMs = MsUntil(gLoadIdleAt, kLoadMinMs);
    if (loadRemainingMs > 0)
        return SkipUnsafe(handleId, "loading", idleMs, snapshot.reset, extent, loadRemainingMs);

    if (gHaveLastKey && key != gLastKey)
    {
        if (AttachInFlight())
        {
            gLastKey = key;
            gHaveLastKey = true;
            return SkipUnsafe(handleId, DlssdTranslatedSession::IsCreating() ? "creating" : "resources_creating", idleMs,
                              snapshot.reset, extent,
                              DlssdTranslatedSession::IsCreating() ? DlssdTranslatedSession::CreatingElapsedMs()
                                                                   : kSettleStableMs);
        }
        DropTranslatedMappings();
        gSettling = true;
        gSettleStarted = Clock::now();
        gInWorld = false;
        gLastKey = key;
        gHaveLastKey = true;
        return SkipUnsafe(handleId, "resources", idleMs, snapshot.reset, extent, kSettleStableMs);
    }

    if (snapshot.reset != 0)
    {
        if (AttachInFlight())
            return SkipUnsafe(handleId, "creating", idleMs, snapshot.reset, extent,
                              DlssdTranslatedSession::CreatingElapsedMs());
        if (gDispatched.load() != 0 || gHaveLastDispatch || DlssdTranslatedSession::IsAttached())
            DropTranslatedMappings();
        gSettling = true;
        gSettleStarted = Clock::now();
        gInWorld = false;
        return SkipUnsafe(handleId, "reset", idleMs, snapshot.reset, extent, kSettleStableMs);
    }

    if (!gSettling && !gHaveLastDispatch && !gInWorld && !AttachInFlight() &&
        !DlssdTranslatedSession::IsAttached())
    {
        gSettling = true;
        gSettleStarted = Clock::now();
        gLastKey = key;
        gHaveLastKey = true;
    }

    if (gSettling)
    {
        const long long settleRemainingMs = MsUntil(gSettleStarted, kSettleStableMs);
        if (settleRemainingMs > 0)
            return SkipUnsafe(handleId, "settle", idleMs, snapshot.reset, extent, settleRemainingMs);
        gSettling = false;
    }

    if (!gInWorld)
    {
        gInWorld = true;
        gInWorldAt = Clock::now();
    }

    const bool vramWindowClimbing =
        gVramQueryOk && gVramWindowMax > gVramWindowMin &&
        (gVramWindowMax - gVramWindowMin) >= kVramClimbBytes;
    if (vramWindowClimbing)
        return SkipUnsafe(handleId, "vram", idleMs, snapshot.reset, extent, kVramStableMs);

    const long long inWorldRemainingMs = MsUntil(gInWorldAt, kInWorldStableMs);
    if (inWorldRemainingMs > 0)
        return SkipUnsafe(handleId, "inworld", idleMs, snapshot.reset, extent, inWorldRemainingMs);

    if (DlssdTranslatedSession::IsCreating())
        return SkipUnsafe(handleId, "creating", idleMs, snapshot.reset, extent,
                          DlssdTranslatedSession::CreatingElapsedMs());

    if (gHaveLastDispatch)
    {
        const long long intervalRemainingMs = MsUntil(gLastDispatchAt, kExperimentalMinIntervalMs);
        if (intervalRemainingMs > 0)
            return SkipUnsafe(handleId, "interval", idleMs, snapshot.reset, extent, intervalRemainingMs);
    }

    if (AbortHungCreateIfTimedOut(handleId, extent))
    {
        NoteEvaluateBoundary();
        return Decision::Rejected;
    }

    if (gRuntimePoisoned.load(std::memory_order_relaxed))
    {
        gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
        NoteEvaluateBoundary();
        return Decision::Rejected;
    }

    if (commandList == nullptr || FAILED(commandList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
        if (gRejectedRuntime.load() == 1)
            LogCounters(handleId, "missing evaluate device");
        NoteEvaluateBoundary();
        return Decision::Rejected;
    }

    gLastKey = key;
    gHaveLastKey = true;
    const auto decision = RunPrepareCreateThenQueue(handleId, snapshot, device, commandList, extent);
    device->Release();
    return decision;
}

void NotifyFrameSubmitted()
{
    if (!Enabled())
        return;
    if (!DlssdTranslatedSession::IsAttached() || DlssdTranslatedSession::IsCreating())
        return;
    OwnerJob pending;
    {
        std::lock_guard lock(gJobMutex);
        if (!gHavePending || gShutdown)
            return;
        if (gHaveJob || gOwnerBusy.load(std::memory_order_acquire))
        {
            if (!gLoggedBusyDefer)
            {
                gLoggedBusyDefer = true;
                LOG_INFO("DLSS-D experimental backend kept unpublished HIP pending because the owner is busy");
                FlushExperimentalLog();
            }
            return;
        }
        pending = std::move(gPending);
        gHavePending = false;
    }
    if (!QueueUnpublishedEvaluate(pending.handleId, pending.snapshot, pending.extent, pending.queue))
    {
        std::lock_guard lock(gJobMutex);
        if (!gHavePending && !gShutdown)
        {
            gPending = std::move(pending);
            gHavePending = true;
            if (!gLoggedBusyDefer)
            {
                gLoggedBusyDefer = true;
                LOG_WARN("DLSS-D experimental backend requeued unpublished HIP after a busy owner");
                FlushExperimentalLog();
            }
        }
        else
            ReleaseJob(pending);
        return;
    }
    gLoggedBusyDefer = false;
    ReleaseJob(pending);
}

void Release(uint32_t handleId)
{
    if (!Enabled())
        return;
    StopOwner(DlssdTranslatedSession::IsCreating());
    DlssdTranslatedSession::Release();
    gHaveLastKey = false;
    gHaveLastDispatch = false;
    gSettling = false;
    gInWorld = false;
    gVramHistory.clear();
    gVramLastBytes = 0;
    gVramQueryOk = false;
    gVramSawClimb = false;
    ReleaseCachedVramAdapter();
    LOG_INFO("DLSS-D experimental backend released handle={} evaluated={} extent={} capture_only={} "
             "inventory={} runtime={} skipped={} dispatched={}",
             handleId, gEvaluated.load(), gRejectedExtent.load(), gRejectedCaptureOnly.load(),
             gRejectedInventory.load(), gRejectedRuntime.load(), gSkippedUnsafe.load(), gDispatched.load());
}
} // namespace DlssdExperimentalBackend
