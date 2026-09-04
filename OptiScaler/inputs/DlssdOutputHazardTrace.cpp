#include "pch.h"

#include "DlssdOutputHazardTrace.h"
#include "DlssdQueueRendezvous.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <d3d12.h>
#include <detours/detours.h>
#include <nvsdk_ngx_params.h>

#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace DlssdOutputHazardTrace
{
namespace
{
constexpr uint32_t kMaxDetailLogs = 8;
constexpr size_t kMaxTrackedHeaps = 256;
constexpr size_t kMaxTrackedDescriptors = 512;

enum class Phase : uint8_t
{
    Idle = 0,
    InEvaluate,
    AfterEvaluateUnsubmitted,
    EvalListSubmitted,
};

enum class ConsumerKind : uint8_t
{
    None = 0,
    SameList,
    SameSubmissionOtherList,
    LaterSubmission,
};

enum class BindKind : uint8_t
{
    None = 0,
    ResolvedOutput,
    UnresolvedTable,
};

const char* PhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::InEvaluate:
        return "in_evaluate";
    case Phase::AfterEvaluateUnsubmitted:
        return "after_eval_before_submit";
    case Phase::EvalListSubmitted:
        return "after_submit";
    default:
        return "before_evaluate";
    }
}

const char* QueueTypeName(D3D12_COMMAND_LIST_TYPE type)
{
    switch (type)
    {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
        return "DIRECT";
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:
        return "BUNDLE";
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        return "COMPUTE";
    case D3D12_COMMAND_LIST_TYPE_COPY:
        return "COPY";
    default:
        return "OTHER";
    }
}

const char* ConsumerKindName(ConsumerKind kind)
{
    switch (kind)
    {
    case ConsumerKind::SameList:
        return "same_list";
    case ConsumerKind::SameSubmissionOtherList:
        return "same_submission_other_list";
    case ConsumerKind::LaterSubmission:
        return "later_submission";
    default:
        return "none";
    }
}

bool IsReadLikeState(D3D12_RESOURCE_STATES state)
{
    constexpr D3D12_RESOURCE_STATES readBits =
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE |
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE | D3D12_RESOURCE_STATE_GENERIC_READ | D3D12_RESOURCE_STATE_PRESENT;
    return state == D3D12_RESOURCE_STATE_COMMON || (state & readBits) != 0;
}

struct HandleState
{
    NVSDK_NGX_Feature featureId {};
    ID3D12Resource* output = nullptr;
    uint64_t outputGpuVa = 0;
    ID3D12GraphicsCommandList* evalList = nullptr;
    uint32_t evalCount = 0;
    uint32_t lastEvalSeq = 0;
    uint64_t lastEvalSubmitSeq = 0;
    uintptr_t lastEvalQueue = 0;
    D3D12_COMMAND_LIST_TYPE lastEvalQueueType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Phase phase = Phase::Idle;
    bool outputLogged = false;
    bool evalListTransitionLogged = false;
    uint32_t matchLogs = 0;
    uint32_t sameListLogs = 0;
    uint32_t laterLogs = 0;
    uint32_t unresolvedLogs = 0;
    uint32_t missedSubmitCount = 0;
    uint32_t matchingSubmissions = 0;
    uint32_t sameListConsumers = 0;
    uint32_t sameSubmissionOtherListConsumers = 0;
    uint32_t laterSubmissionConsumers = 0;
    uint32_t unresolvedEvents = 0;
    uint32_t closedEvalListsWithoutConsumer = 0;
    ConsumerKind firstConsumerKind = ConsumerKind::None;
    uint32_t firstConsumerEval = 0;
    uint64_t firstConsumerSubmit = 0;
    uintptr_t firstConsumerList = 0;
    std::string firstConsumerEvent;
    uintptr_t firstMatchingList = 0;
    uint64_t firstMatchingSubmit = 0;
    uintptr_t lastSeenEvalList = 0;
    std::vector<std::pair<ID3D12GraphicsCommandList*, std::string>> pendingOtherListUses;
};

struct HeapSpan
{
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uint64_t cpuStart = 0;
    uint64_t gpuStart = 0;
    UINT count = 0;
    UINT increment = 0;
};

struct PendingBind
{
    BindKind kind = BindKind::None;
    uint32_t handleId = 0;
};

struct TrackedDescriptor
{
    uint32_t handleId = 0;
    ID3D12Resource* resource = nullptr;
};

using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_ResourceBarrier = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using PFN_CopyResource = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using PFN_CopyTextureRegion = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*,
                                                       UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*,
                                                       const D3D12_BOX*);
using PFN_CopyBufferRegion = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64,
                                                      ID3D12Resource*, UINT64, UINT64);
using PFN_ResolveSubresource = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT,
                                                        ID3D12Resource*, UINT, DXGI_FORMAT);
using PFN_Dispatch = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_ExecuteIndirect = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT,
                                                     ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using PFN_ExecuteBundle = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
using PFN_Close = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_SetComputeRootDescriptorTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                                   D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_SetGraphicsRootDescriptorTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                                    D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_SetComputeRootView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE*);
using PFN_CreateDescriptorHeap = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                             void**);
using PFN_CreateShaderResourceView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                              const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                                              D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateUnorderedAccessView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*,
                                                               const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CreateRenderTargetView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*,
                                                            const D3D12_RENDER_TARGET_VIEW_DESC*,
                                                            D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*,
                                                     const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
                                                     D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);

std::mutex StateMutex;
std::unordered_map<uint32_t, HandleState> Handles;
std::vector<HeapSpan> Heaps;
std::unordered_map<uint64_t, TrackedDescriptor> CpuDescriptors;
std::unordered_map<uint64_t, TrackedDescriptor> GpuDescriptors;
std::unordered_map<ID3D12GraphicsCommandList*, PendingBind> PendingBinds;
std::atomic<uint64_t> SubmissionSeq { 0 };
bool HooksInstalled = false;
bool HookInstallFailed = false;
bool LoggedEnable = false;
bool LoggedDisclaimer = false;
bool DescriptorMapCapped = false;

PFN_ExecuteCommandLists o_ExecuteCommandLists = nullptr;
PFN_ResourceBarrier o_ResourceBarrier = nullptr;
PFN_CopyResource o_CopyResource = nullptr;
PFN_CopyTextureRegion o_CopyTextureRegion = nullptr;
PFN_CopyBufferRegion o_CopyBufferRegion = nullptr;
PFN_ResolveSubresource o_ResolveSubresource = nullptr;
PFN_Dispatch o_Dispatch = nullptr;
PFN_DrawInstanced o_DrawInstanced = nullptr;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
PFN_ExecuteIndirect o_ExecuteIndirect = nullptr;
PFN_ExecuteBundle o_ExecuteBundle = nullptr;
PFN_Close o_Close = nullptr;
PFN_SetComputeRootDescriptorTable o_SetComputeRootDescriptorTable = nullptr;
PFN_SetGraphicsRootDescriptorTable o_SetGraphicsRootDescriptorTable = nullptr;
PFN_SetComputeRootView o_SetComputeRootSRV = nullptr;
PFN_SetComputeRootView o_SetComputeRootUAV = nullptr;
PFN_SetComputeRootView o_SetGraphicsRootSRV = nullptr;
PFN_SetComputeRootView o_SetGraphicsRootUAV = nullptr;
PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
PFN_CreateDescriptorHeap o_CreateDescriptorHeap = nullptr;
PFN_CreateShaderResourceView o_CreateShaderResourceView = nullptr;
PFN_CreateUnorderedAccessView o_CreateUnorderedAccessView = nullptr;
PFN_CreateRenderTargetView o_CreateRenderTargetView = nullptr;
PFN_CopyDescriptors o_CopyDescriptors = nullptr;
PFN_CopyDescriptorsSimple o_CopyDescriptorsSimple = nullptr;

void LogDisclaimerOnce()
{
    if (LoggedDisclaimer)
        return;
    LoggedDisclaimer = true;
    LOG_INFO("DLSS-D output-order: feature-13 command-list vs later-submission diagnostic only; "
             "this does not prove descriptor-to-HIP conversion, kernel correctness, or same-list HIP insertion");
}

HandleState* FindHandleByOutputLocked(ID3D12Resource* resource)
{
    if (resource == nullptr)
        return nullptr;
    for (auto& [id, handle] : Handles)
    {
        if (handle.output == resource)
            return &handle;
    }
    return nullptr;
}

uint32_t HandleIdOf(const HandleState* handle)
{
    for (const auto& [id, value] : Handles)
    {
        if (&value == handle)
            return id;
    }
    return 0;
}

Phase ClassifyLocked(const HandleState& handle, ID3D12GraphicsCommandList* commandList)
{
    if (handle.phase == Phase::InEvaluate && commandList == handle.evalList)
        return Phase::InEvaluate;
    if (handle.phase == Phase::AfterEvaluateUnsubmitted && commandList == handle.evalList)
        return Phase::AfterEvaluateUnsubmitted;
    if (handle.phase == Phase::EvalListSubmitted)
        return Phase::EvalListSubmitted;
    if (handle.phase == Phase::AfterEvaluateUnsubmitted)
        return Phase::AfterEvaluateUnsubmitted;
    return Phase::Idle;
}

ConsumerKind KindFromPhaseLocked(const HandleState& handle, ID3D12GraphicsCommandList* commandList)
{
    const Phase phase = ClassifyLocked(handle, commandList);
    if (phase == Phase::AfterEvaluateUnsubmitted && commandList == handle.evalList)
        return ConsumerKind::SameList;
    if (phase == Phase::AfterEvaluateUnsubmitted)
        return ConsumerKind::SameSubmissionOtherList;
    if (phase == Phase::EvalListSubmitted)
        return ConsumerKind::LaterSubmission;
    return ConsumerKind::None;
}

void NoteFirstConsumerLocked(HandleState& handle, ConsumerKind kind, ID3D12GraphicsCommandList* commandList,
                             const char* eventName)
{
    if (handle.firstConsumerKind != ConsumerKind::None)
        return;
    handle.firstConsumerKind = kind;
    handle.firstConsumerEval = handle.lastEvalSeq;
    handle.firstConsumerSubmit = handle.lastEvalSubmitSeq;
    handle.firstConsumerList = reinterpret_cast<uintptr_t>(commandList);
    handle.firstConsumerEvent = eventName;
}

void CountConsumerLocked(HandleState& handle, ConsumerKind kind)
{
    switch (kind)
    {
    case ConsumerKind::SameList:
        handle.sameListConsumers++;
        break;
    case ConsumerKind::SameSubmissionOtherList:
        handle.sameSubmissionOtherListConsumers++;
        break;
    case ConsumerKind::LaterSubmission:
        handle.laterSubmissionConsumers++;
        break;
    default:
        break;
    }
}

bool ShouldLogConsumerLocked(HandleState& handle, ConsumerKind kind)
{
    switch (kind)
    {
    case ConsumerKind::SameList:
        if (handle.sameListLogs >= kMaxDetailLogs)
            return false;
        handle.sameListLogs++;
        return true;
    case ConsumerKind::LaterSubmission:
    case ConsumerKind::SameSubmissionOtherList:
        if (handle.laterLogs >= kMaxDetailLogs)
            return false;
        handle.laterLogs++;
        return true;
    default:
        return false;
    }
}

void RecordResolvedUseLocked(HandleState& handle, uint32_t handleId, ID3D12GraphicsCommandList* commandList,
                             const char* eventName, const char* detail)
{
    const ConsumerKind kind = KindFromPhaseLocked(handle, commandList);
    if (kind == ConsumerKind::None)
        return;

    if (kind == ConsumerKind::SameSubmissionOtherList)
    {
        handle.pendingOtherListUses.emplace_back(commandList, eventName);
        if (handle.laterLogs < kMaxDetailLogs)
        {
            handle.laterLogs++;
            LOG_INFO("DLSS-D output-order pending-other-list handle={} eval={} cmdList=0x{:X} event={} {}", handleId,
                     handle.lastEvalSeq, reinterpret_cast<uintptr_t>(commandList), eventName, detail);
        }
        return;
    }

    CountConsumerLocked(handle, kind);
    NoteFirstConsumerLocked(handle, kind, commandList, eventName);
    if (!ShouldLogConsumerLocked(handle, kind))
        return;

    LOG_INFO("DLSS-D output-order consume handle={} eval={} phase={} kind={} cmdList=0x{:X} event={} {}", handleId,
             handle.lastEvalSeq, PhaseName(ClassifyLocked(handle, commandList)), ConsumerKindName(kind),
             reinterpret_cast<uintptr_t>(commandList), eventName, detail);
}

void RecordUnresolvedLocked(HandleState& handle, uint32_t handleId, ID3D12GraphicsCommandList* commandList,
                            const char* eventName)
{
    handle.unresolvedEvents++;
    if (handle.unresolvedLogs >= kMaxDetailLogs)
        return;
    handle.unresolvedLogs++;
    LOG_INFO("DLSS-D output-order unresolved handle={} eval={} phase={} cmdList=0x{:X} event={}", handleId,
             handle.lastEvalSeq, PhaseName(ClassifyLocked(handle, commandList)),
             reinterpret_cast<uintptr_t>(commandList), eventName);
}

const HeapSpan* FindHeapByCpuLocked(uint64_t cpuPtr)
{
    for (const auto& heap : Heaps)
    {
        if (heap.increment == 0 || heap.count == 0)
            continue;
        if (cpuPtr < heap.cpuStart)
            continue;
        const uint64_t offset = cpuPtr - heap.cpuStart;
        if (offset % heap.increment != 0)
            continue;
        if (offset / heap.increment < heap.count)
            return &heap;
    }
    return nullptr;
}

void RememberDescriptorLocked(uint32_t handleId, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    if (resource == nullptr || cpu.ptr == 0)
        return;
    if (CpuDescriptors.size() >= kMaxTrackedDescriptors)
    {
        if (!DescriptorMapCapped)
        {
            DescriptorMapCapped = true;
            LOG_WARN("DLSS-D output-order: descriptor map capped at {}", kMaxTrackedDescriptors);
        }
        return;
    }

    CpuDescriptors[cpu.ptr] = TrackedDescriptor { handleId, resource };
    if (const HeapSpan* heap = FindHeapByCpuLocked(cpu.ptr); heap != nullptr && heap->gpuStart != 0)
    {
        const uint64_t index = (cpu.ptr - heap->cpuStart) / heap->increment;
        GpuDescriptors[heap->gpuStart + index * heap->increment] = TrackedDescriptor { handleId, resource };
    }
}

void CopyTrackedRangeLocked(D3D12_CPU_DESCRIPTOR_HANDLE destStart, D3D12_CPU_DESCRIPTOR_HANDLE srcStart, UINT count,
                            D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    UINT increment = 0;
    if (const HeapSpan* heap = FindHeapByCpuLocked(srcStart.ptr); heap != nullptr)
        increment = heap->increment;
    else if (const HeapSpan* heap = FindHeapByCpuLocked(destStart.ptr); heap != nullptr)
        increment = heap->increment;
    if (increment == 0)
        increment = (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) ? 16u : 32u;

    for (UINT i = 0; i < count; ++i)
    {
        const uint64_t src = srcStart.ptr + static_cast<uint64_t>(i) * increment;
        const auto it = CpuDescriptors.find(src);
        if (it == CpuDescriptors.end())
            continue;
        D3D12_CPU_DESCRIPTOR_HANDLE dest { destStart.ptr + static_cast<uint64_t>(i) * increment };
        RememberDescriptorLocked(it->second.handleId, it->second.resource, dest);
    }
}

ID3D12Resource* TryGetOutput(const NVSDK_NGX_Parameter& parameters)
{
    ID3D12Resource* resource = nullptr;
    if (parameters.Get("Output", &resource) == NVSDK_NGX_Result_Success && resource != nullptr)
        return resource;

    void* pointer = nullptr;
    if (parameters.Get("Output", &pointer) == NVSDK_NGX_Result_Success && pointer != nullptr)
        return static_cast<ID3D12Resource*>(pointer);
    return nullptr;
}

const char* VerdictName(const HandleState& handle)
{
    if (handle.sameListConsumers > 0)
        return "same_list_consumer_observed";
    if (handle.laterSubmissionConsumers > 0 && handle.sameSubmissionOtherListConsumers == 0 &&
        handle.sameListConsumers == 0)
        return "later_submission_candidate";
    return "inconclusive";
}

void LogSummaryLocked(uint32_t handleId, const HandleState& handle, const char* reason)
{
    LOG_INFO("DLSS-D output-order summary handle={} evals={} output=0x{:X} evalList=0x{:X} "
             "first_match_submit={} last_match_submit={} matching_submits={} "
             "same_list={} same_submission_other_list={} later_submission={} unresolved={} "
             "missed_submit={} closed_without_consumer={} first_consumer_kind={} "
             "first_consumer_eval={} first_consumer_submit={} first_consumer_list=0x{:X} "
             "first_consumer_event={} verdict={} reason={}",
             handleId, handle.evalCount, reinterpret_cast<uintptr_t>(handle.output),
             handle.firstMatchingList != 0 ? handle.firstMatchingList : reinterpret_cast<uintptr_t>(handle.evalList),
             handle.firstMatchingSubmit, handle.lastEvalSubmitSeq, handle.matchingSubmissions, handle.sameListConsumers,
             handle.sameSubmissionOtherListConsumers, handle.laterSubmissionConsumers, handle.unresolvedEvents,
             handle.missedSubmitCount, handle.closedEvalListsWithoutConsumer, ConsumerKindName(handle.firstConsumerKind),
             handle.firstConsumerEval, handle.firstConsumerSubmit, handle.firstConsumerList,
             handle.firstConsumerEvent.empty() ? "none" : handle.firstConsumerEvent.c_str(), VerdictName(handle),
             reason);
}

void BeginEvaluateLocked(uint32_t handleId, NVSDK_NGX_Feature featureId, ID3D12GraphicsCommandList* commandList,
                         ID3D12Resource* output)
{
    auto& handle = Handles[handleId];
    if (handle.phase == Phase::AfterEvaluateUnsubmitted)
        handle.missedSubmitCount++;

    handle.featureId = featureId;
    handle.evalCount++;
    handle.lastEvalSeq = handle.evalCount;
    handle.evalList = commandList;
    handle.phase = Phase::InEvaluate;
    handle.output = output;
    handle.outputGpuVa = 0;
    handle.pendingOtherListUses.clear();
    if (output != nullptr)
        handle.outputGpuVa = output->GetGPUVirtualAddress();

    if (!handle.outputLogged)
    {
        handle.outputLogged = true;
        LOG_INFO("DLSS-D output-order output handle={} resource=0x{:X} gpuVa=0x{:X}", handleId,
                 reinterpret_cast<uintptr_t>(output), handle.outputGpuVa);
    }
    else if (handle.lastSeenEvalList != reinterpret_cast<uintptr_t>(commandList) && !handle.evalListTransitionLogged)
    {
        handle.evalListTransitionLogged = true;
        LOG_INFO("DLSS-D output-order cmdList changed handle={} from=0x{:X} to=0x{:X} eval={}", handleId,
                 handle.lastSeenEvalList, reinterpret_cast<uintptr_t>(commandList), handle.lastEvalSeq);
    }
    handle.lastSeenEvalList = reinterpret_cast<uintptr_t>(commandList);
}

void EndEvaluateLocked(uint32_t handleId, ID3D12GraphicsCommandList* commandList)
{
    auto it = Handles.find(handleId);
    if (it == Handles.end())
        return;
    auto& handle = it->second;
    if (handle.phase == Phase::InEvaluate && handle.evalList == commandList)
        handle.phase = Phase::AfterEvaluateUnsubmitted;
}

void ObserveExecuteLocked(ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists)
{
    const uint64_t seq = ++SubmissionSeq;
    D3D12_COMMAND_QUEUE_DESC desc {};
    D3D12_COMMAND_LIST_TYPE queueType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (queue != nullptr)
    {
        desc = queue->GetDesc();
        queueType = desc.Type;
    }

    for (auto& [handleId, handle] : Handles)
    {
        if (handle.featureId != NVSDK_NGX_Feature_RayReconstruction || handle.evalList == nullptr)
            continue;

        int matchIndex = -1;
        for (UINT i = 0; i < numLists; ++i)
        {
            if (lists[i] == handle.evalList)
                matchIndex = static_cast<int>(i);
        }

        if (matchIndex >= 0)
        {
            handle.matchingSubmissions++;
            handle.lastEvalSubmitSeq = seq;
            handle.lastEvalQueue = reinterpret_cast<uintptr_t>(queue);
            handle.lastEvalQueueType = queueType;
            if (handle.firstMatchingList == 0)
            {
                handle.firstMatchingList = reinterpret_cast<uintptr_t>(handle.evalList);
                handle.firstMatchingSubmit = seq;
            }
            handle.phase = Phase::EvalListSubmitted;

            if (handle.matchLogs < kMaxDetailLogs)
            {
                handle.matchLogs++;
                std::ostringstream listsText;
                for (UINT i = 0; i < numLists; ++i)
                {
                    if (i != 0)
                        listsText << ',';
                    listsText << "0x" << std::hex << reinterpret_cast<uintptr_t>(lists[i]);
                }
                LOG_INFO("DLSS-D output-order submit seq={} queue=0x{:X} type={} lists={} match_index={} "
                         "handle={} eval={} evalList=0x{:X} [{}]",
                         seq, reinterpret_cast<uintptr_t>(queue), QueueTypeName(queueType), numLists, matchIndex, handleId,
                         handle.lastEvalSeq, reinterpret_cast<uintptr_t>(handle.evalList), listsText.str());
            }
        }

        auto pending = handle.pendingOtherListUses.begin();
        while (pending != handle.pendingOtherListUses.end())
        {
            bool inThisSubmit = false;
            for (UINT i = 0; i < numLists; ++i)
            {
                if (lists[i] == pending->first)
                {
                    inThisSubmit = true;
                    break;
                }
            }

            ConsumerKind resolved = ConsumerKind::None;
            if (matchIndex >= 0 && inThisSubmit)
                resolved = ConsumerKind::SameSubmissionOtherList;
            else if (handle.phase == Phase::EvalListSubmitted && inThisSubmit)
                resolved = ConsumerKind::LaterSubmission;

            if (resolved == ConsumerKind::None)
            {
                ++pending;
                continue;
            }

            CountConsumerLocked(handle, resolved);
            NoteFirstConsumerLocked(handle, resolved, pending->first, pending->second.c_str());
            if (ShouldLogConsumerLocked(handle, resolved))
            {
                LOG_INFO("DLSS-D output-order consume handle={} eval={} kind={} cmdList=0x{:X} event={} submit={}",
                         handleId, handle.lastEvalSeq, ConsumerKindName(resolved),
                         reinterpret_cast<uintptr_t>(pending->first), pending->second, seq);
            }
            pending = handle.pendingOtherListUses.erase(pending);
        }
    }
}

void ObserveBarrierLocked(ID3D12GraphicsCommandList* commandList, UINT numBarriers,
                          const D3D12_RESOURCE_BARRIER* barriers)
{
    if (barriers == nullptr)
        return;
    for (UINT i = 0; i < numBarriers; ++i)
    {
        const auto& barrier = barriers[i];
        ID3D12Resource* resource = nullptr;
        const char* eventName = "barrier";
        std::string detail;
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            resource = barrier.Transition.pResource;
            eventName = IsReadLikeState(barrier.Transition.StateAfter) ? "barrier_to_read" : "barrier_to_write";
            detail = std::format("before=0x{:X} after=0x{:X}", static_cast<unsigned>(barrier.Transition.StateBefore),
                                 static_cast<unsigned>(barrier.Transition.StateAfter));
        }
        else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
        {
            if (HandleState* handle = FindHandleByOutputLocked(barrier.Aliasing.pResourceBefore); handle != nullptr)
            {
                RecordResolvedUseLocked(*handle, HandleIdOf(handle), commandList, "aliasing_barrier_before", "");
            }
            resource = barrier.Aliasing.pResourceAfter;
            eventName = "aliasing_barrier_after";
        }
        else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
        {
            resource = barrier.UAV.pResource;
            eventName = "uav_barrier";
        }

        HandleState* handle = FindHandleByOutputLocked(resource);
        if (handle == nullptr)
            continue;
        RecordResolvedUseLocked(*handle, HandleIdOf(handle), commandList, eventName, detail.c_str());
    }
}

void ObserveCopyLocked(ID3D12GraphicsCommandList* commandList, ID3D12Resource* dst, ID3D12Resource* src,
                       const char* eventName)
{
    if (HandleState* handle = FindHandleByOutputLocked(src); handle != nullptr)
        RecordResolvedUseLocked(*handle, HandleIdOf(handle), commandList, eventName, "src");
    if (HandleState* handle = FindHandleByOutputLocked(dst); handle != nullptr)
        RecordResolvedUseLocked(*handle, HandleIdOf(handle), commandList, eventName, "dst");
}

void ObserveRootViewLocked(ID3D12GraphicsCommandList* commandList, D3D12_GPU_VIRTUAL_ADDRESS address,
                           const char* eventName)
{
    for (auto& [handleId, handle] : Handles)
    {
        if (handle.outputGpuVa == 0 || address < handle.outputGpuVa || address >= handle.outputGpuVa + 16)
            continue;
        PendingBinds[commandList] = PendingBind { BindKind::ResolvedOutput, handleId };
        RecordResolvedUseLocked(handle, handleId, commandList, eventName, "");
    }
}

void ObserveTableLocked(ID3D12GraphicsCommandList* commandList, D3D12_GPU_DESCRIPTOR_HANDLE gpu, const char* eventName)
{
    const auto exact = GpuDescriptors.find(gpu.ptr);
    if (exact == GpuDescriptors.end())
        return;
    auto it = Handles.find(exact->second.handleId);
    if (it == Handles.end())
        return;
    PendingBinds[commandList] = PendingBind { BindKind::ResolvedOutput, exact->second.handleId };
    RecordResolvedUseLocked(it->second, exact->second.handleId, commandList, eventName, "resolved_gpu_handle");
}

void ObserveCpuHandlesLocked(ID3D12GraphicsCommandList* commandList, const D3D12_CPU_DESCRIPTOR_HANDLE* handles,
                             UINT count, const char* eventName)
{
    if (handles == nullptr)
        return;
    for (UINT i = 0; i < count; ++i)
    {
        const auto it = CpuDescriptors.find(handles[i].ptr);
        if (it == CpuDescriptors.end())
            continue;
        auto handleIt = Handles.find(it->second.handleId);
        if (handleIt == Handles.end())
            continue;
        RecordResolvedUseLocked(handleIt->second, it->second.handleId, commandList, eventName, "");
    }
}

void ObserveDispatchLocked(ID3D12GraphicsCommandList* commandList, const char* eventName)
{
    const auto pending = PendingBinds.find(commandList);
    if (pending == PendingBinds.end() || pending->second.kind == BindKind::None)
        return;

    const uint32_t handleId = pending->second.handleId;
    const BindKind kind = pending->second.kind;
    pending->second = {};
    auto it = Handles.find(handleId);
    if (it == Handles.end())
        return;
    if (kind == BindKind::UnresolvedTable)
        RecordUnresolvedLocked(it->second, handleId, commandList, eventName);
    else
        RecordResolvedUseLocked(it->second, handleId, commandList, eventName, "after_resolved_bind");
}

void ObserveCloseLocked(ID3D12GraphicsCommandList* commandList)
{
    PendingBinds.erase(commandList);
    for (auto& [handleId, handle] : Handles)
    {
        if (handle.evalList != commandList || handle.phase != Phase::AfterEvaluateUnsubmitted)
            continue;
        if (handle.sameListConsumers == 0)
            handle.closedEvalListsWithoutConsumer++;
    }
}

void hkExecuteCommandLists(ID3D12CommandQueue* This, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists)
{
    // The experimental rendezvous deliberately serializes only instrumented
    // queue submissions. Its HIP waiter must be enqueued immediately before the
    // matching D3D12 submission, with no other probe submission interleaved.
    static std::mutex rendezvousSubmissionMutex;
    std::unique_lock rendezvousLock(rendezvousSubmissionMutex, std::defer_lock);
    if (DlssdQueueRendezvous::SubmissionContainsProbe(NumCommandLists, ppCommandLists))
        rendezvousLock.lock();

    const uint64_t rendezvousToken = DlssdQueueRendezvous::BeforeExecuteCommandLists(
        This, NumCommandLists, ppCommandLists);
    if (Enabled() && !State::Instance().isShuttingDown && ppCommandLists != nullptr && NumCommandLists > 0)
    {
        std::lock_guard lock(StateMutex);
        ObserveExecuteLocked(This, NumCommandLists, ppCommandLists);
    }
    o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);
    DlssdQueueRendezvous::AfterExecuteCommandLists(This, rendezvousToken);
}

void hkResourceBarrier(ID3D12GraphicsCommandList* This, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveBarrierLocked(This, NumBarriers, pBarriers);
    }
    o_ResourceBarrier(This, NumBarriers, pBarriers);
}

void hkCopyResource(ID3D12GraphicsCommandList* This, ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveCopyLocked(This, pDstResource, pSrcResource, "CopyResource");
    }
    o_CopyResource(This, pDstResource, pSrcResource);
}

void hkCopyTextureRegion(ID3D12GraphicsCommandList* This, const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY,
                         UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveCopyLocked(This, pDst != nullptr ? pDst->pResource : nullptr, pSrc != nullptr ? pSrc->pResource : nullptr,
                          "CopyTextureRegion");
    }
    o_CopyTextureRegion(This, pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}

void hkCopyBufferRegion(ID3D12GraphicsCommandList* This, ID3D12Resource* pDstBuffer, UINT64 DstOffset,
                        ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveCopyLocked(This, pDstBuffer, pSrcBuffer, "CopyBufferRegion");
    }
    o_CopyBufferRegion(This, pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
}

void hkResolveSubresource(ID3D12GraphicsCommandList* This, ID3D12Resource* pDstResource, UINT DstSubresource,
                          ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveCopyLocked(This, pDstResource, pSrcResource, "ResolveSubresource");
    }
    o_ResolveSubresource(This, pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}

void hkDispatch(ID3D12GraphicsCommandList* This, UINT x, UINT y, UINT z)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveDispatchLocked(This, "Dispatch");
    }
    o_Dispatch(This, x, y, z);
}

void hkDrawInstanced(ID3D12GraphicsCommandList* This, UINT a, UINT b, UINT c, UINT d)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveDispatchLocked(This, "DrawInstanced");
    }
    o_DrawInstanced(This, a, b, c, d);
}

void hkDrawIndexedInstanced(ID3D12GraphicsCommandList* This, UINT a, UINT b, UINT c, INT d, UINT e)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveDispatchLocked(This, "DrawIndexedInstanced");
    }
    o_DrawIndexedInstanced(This, a, b, c, d, e);
}

void hkExecuteIndirect(ID3D12GraphicsCommandList* This, ID3D12CommandSignature* signature, UINT count,
                       ID3D12Resource* argument, UINT64 argumentOffset, ID3D12Resource* countBuffer, UINT64 countOffset)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        const auto pending = PendingBinds.find(This);
        if (pending != PendingBinds.end() && pending->second.kind != BindKind::None)
        {
            ObserveDispatchLocked(This, "ExecuteIndirect");
        }
        else
        {
            for (auto& [handleId, handle] : Handles)
            {
                if (handle.evalList == This && KindFromPhaseLocked(handle, This) == ConsumerKind::SameList)
                    RecordUnresolvedLocked(handle, handleId, This, "ExecuteIndirect");
            }
        }
    }
    o_ExecuteIndirect(This, signature, count, argument, argumentOffset, countBuffer, countOffset);
}

void hkExecuteBundle(ID3D12GraphicsCommandList* This, ID3D12GraphicsCommandList* bundle)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        for (auto& [handleId, handle] : Handles)
        {
            if (handle.evalList == This && KindFromPhaseLocked(handle, This) == ConsumerKind::SameList)
                RecordUnresolvedLocked(handle, handleId, This, "ExecuteBundle");
        }
    }
    o_ExecuteBundle(This, bundle);
}

HRESULT hkClose(ID3D12GraphicsCommandList* This)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveCloseLocked(This);
    }
    return o_Close(This);
}

void hkSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveTableLocked(This, gpu, "SetComputeRootDescriptorTable");
    }
    o_SetComputeRootDescriptorTable(This, index, gpu);
}

void hkSetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* This, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveTableLocked(This, gpu, "SetGraphicsRootDescriptorTable");
    }
    o_SetGraphicsRootDescriptorTable(This, index, gpu);
}

void hkSetComputeRootSRV(ID3D12GraphicsCommandList* This, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveRootViewLocked(This, address, "SetComputeRootShaderResourceView");
    }
    o_SetComputeRootSRV(This, index, address);
}

void hkSetComputeRootUAV(ID3D12GraphicsCommandList* This, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveRootViewLocked(This, address, "SetComputeRootUnorderedAccessView");
    }
    o_SetComputeRootUAV(This, index, address);
}

void hkSetGraphicsRootSRV(ID3D12GraphicsCommandList* This, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveRootViewLocked(This, address, "SetGraphicsRootShaderResourceView");
    }
    o_SetGraphicsRootSRV(This, index, address);
}

void hkSetGraphicsRootUAV(ID3D12GraphicsCommandList* This, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveRootViewLocked(This, address, "SetGraphicsRootUnorderedAccessView");
    }
    o_SetGraphicsRootUAV(This, index, address);
}

void hkOMSetRenderTargets(ID3D12GraphicsCommandList* This, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                          BOOL singleHandle, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        ObserveCpuHandlesLocked(This, rtvs, count, "OMSetRenderTargets");
        if (dsv != nullptr)
            ObserveCpuHandlesLocked(This, dsv, 1, "OMSetRenderTargets_dsv");
    }
    o_OMSetRenderTargets(This, count, rtvs, singleHandle, dsv);
}

HRESULT hkCreateDescriptorHeap(ID3D12Device* This, const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid,
                               void** ppvHeap)
{
    const HRESULT result = o_CreateDescriptorHeap(This, pDescriptorHeapDesc, riid, ppvHeap);
    if (FAILED(result) || ppvHeap == nullptr || *ppvHeap == nullptr || pDescriptorHeapDesc == nullptr)
        return result;

    auto* heap = static_cast<ID3D12DescriptorHeap*>(*ppvHeap);
    HeapSpan span;
    span.heap = heap;
    span.type = pDescriptorHeapDesc->Type;
    span.count = pDescriptorHeapDesc->NumDescriptors;
    span.increment = This->GetDescriptorHandleIncrementSize(pDescriptorHeapDesc->Type);
    span.cpuStart = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if ((pDescriptorHeapDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0)
        span.gpuStart = heap->GetGPUDescriptorHandleForHeapStart().ptr;

    std::lock_guard lock(StateMutex);
    if (Heaps.size() < kMaxTrackedHeaps)
        Heaps.push_back(span);
    return result;
}

void TrackCreatedView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    std::lock_guard lock(StateMutex);
    HandleState* handle = FindHandleByOutputLocked(resource);
    if (handle == nullptr)
        return;
    RememberDescriptorLocked(HandleIdOf(handle), resource, dest);
}

void hkCreateShaderResourceView(ID3D12Device* This, ID3D12Resource* pResource,
                                const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    o_CreateShaderResourceView(This, pResource, pDesc, DestDescriptor);
    TrackCreatedView(pResource, DestDescriptor);
}

void hkCreateUnorderedAccessView(ID3D12Device* This, ID3D12Resource* pResource, ID3D12Resource* pCounterResource,
                                 const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                 D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    o_CreateUnorderedAccessView(This, pResource, pCounterResource, pDesc, DestDescriptor);
    TrackCreatedView(pResource, DestDescriptor);
}

void hkCreateRenderTargetView(ID3D12Device* This, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                              D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    o_CreateRenderTargetView(This, pResource, pDesc, DestDescriptor);
    TrackCreatedView(pResource, DestDescriptor);
}

void hkCopyDescriptors(ID3D12Device* This, UINT NumDestDescriptorRanges,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts,
                       const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, const UINT* pSrcDescriptorRangeSizes,
                       D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    if (!State::Instance().isShuttingDown && pDestDescriptorRangeStarts != nullptr &&
        pSrcDescriptorRangeStarts != nullptr)
    {
        std::lock_guard lock(StateMutex);
        UINT destRange = 0, srcRange = 0, destOffset = 0, srcOffset = 0;
        const UINT destSize0 = pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[0] : 1;
        const UINT srcSize0 = pSrcDescriptorRangeSizes ? pSrcDescriptorRangeSizes[0] : 1;
        UINT destRemaining = destSize0;
        UINT srcRemaining = srcSize0;
        UINT copied = 0;
        UINT totalDest = 0;
        for (UINT i = 0; i < NumDestDescriptorRanges; ++i)
            totalDest += pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[i] : 1;
        while (copied < totalDest && destRange < NumDestDescriptorRanges && srcRange < NumSrcDescriptorRanges)
        {
            const UINT take = destRemaining < srcRemaining ? destRemaining : srcRemaining;
            const UINT increment = This->GetDescriptorHandleIncrementSize(DescriptorHeapsType);
            D3D12_CPU_DESCRIPTOR_HANDLE dest { pDestDescriptorRangeStarts[destRange].ptr +
                                               static_cast<uint64_t>(destOffset) * increment };
            D3D12_CPU_DESCRIPTOR_HANDLE src { pSrcDescriptorRangeStarts[srcRange].ptr +
                                              static_cast<uint64_t>(srcOffset) * increment };
            CopyTrackedRangeLocked(dest, src, take, DescriptorHeapsType);
            copied += take;
            destOffset += take;
            srcOffset += take;
            destRemaining -= take;
            srcRemaining -= take;
            if (destRemaining == 0)
            {
                destRange++;
                destOffset = 0;
                destRemaining = (destRange < NumDestDescriptorRanges)
                                    ? (pDestDescriptorRangeSizes ? pDestDescriptorRangeSizes[destRange] : 1)
                                    : 0;
            }
            if (srcRemaining == 0)
            {
                srcRange++;
                srcOffset = 0;
                srcRemaining = (srcRange < NumSrcDescriptorRanges)
                                   ? (pSrcDescriptorRangeSizes ? pSrcDescriptorRangeSizes[srcRange] : 1)
                                   : 0;
            }
        }
    }
    o_CopyDescriptors(This, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
                      NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);
}

void hkCopyDescriptorsSimple(ID3D12Device* This, UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart,
                             D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart,
                             D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
    if (!State::Instance().isShuttingDown)
    {
        std::lock_guard lock(StateMutex);
        CopyTrackedRangeLocked(DestDescriptorRangeStart, SrcDescriptorRangeStart, NumDescriptors, DescriptorHeapsType);
    }
    o_CopyDescriptorsSimple(This, NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType);
}

bool Attach(PVOID* pointer, PVOID detour)
{
    return pointer != nullptr && *pointer != nullptr && DetourAttach(pointer, detour) == NO_ERROR;
}

void InstallHooksLocked(ID3D12Device* device)
{
    if (HooksInstalled || HookInstallFailed || device == nullptr)
        return;

    ID3D12Device* realDevice = device;
    IUnknown* realUnknown = nullptr;
    if (Util::CheckForRealObject("DlssdOutputHazardTrace", device, &realUnknown) && realUnknown != nullptr)
        realDevice = static_cast<ID3D12Device*>(realUnknown);

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(realDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))) || queue == nullptr)
        return;

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* commandList = nullptr;
    if (FAILED(realDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(realDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                             IID_PPV_ARGS(&commandList))))
    {
        if (queue != nullptr)
            queue->Release();
        if (allocator != nullptr)
            allocator->Release();
        return;
    }

    PVOID* queueVTable = *reinterpret_cast<PVOID**>(queue);
    PVOID* listVTable = *reinterpret_cast<PVOID**>(commandList);
    PVOID* deviceVTable = *reinterpret_cast<PVOID**>(realDevice);

    o_ExecuteCommandLists = reinterpret_cast<PFN_ExecuteCommandLists>(queueVTable[10]);
    o_Close = reinterpret_cast<PFN_Close>(listVTable[9]);
    o_DrawInstanced = reinterpret_cast<PFN_DrawInstanced>(listVTable[12]);
    o_DrawIndexedInstanced = reinterpret_cast<PFN_DrawIndexedInstanced>(listVTable[13]);
    o_Dispatch = reinterpret_cast<PFN_Dispatch>(listVTable[14]);
    o_CopyBufferRegion = reinterpret_cast<PFN_CopyBufferRegion>(listVTable[15]);
    o_CopyTextureRegion = reinterpret_cast<PFN_CopyTextureRegion>(listVTable[16]);
    o_CopyResource = reinterpret_cast<PFN_CopyResource>(listVTable[17]);
    o_ResolveSubresource = reinterpret_cast<PFN_ResolveSubresource>(listVTable[19]);
    o_ResourceBarrier = reinterpret_cast<PFN_ResourceBarrier>(listVTable[26]);
    o_ExecuteBundle = reinterpret_cast<PFN_ExecuteBundle>(listVTable[27]);
    o_SetComputeRootDescriptorTable = reinterpret_cast<PFN_SetComputeRootDescriptorTable>(listVTable[31]);
    o_SetGraphicsRootDescriptorTable = reinterpret_cast<PFN_SetGraphicsRootDescriptorTable>(listVTable[32]);
    o_SetComputeRootSRV = reinterpret_cast<PFN_SetComputeRootView>(listVTable[39]);
    o_SetGraphicsRootSRV = reinterpret_cast<PFN_SetComputeRootView>(listVTable[40]);
    o_SetComputeRootUAV = reinterpret_cast<PFN_SetComputeRootView>(listVTable[41]);
    o_SetGraphicsRootUAV = reinterpret_cast<PFN_SetComputeRootView>(listVTable[42]);
    o_OMSetRenderTargets = reinterpret_cast<PFN_OMSetRenderTargets>(listVTable[46]);
    o_ExecuteIndirect = reinterpret_cast<PFN_ExecuteIndirect>(listVTable[59]);
    o_CreateDescriptorHeap = reinterpret_cast<PFN_CreateDescriptorHeap>(deviceVTable[14]);
    o_CreateShaderResourceView = reinterpret_cast<PFN_CreateShaderResourceView>(deviceVTable[18]);
    o_CreateUnorderedAccessView = reinterpret_cast<PFN_CreateUnorderedAccessView>(deviceVTable[19]);
    o_CreateRenderTargetView = reinterpret_cast<PFN_CreateRenderTargetView>(deviceVTable[20]);
    o_CopyDescriptors = reinterpret_cast<PFN_CopyDescriptors>(deviceVTable[23]);
    o_CopyDescriptorsSimple = reinterpret_cast<PFN_CopyDescriptorsSimple>(deviceVTable[24]);

    commandList->Close();
    commandList->Release();
    allocator->Release();
    queue->Release();
    commandList = nullptr;
    allocator = nullptr;
    queue = nullptr;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    Attach(reinterpret_cast<PVOID*>(&o_ExecuteCommandLists), hkExecuteCommandLists);
    if (Enabled())
    {
        Attach(reinterpret_cast<PVOID*>(&o_ResourceBarrier), hkResourceBarrier);
        Attach(reinterpret_cast<PVOID*>(&o_CopyResource), hkCopyResource);
        Attach(reinterpret_cast<PVOID*>(&o_CopyTextureRegion), hkCopyTextureRegion);
        Attach(reinterpret_cast<PVOID*>(&o_CopyBufferRegion), hkCopyBufferRegion);
        Attach(reinterpret_cast<PVOID*>(&o_ResolveSubresource), hkResolveSubresource);
        Attach(reinterpret_cast<PVOID*>(&o_Dispatch), hkDispatch);
        Attach(reinterpret_cast<PVOID*>(&o_DrawInstanced), hkDrawInstanced);
        Attach(reinterpret_cast<PVOID*>(&o_DrawIndexedInstanced), hkDrawIndexedInstanced);
        Attach(reinterpret_cast<PVOID*>(&o_ExecuteIndirect), hkExecuteIndirect);
        Attach(reinterpret_cast<PVOID*>(&o_ExecuteBundle), hkExecuteBundle);
        Attach(reinterpret_cast<PVOID*>(&o_Close), hkClose);
        Attach(reinterpret_cast<PVOID*>(&o_SetComputeRootDescriptorTable), hkSetComputeRootDescriptorTable);
        Attach(reinterpret_cast<PVOID*>(&o_SetGraphicsRootDescriptorTable), hkSetGraphicsRootDescriptorTable);
        Attach(reinterpret_cast<PVOID*>(&o_SetComputeRootSRV), hkSetComputeRootSRV);
        Attach(reinterpret_cast<PVOID*>(&o_SetComputeRootUAV), hkSetComputeRootUAV);
        Attach(reinterpret_cast<PVOID*>(&o_SetGraphicsRootSRV), hkSetGraphicsRootSRV);
        Attach(reinterpret_cast<PVOID*>(&o_SetGraphicsRootUAV), hkSetGraphicsRootUAV);
        Attach(reinterpret_cast<PVOID*>(&o_OMSetRenderTargets), hkOMSetRenderTargets);
        Attach(reinterpret_cast<PVOID*>(&o_CreateDescriptorHeap), hkCreateDescriptorHeap);
        Attach(reinterpret_cast<PVOID*>(&o_CreateShaderResourceView), hkCreateShaderResourceView);
        Attach(reinterpret_cast<PVOID*>(&o_CreateUnorderedAccessView), hkCreateUnorderedAccessView);
        Attach(reinterpret_cast<PVOID*>(&o_CreateRenderTargetView), hkCreateRenderTargetView);
        Attach(reinterpret_cast<PVOID*>(&o_CopyDescriptors), hkCopyDescriptors);
        Attach(reinterpret_cast<PVOID*>(&o_CopyDescriptorsSimple), hkCopyDescriptorsSimple);
    }

    const LONG error = DetourTransactionCommit();
    if (error != NO_ERROR)
    {
        LOG_ERROR("DLSS-D output-order: hook install failed {:X}", static_cast<unsigned>(error));
        HookInstallFailed = true;
        return;
    }

    HooksInstalled = true;
    LOG_INFO("DLSS-D diagnostics: installed ExecuteCommandLists hook (output-order={}, rendezvous={})", Enabled(),
             DlssdQueueRendezvous::Enabled());
}
} // namespace

bool Enabled() { return Config::Instance()->FSRRTraceDlssdOutputOrdering.value_or_default(); }

void InstallForDevice(ID3D12Device* device)
{
    if (device == nullptr)
        return;

    DlssdQueueRendezvous::InstallForDevice(device);
    if (!Enabled() && !DlssdQueueRendezvous::Enabled())
        return;

    std::lock_guard lock(StateMutex);
    if (!LoggedEnable)
    {
        LoggedEnable = true;
        LogDisclaimerOnce();
        LOG_INFO("DLSS-D output-order: enabled");
    }
    InstallHooksLocked(device);
}

void OnCreate(uint32_t handleId, NVSDK_NGX_Feature featureId, ID3D12GraphicsCommandList* commandList)
{
    if (featureId != NVSDK_NGX_Feature_RayReconstruction || handleId == 0)
        return;

    if (commandList != nullptr)
    {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(commandList->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
        {
            InstallForDevice(device);
            device->Release();
        }
    }

    if (!Enabled())
        return;

    std::lock_guard lock(StateMutex);
    auto& handle = Handles[handleId];
    handle.featureId = featureId;
    LOG_INFO("DLSS-D output-order create handle={} cmdList=0x{:X}", handleId,
             reinterpret_cast<uintptr_t>(commandList));
}

void OnRelease(uint32_t handleId)
{
    if (!Enabled())
        return;

    std::lock_guard lock(StateMutex);
    const auto it = Handles.find(handleId);
    if (it == Handles.end())
        return;
    LogSummaryLocked(handleId, it->second, "release");
    Handles.erase(it);
}

void Shutdown()
{
    DlssdQueueRendezvous::Shutdown();
    if (!Enabled())
        return;

    std::lock_guard lock(StateMutex);
    for (const auto& [handleId, handle] : Handles)
        LogSummaryLocked(handleId, handle, "shutdown");
}

ScopedEvaluate::ScopedEvaluate(uint32_t handleId, NVSDK_NGX_Feature featureId, ID3D12GraphicsCommandList* commandList,
                               const NVSDK_NGX_Parameter* parameters)
    : _handleId(handleId), _featureId(featureId), _commandList(commandList)
{
    if (!Enabled() || featureId != NVSDK_NGX_Feature_RayReconstruction || commandList == nullptr ||
        parameters == nullptr)
        return;

    ID3D12Device* device = nullptr;
    if (SUCCEEDED(commandList->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
    {
        InstallForDevice(device);
        device->Release();
    }

    ID3D12Resource* output = TryGetOutput(*parameters);
    std::lock_guard lock(StateMutex);
    LogDisclaimerOnce();
    BeginEvaluateLocked(handleId, featureId, commandList, output);
    _active = true;
}

ScopedEvaluate::~ScopedEvaluate()
{
    if (!_active)
        return;
    std::lock_guard lock(StateMutex);
    EndEvaluateLocked(_handleId, _commandList);
}
} // namespace DlssdOutputHazardTrace
