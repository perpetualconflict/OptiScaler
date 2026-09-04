// Standalone validation of the exact queue sequence used by OptiScaler's
// opt-in DLSS-D rendezvous diagnostic. No game, NGX, or vendor model is loaded.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <cstring>

#include "dlssd_queue_rendezvous_abi.h"

#include "../OptiScaler/denoisers/dx12/shaders/DlssdQueueRendezvousSignal_Shader.h"
#include "../OptiScaler/denoisers/dx12/shaders/DlssdQueueRendezvousWait_Shader.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint32_t kSlotBytes = 64;
constexpr uint32_t kD3dWaitIterations = 4u * 1024u * 1024u;
constexpr uint32_t kHipWaitIterations = 4u * 1024u * 1024u;
constexpr uint32_t kWorkIterations = 4096;

using PFN_Initialize = uint32_t(__cdecl*)(uint32_t, int32_t, void*, uint64_t, void**);
using PFN_Execute = uint32_t(__cdecl*)(void*, uint32_t, uint32_t, uint32_t, uint32_t);
using PFN_Synchronize = uint32_t(__cdecl*)(void*);
using PFN_Destroy = uint32_t(__cdecl*)(void*);
using PFN_GetLastErrorText = const char*(__cdecl*)();
using PFN_GetLastErrorStatus = uint32_t(__cdecl*)();
using HipMalloc = int(__cdecl*)(void**, size_t);
using HipFree = int(__cdecl*)(void*);
using HipMemcpy = int(__cdecl*)(void*, const void*, size_t, int);
using HipMemsetAsync = int(__cdecl*)(void*, int, size_t, void*);

struct CallbackContext
{
    HipMemsetAsync memsetAsync = nullptr;
    void* scratch = nullptr;
    uint32_t calls = 0;
    uint32_t mode = 0;
};

uint32_t __cdecl EnqueueScratch(void* opaque, void* stream, uint32_t sequence)
{
    auto& context = *static_cast<CallbackContext*>(opaque);
    ++context.calls;
    // Real async GPU work, also deliberately queued before reporting failure.
    const int result = context.memsetAsync(context.scratch, static_cast<int>(sequence), 256, stream);
    if (result != 0)
        return static_cast<uint32_t>(result);
    if (context.mode == 2)
        throw std::runtime_error("intentional callback exception");
    return context.mode == 1 ? 0xBAD00001u : 0u;
}

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        char message[160] {};
        std::snprintf(message, sizeof(message), "%s failed: 0x%08X", operation, static_cast<uint32_t>(result));
        throw std::runtime_error(message);
    }
}

D3D12_RESOURCE_DESC BufferDesc(uint64_t size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
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
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::fwprintf(stderr, L"usage: %s <DlssdQueueRendezvousHip.dll>\n", argv[0]);
        return 2;
    }

    HMODULE hipModule = nullptr;
    void* hipToken = nullptr;
    HANDLE sharedHandle = nullptr;
    HANDLE completionEvent = nullptr;
    uint8_t* readback = nullptr;
    ComPtr<ID3D12Resource> readbackBuffer;
    // Keep the shared allocation alive through error-path HIP destruction too.
    ComPtr<ID3D12Resource> sharedBuffer;
    CallbackContext callback;
    HipFree hipFree = nullptr;
    try
    {
        ComPtr<IDXGIFactory6> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                     IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            Check(candidate->GetDesc1(&desc), "GetDesc1");
            if (desc.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                                                      __uuidof(ID3D12Device), nullptr)))
            {
                adapter = candidate;
                break;
            }
        }
        if (adapter == nullptr)
            throw std::runtime_error("no usable AMD D3D12 adapter found");

        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        ComPtr<ID3D12CommandAllocator> allocator;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
              "CreateCommandAllocator");
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list)),
              "CreateCommandList");

        D3D12_HEAP_PROPERTIES defaultHeap {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        defaultHeap.CreationNodeMask = 1;
        defaultHeap.VisibleNodeMask = 1;
        const auto sharedDesc = BufferDesc(kSlotBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Check(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_SHARED, &sharedDesc,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              IID_PPV_ARGS(&sharedBuffer)),
              "CreateCommittedResource(shared)");

        D3D12_HEAP_PROPERTIES readbackHeap {};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        readbackHeap.CreationNodeMask = 1;
        readbackHeap.VisibleNodeMask = 1;
        const auto readbackDesc = BufferDesc(kSlotBytes, D3D12_RESOURCE_FLAG_NONE);
        Check(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&readbackBuffer)),
              "CreateCommittedResource(readback)");
        Check(readbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&readback)), "Map(readback)");

        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.Num32BitValues = 3;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = parameters;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;
        Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error),
              "D3D12SerializeRootSignature");
        ComPtr<ID3D12RootSignature> rootSignature;
        Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                          IID_PPV_ARGS(&rootSignature)),
              "CreateRootSignature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
        pipelineDesc.pRootSignature = rootSignature.Get();
        pipelineDesc.CS = { DlssdQueueRendezvousSignal_cso, sizeof(DlssdQueueRendezvousSignal_cso) };
        ComPtr<ID3D12PipelineState> signalPipeline;
        Check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&signalPipeline)),
              "CreateComputePipelineState(signal)");
        pipelineDesc.CS = { DlssdQueueRendezvousWait_cso, sizeof(DlssdQueueRendezvousWait_cso) };
        ComPtr<ID3D12PipelineState> waitPipeline;
        Check(device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&waitPipeline)),
              "CreateComputePipelineState(wait)");

        Check(device->CreateSharedHandle(sharedBuffer.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle),
              "CreateSharedHandle");
        hipModule = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (hipModule == nullptr)
            throw std::runtime_error("unable to load HIP rendezvous companion");
        const auto initialize = reinterpret_cast<PFN_Initialize>(GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_Initialize"));
        const auto execute = reinterpret_cast<PFN_Execute>(GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_Execute"));
        const auto executeWorkload = reinterpret_cast<DlssdQueueRendezvousAbi::ExecuteWorkload>(
            GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_ExecuteWorkload"));
        const auto errorStatus = reinterpret_cast<PFN_GetLastErrorStatus>(
            GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_GetLastErrorStatus"));
        const auto synchronize =
            reinterpret_cast<PFN_Synchronize>(GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_Synchronize"));
        const auto destroy = reinterpret_cast<PFN_Destroy>(GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_Destroy"));
        const auto errorText =
            reinterpret_cast<PFN_GetLastErrorText>(GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_GetLastErrorText"));
        if (initialize == nullptr || execute == nullptr || synchronize == nullptr || destroy == nullptr ||
            errorText == nullptr || executeWorkload == nullptr || errorStatus == nullptr)
            throw std::runtime_error("HIP rendezvous companion export missing");
        const LUID luid = device->GetAdapterLuid();
        uint32_t hipResult = initialize(luid.LowPart, luid.HighPart, sharedHandle, kSlotBytes, &hipToken);
        CloseHandle(sharedHandle);
        sharedHandle = nullptr;
        if (hipResult != 0 || hipToken == nullptr)
            throw std::runtime_error(errorText());

        const HMODULE runtime = GetModuleHandleW(L"amdhip64_7.dll");
        const auto hipMalloc = reinterpret_cast<HipMalloc>(GetProcAddress(runtime, "hipMalloc"));
        hipFree = reinterpret_cast<HipFree>(GetProcAddress(runtime, "hipFree"));
        const auto hipMemcpy = reinterpret_cast<HipMemcpy>(GetProcAddress(runtime, "hipMemcpy"));
        callback.memsetAsync = reinterpret_cast<HipMemsetAsync>(GetProcAddress(runtime, "hipMemsetAsync"));
        if (!hipMalloc || !hipFree || !hipMemcpy || !callback.memsetAsync ||
            hipMalloc(&callback.scratch, 256) != 0)
            throw std::runtime_error("scratch HIP runtime setup failed");
        if (callback.memsetAsync(callback.scratch, 0, 256, nullptr) != 0 || synchronize(hipToken) != 0)
            throw std::runtime_error("scratch workload warmup failed");

        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (completionEvent == nullptr)
            throw std::runtime_error("CreateEvent failed");

        for (uint32_t sequence = 1; sequence <= 36; ++sequence)
        {
            // A poisoned token cannot be reused. Start a fresh lifetime only for
            // the separate exception test, after both queues have drained.
            if (sequence == 35)
            {
                if (destroy(hipToken) != 0)
                    throw std::runtime_error(errorText());
                hipToken = nullptr;
                Check(device->CreateSharedHandle(sharedBuffer.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle),
                      "CreateSharedHandle(exception test)");
                hipResult = initialize(luid.LowPart, luid.HighPart, sharedHandle, kSlotBytes, &hipToken);
                CloseHandle(sharedHandle);
                sharedHandle = nullptr;
                if (hipResult != 0)
                    throw std::runtime_error(errorText());
            }
            if (sequence > 1)
            {
                Check(allocator->Reset(), "CommandAllocator::Reset");
                Check(list->Reset(allocator.Get(), nullptr), "CommandList::Reset");
            }
            const uint32_t constants[3] = { 0, sequence, kD3dWaitIterations };
            list->SetComputeRootSignature(rootSignature.Get());
            list->SetComputeRootUnorderedAccessView(0, sharedBuffer->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstants(1, 3, constants, 0);
            list->SetPipelineState(signalPipeline.Get());
            list->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = sharedBuffer.Get();
            list->ResourceBarrier(1, &barrier);
            list->SetPipelineState(waitPipeline.Get());
            list->Dispatch(1, 1, 1);
            list->ResourceBarrier(1, &barrier);
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = sharedBuffer.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier);
            list->CopyBufferRegion(readbackBuffer.Get(), 0, sharedBuffer.Get(), 0, kSlotBytes);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
            Check(list->Close(), "CommandList::Close");

            const bool intentionalTimeout = sequence == 36;
            const bool generic = sequence >= 33 && sequence <= 35;
            const bool callbackFailure = sequence == 34 || sequence == 35;
            if (!intentionalTimeout)
            {
                callback.mode = sequence == 35 ? 2u : (sequence == 34 ? 1u : 0u);
                callback.calls = 0;
                hipResult = generic ? executeWorkload(hipToken, 0, sequence, kHipWaitIterations,
                                                      EnqueueScratch, &callback)
                                    : execute(hipToken, 0, sequence, kHipWaitIterations, kWorkIterations);
                if (hipResult != (callbackFailure ? DlssdQueueRendezvousAbi::WorkloadEnqueueFailed : 0u) ||
                    errorStatus() != hipResult || (generic && callback.calls != 1))
                    throw std::runtime_error(errorText());
                if (callbackFailure)
                {
                    if (std::strlen(errorText()) == 0 ||
                        executeWorkload(hipToken, 0, sequence + 100, kHipWaitIterations,
                                        EnqueueScratch, &callback) != DlssdQueueRendezvousAbi::TokenFailed ||
                        callback.calls != 1)
                        throw std::runtime_error("failed callback did not poison token");
                }
            }
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            if (!intentionalTimeout)
            {
                hipResult = synchronize(hipToken);
                if (hipResult != 0)
                    throw std::runtime_error(errorText());
            }
            Check(queue->Signal(fence.Get(), sequence), "CommandQueue::Signal");
            Check(fence->SetEventOnCompletion(sequence, completionEvent), "Fence::SetEventOnCompletion");
            if (WaitForSingleObject(completionEvent, 10000) != WAIT_OBJECT_0)
                throw std::runtime_error("D3D12 completion timed out");

            const auto* words = reinterpret_cast<const uint32_t*>(readback);
            Check(device->GetDeviceRemovedReason(), "GetDeviceRemovedReason");
            if (generic)
            {
                unsigned char scratch[256] {};
                if (hipMemcpy(scratch, callback.scratch, sizeof(scratch), 2) != 0)
                    throw std::runtime_error("scratch readback failed");
                for (const auto byte : scratch)
                    if (byte != static_cast<unsigned char>(sequence))
                        throw std::runtime_error("queued callback scratch payload mismatch");
            }
            const uint32_t expected = generic ? 0u : MakePayload(sequence);
            const bool passed = intentionalTimeout
                                    ? words[0] == sequence && words[1] == 0 && words[2] == 0 && words[3] == 2 &&
                                          words[4] == 0 && words[5] == 0 && words[8] == 0
                                    : words[0] == sequence && words[1] == sequence &&
                                          words[2] == (callbackFailure ? 3u : 1u) &&
                                          words[3] == 1 && words[4] == expected && words[5] == sequence &&
                                          words[8] == sequence;
            std::printf("sequence=%u result=%s payload=%08X expected=%08X hip_wait=%u d3d_wait=%u\n", sequence,
                        passed ? (intentionalTimeout ? "bounded-timeout-pass" :
                                  callbackFailure ? "callback-failure-pass" :
                                  generic ? "callback-success-pass" : "pass") : "fail", words[4],
                        intentionalTimeout ? 0 : expected, words[6], words[7]);
            if (!passed)
                throw std::runtime_error("rendezvous result mismatch");
        }

        if (hipFree(callback.scratch) != 0)
            throw std::runtime_error("hipFree(scratch) failed");
        callback.scratch = nullptr;
        const uint32_t destroyResult = destroy(hipToken);
        hipToken = nullptr;
        if (destroyResult != 0)
            throw std::runtime_error(errorText());
        readbackBuffer->Unmap(0, nullptr);
        readback = nullptr;
        CloseHandle(completionEvent);
        FreeLibrary(hipModule);
        std::puts("rendezvous standalone result=pass iterations=32 callback_success=1 callback_failure=1 "
                  "callback_exception=1 poisoned_token_checks=2 bounded_timeout_checks=1");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "rendezvous standalone result=fail error=%s\n", error.what());
        if (readbackBuffer != nullptr && readback != nullptr)
            readbackBuffer->Unmap(0, nullptr);
        if (completionEvent != nullptr)
            CloseHandle(completionEvent);
        if (sharedHandle != nullptr)
            CloseHandle(sharedHandle);
        if (hipModule != nullptr)
        {
            const auto destroy = reinterpret_cast<PFN_Destroy>(GetProcAddress(hipModule, "DLSSD_RENDEZVOUS_Destroy"));
            if (destroy != nullptr && hipToken != nullptr)
                (void) destroy(hipToken);
            if (hipFree != nullptr && callback.scratch != nullptr)
                (void) hipFree(callback.scratch);
            FreeLibrary(hipModule);
        }
        return 1;
    }
}
