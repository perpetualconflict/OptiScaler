#include "pch.h"

#include "DlssdQueueRendezvous.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <denoisers/dx12/shaders/DlssdQueueRendezvousSignal_Shader.h>
#include <denoisers/dx12/shaders/DlssdQueueRendezvousWait_Shader.h>

#include <array>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace DlssdQueueRendezvous
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr uint32_t kSlotCount = 16;
constexpr uint32_t kSlotBytes = 64;
constexpr uint32_t kSlotWords = kSlotBytes / sizeof(uint32_t);
constexpr uint64_t kSharedBytes = static_cast<uint64_t>(kSlotCount) * kSlotBytes;
// Keep both waits comfortably below the Windows watchdog path. The helper
// pre-warms the exact HIP kernel before the first D3D12 submission so driver
// initialization cannot consume the consumer's bounded wait.
constexpr uint32_t kD3dWaitIterations = 4u * 1024u * 1024u;
constexpr uint32_t kHipWaitIterations = 4u * 1024u * 1024u;
constexpr uint32_t kWorkIterations = 4096;
constexpr uint32_t kMaxDetailLogs = 12;

constexpr uint32_t kReadyWord = 0;
constexpr uint32_t kDoneWord = 1;
constexpr uint32_t kHipStatusWord = 2;
constexpr uint32_t kD3dStatusWord = 3;
constexpr uint32_t kPayloadWord = 4;
constexpr uint32_t kD3dObservedWord = 5;
constexpr uint32_t kHipWaitIterationsWord = 6;
constexpr uint32_t kD3dWaitIterationsWord = 7;
constexpr uint32_t kSequenceMirrorWord = 8;

using PFN_Initialize = uint32_t(__cdecl*)(uint32_t, int32_t, void*, uint64_t, void**);
using PFN_Execute = uint32_t(__cdecl*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
using PFN_Synchronize = uint32_t(__cdecl*)(void*);
using PFN_Destroy = uint32_t(__cdecl*)(void*);
using PFN_GetLastErrorStatus = uint32_t(__cdecl*)();
using PFN_GetLastErrorText = const char*(__cdecl*)();

struct Job
{
    uint32_t slot = 0;
    uint32_t sequence = 0;
    uint32_t handleId = 0;
    ID3D12GraphicsCommandList* commandList = nullptr;
};

struct SlotState
{
    bool inUse = false;
    bool submitted = false;
    uint32_t sequence = 0;
    uint32_t handleId = 0;
    uint32_t expectedPayload = 0;
    ID3D12CommandQueue* queue = nullptr;
    uint64_t fenceValue = 0;
};

struct QueueState
{
    ComPtr<ID3D12Fence> fence;
    uint64_t nextFenceValue = 1;
};

struct Counters
{
    uint64_t recorded = 0;
    uint64_t submitted = 0;
    uint64_t hipLaunchFailures = 0;
    uint64_t fenceFailures = 0;
    uint64_t passed = 0;
    uint64_t hipTimeouts = 0;
    uint64_t d3dTimeouts = 0;
    uint64_t payloadMismatches = 0;
    uint64_t protocolMismatches = 0;
    uint64_t slotsUnavailable = 0;
};

std::mutex Mutex;
ComPtr<ID3D12Device> Device;
ComPtr<ID3D12Resource> SharedBuffer;
ComPtr<ID3D12Resource> ReadbackBuffer;
ComPtr<ID3D12RootSignature> RootSignature;
ComPtr<ID3D12PipelineState> SignalPipeline;
ComPtr<ID3D12PipelineState> WaitPipeline;
uint8_t* ReadbackMapping = nullptr;
HMODULE HipModule = nullptr;
void* HipToken = nullptr;
PFN_Initialize HipInitialize = nullptr;
PFN_Execute HipExecute = nullptr;
PFN_Synchronize HipSynchronize = nullptr;
PFN_Destroy HipDestroy = nullptr;
PFN_GetLastErrorStatus HipGetLastErrorStatus = nullptr;
PFN_GetLastErrorText HipGetLastErrorText = nullptr;
std::array<SlotState, kSlotCount> Slots;
std::unordered_map<ID3D12CommandList*, std::vector<Job>> PendingJobs;
std::unordered_map<uint64_t, std::vector<Job>> SubmissionBatches;
std::unordered_map<ID3D12CommandQueue*, QueueState> Queues;
Counters Stats;
uint32_t NextSequence = 1;
uint64_t NextSubmissionToken = 1;
bool Initialized = false;
bool InitializationFailed = false;
bool RuntimeFailed = false;
bool LoggedEnable = false;
uint32_t DetailLogs = 0;
uint32_t FailureLogs = 0;

uint32_t MakePayload(uint32_t sequence)
{
    uint32_t value = sequence ^ 0xA5A55A5Au;
    for (uint32_t i = 0; i < kWorkIterations; ++i)
    {
        value = value * 1664525u + 1013904223u;
        value ^= value >> 16;
    }
    return value;
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

std::string HipErrorTextLocked()
{
    if (HipGetLastErrorText != nullptr)
    {
        const char* text = HipGetLastErrorText();
        if (text != nullptr && text[0] != '\0')
            return text;
    }
    if (HipGetLastErrorStatus != nullptr)
        return std::format("status=0x{:X}", HipGetLastErrorStatus());
    return "no helper error text";
}

void ReleaseRuntimeLocked()
{
    if (HipDestroy != nullptr && HipToken != nullptr)
    {
        const uint32_t result = HipDestroy(HipToken);
        if (result != 0)
            LOG_WARN("DLSS-D rendezvous: HIP helper destroy failed 0x{:X} ({})", result, HipErrorTextLocked());
    }
    HipToken = nullptr;
    HipInitialize = nullptr;
    HipExecute = nullptr;
    HipSynchronize = nullptr;
    HipDestroy = nullptr;
    HipGetLastErrorStatus = nullptr;
    HipGetLastErrorText = nullptr;
    if (HipModule != nullptr)
        FreeLibrary(HipModule);
    HipModule = nullptr;

    if (ReadbackBuffer != nullptr && ReadbackMapping != nullptr)
        ReadbackBuffer->Unmap(0, nullptr);
    ReadbackMapping = nullptr;
    Queues.clear();
    SubmissionBatches.clear();
    PendingJobs.clear();
    Slots = {};
    WaitPipeline.Reset();
    SignalPipeline.Reset();
    RootSignature.Reset();
    ReadbackBuffer.Reset();
    SharedBuffer.Reset();
    Device.Reset();
    Initialized = false;
}

bool LoadHipHelperLocked()
{
    std::filesystem::path helperPath = Config::Instance()->MainDllPath.value_or(Util::DllPath().parent_path());
    helperPath /= L"DlssdQueueRendezvousHip.dll";
    HipModule = LoadLibraryExW(helperPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (HipModule == nullptr)
    {
        LOG_ERROR("DLSS-D rendezvous: failed to load {} (Win32 {})", wstring_to_string(helperPath.wstring()),
                  GetLastError());
        return false;
    }

    HipInitialize = reinterpret_cast<PFN_Initialize>(GetProcAddress(HipModule, "DLSSD_RENDEZVOUS_Initialize"));
    HipExecute = reinterpret_cast<PFN_Execute>(GetProcAddress(HipModule, "DLSSD_RENDEZVOUS_Execute"));
    HipSynchronize =
        reinterpret_cast<PFN_Synchronize>(GetProcAddress(HipModule, "DLSSD_RENDEZVOUS_Synchronize"));
    HipDestroy = reinterpret_cast<PFN_Destroy>(GetProcAddress(HipModule, "DLSSD_RENDEZVOUS_Destroy"));
    HipGetLastErrorStatus =
        reinterpret_cast<PFN_GetLastErrorStatus>(GetProcAddress(HipModule, "DLSSD_RENDEZVOUS_GetLastErrorStatus"));
    HipGetLastErrorText =
        reinterpret_cast<PFN_GetLastErrorText>(GetProcAddress(HipModule, "DLSSD_RENDEZVOUS_GetLastErrorText"));
    if (HipInitialize == nullptr || HipExecute == nullptr || HipSynchronize == nullptr || HipDestroy == nullptr ||
        HipGetLastErrorStatus == nullptr || HipGetLastErrorText == nullptr)
    {
        LOG_ERROR("DLSS-D rendezvous: helper is missing a required export");
        return false;
    }
    return true;
}

bool CreatePipelinesLocked()
{
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.RegisterSpace = 0;
    parameters[1].Constants.Num32BitValues = 3;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = static_cast<UINT>(std::size(parameters));
    rootDesc.pParameters = parameters;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr))
    {
        const char* message = error != nullptr ? static_cast<const char*>(error->GetBufferPointer()) : "unknown";
        LOG_ERROR("DLSS-D rendezvous: root-signature serialization failed 0x{:X} ({})", static_cast<uint32_t>(hr),
                  message);
        return false;
    }
    hr = Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                     IID_PPV_ARGS(&RootSignature));
    if (FAILED(hr))
    {
        LOG_ERROR("DLSS-D rendezvous: CreateRootSignature failed 0x{:X}", static_cast<uint32_t>(hr));
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
    pipelineDesc.pRootSignature = RootSignature.Get();
    pipelineDesc.CS = { DlssdQueueRendezvousSignal_cso, sizeof(DlssdQueueRendezvousSignal_cso) };
    hr = Device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&SignalPipeline));
    if (FAILED(hr))
    {
        LOG_ERROR("DLSS-D rendezvous: signal pipeline creation failed 0x{:X}", static_cast<uint32_t>(hr));
        return false;
    }
    pipelineDesc.CS = { DlssdQueueRendezvousWait_cso, sizeof(DlssdQueueRendezvousWait_cso) };
    hr = Device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&WaitPipeline));
    if (FAILED(hr))
    {
        LOG_ERROR("DLSS-D rendezvous: wait pipeline creation failed 0x{:X}", static_cast<uint32_t>(hr));
        return false;
    }
    return true;
}

bool InitializeLocked(ID3D12Device* device)
{
    if (Initialized)
        return Device.Get() == device;
    if (InitializationFailed || device == nullptr)
        return false;

    Device = device;
    if (!LoadHipHelperLocked())
    {
        InitializationFailed = true;
        ReleaseRuntimeLocked();
        return false;
    }

    D3D12_HEAP_PROPERTIES defaultHeap {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask = 1;
    const auto sharedDesc = BufferDesc(kSharedBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_SHARED, &sharedDesc,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(&SharedBuffer));
    if (FAILED(hr))
    {
        LOG_ERROR("DLSS-D rendezvous: shared buffer creation failed 0x{:X}", static_cast<uint32_t>(hr));
        InitializationFailed = true;
        ReleaseRuntimeLocked();
        return false;
    }

    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    readbackHeap.CreationNodeMask = 1;
    readbackHeap.VisibleNodeMask = 1;
    const auto readbackDesc = BufferDesc(kSharedBytes, D3D12_RESOURCE_FLAG_NONE);
    hr = Device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ReadbackBuffer));
    if (FAILED(hr) || FAILED(ReadbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&ReadbackMapping))))
    {
        LOG_ERROR("DLSS-D rendezvous: readback buffer creation/map failed 0x{:X}", static_cast<uint32_t>(hr));
        InitializationFailed = true;
        ReleaseRuntimeLocked();
        return false;
    }

    if (!CreatePipelinesLocked())
    {
        InitializationFailed = true;
        ReleaseRuntimeLocked();
        return false;
    }

    HANDLE sharedHandle = nullptr;
    hr = Device->CreateSharedHandle(SharedBuffer.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle);
    if (FAILED(hr) || sharedHandle == nullptr)
    {
        LOG_ERROR("DLSS-D rendezvous: CreateSharedHandle failed 0x{:X}", static_cast<uint32_t>(hr));
        InitializationFailed = true;
        ReleaseRuntimeLocked();
        return false;
    }
    const LUID luid = Device->GetAdapterLuid();
    const uint32_t hipResult =
        HipInitialize(luid.LowPart, luid.HighPart, sharedHandle, kSharedBytes, &HipToken);
    CloseHandle(sharedHandle);
    if (hipResult != 0 || HipToken == nullptr)
    {
        LOG_ERROR("DLSS-D rendezvous: HIP shared-buffer initialization failed 0x{:X} ({})", hipResult,
                  HipErrorTextLocked());
        InitializationFailed = true;
        ReleaseRuntimeLocked();
        return false;
    }

    Initialized = true;
    LOG_INFO("DLSS-D rendezvous: initialized synthetic shared-buffer probe slots={} bytes={} d3dWait={} hipWait={} "
             "workIterations={}",
             kSlotCount, kSharedBytes, kD3dWaitIterations, kHipWaitIterations, kWorkIterations);
    return true;
}

void LogSummaryLocked(const char* reason)
{
    LOG_INFO("DLSS-D rendezvous summary reason={} recorded={} submitted={} passed={} hip_timeouts={} d3d_timeouts={} "
             "payload_mismatches={} protocol_mismatches={} hip_launch_failures={} fence_failures={} "
             "slots_unavailable={} runtime_failed={}",
             reason, Stats.recorded, Stats.submitted, Stats.passed, Stats.hipTimeouts, Stats.d3dTimeouts,
             Stats.payloadMismatches, Stats.protocolMismatches, Stats.hipLaunchFailures, Stats.fenceFailures,
             Stats.slotsUnavailable, RuntimeFailed);
}

void ProcessCompletedLocked(ID3D12CommandQueue* queue)
{
    const auto queueIt = Queues.find(queue);
    if (queueIt == Queues.end() || queueIt->second.fence == nullptr || ReadbackMapping == nullptr)
        return;
    const uint64_t completed = queueIt->second.fence->GetCompletedValue();

    for (uint32_t slotIndex = 0; slotIndex < kSlotCount; ++slotIndex)
    {
        SlotState& slot = Slots[slotIndex];
        if (!slot.inUse || !slot.submitted || slot.queue != queue || slot.fenceValue == 0 ||
            slot.fenceValue > completed)
            continue;

        const auto* words = reinterpret_cast<const uint32_t*>(ReadbackMapping + slotIndex * kSlotBytes);
        const bool hipTimeout = words[kHipStatusWord] != 1;
        const bool d3dTimeout = words[kD3dStatusWord] != 1;
        const bool payloadMismatch = words[kPayloadWord] != slot.expectedPayload;
        const bool protocolMismatch = words[kReadyWord] != slot.sequence || words[kDoneWord] != slot.sequence ||
                                      words[kD3dObservedWord] != slot.sequence ||
                                      words[kSequenceMirrorWord] != slot.sequence;
        const bool passed = !hipTimeout && !d3dTimeout && !payloadMismatch && !protocolMismatch;

        if (passed)
            Stats.passed++;
        else
        {
            Stats.hipTimeouts += hipTimeout ? 1 : 0;
            Stats.d3dTimeouts += d3dTimeout ? 1 : 0;
            Stats.payloadMismatches += payloadMismatch ? 1 : 0;
            Stats.protocolMismatches += protocolMismatch ? 1 : 0;
            RuntimeFailed = true;
        }

        const bool logDetail = passed ? (DetailLogs++ < kMaxDetailLogs || Stats.passed % 600 == 0)
                                      : (FailureLogs++ < kMaxDetailLogs || FailureLogs % 300 == 0);
        if (logDetail)
        {
            LOG_INFO("DLSS-D rendezvous result={} handle={} sequence={} slot={} ready={} done={} hip_status={} "
                     "d3d_status={} payload=0x{:08X} expected=0x{:08X} hip_wait_iterations={} "
                     "d3d_wait_iterations={} d3d_observed={}",
                     passed ? "pass" : "fail", slot.handleId, slot.sequence, slotIndex, words[kReadyWord],
                     words[kDoneWord], words[kHipStatusWord], words[kD3dStatusWord], words[kPayloadWord],
                     slot.expectedPayload, words[kHipWaitIterationsWord], words[kD3dWaitIterationsWord],
                     words[kD3dObservedWord]);
        }

        slot = {};
    }
}
} // namespace

bool Enabled() { return Config::Instance()->FSRRTestDlssdQueueRendezvous.value_or_default(); }

void InstallForDevice(ID3D12Device* device)
{
    if (!Enabled() || device == nullptr)
        return;
    std::lock_guard lock(Mutex);
    if (!LoggedEnable)
    {
        LoggedEnable = true;
        LOG_WARN("DLSS-D rendezvous: experimental synthetic D3D12/HIP ordering probe enabled; no DLSS-D image or "
                 "model resource is modified");
    }
    (void) InitializeLocked(device);
}

bool RecordEvaluateTail(uint32_t handleId, ID3D12GraphicsCommandList* commandList)
{
    if (!Enabled() || handleId == 0 || commandList == nullptr || State::Instance().isShuttingDown)
        return false;
    if (commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        LOG_ERROR("DLSS-D rendezvous: only DIRECT evaluate lists are supported");
        return false;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(commandList->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    std::lock_guard lock(Mutex);
    const bool ready = InitializeLocked(device);
    device->Release();
    if (!ready || RuntimeFailed)
        return false;

    uint32_t slotIndex = kSlotCount;
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        if (!Slots[i].inUse)
        {
            slotIndex = i;
            break;
        }
    }
    if (slotIndex == kSlotCount)
    {
        Stats.slotsUnavailable++;
        if (Stats.slotsUnavailable <= kMaxDetailLogs)
            LOG_WARN("DLSS-D rendezvous: no free probe slot; evaluation {} was not instrumented", handleId);
        return false;
    }

    uint32_t sequence = NextSequence++;
    if (sequence == 0)
        sequence = NextSequence++;
    SlotState& slot = Slots[slotIndex];
    slot.inUse = true;
    slot.sequence = sequence;
    slot.handleId = handleId;
    slot.expectedPayload = MakePayload(sequence);

    const uint32_t constants[3] = { slotIndex * kSlotBytes, sequence, kD3dWaitIterations };
    commandList->SetComputeRootSignature(RootSignature.Get());
    commandList->SetComputeRootUnorderedAccessView(0, SharedBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRoot32BitConstants(1, static_cast<UINT>(std::size(constants)), constants, 0);
    commandList->SetPipelineState(SignalPipeline.Get());
    commandList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = SharedBuffer.Get();
    commandList->ResourceBarrier(1, &barrier);
    commandList->SetPipelineState(WaitPipeline.Get());
    commandList->Dispatch(1, 1, 1);
    commandList->ResourceBarrier(1, &barrier);

    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = SharedBuffer.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &barrier);
    commandList->CopyBufferRegion(ReadbackBuffer.Get(), slotIndex * kSlotBytes, SharedBuffer.Get(),
                                  slotIndex * kSlotBytes, kSlotBytes);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commandList->ResourceBarrier(1, &barrier);

    PendingJobs[commandList].push_back(Job { slotIndex, sequence, handleId, commandList });
    Stats.recorded++;
    if (Stats.recorded <= kMaxDetailLogs)
    {
        LOG_INFO("DLSS-D rendezvous: recorded ready/wait tail handle={} sequence={} slot={} cmdList=0x{:X}",
                 handleId, sequence, slotIndex, reinterpret_cast<uintptr_t>(commandList));
    }
    return true;
}

bool SubmissionContainsProbe(uint32_t commandListCount, ID3D12CommandList* const* commandLists)
{
    if (!Enabled() || commandLists == nullptr || commandListCount == 0 || State::Instance().isShuttingDown)
        return false;

    std::lock_guard lock(Mutex);
    for (uint32_t i = 0; i < commandListCount; ++i)
    {
        if (PendingJobs.contains(commandLists[i]))
            return true;
    }
    return false;
}

uint64_t BeforeExecuteCommandLists(ID3D12CommandQueue* queue, uint32_t commandListCount,
                                   ID3D12CommandList* const* commandLists)
{
    if (!Enabled() || queue == nullptr || commandLists == nullptr || commandListCount == 0 ||
        State::Instance().isShuttingDown)
        return 0;

    std::lock_guard lock(Mutex);
    ProcessCompletedLocked(queue);
    std::vector<Job> jobs;
    for (uint32_t i = 0; i < commandListCount; ++i)
    {
        const auto pending = PendingJobs.find(commandLists[i]);
        if (pending == PendingJobs.end())
            continue;
        jobs.insert(jobs.end(), pending->second.begin(), pending->second.end());
        PendingJobs.erase(pending);
    }
    if (jobs.empty())
        return 0;

    uint64_t token = NextSubmissionToken++;
    if (token == 0)
        token = NextSubmissionToken++;
    SubmissionBatches.emplace(token, std::move(jobs));

    auto& batch = SubmissionBatches.at(token);
    for (const Job& job : batch)
    {
        const uint32_t result = HipExecute(HipToken, job.slot * kSlotWords, job.sequence, kHipWaitIterations,
                                           kWorkIterations);
        if (result != 0)
        {
            Stats.hipLaunchFailures++;
            RuntimeFailed = true;
            LOG_ERROR("DLSS-D rendezvous: HIP waiter enqueue failed handle={} sequence={} slot={} result=0x{:X} ({})",
                      job.handleId, job.sequence, job.slot, result, HipErrorTextLocked());
        }
    }
    return token;
}

void AfterExecuteCommandLists(ID3D12CommandQueue* queue, uint64_t submissionToken)
{
    if (submissionToken == 0 || queue == nullptr)
        return;

    std::lock_guard lock(Mutex);
    const auto batchIt = SubmissionBatches.find(submissionToken);
    if (batchIt == SubmissionBatches.end())
        return;
    std::vector<Job> jobs = std::move(batchIt->second);
    SubmissionBatches.erase(batchIt);
    Stats.submitted += jobs.size();

    const auto queueDesc = queue->GetDesc();
    if (queueDesc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT || HipSynchronize == nullptr || HipToken == nullptr)
    {
        LOG_ERROR("DLSS-D rendezvous: submitted probe requires a DIRECT queue and initialized HIP helper");
        RuntimeFailed = true;
        return;
    }

    const uint32_t hipResult = HipSynchronize(HipToken);
    if (hipResult != 0)
    {
        Stats.hipLaunchFailures++;
        RuntimeFailed = true;
        LOG_ERROR("DLSS-D rendezvous: HIP waiter synchronization failed result=0x{:X} ({})", hipResult,
                  HipErrorTextLocked());
    }

    QueueState& queueState = Queues[queue];
    if (queueState.fence == nullptr)
    {
        const HRESULT hr = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queueState.fence));
        if (FAILED(hr))
        {
            Stats.fenceFailures++;
            RuntimeFailed = true;
            LOG_ERROR("DLSS-D rendezvous: queue fence creation failed 0x{:X}", static_cast<uint32_t>(hr));
            return;
        }
    }

    const uint64_t fenceValue = queueState.nextFenceValue++;
    const HRESULT signalResult = queue->Signal(queueState.fence.Get(), fenceValue);
    if (FAILED(signalResult))
    {
        Stats.fenceFailures++;
        RuntimeFailed = true;
        LOG_ERROR("DLSS-D rendezvous: queue fence signal failed 0x{:X}", static_cast<uint32_t>(signalResult));
        return;
    }

    for (const Job& job : jobs)
    {
        SlotState& slot = Slots[job.slot];
        if (slot.inUse && slot.sequence == job.sequence)
        {
            slot.submitted = true;
            slot.queue = queue;
            slot.fenceValue = fenceValue;
        }
    }
}

void Shutdown()
{
    std::lock_guard lock(Mutex);
    if (!LoggedEnable && !Initialized)
        return;
    for (auto& [queue, state] : Queues)
        ProcessCompletedLocked(queue);
    LogSummaryLocked("shutdown");
    ReleaseRuntimeLocked();
}
} // namespace DlssdQueueRendezvous
