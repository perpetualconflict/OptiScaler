#include "pch.h"

#include "DlssdTranslatedSession.h"
#include "DlssdQueueRendezvous.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <denoisers/RrInputRegistry.h>
#include <DllNames.h>
#include <proxies/Ntdll_Proxy.h>

#include "../../native/dlssd_translated_runtime.h"

#include <d3d12.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace DlssdTranslatedSession
{
namespace
{
using namespace RayReconstruction;

std::mutex gLock;
HMODULE gModule = nullptr;
HMODULE gCallerShim = nullptr;
PFN_DlssdRuntime_Attach gAttach = nullptr;
PFN_DlssdRuntime_PrepareAttach gPrepareAttach = nullptr;
PFN_DlssdRuntime_FinishAttach gFinishAttach = nullptr;
PFN_DlssdRuntime_CreateProgress gCreateProgress = nullptr;
PFN_DlssdRuntime_Evaluate gEvaluate = nullptr;
PFN_DlssdRuntime_Release gRelease = nullptr;
PFN_DlssdRuntime_LastError gLastError = nullptr;
PFN_DlssdRuntime_SetLog gSetLog = nullptr;
std::atomic<bool> gAttached { false };
std::atomic<bool> gPrepared { false };
std::atomic<bool> gCreating { false };
std::atomic<bool> gCreateFailed { false };
std::atomic<bool> gReleaseAfterCreate { false };
std::atomic<int> gAttachLoadBypass { 0 };
thread_local int gAttachLoadBypassTls = 0;
HMODULE gSidecarNvapi = nullptr;
std::mutex gSidecarNvapiLock;
std::chrono::steady_clock::time_point gCreateStarted {};
uint32_t gAttachedRenderWidth = 0;
uint32_t gAttachedRenderHeight = 0;
uint32_t gAttachedOutputWidth = 0;
uint32_t gAttachedOutputHeight = 0;
// Device hooks patch the vtable process-wide. Refuse a cross-device attach
// instead of silently reusing hooks captured from a different adapter.
LUID gPreparedLuid {};
bool gHavePreparedLuid = false;
std::wstring gDataPath;
std::wstring gCudaPath;
std::atomic<bool> gAbandoned { false };
// AbandonHungCreate clears gPrepared so nothing treats the session as live,
// but the sidecar modules stay mapped for a still-blocked owner. This latch
// remembers that a releasable prepared state existed, so Release can run the
// normal sidecar release once the hung create has returned. Poison flags are
// separate and never cleared here, so this never re-arms attach.
std::atomic<bool> gAbandonedPrepared { false };

std::filesystem::path ModuleDirectory()
{
    wchar_t path[MAX_PATH] {};
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self) ||
        GetModuleFileNameW(self, path, MAX_PATH) == 0)
        return {};
    return std::filesystem::path(path).parent_path();
}

// Companions must never sit in the game folder under nvngx.dll / nvapi64.dll /
// nvcuda.dll. After Streamline's handshake, attach may Ldr-load sidecar
// OptiScaler/dlssd_runtime/nvngx.dll so NGX entry points come from that
// basename. Never LoadLibrary nvapi64.dll in the game process.
std::filesystem::path RuntimeHome()
{
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;
    if (Config::Instance()->MainDllPath.has_value())
        candidates.emplace_back(std::filesystem::path(*Config::Instance()->MainDllPath) / L"dlssd_runtime");
    candidates.emplace_back(ModuleDirectory() / L"OptiScaler" / L"dlssd_runtime");
    for (const auto& home : candidates)
    {
        if (std::filesystem::exists(home / L"dlssd_translated_runtime.dll", ec))
            return home;
    }
    return {};
}

const char* RuntimeError()
{
    if (gLastError != nullptr)
    {
        if (const char* text = gLastError(); text != nullptr && text[0] != '\0')
            return text;
    }
    return "translated runtime rejected the request";
}

bool LoadRuntime(const char** error)
{
    if (gModule != nullptr)
        return true;
    const auto home = RuntimeHome();
    const auto runtimePath = home / L"dlssd_translated_runtime.dll";
    if (home.empty())
    {
        if (error)
            *error = "dlssd_translated_runtime.dll was not found under OptiScaler/dlssd_runtime";
        return false;
    }
    gModule = LoadLibraryW(runtimePath.c_str());
    if (gModule == nullptr)
    {
        if (error)
            *error = "dlssd_translated_runtime.dll could not be loaded from OptiScaler/dlssd_runtime";
        return false;
    }
    gAttach = reinterpret_cast<PFN_DlssdRuntime_Attach>(GetProcAddress(gModule, "DlssdRuntime_Attach"));
    gPrepareAttach =
        reinterpret_cast<PFN_DlssdRuntime_PrepareAttach>(GetProcAddress(gModule, "DlssdRuntime_PrepareAttach"));
    gFinishAttach =
        reinterpret_cast<PFN_DlssdRuntime_FinishAttach>(GetProcAddress(gModule, "DlssdRuntime_FinishAttach"));
    // Optional progress export for the host create watchdog. Older sidecar
    // DLLs lack it; a missing export only disables stall detection, never
    // attach itself.
    gCreateProgress =
        reinterpret_cast<PFN_DlssdRuntime_CreateProgress>(GetProcAddress(gModule, "DlssdRuntime_CreateProgress"));
    gEvaluate = reinterpret_cast<PFN_DlssdRuntime_Evaluate>(GetProcAddress(gModule, "DlssdRuntime_Evaluate"));
    gRelease = reinterpret_cast<PFN_DlssdRuntime_Release>(GetProcAddress(gModule, "DlssdRuntime_Release"));
    gLastError = reinterpret_cast<PFN_DlssdRuntime_LastError>(GetProcAddress(gModule, "DlssdRuntime_LastError"));
    gSetLog = reinterpret_cast<PFN_DlssdRuntime_SetLog>(GetProcAddress(gModule, "DlssdRuntime_SetLog"));
    if (gAttach == nullptr || gPrepareAttach == nullptr || gFinishAttach == nullptr || gEvaluate == nullptr ||
        gRelease == nullptr)
    {
        if (error)
        {
            if (gAttach == nullptr)
                *error = "dlssd_translated_runtime.dll is missing DlssdRuntime_Attach";
            else if (gPrepareAttach == nullptr)
                *error = "dlssd_translated_runtime.dll is missing DlssdRuntime_PrepareAttach";
            else if (gFinishAttach == nullptr)
                *error = "dlssd_translated_runtime.dll is missing DlssdRuntime_FinishAttach";
            else if (gEvaluate == nullptr)
                *error = "dlssd_translated_runtime.dll is missing DlssdRuntime_Evaluate";
            else
                *error = "dlssd_translated_runtime.dll is missing DlssdRuntime_Release";
        }
        FreeLibrary(gModule);
        gModule = nullptr;
        gAttach = nullptr;
        gPrepareAttach = nullptr;
        gFinishAttach = nullptr;
        gCreateProgress = nullptr;
        gEvaluate = nullptr;
        gRelease = nullptr;
        gLastError = nullptr;
        return false;
    }
    gDataPath = (home / L"data").wstring();
    std::error_code ec;
    std::filesystem::create_directories(home / L"data", ec);
    return true;
}

std::optional<DlssdRuntimeSemantic> ToRuntimeSemantic(InputSemantic semantic)
{
    switch (semantic)
    {
    case InputSemantic::Color:
        return DlssdRuntimeSemantic_Color;
    case InputSemantic::Output:
        return DlssdRuntimeSemantic_Output;
    case InputSemantic::Depth:
        return DlssdRuntimeSemantic_Depth;
    case InputSemantic::MotionVectors:
        return DlssdRuntimeSemantic_MotionVectors;
    case InputSemantic::Normals:
        return DlssdRuntimeSemantic_Normals;
    case InputSemantic::DiffuseAlbedo:
        return DlssdRuntimeSemantic_DiffuseAlbedo;
    case InputSemantic::SpecularAlbedo:
        return DlssdRuntimeSemantic_SpecularAlbedo;
    case InputSemantic::SpecularHitDistance:
        return DlssdRuntimeSemantic_SpecularHitDistance;
    case InputSemantic::ColorBeforeParticles:
        return DlssdRuntimeSemantic_ColorBeforeParticles;
    case InputSemantic::BiasMask:
        return DlssdRuntimeSemantic_BiasMask;
    case InputSemantic::ScreenSpaceSubsurfaceScatteringGuide:
        return DlssdRuntimeSemantic_SssGuide;
    default:
        return std::nullopt;
    }
}

D3D12_RESOURCE_STATES StreamlineEvaluateState(DlssdRuntimeSemantic semantic)
{
    return semantic == DlssdRuntimeSemantic_Output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                   : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}
} // namespace

bool IsAttached()
{
    return gAttached.load(std::memory_order_acquire);
}

bool IsPrepared()
{
    return gPrepared.load(std::memory_order_acquire);
}

bool IsCreating()
{
    return gCreating.load(std::memory_order_acquire);
}

long long CreatingElapsedMs()
{
    if (!gCreating.load(std::memory_order_acquire))
        return 0;
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gCreateStarted)
        .count();
}

bool AttachLoadBypassActive()
{
    return gAttachLoadBypassTls > 0;
}

HMODULE LoadSidecarNvapiForAttach()
{
    std::lock_guard lock(gSidecarNvapiLock);
    if (gSidecarNvapi != nullptr)
        return gSidecarNvapi;
    const auto home = RuntimeHome();
    const auto bridge = home / L"dlssd_rr_bridge.dll";
    std::error_code ec;
    if (home.empty() || !std::filesystem::exists(bridge, ec))
        return nullptr;
    LOG_INFO("DLSS-D attach nvapi64 request: loading sidecar bridge, not OptiScaler");
    gSidecarNvapi = NtdllProxy::LoadLibraryExW_Ldr(bridge.c_str(), NULL, 0);
    return gSidecarNvapi;
}

HMODULE NvapiHandleForAttach()
{
    return LoadSidecarNvapiForAttach();
}

void ReleaseSidecarNvapi()
{
    std::lock_guard lock(gSidecarNvapiLock);
    if (gSidecarNvapi != nullptr)
    {
        FreeLibrary(gSidecarNvapi);
        gSidecarNvapi = nullptr;
    }
}

ScopedAttachLoadBypass::ScopedAttachLoadBypass()
{
    ++gAttachLoadBypassTls;
    gAttachLoadBypass.fetch_add(1, std::memory_order_acq_rel);
}

ScopedAttachLoadBypass::~ScopedAttachLoadBypass()
{
    gAttachLoadBypass.fetch_sub(1, std::memory_order_acq_rel);
    --gAttachLoadBypassTls;
}

void RuntimeLog(const char* text)
{
    if (text == nullptr || text[0] == '\0')
        return;
    LOG_INFO("{}", text);
    if (auto logger = spdlog::default_logger())
        logger->flush();
}

bool EnsurePrepared(ID3D12Device* device, uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth,
                    uint32_t outputHeight, const char** error)
{
    ScopedAttachLoadBypass bypassLoads;
    std::unique_lock lock(gLock);
    if (gAbandoned.load(std::memory_order_acquire) || gCreateFailed.load(std::memory_order_acquire))
    {
        if (error)
            *error = "translated runtime create already failed";
        return false;
    }
    if ((gPrepared || gAttached) && gAttachedRenderWidth == renderWidth && gAttachedRenderHeight == renderHeight &&
        gAttachedOutputWidth == outputWidth && gAttachedOutputHeight == outputHeight)
        return true;
    if (gCreating.load(std::memory_order_acquire))
    {
        if (error)
            *error = "translated runtime create is already running";
        return false;
    }
    if (gAttached || gPrepared)
    {
        if (error)
            *error = "translated runtime is bound to a different extent";
        return false;
    }
    if (device == nullptr)
    {
        if (error)
            *error = "translated runtime requires the evaluate device";
        return false;
    }
    const LUID deviceLuid = device->GetAdapterLuid();
    if ((gPrepared.load(std::memory_order_acquire) || gAttached.load(std::memory_order_acquire)) &&
        gHavePreparedLuid &&
        (gPreparedLuid.LowPart != deviceLuid.LowPart || gPreparedLuid.HighPart != deviceLuid.HighPart))
    {
        if (error)
            *error = "translated runtime is bound to a different adapter";
        return false;
    }
    if (!LoadRuntime(error))
        return false;

    const auto& state = State::Instance();
    if (!state.NVNGX_DLSSD_Path)
    {
        if (error)
            *error = "nvngx_dlssd.dll path is unresolved";
        return false;
    }
    const auto home = RuntimeHome();
    const auto nvapi = home / L"dlssd_rr_bridge.dll";
    const auto cuda = home / L"nvcuda.dll";
    const auto caller = home / L"nvngx.dll";
    if (home.empty() || !std::filesystem::exists(nvapi) || !std::filesystem::exists(cuda) ||
        !std::filesystem::exists(caller))
    {
        if (error)
            *error = "dlssd_rr_bridge.dll, nvcuda.dll, or sidecar nvngx.dll is missing under "
                     "OptiScaler/dlssd_runtime";
        return false;
    }

    // Isolated Gate 5/6 requires NGX entry points from a module named nvngx.dll.
    // Ldr-load the sidecar copy after Streamline's handshake so the game folder
    // stays empty and OptiScaler's substitution hook does not steal the shim.
    // Never Ldr-load nvapi64.dll here: that reserved name is process-wide.
    if (gCallerShim != nullptr)
    {
        FreeLibrary(gCallerShim);
        gCallerShim = nullptr;
    }
    gCallerShim = NtdllProxy::LoadLibraryExW_Ldr(caller.c_str(), NULL, 0);
    if (gCallerShim == nullptr || GetProcAddress(gCallerShim, "ProbeNvngx_SetTarget") == nullptr)
    {
        if (error)
            *error = "sidecar nvngx.dll caller shim is missing ProbeNvngx_* or was substituted";
        if (gCallerShim != nullptr)
        {
            FreeLibrary(gCallerShim);
            gCallerShim = nullptr;
        }
        return false;
    }
    wchar_t mapped[MAX_PATH] {};
    if (GetModuleFileNameW(gCallerShim, mapped, MAX_PATH) == 0 ||
        !IsDlssdRuntimeSidecarNvngxW(mapped))
    {
        if (error)
            *error = "sidecar nvngx.dll did not map from OptiScaler/dlssd_runtime";
        FreeLibrary(gCallerShim);
        gCallerShim = nullptr;
        return false;
    }
    LOG_INFO("DLSS-D experimental backend loaded sidecar NGX caller {}", wstring_to_string(mapped));

    if (gSetLog != nullptr)
        gSetLog(&RuntimeLog);
    LOG_INFO("DLSS-D experimental backend preparing translated attach {}x{} -> {}x{}", renderWidth, renderHeight,
             outputWidth, outputHeight);
    if (auto logger = spdlog::default_logger())
        logger->flush();

    const std::wstring dlssd = std::filesystem::path(*state.NVNGX_DLSSD_Path).wstring();
    if (gPrepareAttach(device, dlssd.c_str(), nvapi.c_str(), cuda.c_str(), gDataPath.c_str(), renderWidth,
                       renderHeight, outputWidth, outputHeight) != 0)
    {
        if (error)
            *error = RuntimeError();
        FreeLibrary(gCallerShim);
        gCallerShim = nullptr;
        return false;
    }
    gPrepared = true;
    gCudaPath = cuda.wstring();
    gPreparedLuid = deviceLuid;
    gHavePreparedLuid = true;
    gAttachedRenderWidth = renderWidth;
    gAttachedRenderHeight = renderHeight;
    gAttachedOutputWidth = outputWidth;
    gAttachedOutputHeight = outputHeight;
    return true;
}

void BindCudaOnThisThread()
{
    if (gCudaPath.empty())
        return;
    const HMODULE cuda = LoadLibraryW(gCudaPath.c_str());
    LOG_INFO("DLSS-D experimental backend bound sidecar nvcuda on owner tid={} loaded={}", GetCurrentThreadId(),
             cuda != nullptr ? 1 : 0);
    if (auto logger = spdlog::default_logger())
        logger->flush();
}

void AbandonHungCreate()
{
    gAbandonedPrepared.store(gPrepared.load(std::memory_order_acquire) ||
                                 gAttached.load(std::memory_order_acquire),
                             std::memory_order_release);
    gCreating.store(false, std::memory_order_release);
    gCreateFailed.store(true, std::memory_order_release);
    gPrepared.store(false, std::memory_order_release);
    gAttached.store(false, std::memory_order_release);
    gAbandoned.store(true, std::memory_order_release);
    gReleaseAfterCreate.store(false, std::memory_order_release);
    LOG_WARN("DLSS-D experimental backend abandoned a hung NGX create; leaking sidecar modules");
    if (auto logger = spdlog::default_logger())
        logger->flush();
}

bool ReleasePreparedRuntimeLocked()
{
    if (gRelease != nullptr && gRelease() != 0)
    {
        // A failed drain is not cancellation. Keep every companion loaded and
        // prevent reattach over the runtime's retained objects. No automatic
        // late-release retry: cleanup may already have retired some objects.
        gAbandoned.store(true, std::memory_order_release);
        gAbandonedPrepared.store(false, std::memory_order_release);
        gCreateFailed.store(true, std::memory_order_release);
        gAttached.store(false, std::memory_order_release);
        gPrepared.store(false, std::memory_order_release);
        LOG_WARN("DLSS-D runtime release failed; retaining sidecar modules and disabling reattach");
        return false;
    }
    if (gCallerShim != nullptr)
    {
        FreeLibrary(gCallerShim);
        gCallerShim = nullptr;
    }
    ReleaseSidecarNvapi();
    gAttached.store(false, std::memory_order_release);
    gPrepared.store(false, std::memory_order_release);
    gAbandonedPrepared.store(false, std::memory_order_release);
    gHavePreparedLuid = false;
    gPreparedLuid = {};
    gAttachedRenderWidth = 0;
    gAttachedRenderHeight = 0;
    gAttachedOutputWidth = 0;
    gAttachedOutputHeight = 0;
    return true;
}

bool StartFinishAttach(const char** error)
{
    std::unique_lock lock(gLock);
    if (gAttached.load(std::memory_order_acquire))
        return true;
    if (gAbandoned.load(std::memory_order_acquire) || gCreateFailed.load(std::memory_order_acquire))
    {
        if (error)
            *error = "translated runtime create already failed";
        return false;
    }
    if (gCreating.load(std::memory_order_acquire))
        return true;
    if (!gPrepared.load(std::memory_order_acquire) || gFinishAttach == nullptr)
    {
        if (error)
            *error = "translated runtime is not prepared";
        return false;
    }

    // Do not block slEvaluateFeature. The persistent owner thread loads
    // nvcuda, selects HIP, and runs Init+CreateFeature so ZLUDA's TLS
    // stack stays on that thread. Streamline's evaluate caller returns.
    gCreateStarted = std::chrono::steady_clock::now();
    gCreating.store(true, std::memory_order_release);
    DlssdQueueRendezvous::PauseRecording();
    LOG_INFO("DLSS-D experimental backend queued finish attach for the NGX owner thread");
    if (auto logger = spdlog::default_logger())
        logger->flush();
    return true;
}

bool FinishAttachOnOwnerThread(const char** error)
{
    ScopedAttachLoadBypass bypassLoads;
    LOG_INFO("DLSS-D experimental backend finish attach begin tid={}", GetCurrentThreadId());
    if (auto logger = spdlog::default_logger())
        logger->flush();
    if (!DlssdQueueRendezvous::WaitUntilIdle(2000))
        LOG_WARN("DLSS-D experimental backend starting NGX create while rendezvous HIP is still in flight");
    struct ResumeRendezvous
    {
        ~ResumeRendezvous() { DlssdQueueRendezvous::Resume(); }
    } resumeRendezvous;
    const auto result = gFinishAttach != nullptr ? gFinishAttach() : 1;
    const char* runtimeError = result != 0 ? RuntimeError() : nullptr;

    std::lock_guard lock(gLock);
    gCreating.store(false, std::memory_order_release);
    if (result == 0)
    {
        gAttached.store(true, std::memory_order_release);
        LOG_INFO("DLSS-D experimental backend finish attach ok tid={}", GetCurrentThreadId());
    }
    else
    {
        gPrepared.store(false, std::memory_order_release);
        gCreateFailed.store(true, std::memory_order_release);
        LOG_WARN("DLSS-D experimental backend finish attach failed tid={}: {}", GetCurrentThreadId(),
                 runtimeError != nullptr ? runtimeError : "unknown");
        if (error)
            *error = runtimeError != nullptr ? runtimeError : "translated runtime create failed";
    }
    if (auto logger = spdlog::default_logger())
        logger->flush();
    if (gReleaseAfterCreate.exchange(false, std::memory_order_acq_rel))
        ReleasePreparedRuntimeLocked();
    return result == 0 && gAttached.load(std::memory_order_acquire);
}

bool Evaluate(ID3D12GraphicsCommandList* commandList, const InputSnapshot& snapshot, NVSDK_NGX_Parameter* parameters,
              bool publishOutput, const char** error, uint32_t runtimeFlags)
{
    (void) parameters;
    // Crash hardening (2026-09-07): never block any thread on the session
    // lock. The owner used to block here while the game thread can hold the
    // lock across sidecar loads; a busy owner now skips the frame instead and
    // the backend requeues fail-closed, mirroring the caller-list path.
    std::unique_lock lock(gLock, std::defer_lock);
    if (!lock.try_lock())
    {
        if (error)
            *error = "translated runtime is busy";
        return false;
    }
    if (!gAttached.load(std::memory_order_acquire) || gEvaluate == nullptr)
    {
        if (error)
            *error = "translated runtime is not attached";
        return false;
    }

    // Streamline 2.12 transitions evaluate inputs to eTextureRead and output to
    // eStorageRW on this command list. Isolated staging is packed at the origin,
    // so live subrects become the copy box and NGX subrect keys stay 0.
    std::array<DlssdRuntimeResource, 48> resources {};
    uint32_t count = 0;
    for (const auto& input : snapshot.resources)
    {
        if (!input.IsPresent() || input.definition == nullptr)
            continue;
        const auto semantic = ToRuntimeSemantic(input.definition->semantic);
        if (!publishOutput && semantic && *semantic == DlssdRuntimeSemantic_Output)
            continue;
        const auto copyWidth =
            semantic && *semantic == DlssdRuntimeSemantic_Output ? snapshot.outputWidth : snapshot.renderWidth;
        const auto copyHeight =
            semantic && *semantic == DlssdRuntimeSemantic_Output ? snapshot.outputHeight : snapshot.renderHeight;
        const auto state = semantic ? StreamlineEvaluateState(*semantic) : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        resources[count++] = { input.resource,
                               semantic ? static_cast<uint32_t>(*semantic) : UINT32_MAX,
                               input.subrectBaseX,
                               input.subrectBaseY,
                               static_cast<uint32_t>(state),
                               1u,
                               copyWidth,
                               copyHeight,
                               input.definition->ngxKey };
        if (count >= resources.size())
            break;
    }

    DlssdRuntimeFrame frame {};
    frame.renderWidth = snapshot.renderWidth;
    frame.renderHeight = snapshot.renderHeight;
    frame.outputWidth = snapshot.outputWidth;
    frame.outputHeight = snapshot.outputHeight;
    frame.reset = snapshot.reset;
    frame.jitterX = snapshot.jitterX;
    frame.jitterY = snapshot.jitterY;
    frame.motionScaleX = snapshot.motionScaleX;
    frame.motionScaleY = snapshot.motionScaleY;
    frame.preExposure = snapshot.preExposure.value_or(1.0f);
    frame.exposureScale = snapshot.exposureScale.value_or(1.0f);
    frame.sharpness = snapshot.sharpness.value_or(0.0f);
    frame.worldToView = snapshot.hasWorldToView ? snapshot.worldToView.data() : nullptr;
    frame.viewToClip = snapshot.hasViewToClip ? snapshot.viewToClip.data() : nullptr;
    frame.commandList = commandList;
    frame.resources = resources.data();
    frame.resourceCount = count;
    frame.invViewProjection = snapshot.hasInvViewProjection ? snapshot.invViewProjection.data() : nullptr;
    frame.clipToPrevClip = snapshot.hasClipToPrevClip ? snapshot.clipToPrevClip.data() : nullptr;
    frame.flags = DlssdRuntimeFrame_RequireValidation | runtimeFlags;
    if (publishOutput)
        frame.flags |= DlssdRuntimeFrame_PublishOutput;
    frame.frameTimeDelta = snapshot.frameTimeDelta.value_or(16.6667f);
    frame.indicatorInvertX = snapshot.indicatorInvertX.value_or(0);
    frame.indicatorInvertY = snapshot.indicatorInvertY.value_or(0);
    frame.depthType = snapshot.depthType.value_or(1);
    frame.roughnessMode = snapshot.roughnessMode.value_or(1);
    frame.denoiseMode = snapshot.denoiseMode.value_or(1);
    frame.perfQuality = snapshot.perfQuality.value_or(0);
    frame.liveScalars = 1;
    if (gEvaluate(&frame) != 0)
    {
        if (error)
            *error = RuntimeError();
        return false;
    }
    return true;
}

void Release()
{
    std::unique_lock lock(gLock);
    if (gAbandoned.load(std::memory_order_acquire))
    {
        gCallerShim = nullptr;
        gSidecarNvapi = nullptr;
        gAttached.store(false, std::memory_order_release);
        gPrepared.store(false, std::memory_order_release);
        gCreating.store(false, std::memory_order_release);
        gHavePreparedLuid = false;
        gPreparedLuid = {};
        gAttachedRenderWidth = 0;
        gAttachedRenderHeight = 0;
        gAttachedOutputWidth = 0;
        gAttachedOutputHeight = 0;
        LOG_WARN("DLSS-D experimental backend skipped sidecar release after an abandoned create");
        return;
    }
    if (gCreating.load(std::memory_order_acquire))
    {
        gReleaseAfterCreate.store(true, std::memory_order_release);
        LOG_INFO("DLSS-D experimental backend deferred translated release until finish attach returns");
        return;
    }
    if (gRelease != nullptr && (gAttached.load(std::memory_order_acquire) || gPrepared.load(std::memory_order_acquire)))
        ReleasePreparedRuntimeLocked();
    else
    {
        if (gCallerShim != nullptr)
        {
            FreeLibrary(gCallerShim);
            gCallerShim = nullptr;
        }
        ReleaseSidecarNvapi();
        gAttached.store(false, std::memory_order_release);
        gPrepared.store(false, std::memory_order_release);
        gHavePreparedLuid = false;
        gPreparedLuid = {};
        gAttachedRenderWidth = 0;
        gAttachedRenderHeight = 0;
        gAttachedOutputWidth = 0;
        gAttachedOutputHeight = 0;
    }
    if (!gAbandoned.load(std::memory_order_acquire))
        gCreateFailed.store(false, std::memory_order_release);
}

bool ReleaseAbandonedLate()
{
    std::unique_lock lock(gLock);
    if (!gAbandoned.load(std::memory_order_acquire) || gCreating.load(std::memory_order_acquire) ||
        !gAbandonedPrepared.load(std::memory_order_acquire) || gRelease == nullptr)
        return false;
    // Caller guarantees no thread is inside the runtime (the hung owner has
    // exited). Unregister bridge resources through the normal path so process
    // detach does not trip over them. Poison flags stay set: no re-arm.
    LOG_INFO("DLSS-D experimental backend late-releasing abandoned sidecar after the hung owner returned");
    if (auto logger = spdlog::default_logger())
        logger->flush();
    if (!ReleasePreparedRuntimeLocked())
        return false;
    gAttachedRenderWidth = 0;
    gAttachedRenderHeight = 0;
    gAttachedOutputWidth = 0;
    gAttachedOutputHeight = 0;
    LOG_WARN("DLSS-D experimental backend released abandoned sidecar late; path stays poisoned");
    return true;
}

bool CreateProgress(uint32_t* allocHeartbeats, uint32_t* functionsCreated)
{
    if (gCreateProgress == nullptr)
    {
        if (allocHeartbeats != nullptr)
            *allocHeartbeats = 0;
        if (functionsCreated != nullptr)
            *functionsCreated = 0;
        return false;
    }
    const uint32_t alloc = gCreateProgress(functionsCreated);
    if (allocHeartbeats != nullptr)
        *allocHeartbeats = alloc;
    return true;
}
} // namespace DlssdTranslatedSession
