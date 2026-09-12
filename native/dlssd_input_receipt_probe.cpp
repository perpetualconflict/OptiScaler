// Standalone D3D12 receipt tests. No game, NGX, HIP, or vendor model is loaded.
#include "dlssd_input_receipt.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using DlssdInputReceipt::Receipt;
static void Require(bool ok, const char* label)
{
    if (!ok) throw std::runtime_error(label);
}
static void Check(HRESULT hr, const char* label) { Require(SUCCEEDED(hr), label); }

struct List
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    void Create(ID3D12Device* device)
    {
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&list)), "list");
    }
    void ResetCompleted(Receipt& receipt)
    {
        Check(allocator->Reset(), "allocator reset after completion/cancel");
        Check(list->Reset(allocator.Get(), nullptr), "list reset");
        receipt.OnSuccessfulReset(list.Get());
    }
};

int main()
{
    try
    {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer required");
        debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc {};
            Check(candidate->GetDesc1(&desc), "adapter desc");
            if (desc.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                                                  __uuidof(ID3D12Device), nullptr)))
            { adapter = candidate; break; }
        }
        Require(adapter != nullptr, "AMD adapter required");
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
        ComPtr<ID3D12InfoQueue> info;
        Check(device.As(&info), "info queue");
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue, wrongQueue;
        Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
        Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&wrongQueue)), "other queue");
        List current, unrelated;
        current.Create(device.Get()); unrelated.Create(device.Get());
        Check(unrelated.list->Close(), "close unrelated");

        {
            Receipt receipt;
            Require(receipt.Initialize(device.Get(), 101), "init positive");
            Require(receipt.Record(current.list.Get()), "record positive");
            Check(current.list->Close(), "close positive");
            ID3D12CommandList* foreign[] = { unrelated.list.Get() };
            Require(!receipt.BeforeSubmit(queue.Get(), 1, foreign), "unrelated submission rejected");
            ID3D12CommandList* duplicate[] = { current.list.Get(), current.list.Get() };
            Require(!receipt.BeforeSubmit(queue.Get(), 2, duplicate), "duplicate list rejected");
            ID3D12CommandList* lists[] = { unrelated.list.Get(), current.list.Get() };
            Require(receipt.BeforeSubmit(queue.Get(), 2, lists), "exact list match");
            Require(receipt.ListIndex() == 1 && receipt.SubmissionQueue() == queue.Get(), "receipt identity");
            queue->ExecuteCommandLists(2, lists);
            Require(!receipt.AfterSubmit(wrongQueue.Get()), "wrong signal queue rejected");
            Require(receipt.AfterSubmit(queue.Get()), "signal exact queue");
            Require(receipt.WaitAndVerify(5000), "positive marker and fence");
            Require(!receipt.CanRetire(), "closed list can still be replayed before reset");
            current.ResetCompleted(receipt);
            Require(receipt.CanRetire(), "completion plus reset permits retirement");
            std::puts("receipt exact-list-index actual-queue marker=passed");
        }
        {
            Receipt receipt;
            Require(receipt.Initialize(device.Get(), 102) && receipt.Record(current.list.Get()), "init cancel");
            Check(current.list->Close(), "close cancel");
            current.ResetCompleted(receipt);
            Require(receipt.State() == Receipt::Phase::Cancelled && receipt.CanRetire(), "reset cancels unsubmitted");
            ID3D12CommandList* lists[] = { current.list.Get() };
            Require(!receipt.BeforeSubmit(queue.Get(), 1, lists), "cancelled generation cannot match reused pointer");
            std::puts("receipt reset-before-submit pointer-reuse=passed");
        }
        {
            Receipt receipt;
            Require(receipt.Initialize(device.Get(), 103) && receipt.Record(current.list.Get()), "init dropped");
            Check(current.list->Close(), "close dropped");
            ID3D12CommandList* lists[] = { current.list.Get() };
            Require(receipt.BeforeSubmit(queue.Get(), 1, lists), "dropped match");
            // Deliberately omit Execute: even a successful later Signal must
            // not certify that the marker/input-copy list actually executed.
            Require(receipt.AfterSubmit(queue.Get()), "dropped signal");
            Require(!receipt.WaitAndVerify(5000) && receipt.State() == Receipt::Phase::Failed,
                    "signal without execution rejected by marker");
            current.ResetCompleted(receipt);
            Require(receipt.CanRetire(), "dropped list reset and fence permit retirement");
            std::puts("receipt signal-without-execution=correctly-rejected");
        }
        {
            Receipt receipt;
            Require(receipt.Initialize(device.Get(), 104) && receipt.Record(current.list.Get()), "init delayed");
            Check(current.list->Close(), "close delayed");
            ComPtr<ID3D12Fence> blocker;
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)), "blocker");
            Check(queue->Wait(blocker.Get(), 1), "queue wait");
            ID3D12CommandList* lists[] = { current.list.Get() };
            Require(receipt.BeforeSubmit(queue.Get(), 1, lists), "delayed match");
            queue->ExecuteCommandLists(1, lists);
            Require(receipt.AfterSubmit(queue.Get()), "delayed signal");
            // Reset the list with a DIFFERENT allocator while old work is in
            // flight; never reset the in-flight allocator.
            ComPtr<ID3D12CommandAllocator> fresh;
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fresh)), "fresh allocator");
            Check(current.list->Reset(fresh.Get(), nullptr), "reset submitted list");
            receipt.OnSuccessfulReset(current.list.Get());
            const bool premature = receipt.CanRetire() || receipt.WaitAndVerify(0);
            Check(blocker->Signal(1), "unblock queue"); // Always unblock before reporting a test failure.
            Require(!premature, "reset and timeout do not establish completion");
            Require(receipt.WaitAndVerify(5000) && receipt.CanRetire(), "delayed completion retry");
            current.allocator = fresh;
            std::puts("receipt reset-in-flight timeout retention retry=passed");
        }
        Check(current.list->Close(), "close final");
        UINT errors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            Check(info->GetMessage(i, nullptr, &bytes), "message size");
            std::vector<unsigned char> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(i, message, &bytes), "message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            { ++errors; std::printf("D3D_ERROR %s\n", message->pDescription); }
        }
        std::printf("D3D_DEBUG errors=%u discarded=%llu\n", errors,
                    static_cast<unsigned long long>(info->GetNumMessagesDiscardedByMessageCountLimit()));
        Require(errors == 0 && info->GetNumMessagesDiscardedByMessageCountLimit() == 0, "debug audit");
        std::puts("PASS input receipt isolated prerequisite; game integration/publication not established");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
