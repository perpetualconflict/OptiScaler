#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>

#include "../OptiScaler/inputs/DlssdFenceWait.h"

// Isolated prerequisite for the game handoff. The caller serializes methods
// and retains this object plus ALL staging/HIP leases until CanRetire().
// List identity must already be normalized to the interface seen by Execute.
// Reset means successful command-list Reset, never allocator Reset.
namespace DlssdInputReceipt
{
class Receipt
{
  public:
    enum class Phase { Empty, Recorded, Submitting, Submitted, Complete, Cancelled, Failed };

    bool Initialize(ID3D12Device* device, std::uint64_t generation)
    {
        if (!device || generation == 0 || generation == UINT64_MAX || event_ || phase_ != Phase::Empty)
            return false;
        generation_ = generation;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(generation_);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_))))
            return false;
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
            return false;
        void* mapped = nullptr;
        const D3D12_RANGE empty { 0, 0 };
        if (FAILED(upload_->Map(0, &empty, &mapped))) return false;
        std::memcpy(mapped, &generation_, sizeof(generation_));
        const D3D12_RANGE written { 0, sizeof(generation_) };
        upload_->Unmap(0, &written);
        if (FAILED(readback_->Map(0, &empty, &mapped))) return false;
        std::memset(mapped, 0, sizeof(generation_));
        readback_->Unmap(0, &written);
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event_ != nullptr;
    }

    ~Receipt()
    {
        // A violated retention contract must not free GPU-referenced memory.
        // Deliberately leak only this failed diagnostic receipt; integration
        // must also retain the separate staging/HIP leases in that case.
        if (!CanRetire())
        {
            upload_.Detach(); readback_.Detach(); fence_.Detach();
            list_.Detach(); queue_.Detach();
        }
        else if (event_) CloseHandle(event_);
    }
    Receipt() = default;
    Receipt(const Receipt&) = delete;
    Receipt& operator=(const Receipt&) = delete;

    bool Record(ID3D12GraphicsCommandList* list)
    {
        if (!event_ || !list || phase_ != Phase::Empty || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
            return false;
        list_ = list;
        list->CopyBufferRegion(readback_.Get(), 0, upload_.Get(), 0, sizeof(generation_));
        phase_ = Phase::Recorded;
        return true;
    }

    bool BeforeSubmit(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        if (!queue || !lists ||
            queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
        UINT matches = 0;
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == list_.Get()) { ++matches; index_ = i; }
        if (matches != 1) return false;
        if (phase_ != Phase::Recorded)
        {
            if (phase_ != Phase::Cancelled && phase_ != Phase::Empty && !reset_)
            {
                replayed_ = true;
                phase_ = Phase::Failed;
            }
            return false;
        }
        queue_ = queue;
        phase_ = Phase::Submitting;
        return true;
    }

    // Call only immediately after the real ExecuteCommandLists for the
    // matching BeforeSubmit. A fence alone cannot prove a void Execute ran.
    bool AfterSubmit(ID3D12CommandQueue* queue)
    {
        if (phase_ != Phase::Submitting || queue != queue_.Get()) return false;
        if (FAILED(queue->Signal(fence_.Get(), 1))) { phase_ = Phase::Failed; return false; }
        phase_ = Phase::Submitted;
        return true;
    }

    void OnSuccessfulReset(ID3D12GraphicsCommandList* list)
    {
        if (list != list_.Get()) return;
        reset_ = true;
        if (phase_ == Phase::Recorded) phase_ = Phase::Cancelled;
        // Submitted allocations still belong to the GPU, even after Reset.
    }

    bool WaitAndVerify(DWORD timeoutMs)
    {
        if (phase_ != Phase::Submitted || !DlssdFenceWait::Complete(fence_.Get(), 1, event_, timeoutMs))
            return false; // Timeout retains the submitted receipt for retry.
        gpuComplete_ = true;
        void* mapped = nullptr;
        const D3D12_RANGE range { 0, sizeof(generation_) };
        if (FAILED(readback_->Map(0, &range, &mapped))) { phase_ = Phase::Failed; return false; }
        std::uint64_t observed = 0;
        std::memcpy(&observed, mapped, sizeof(observed));
        const D3D12_RANGE empty { 0, 0 };
        readback_->Unmap(0, &empty);
        phase_ = observed == generation_ ? Phase::Complete : Phase::Failed;
        return phase_ == Phase::Complete;
    }

    bool CanRetire() const
    {
        return !replayed_ && (phase_ == Phase::Empty || phase_ == Phase::Cancelled || (reset_ && gpuComplete_));
    }
    Phase State() const { return phase_; }
    // Read-only introspection for submission-mismatch diagnostics. The
    // recorded pointer is only meaningful while the caller retains the
    // receipt; compare, never dereference, outside the receipt lock.
    ID3D12GraphicsCommandList* RecordedList() const
    {
        return const_cast<ID3D12GraphicsCommandList*>(list_.Get());
    }
    UINT ListIndex() const { return index_; }
    ID3D12CommandQueue* SubmissionQueue() const { return queue_.Get(); }

  private:
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_, readback_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    HANDLE event_ = nullptr;
    std::uint64_t generation_ = 0;
    Phase phase_ = Phase::Empty;
    UINT index_ = 0;
    bool reset_ = false;
    bool gpuComplete_ = false;
    bool replayed_ = false;
};
}
