#include "pch.h"
#include "DlssdFenceWait.h"

#include "DlssdExperimentalBackend.h"
#include "DlssdTranslatedSession.h"
#include "DlssdOutputHazardTrace.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <denoisers/RrInputRegistry.h>

#include "../../native/dlssd_translated_runtime.h"
#include "../../native/dlssd_frame_lease.h"
#include "../../native/dlssd_input_receipt.h"

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
#include <memory>
#include <string>
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
// Host-granularity owner-evaluate outcome inventory for backwards debugging:
// the sidecar only reports ok/fail plus an error string, so unique failure
// texts are counted here to point at descriptor/translation faults per run.
std::atomic<uint32_t> gOwnerOk { 0 };
std::atomic<uint32_t> gOwnerFailed { 0 };
std::atomic<uint32_t> gPublished { 0 };
// Starvation diagnostics (2026-09-12): why leases never reach Submitted.
// Counters only; call sites throttle the log lines. No behavior change.
std::atomic<uint32_t> gDeferredResetApplied { 0 };
std::atomic<uint32_t> gDeferredResetIgnoredSelf { 0 };
std::atomic<uint32_t> gResetCancels { 0 };
std::atomic<uint32_t> gSubmitMismatches { 0 };
std::atomic<uint32_t> gSubmitMatches { 0 };
std::atomic<uint32_t> gAfterSubmitFails { 0 };
std::atomic<uint32_t> gNativeMatches { 0 };
// SL proxy resolution for submission matching (2026-09-13): sl.dlss_d
// forwards the NATIVE list (common::getNativeCommandBuffer) to NGX while the
// game may submit the SL proxy, so raw pointer comparison can never hit.
// slGetNativeInterface (sl.interposer.dll, public export) maps proxy ->
// native and passes natives through. Loaded without side effects: SL is
// already in-process; GetModuleHandle never loads anything new.
using SlGetNativeInterfaceFn = int (*)(void* proxyInterface, void** baseInterface);
SlGetNativeInterfaceFn gSlGetNativeInterface = nullptr;
std::once_flag gSlResolverInit;
void EnsureSlResolver()
{
    std::call_once(gSlResolverInit,
                   []
                   {
                       if (const HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll"))
                       {
                           gSlGetNativeInterface = reinterpret_cast<SlGetNativeInterfaceFn>(
                               GetProcAddress(interposer, "slGetNativeInterface"));
                       }
                   });
}
// Compare-only native identity. Drops the AddRef the resolver grants; the
// game owns both lifetimes across the Execute call. Falls back to the raw
// pointer when the resolver is unavailable or rejects the object. Optionally
// reports whether the object actually resolved (vs pass-through/fallback).
void* ResolveNativeForMatch(void* list, bool* resolved = nullptr)
{
    EnsureSlResolver();
    if (resolved != nullptr)
        *resolved = false;
    if (gSlGetNativeInterface == nullptr || list == nullptr)
        return list;
    void* base = nullptr;
    if (gSlGetNativeInterface(list, &base) != 0 || base == nullptr)
        return list;
    static_cast<IUnknown*>(base)->Release();
    if (resolved != nullptr)
        *resolved = true;
    return base;
}
// Late-submission probe (2026-09-13): the NGX-time evaluate list never
// appears in Execute batches while its lease is pending. Keep recent
// producers so a submission 1+ frames later still attributes. Caller holds
// gJobMutex for both push and scan.
struct RecentProducer
{
    void* raw = nullptr;
    void* native = nullptr;
    uint64_t receipt = 0;
};
constexpr size_t kRecentProducers = 8;
std::array<RecentProducer, kRecentProducers> gRecentProducers {};
size_t gRecentProducerNext = 0;
void RememberProducerLocked(void* raw, void* native, uint64_t receipt)
{
    gRecentProducers[gRecentProducerNext] = RecentProducer { raw, native, receipt };
    gRecentProducerNext = (gRecentProducerNext + 1) % kRecentProducers;
}
// Returns true on a ring hit; age is currentReceipt - hitReceipt in leases.
bool FindProducerLocked(void* native, uint64_t currentReceipt, uint64_t& hitReceipt, uint64_t& age)
{
    for (const auto& entry : gRecentProducers)
    {
        if (entry.receipt != 0 && (entry.native == native || entry.raw == native))
        {
            hitReceipt = entry.receipt;
            age = currentReceipt >= entry.receipt ? currentReceipt - entry.receipt : 0;
            return true;
        }
    }
    return false;
}
std::atomic<uint32_t> gLateHits { 0 };
std::mutex gFailInventoryMutex;
struct FailEntry
{
    std::string text;
    uint32_t count = 0;
};
std::array<FailEntry, 4> gFailInventory {};
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
// Post-attach quiesce (2026-09-07): NGX CreateFeature returned after 81.6 s,
// but the owner kept servicing seq=39 allocations + copy-ins concurrently
// with game evaluates and the process AV'd on a recycled 0x26BF... list
// address. After attach, stay unpublished with no caller-list copies and no
// owner evaluates until this window elapses. FSR-RR stays presented.
Clock::time_point gAttachReadyAt {};
bool gHaveAttachReady = false;
constexpr long long kPostAttachQuiesceMs = 120000;

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
// Measured 2026-09-07: cold isolated ZLUDA module compile for the 1706
// contract is ~70 s with zero progress reuse between processes. The old 8 s
// bound therefore killed every game create mid-compile. Abandon only at a
// generous cap or when heartbeat progress stalls (see below).
constexpr long long kCreateCapMs = 240000;
constexpr long long kCreateStallMs = 45000;
constexpr long long kVramStableMs = 10000;
constexpr uint64_t kVramClimbBytes = 256ull * 1024ull * 1024ull;
constexpr long long kSettleStableMs = 10000;
constexpr long long kInWorldStableMs = 60000;
constexpr long long kExperimentalMinIntervalMs = 2000;
constexpr uint32_t kSkipLogEvery = 60;
// Create-hang probe (2026-09-07): skip the unvalidated settle/vram/inworld
// stability heuristics for one run to isolate the hang from gating.
// Load-window + intro/resource/reset/idle safety stays. Reversible by setting
// this back to false. When true, VRAM is still sampled (throttled, log only)
// but never gates attach.
constexpr bool kProbeMinimalGates = true;
constexpr long long kVramQueryMinIntervalMs = 500;

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
Clock::time_point gLastVramQueryAt {};
bool gHaveVramQueryAt = false;
const char* gLastSkipReason = "";
uint32_t gLastSkipReasonCount = 0;

enum class OwnerOp
{
    FinishAttach,
    Evaluate,
};

// One allocation set is shared by caller-list staging and the HIP owner.
// Keep the complete lease, not merely resource pointers, until both APIs
// finish and the recorded game list can no longer replay the staging copy.
struct FrameLease
{
    DlssdFrameTag tag;
    DlssdInputReceipt::Receipt input;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> producer;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Resource> ready;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> publicationList;
    Microsoft::WRL::ComPtr<ID3D12Fence> publicationFence;
    uint32_t outputX = 0, outputY = 0, width = 0, height = 0;
    uint32_t readyState = 0;
    bool copyFailed = false, ownerDone = false, ownerOk = false;
    bool split = false, cancelled = false, published = false, publicationSignalled = false;
    bool handoffActive = false;
    bool publicationCounted = false;
};
// The single outstanding lease must survive proxy unload if GPU retirement
// was never proven. Normal retirement resets it and frees all owned objects.
auto& gFrameLease = *new std::shared_ptr<FrameLease>();
std::atomic<ID3D12GraphicsCommandList*> gReceiptProducer { nullptr };
// Reset observed by hkReset while gJobMutex was unavailable (2026-09-12
// EDEADLK: Record -> re-entrant Reset on the producer under stagingLock).
// Packed pointer | self bit in one atomic so the drainer can tell same-thread
// re-entrancy from cross-thread pool reuse: bit0 is free (COM pointers are at
// least 8-byte aligned). 0 means none pending. Never dereferenced without
// validation against the retained producer under lock. Hook callbacks must
// never block on gJobMutex.
std::atomic<uint64_t> gDeferredReset { 0 };
// Re-entrancy probe: nonzero while this thread holds the lease staging lock.
// Lets Notify attribute a deferred reset to self vs another thread.
thread_local int tStagingDepth = 0;
uint64_t gNextReceipt = 0;
uint64_t gPreviousOwnerFrame = 0;
uint32_t gPreviousOwnerHandle = 0;
std::atomic<long long> gLastMatchedSubmitEnd { 0 };

struct OwnerJob
{
    OwnerOp op = OwnerOp::Evaluate;
    InputSnapshot snapshot;
    uint32_t handleId = 0;
    const AcceptedExtent* extent = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    std::shared_ptr<FrameLease> lease;
};

std::mutex gJobMutex;
std::condition_variable gJobCv;
std::mutex gThreadMutex;
std::thread gOwner;
bool gHaveJob = false;
std::atomic<bool> gOwnerBusy { false };
// Create-hang probe (2026-09-07): the owner may be blocked inside proprietary
// NGX CreateFeature. Terminating that thread corrupts driver/C++ locks held
// at the kill point and orphans the CaptureNgxDescriptorsGuard. Instead we
// detach-and-leak the hung owner once, poison the path, and never start a
// second owner that would compete for ZLUDA TLS and device hooks.
std::atomic<bool> gOwnerHungLeaked { false };
std::atomic<bool> gOwnerAlive { false };
std::atomic<bool> gShutdown { false };
OwnerJob gJob;
OwnerJob gPending;
bool gHavePending = false;
bool gReleaseAfterOwner = false;
ID3D12Fence* gWaitFence = nullptr;
HANDLE gWaitEvent = nullptr;
UINT64 gWaitValue = 0;
// Create-watchdog progress: last observed heartbeat counters and when they
// last advanced. Abandon fires on cap expiry or on a stall (no advance).
// Sidecar queries are throttled: the owner may be blocked inside proprietary
// CreateFeature while the game thread evaluates, so per-evaluate cross-module
// stats calls are avoided and the quit path only reads this cache.
uint32_t gLastCreateAlloc = 0;
uint32_t gLastCreateFuncs = 0;
bool gLastCreateHaveProgress = false;
Clock::time_point gLastCreateProgressAt {};
Clock::time_point gLastCreateQueryAt {};
bool gHaveCreateProgressSample = false;
constexpr long long kCreateProgressQueryMs = 500;

void AddRefSnapshot(InputSnapshot& snapshot);
void ReleaseSnapshot(InputSnapshot& snapshot);
void AddRefJob(OwnerJob& job, ID3D12CommandQueue* queue);
void ReleaseJob(OwnerJob& job);
void EnsureOwner();
void StopOwner(bool abortIfCreating);
bool QueueFinishAttach(uint32_t handleId);
bool AbortHungCreateIfTimedOut(uint32_t handleId, const AcceptedExtent* extent);
long long MsUntil(const Clock::time_point& start, long long durationMs);
long long PostAttachQuiesceRemainingMs();

void FlushExperimentalLog()
{
    if (auto logger = spdlog::default_logger())
        logger->flush();
}

// Parked by default: the sidecar's first-Evaluate lazy init (seq-39 allocs +
// copy-ins) AV'd twice on game-heap addresses, so no owner work runs until
// explicitly opted in. Attach + quiesce + FSR-RR presentation are unaffected.
bool OwnerEvaluatesAllowed()
{
    return Config::Instance()->FSRRDlssdOwnerEvaluates.value_or_default();
}

void LogPublicationContract()
{
    if (!Config::Instance()->FSRRDlssdPresentTranslated.value_or_default())
        return;
    static std::once_flag logged;
    std::call_once(logged, []
                   {
                       LOG_WARN("DLSS-D experimental backend matching-frame publication requested: only a verified "
                                "producer/suffix boundary may publish; unknown boundaries retain FSR-RR. "
                                "The queue submission may wait for the translated owner result.");
                   });
}

bool PostAttachParked()
{
    return DlssdTranslatedSession::IsAttached() && !OwnerEvaluatesAllowed();
}

void NoteOwnerFailure(const char* error)
{
    const uint32_t total = gOwnerFailed.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string text = (error != nullptr && error[0] != '\0') ? error : "unknown";
    if (text.size() > 160)
        text.resize(160);
    std::string inventory;
    {
        std::lock_guard lock(gFailInventoryMutex);
        bool found = false;
        for (auto& entry : gFailInventory)
        {
            if (entry.count > 0 && entry.text == text)
            {
                entry.count++;
                found = true;
                break;
            }
        }
        if (!found)
        {
            for (size_t i = gFailInventory.size() - 1; i > 0; --i)
                gFailInventory[i] = std::move(gFailInventory[i - 1]);
            gFailInventory[0] = FailEntry { text, 1 };
        }
        for (const auto& entry : gFailInventory)
        {
            if (entry.count == 0)
                continue;
            if (!inventory.empty())
                inventory += " | ";
            inventory += std::to_string(entry.count) + "x " + entry.text;
        }
    }
    LOG_WARN("DLSS-D experimental backend owner evaluate failed fail_total={} fails=[{}]", total, inventory);
    FlushExperimentalLog();
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
                                  "10 s resource settle and 60 s in-world. Create runs up to 240 s while "
                                  "heartbeat progresses and fails closed on a 45 s stall. "
                                  "After attach, a 120 s post-attach quiesce keeps FSR-RR with no "
                                  "caller-list copies and no owner evaluates while the sidecar settles. "
                                  "Owner evaluates are parked by default ([FSRR] DlssdOwnerEvaluates) "
                                  "after two post-attach AVs in the sidecar lazy init; the present "
                                  "toggle ([FSRR] DlssdPresentTranslated, default off) remains gated "
                                  "until caller-list output handoff is implemented. "
                                  "Failures keep FSR-RR. Probe 2026-09-07: minimal gates active, "
                                 "settle/vram/inworld stability skipped, VRAM log-only at 2 Hz, hung "
                                 "owner detached without TerminateThread.");
                   });
}

void LogCounters(uint32_t handleId, const char* reason)
{
    LOG_WARN("DLSS-D experimental backend handle={} {}: evaluated={} extent={} capture_only={} "
             "inventory={} runtime={} skipped={} dispatched={} owner_ok={} owner_fail={} published={}",
             handleId, reason, gEvaluated.load(), gRejectedExtent.load(), gRejectedCaptureOnly.load(),
             gRejectedInventory.load(), gRejectedRuntime.load(), gSkippedUnsafe.load(), gDispatched.load(),
             gOwnerOk.load(), gOwnerFailed.load(), gPublished.load());
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
    const auto submitEnd = Clock::time_point(Clock::duration(gLastMatchedSubmitEnd.load(std::memory_order_acquire)));
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
        (std::max)(gLastEvaluateExit, submitEnd)).count();
}

void NoteEvaluateBoundary()
{
    gLastEvaluateExit = Clock::now();
    gHaveLastEvaluateExit = true;
}

// Caller holds gJobMutex. No GPU wait or blocking session lock is permitted.
bool RetireFrameLeaseLocked()
{
    if (!gFrameLease)
        return true;
    auto& lease = *gFrameLease;
    if (lease.handoffActive)
        return false;
    if (lease.published && lease.publicationSignalled && lease.publicationFence && !lease.publicationCounted)
    {
        const auto completed = lease.publicationFence->GetCompletedValue();
        if (completed != UINT64_MAX && completed >= 1)
        {
            lease.publicationCounted = true;
            gPublished.fetch_add(1);
            LOG_INFO("DLSS-D frame receipt={} source_frame={} publication_complete=1",
                     lease.tag.receiptId, lease.tag.sourceFrame);
        }
    }
    if (lease.input.State() == DlssdInputReceipt::Receipt::Phase::Submitted)
        lease.input.WaitAndVerify(0);
    const bool cancelled = lease.input.State() == DlssdInputReceipt::Receipt::Phase::Cancelled;
    if (!lease.input.CanRetire() || (!cancelled && !lease.ownerDone))
        return false;
    if (!cancelled && !lease.tag.inputSubmissionVerified)
    {
        auto verified = lease.tag;
        verified.inputSubmissionVerified = 1;
        const char* verifyError = nullptr;
        if (!DlssdTranslatedSession::VerifyFrameInput(verified, &verifyError))
            return false;
        lease.tag = verified;
    }
    if (lease.published)
    {
        if (!lease.publicationSignalled || !lease.publicationFence)
            return false;
        const auto completed = lease.publicationFence->GetCompletedValue();
        if (completed == UINT64_MAX || completed < 1)
            return false;
    }
    const uint32_t retirement = (cancelled ? DlssdLease_InputCancelled : DlssdLease_InputCompleted) |
        (lease.published ? DlssdLease_OutputCompleted : DlssdLease_OutputNotPublished);
    const char* error = nullptr;
    if (!DlssdTranslatedSession::EndFrameLease(lease.tag.receiptId, retirement, &error))
        return false;
    LOG_INFO("DLSS-D frame receipt={} source_frame={} retired=1 published={}",
             lease.tag.receiptId, lease.tag.sourceFrame, lease.published ? 1 : 0);
    gReceiptProducer.store(nullptr, std::memory_order_release);
    gFrameLease.reset();
    return true;
}

// Caller holds gJobMutex. Applies one producer reset: cancel a recorded-but-
// unsubmitted receipt, drop its pending job, and retire when possible.
void ApplyProducerResetLocked(ID3D12GraphicsCommandList* list)
{
    if (!gFrameLease || list != gFrameLease->producer.Get())
        return;
    const auto before = gFrameLease->input.State();
    const bool hadPending = gHavePending;
    const uint64_t receipt = gFrameLease->tag.receiptId;
    gFrameLease->input.OnSuccessfulReset(list);
    const bool cancelledNow =
        gFrameLease->input.State() == DlssdInputReceipt::Receipt::Phase::Cancelled;
    if (cancelledNow && gHavePending)
    {
        ReleaseJob(gPending);
        gPending = {};
        gHavePending = false;
    }
    RetireFrameLeaseLocked();
    // Cancel attribution: a Recorded receipt dropped with a live pending job
    // is the starvation path (never reaches BeforeSubmit as Recorded). Read
    // before retire, which may release the lease.
    if (hadPending && before == DlssdInputReceipt::Receipt::Phase::Recorded && cancelledNow)
    {
        const auto total = gResetCancels.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 2 || (total % 120) == 0)
        {
            LOG_WARN("DLSS-D frame receipt={} cancelled by producer Reset total={} producer={:p} tid={}",
                     receipt, total, (void*) list, GetCurrentThreadId());
            FlushExperimentalLog();
        }
    }
}

// Caller holds gJobMutex. Applies a reset deferred by NotifyCommandListReset
// when the lock was unavailable. Observations for an already-retired producer
// are dropped by the producer check above.
void DrainDeferredResetLocked()
{
    const uint64_t packed = gDeferredReset.exchange(0, std::memory_order_acq_rel);
    if (packed == 0)
        return;
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(packed & ~uint64_t { 1 });
    const bool self = (packed & uint64_t { 1 }) != 0;
    const bool live = gFrameLease && list == gFrameLease->producer.Get();
    const uint64_t receipt = live ? gFrameLease->tag.receiptId : 0;
    ApplyProducerResetLocked(list);
    if (live)
    {
        const auto total = gDeferredResetApplied.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 3 || (total % 120) == 0)
        {
            LOG_INFO("DLSS-D deferred producer reset applied receipt={} total={} self={} tid={}", receipt, total,
                     self ? 1 : 0, GetCurrentThreadId());
            FlushExperimentalLog();
        }
    }
}

void DropTranslatedMappings()
{
    bool releaseNow = true;
    {
        std::unique_lock lock(gJobMutex);
        DrainDeferredResetLocked();
        if (!RetireFrameLeaseLocked())
        {
            // A CPU timeout or feature reset cannot revoke recorded GPU work.
            // Keep the runtime allocation set alive until its receipts retire.
            if (gFrameLease)
                gFrameLease->cancelled = true;
            LOG_WARN("DLSS-D mapping drop deferred: unretired frame receipt");
            return;
        }
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
    gHaveAttachReady = false;
    gSettling = false;
    gInWorld = false;
    DlssdOutputHazardTrace::InvalidateAll();
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

// Post-attach quiesce: milliseconds remaining before owner evaluates and
// caller-list copies are allowed again. Zero when attach is not ready.
long long PostAttachQuiesceRemainingMs()
{
    if (!gHaveAttachReady || !DlssdTranslatedSession::IsAttached())
        return 0;
    return MsUntil(gAttachReadyAt, kPostAttachQuiesceMs);
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

void OwnerLoop()
{
    // Narrow bypass: only the CUDA bind and the blocking NGX create/evaluate
    // calls need attach-load handling. Holding bypass for the whole thread
    // lifetime masks game-thread loads and was only hiding bugs.
    // FinishAttachOnOwnerThread already scopes its own bypass around the
    // proprietary CreateFeature call below.
    {
        DlssdTranslatedSession::ScopedAttachLoadBypass bindBypass;
        DlssdTranslatedSession::BindCudaOnThisThread();
    }
    // Liveness for teardown diagnostics: a lingering process with this false
    // means the hung owner returned late (or never started), so the sidecar
    // release path may proceed without touching a blocked thread.
    struct AliveGuard
    {
        ~AliveGuard() { gOwnerAlive.store(false, std::memory_order_release); }
    } aliveGuard;
    gOwnerAlive.store(true, std::memory_order_release);
    LOG_INFO("DLSS-D experimental backend NGX owner tid={}", GetCurrentThreadId());
    FlushExperimentalLog();
    for (;;)
    {
        OwnerJob job;
        {
            std::unique_lock lock(gJobMutex);
            gJobCv.wait(lock, [] { return gShutdown || gHaveJob; });
            if (gShutdown && !gHaveJob)
            {
                // NGX/CUDA teardown belongs to the context owner, just like
                // Init/Create/Evaluate. Never hold the job mutex during drain.
                RetireFrameLeaseLocked();
                lock.unlock();
                DlssdTranslatedSession::Release();
                return;
            }
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
                gAttachReadyAt = Clock::now();
                gHaveAttachReady = true;
                LOG_INFO("DLSS-D experimental backend handle={} finish attach ok ms={} tid={} "
                         "post_attach_quiesce_ms={} parked={}",
                         job.handleId, elapsedMs, GetCurrentThreadId(), kPostAttachQuiesceMs,
                         OwnerEvaluatesAllowed() ? 0 : 1);
                FlushExperimentalLog();
            }
        }
        else
        {
            const char* error = nullptr;
            const auto enter = Clock::now();
            bool ok = false;
            if (job.lease)
            {
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                do
                {
                    {
                        std::lock_guard lock(gJobMutex);
                        auto& lease = *job.lease;
                        if (lease.input.State() == DlssdInputReceipt::Receipt::Phase::Submitted)
                            lease.input.WaitAndVerify(0);
                        ok = lease.input.State() == DlssdInputReceipt::Receipt::Phase::Complete;
                        if (ok || lease.cancelled)
                            break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (Clock::now() < deadline);
                if (ok)
                {
                    std::lock_guard lock(gJobMutex);
                    auto verified = job.lease->tag;
                    verified.inputSubmissionVerified = 1;
                    ok = DlssdTranslatedSession::VerifyFrameInput(verified, &error);
                    if (ok)
                        job.lease->tag = verified;
                    ok = ok && !job.lease->copyFailed && !job.lease->cancelled;
                }
            }
            const bool waitFailed = !ok;
            // The queue wait only orders earlier input copies. It does not
            // grant the owner exclusive access to a live game output or order
            // its consumers. Keep validated output private until a completion
            // handoff can record publication on the current NGX caller list.
            if (!ok)
            {
                if (error == nullptr)
                    error = "input receipt incomplete, cancelled or caller copy failed";
            }
            else
            {
                // Sparse owner samples are separated by many game frames.
                // Their motion describes adjacent game frames, not the prior
                // private sample. Reset only this diagnostic job's history.
                const auto requestedReset = job.snapshot.reset;
                const bool contiguous = job.lease && job.lease->split &&
                    gPreviousOwnerHandle == job.handleId &&
                    gPreviousOwnerFrame + 1 == job.snapshot.frameIndex;
                job.snapshot.reset = contiguous ? requestedReset : 1;
                LOG_INFO("DLSS-D experimental backend handle={} owner evaluate source_ngx_frame={} reset_requested={} reset_effective={} "
                         "reason=receipt_history_continuity",
                         job.handleId, job.snapshot.frameIndex, requestedReset, job.snapshot.reset);
                FlushExperimentalLog();
                ok = DlssdTranslatedSession::Evaluate(nullptr, job.snapshot, nullptr, false, &error,
                                                      DlssdRuntimeFrame_SkipInputCopy);
                gPreviousOwnerFrame = ok ? job.snapshot.frameIndex : 0;
                gPreviousOwnerHandle = ok ? job.handleId : 0;
            }
            if (job.lease)
            {
                std::lock_guard lock(gJobMutex);
                auto& lease = *job.lease;
                if (ok && lease.split && !lease.cancelled)
                {
                    DlssdReadyOutput ready;
                    ready.receiptId = lease.tag.receiptId;
                    ready.sourceFrame = lease.tag.sourceFrame;
                    ready.handleId = lease.tag.handleId;
                    ok = DlssdTranslatedSession::GetReadyOutput(ready, &error);
                    if (ok)
                    {
                        lease.ready.Attach(static_cast<ID3D12Resource*>(ready.resource));
                        lease.readyState = ready.state;
                    }
                }
                lease.ownerOk = ok;
                lease.ownerDone = true;
                gJobCv.notify_all();
            }
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - enter).count();
            ReleaseJob(job);

            // A busy session is a skip, never a poison: with try-lock on both
            // paths the owner can lose to a game thread inside prepare/release.
            const bool busyFailed =
                !ok && !waitFailed && error != nullptr && std::strcmp(error, "translated runtime is busy") == 0;
            if (!ok && (waitFailed || busyFailed))
            {
                gSkippedUnsafe.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN("DLSS-D experimental backend handle={} owner skipped after {} ms: {}", job.handleId, elapsedMs,
                         error);
                FlushExperimentalLog();
            }
            else if (!ok)
            {
                NoteOwnerFailure(error);
                // A failed leased evaluate still owns recorded GPU inputs.
                // Retire on Reset/completion; final release remains guarded
                // by the sidecar's active-lease check if proof never arrives.
                if (!job.lease)
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
                gOwnerOk.fetch_add(1, std::memory_order_relaxed);
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
    if (gOwnerHungLeaked.load(std::memory_order_acquire))
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
            // Do not TerminateThread inside NVNGX/ZLUDA/HIP driver code.
            // Detach-and-leak keeps the hung thread's locks owned instead of
            // orphaning them mid-call. The path is poisoned so no second
            // owner or new create is attempted afterwards.
            gOwner.detach();
            gOwnerHungLeaked.store(true, std::memory_order_release);
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
            // Do not mutate another thread's job ownership without its lock.
            // Abandonment retains the runtime; the late owner sees shutdown.
            gShutdown = true;
            gJobCv.notify_all();
        }
        const auto abandonElapsedMs = DlssdTranslatedSession::CreatingElapsedMs();
        DlssdTranslatedSession::AbandonHungCreate();
        ReleaseWaitFence();
        LOG_WARN("DLSS-D experimental backend detached the hung NGX owner thread without terminating it; "
                 "path is poisoned and no second owner will start");
        LOG_WARN("DLSS-D experimental backend abandoned create after {} ms (alloc={} funcs={} progress_api={}); "
                 "the hung proprietary create may still be inside driver code at quit, so a lingering game process "
                 "after clean exit must be ended via Task Manager",
                 abandonElapsedMs, gLastCreateAlloc, gLastCreateFuncs,
                 gLastCreateHaveProgress ? 1 : 0);
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
    // Baseline the create-watchdog stall clock at queue time so a slow but
    // progressing proprietary compile is never measured from the epoch.
    gLastCreateProgressAt = Clock::now();
    gHaveCreateProgressSample = false;
    gJobCv.notify_one();
    return true;
}

bool AbortHungCreateIfTimedOut(uint32_t handleId, const AcceptedExtent* extent)
{
    if (!DlssdTranslatedSession::IsCreating())
    {
        gHaveCreateProgressSample = false;
        return false;
    }
    const auto elapsedMs = DlssdTranslatedSession::CreatingElapsedMs();
    const auto now = Clock::now();
    // Throttle sidecar progress queries: with the instrumented runtime each
    // query crosses into the bridge stats path while the owner is blocked
    // inside proprietary CreateFeature. Stall math runs on the cache.
    const bool queryDue =
        !gHaveCreateProgressSample ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - gLastCreateQueryAt).count() >=
            kCreateProgressQueryMs;
    if (queryDue)
    {
        uint32_t alloc = 0, funcs = 0;
        gLastCreateHaveProgress = DlssdTranslatedSession::CreateProgress(&alloc, &funcs);
        gLastCreateQueryAt = now;
        if (!gHaveCreateProgressSample || alloc != gLastCreateAlloc || funcs != gLastCreateFuncs)
        {
            if (gHaveCreateProgressSample)
            {
                LOG_INFO("DLSS-D experimental backend handle={} create progress alloc={} funcs={} elapsed_ms={} "
                         "extent={} tid={}",
                         handleId, alloc, funcs, elapsedMs, extent != nullptr ? extent->name : "none",
                         GetCurrentThreadId());
                FlushExperimentalLog();
            }
            gLastCreateAlloc = alloc;
            gLastCreateFuncs = funcs;
            gLastCreateProgressAt = now;
            gHaveCreateProgressSample = true;
        }
    }
    const uint32_t alloc = gLastCreateAlloc;
    const uint32_t funcs = gLastCreateFuncs;
    const bool haveProgress = gLastCreateHaveProgress;
    // Without a first heartbeat sample there is no stall signal yet; report
    // zero instead of measuring from the epoch.
    const long long stalledMs =
        haveProgress
            ? std::chrono::duration_cast<std::chrono::milliseconds>(now - gLastCreateProgressAt).count()
            : 0;
    const bool stalled = haveProgress && stalledMs >= kCreateStallMs;
    if (elapsedMs < kCreateCapMs && !stalled)
        return false;
    LOG_WARN("DLSS-D experimental backend handle={} create {} after {} ms (stall_ms={} alloc={} funcs={} "
             "progress_api={}) extent={}",
             handleId, stalled ? "stalled" : "timed out", elapsedMs, stalledMs, alloc, funcs,
             haveProgress ? 1 : 0, extent != nullptr ? extent->name : "none");
    LOG_WARN("DLSS-D experimental backend rendezvous stays paused while the detached owner is blocked; expect "
             "DLSS-D create heartbeat lines above while the sidecar runtime still compiles");
    FlushExperimentalLog();
    gRuntimePoisoned.store(true, std::memory_order_relaxed);
    gRejectedRuntime.fetch_add(1, std::memory_order_relaxed);
    StopOwner(true);
    LogCounters(handleId, "translated runtime create timed out");
    FlushExperimentalLog();
    return true;
}

// RAII holder for the lease staging section. Same early-unlock interface as
// the unique_lock it replaces; keeps tStagingDepth balanced on all paths,
// including exceptions escaping the sidecar calls below.
struct StagingLock
{
    std::unique_lock<std::mutex> lock;
    StagingLock()
        : lock(gJobMutex)
    {
        ++tStagingDepth;
    }
    void unlock()
    {
        if (lock.owns_lock())
        {
            lock.unlock();
            --tStagingDepth;
        }
    }
    ~StagingLock()
    {
        if (lock.owns_lock())
            --tStagingDepth;
    }
};

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

    // Parked by default ([FSRR] DlssdOwnerEvaluates): never reach the quiesce
    // window, caller-list copies, or owner work while not opted in.
    if (PostAttachParked())
        return SkipUnsafe(handleId, "post_attach_parked", IdleSinceLastBoundaryMs(), snapshot.reset, extent, -2);

    // Post-attach quiesce: no caller-list copies and no owner evaluates until
    // the window elapses. The sidecar otherwise keeps allocating (seq=39) and
    // copying in concurrently with game evaluates.
    if (const long long quiesceMs = PostAttachQuiesceRemainingMs(); quiesceMs > 0)
        return SkipUnsafe(handleId, "post_attach_quiesce", IdleSinceLastBoundaryMs(), snapshot.reset, extent,
                          quiesceMs);

    // One staging set is shared with the owner. Reserve it before recording
    // any GPU copy: the session try-lock alone does not protect a pending job
    // whose caller list has not been submitted yet. Never replace that job.
    if (!DlssdOutputHazardTrace::ReceiptsAvailable())
        return SkipUnsafe(handleId, "receipt_hooks_unavailable", IdleSinceLastBoundaryMs(), snapshot.reset, extent, -1);
    StagingLock stagingLock;
    DrainDeferredResetLocked();
    if (gShutdown || gHavePending || gHaveJob || gOwnerBusy.load(std::memory_order_acquire) ||
        !RetireFrameLeaseLocked())
    {
        stagingLock.unlock();
        return SkipUnsafe(handleId, "staging_in_use", IdleSinceLastBoundaryMs(), snapshot.reset, extent, -1);
    }
    auto lease = std::make_shared<FrameLease>();
    lease->tag.receiptId = ++gNextReceipt;
    lease->tag.sourceFrame = snapshot.frameIndex;
    lease->tag.handleId = handleId;
    lease->tag.resetRequested = snapshot.reset;
    lease->width = snapshot.outputWidth;
    lease->height = snapshot.outputHeight;
    lease->producer = commandList;
    // Normalize the receipt identity to the interface ExecuteCommandLists sees.
    IUnknown* realList = nullptr;
    if (Util::CheckForRealObject("DlssdFrameReceipt", commandList, &realList) && realList)
    {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> normalized;
        if (SUCCEEDED(realList->QueryInterface(IID_PPV_ARGS(&normalized))))
            lease->producer = normalized;
    }
    // Starvation diagnosis: whether the recorded identity differs from the
    // Evaluate caller determines if BeforeSubmit can ever match.
    const bool producerNormalized = lease->producer.Get() != commandList;
    Microsoft::WRL::ComPtr<ID3D12Device> receiptDevice;
    const char* copyError = nullptr;
    if (lease->producer->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(lease->producer->GetDevice(IID_PPV_ARGS(&receiptDevice))) ||
        !lease->input.Initialize(receiptDevice.Get(), lease->tag.receiptId) ||
        !DlssdTranslatedSession::BeginFrameLease(lease->tag, &copyError))
    {
        stagingLock.unlock();
        return SkipUnsafe(handleId, "receipt_begin_failed", IdleSinceLastBoundaryMs(), snapshot.reset, extent, -1);
    }
    for (const auto& input : snapshot.resources)
    {
        if (input.resource)
            lease->resources.emplace_back(input.resource);
        if (input.definition && input.definition->semantic == InputSemantic::Output)
        {
            lease->output = input.resource;
            lease->outputX = input.subrectBaseX;
            lease->outputY = input.subrectBaseY;
        }
    }
    gFrameLease = lease;
    gReceiptProducer.store(lease->producer.Get(), std::memory_order_release);
    RememberProducerLocked(lease->producer.Get(), ResolveNativeForMatch(lease->producer.Get()),
                           lease->tag.receiptId);
    const bool copied = DlssdTranslatedSession::Evaluate(commandList, snapshot, nullptr, false, &copyError,
                                                         DlssdRuntimeFrame_CopyOnCallerList);
    // Append the receipt even after a partial copy failure: those recorded
    // references still require submission completion or successful list Reset.
    lease->copyFailed = !copied;
    if (!copied)
        LOG_WARN("DLSS-D frame receipt={} source_frame={} input_record_failed=1 reason={}",
                 lease->tag.receiptId, lease->tag.sourceFrame, copyError != nullptr ? copyError : "unknown");
    if (!lease->input.Record(lease->producer.Get()))
    {
        lease->cancelled = true;
        gRuntimePoisoned.store(true, std::memory_order_relaxed);
        stagingLock.unlock();
        LOG_ERROR("DLSS-D frame receipt record failed; retaining staging and HIP allocations");
        return Decision::Rejected;
    }

    {
        gPending = {};
        gPending.op = OwnerOp::Evaluate;
        gPending.snapshot = snapshot;
        AddRefJob(gPending, State::Instance().currentCommandQueue);
        gPending.handleId = handleId;
        gPending.extent = extent;
        gPending.lease = lease;
        gHavePending = true;
    }
    stagingLock.unlock();

    gLastDispatchAt = Clock::now();
    gHaveLastDispatch = true;
    void* const nativeProducer = ResolveNativeForMatch(lease->producer.Get());
    LOG_INFO("DLSS-D experimental backend handle={} evaluate prepared publish=0 evaluated={} dispatched={} "
             "extent={} tid={} producer={:p} normalized={} nativeProducer={:p}",
             handleId, gEvaluated.load(), gDispatched.load(), extent != nullptr ? extent->name : "none",
             GetCurrentThreadId(), (void*) lease->producer.Get(), producerNormalized ? 1 : 0, nativeProducer);
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
    LogPublicationContract();
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
        // Throttle VRAM queries off the render thread: factory creation plus
        // adapter enumeration every evaluate hitches. 2 Hz sampling is enough
        // for the log-only probe signal.
        const auto now = Clock::now();
        const bool dueForQuery =
            !gHaveVramQueryAt ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - gLastVramQueryAt).count() >=
                kVramQueryMinIntervalMs;
        if (dueForQuery)
        {
            NoteVram(QueryLocalVramBytes(device));
            gLastVramQueryAt = now;
            gHaveVramQueryAt = true;
        }
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

    if (gSettling && !kProbeMinimalGates)
    {
        const long long settleRemainingMs = MsUntil(gSettleStarted, kSettleStableMs);
        if (settleRemainingMs > 0)
            return SkipUnsafe(handleId, "settle", idleMs, snapshot.reset, extent, settleRemainingMs);
        gSettling = false;
    }
    if (kProbeMinimalGates)
        gSettling = false;

    if (!gInWorld)
    {
        gInWorld = true;
        gInWorldAt = Clock::now();
    }

    // Probe: VRAM is log-only. A climbing window must not block attach until
    // a probe correlates it with create success.
    const bool vramWindowClimbing =
        !kProbeMinimalGates && gVramQueryOk && gVramWindowMax > gVramWindowMin &&
        (gVramWindowMax - gVramWindowMin) >= kVramClimbBytes;
    if (vramWindowClimbing)
        return SkipUnsafe(handleId, "vram", idleMs, snapshot.reset, extent, kVramStableMs);

    const long long inWorldRemainingMs = MsUntil(gInWorldAt, kInWorldStableMs);
    if (!kProbeMinimalGates && inWorldRemainingMs > 0)
        return SkipUnsafe(handleId, "inworld", idleMs, snapshot.reset, extent, inWorldRemainingMs);

    if (DlssdTranslatedSession::IsCreating())
        return SkipUnsafe(handleId, "creating", idleMs, snapshot.reset, extent,
                          DlssdTranslatedSession::CreatingElapsedMs());

    if (gHaveLastDispatch && !Config::Instance()->FSRRDlssdPresentTranslated.value_or_default())
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
    // Present is not evidence that the staging command list executed.
    // Only the exact ExecuteCommandLists receipt schedules the owner.
}

void NotifyCommandListReset(ID3D12GraphicsCommandList* list)
{
    // Private runtime lists can reset while the session is executing under
    // its own lock. Only the retained producer participates in this receipt.
    // This runs on hooked game threads that may already hold gJobMutex
    // (2026-09-12 EDEADLK via Record -> re-entrant Reset on the producer),
    // so never block: defer the observation for the next lock holder.
    if (list != gReceiptProducer.load(std::memory_order_acquire))
        return;
    // 2026-09-13 game log: every sampled deferred reset had self=1 with zero
    // submission matches/mismatches (8015 prepared, 8015 retired flags=5).
    // A Reset observed while this thread holds the staging lock precedes the
    // Record Copy onto the freshly-reset list, so the Record supersedes it.
    // Deferring it cancels valid work before Execute ever runs. Drop self
    // observations; genuine pool reuse arrives outside staging (self=0).
    if (tStagingDepth > 0)
    {
        const auto total = gDeferredResetIgnoredSelf.fetch_add(1, std::memory_order_relaxed) + 1;
        if (total <= 3 || (total % 120) == 0)
        {
            LOG_INFO("DLSS-D deferred producer reset ignored self total={} tid={}", total,
                     GetCurrentThreadId());
            FlushExperimentalLog();
        }
        return;
    }
    std::unique_lock lock(gJobMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        // Attribute the miss: same-thread re-entrancy (the EDEADLK shape)
        // vs another thread resetting a pooled list under our section.
        const uint64_t packed = reinterpret_cast<uint64_t>(list) | (tStagingDepth > 0 ? uint64_t { 1 } : uint64_t { 0 });
        gDeferredReset.store(packed, std::memory_order_release);
        return;
    }
    // A previously deferred reset for this producer is still valid; apply it
    // first so observation order is preserved (repeats are idempotent).
    DrainDeferredResetLocked();
    ApplyProducerResetLocked(list);
}

// All queue timeline operations are serialized by the hook's per-queue gate.
// This function never holds gJobMutex across a GPU wait or owner evaluation.
bool ExecuteMatchingSubmission(ID3D12CommandQueue* queue, unsigned int count,
                               ID3D12CommandList* const* lists, ExecuteLists execute,
                               const DlssdOutputHazardTrace::PublicationPlan& plan)
{
    if (!queue || !lists || !execute || !count || !Enabled())
        return false;
    std::shared_ptr<FrameLease> lease;
    bool split = false;
    UINT prefixCount = count;
    {
        std::lock_guard lock(gJobMutex);
        DrainDeferredResetLocked();
        if (!gHavePending || !gPending.lease || gShutdown || gHaveJob || gOwnerBusy.load())
            return false;
        lease = gPending.lease;
        {
            // Starvation diagnosis: BeforeSubmit is silent on mismatch and it
            // mutates phase, so snapshot the comparison inputs first. The
            // ring catches producers submitted 1+ frames after their lease
            // retired (delayed submission vs never submitted).
            const auto phase = lease->input.State();
            auto* recorded = lease->input.RecordedList();
            bool recordedResolved = false;
            void* const recordedNative = ResolveNativeForMatch(recorded, &recordedResolved);
            UINT matches = 0, nativeMatches = 0, resChanged = 0, resPass = 0, resFailed = 0;
            uint64_t lateHitReceipt = 0, lateHitAge = 0;
            bool lateHit = false;
            for (UINT i = 0; i < count; ++i)
            {
                if (lists[i] == static_cast<ID3D12CommandList*>(recorded))
                    ++matches;
                bool entryResolved = false;
                void* const entryNative = ResolveNativeForMatch(lists[i], &entryResolved);
                if (entryResolved)
                {
                    if (entryNative != lists[i])
                        ++resChanged;
                    else
                        ++resPass;
                }
                else
                    ++resFailed;
                if (entryNative == recordedNative)
                    ++nativeMatches;
                uint64_t hitReceipt = 0, hitAge = 0;
                if (!lateHit && FindProducerLocked(entryNative, lease->tag.receiptId, hitReceipt, hitAge) &&
                    hitReceipt != lease->tag.receiptId)
                {
                    lateHit = true;
                    lateHitReceipt = hitReceipt;
                    lateHitAge = hitAge;
                }
            }
            if (lateHit)
            {
                const auto lateTotal = gLateHits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (lateTotal <= 5 || (lateTotal % 60) == 0)
                {
                    LOG_INFO("DLSS-D late producer sighting current={} hit={} age={} total={} tid={}",
                             lease->tag.receiptId, lateHitReceipt, lateHitAge, lateTotal, GetCurrentThreadId());
                    FlushExperimentalLog();
                }
            }
            if (nativeMatches > 0)
            {
                const auto nativeTotal = gNativeMatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (nativeTotal <= 3 || (nativeTotal % 60) == 0)
                {
                    LOG_INFO("DLSS-D frame receipt={} native submission match nativeTotal={} nativeMatches={}/{} "
                             "recordedNative={:p} tid={}",
                             lease->tag.receiptId, nativeTotal, nativeMatches, count, recordedNative,
                             GetCurrentThreadId());
                    FlushExperimentalLog();
                }
            }
            D3D12_COMMAND_QUEUE_DESC queueDesc {};
            if (queue != nullptr)
                queueDesc = queue->GetDesc();
            if (!lease->input.BeforeSubmit(queue, count, lists))
            {
                const auto total = gSubmitMismatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (total <= 3 || (total % 60) == 0)
                {
                    LOG_WARN("DLSS-D frame receipt={} submission mismatch total={} phase={} matches={}/{} "
                             "nativeMatches={}/{} lateHit={}/{} res={}/{}/{} queueType={} producer={:p} recorded={:p} "
                             "recordedNative={:p} tid={}",
                             lease->tag.receiptId, total, static_cast<int>(phase), matches, count, nativeMatches,
                             count, lateHitReceipt, lateHitAge, resChanged, resPass, resFailed,
                             static_cast<int>(queueDesc.Type), (void*) lease->producer.Get(),
                             (void*) recorded, recordedNative, GetCurrentThreadId());
                    FlushExperimentalLog();
                }
                return false;
            }
            {
                const auto total = gSubmitMatches.fetch_add(1, std::memory_order_relaxed) + 1;
                if (total <= 3 || (total % 120) == 0)
                {
                    LOG_INFO("DLSS-D frame receipt={} submission matched total={} index={} tid={}",
                             lease->tag.receiptId, total, lease->input.ListIndex(), GetCurrentThreadId());
                    FlushExperimentalLog();
                }
            }
        }
        split = Config::Instance()->FSRRDlssdPresentTranslated.value_or_default() && plan.valid &&
            !lease->copyFailed && !lease->cancelled && plan.handleId == lease->tag.handleId &&
            plan.producer == lease->producer.Get() && plan.output == lease->output.Get() &&
            plan.producerIndex == lease->input.ListIndex() && plan.producerIndex + 1 < count;
        lease->split = split;
        lease->handoffActive = true;
        if (split)
            prefixCount = plan.producerIndex + 1;
    }
    execute(queue, prefixCount, lists);
    {
        std::lock_guard lock(gJobMutex);
        if (!lease->input.AfterSubmit(queue))
        {
            const auto total = gAfterSubmitFails.fetch_add(1, std::memory_order_relaxed) + 1;
            if (total <= 3 || (total % 60) == 0)
            {
                LOG_WARN("DLSS-D frame receipt={} after-submit signal failed total={} tid={}", lease->tag.receiptId,
                         total, GetCurrentThreadId());
                FlushExperimentalLog();
            }
            gRuntimePoisoned.store(true);
            lease->cancelled = true;
            lease->ownerDone = true;
            ReleaseJob(gPending);
            gPending = {};
            gHavePending = false;
        }
        else
        {
            gJob = std::move(gPending);
            gPending = {};
            gHavePending = false;
            gHaveJob = true;
            gJobCv.notify_one();
        }
    }
    if (!split)
    {
        std::lock_guard lock(gJobMutex);
        lease->handoffActive = false;
        return true;
    }

    bool ready = false;
    {
        std::unique_lock lock(gJobMutex);
        const bool completed = gJobCv.wait_for(lock, std::chrono::seconds(9), [&] { return lease->ownerDone; });
        ready = completed && lease->ownerOk && !lease->cancelled && lease->ready;
        if (!ready)
            lease->cancelled = true; // Late owners only touch their private output.
    }
    bool submitted = false;
    uint32_t inputVerified = 0;
    if (ready)
    {
        const auto src = lease->ready->GetDesc();
        const auto dst = lease->output->GetDesc();
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        const bool compatible = src.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            dst.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && src.Format == dst.Format &&
            src.SampleDesc.Count == 1 && dst.SampleDesc.Count == 1 &&
            src.MipLevels == 1 && dst.MipLevels == 1 &&
            src.DepthOrArraySize == 1 && dst.DepthOrArraySize == 1 &&
            src.Width >= lease->width && src.Height >= lease->height &&
            uint64_t(lease->outputX) + lease->width <= dst.Width &&
            uint64_t(lease->outputY) + lease->height <= dst.Height;
        if (compatible && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&lease->allocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lease->allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&lease->publicationList))) &&
            SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&lease->publicationFence))))
        {
            D3D12_RESOURCE_BARRIER barriers[2] {};
            barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition = { lease->ready.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                static_cast<D3D12_RESOURCE_STATES>(lease->readyState), D3D12_RESOURCE_STATE_COPY_SOURCE };
            barriers[1].Transition = { lease->output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                static_cast<D3D12_RESOURCE_STATES>(plan.outputStateBefore), D3D12_RESOURCE_STATE_COPY_DEST };
            for (auto& barrier : barriers)
                if (barrier.Transition.StateBefore != barrier.Transition.StateAfter)
                    lease->publicationList->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source {}, destination {};
            source.pResource = lease->ready.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = lease->output.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box { 0, 0, 0, lease->width, lease->height, 1 };
            lease->publicationList->CopyTextureRegion(&destination, lease->outputX, lease->outputY, 0, &source, &box);
            for (auto& barrier : barriers)
            {
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                if (barrier.Transition.StateBefore != barrier.Transition.StateAfter)
                    lease->publicationList->ResourceBarrier(1, &barrier);
            }
            if (SUCCEEDED(lease->publicationList->Close()))
            {
                ID3D12CommandList* publication = lease->publicationList.Get();
                execute(queue, 1, &publication);
                submitted = true;
            }
        }
    }
    // The original consumer suffix always executes, including on timeout.
    // FSR-RR already wrote the matching fallback on the producer prefix.
    execute(queue, count - prefixCount, lists + prefixCount);
    const bool signalled = submitted && SUCCEEDED(queue->Signal(lease->publicationFence.Get(), 1));
    {
        std::lock_guard lock(gJobMutex);
        lease->published = submitted;
        lease->publicationSignalled = signalled;
        lease->handoffActive = false;
        inputVerified = lease->tag.inputSubmissionVerified;
        if (submitted && !signalled)
            gRuntimePoisoned.store(true);
    }
    gLastMatchedSubmitEnd.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
    LOG_INFO("DLSS-D frame receipt={} source_frame={} feature=13 handle={} input_verified={} "
             "publication_submitted={} completion_signalled={} producer_index={} suffix_count={} state_before=0x{:X}",
             lease->tag.receiptId, lease->tag.sourceFrame, lease->tag.handleId, inputVerified,
             submitted ? 1 : 0, signalled ? 1 : 0, prefixCount - 1, count - prefixCount, plan.outputStateBefore);
    FlushExperimentalLog();
    return true;
}
void Release(uint32_t handleId)
{
    if (!Enabled())
        return;
    LOG_INFO("DLSS-D experimental backend release enter creating={} owner_alive={} hung_leaked={} "
             "create_ms={} alloc={} funcs={} progress_api={}",
             DlssdTranslatedSession::IsCreating() ? 1 : 0, gOwnerAlive.load(std::memory_order_acquire) ? 1 : 0,
             gOwnerHungLeaked.load(std::memory_order_acquire) ? 1 : 0, DlssdTranslatedSession::CreatingElapsedMs(),
             gLastCreateAlloc, gLastCreateFuncs, gLastCreateHaveProgress ? 1 : 0);
    FlushExperimentalLog();
    const bool hungLeaked = gOwnerHungLeaked.load(std::memory_order_acquire);
    const bool ownerGone = !gOwnerAlive.load(std::memory_order_acquire);
    StopOwner(DlssdTranslatedSession::IsCreating());
    // Late-release only when the hung owner is verifiably gone. Releasing
    // while it is still blocked inside CreateFeature deadlocks the quit path
    // against it (dump 4: main in Shutdown1 SRW vs owner in module load).
    bool releasedAbandonedLate = false;
    if (hungLeaked && ownerGone)
        releasedAbandonedLate = DlssdTranslatedSession::ReleaseAbandonedLate();
    if (!releasedAbandonedLate)
        DlssdTranslatedSession::Release();
    gHaveLastKey = false;
    gHaveLastDispatch = false;
    gHaveAttachReady = false;
    gSettling = false;
    gInWorld = false;
    DlssdOutputHazardTrace::InvalidateAll();
    gVramHistory.clear();
    gVramLastBytes = 0;
    gVramQueryOk = false;
    gVramSawClimb = false;
    ReleaseCachedVramAdapter();
    LOG_INFO("DLSS-D experimental backend released handle={} evaluated={} extent={} capture_only={} "
             "inventory={} runtime={} skipped={} dispatched={} owner_ok={} owner_fail={} published={}",
             handleId, gEvaluated.load(), gRejectedExtent.load(), gRejectedCaptureOnly.load(),
             gRejectedInventory.load(), gRejectedRuntime.load(), gSkippedUnsafe.load(), gDispatched.load(),
             gOwnerOk.load(), gOwnerFailed.load(), gPublished.load());
}
} // namespace DlssdExperimentalBackend
