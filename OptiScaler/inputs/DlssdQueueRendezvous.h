#pragma once

#include <cstdint>

struct ID3D12CommandList;
struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

// Opt-in synthetic proof for the D3D12-list/HIP-stream rendezvous needed by
// direct DLSS-D translation. It never reads or modifies DLSS-D image resources.
namespace DlssdQueueRendezvous
{
bool Enabled();
void InstallForDevice(ID3D12Device* device);
bool RecordEvaluateTail(uint32_t handleId, ID3D12GraphicsCommandList* commandList);
bool SubmissionContainsProbe(uint32_t commandListCount, ID3D12CommandList* const* commandLists);
uint64_t BeforeExecuteCommandLists(ID3D12CommandQueue* queue, uint32_t commandListCount,
                                   ID3D12CommandList* const* commandLists);
void AfterExecuteCommandLists(ID3D12CommandQueue* queue, uint64_t submissionToken);
void Shutdown();
} // namespace DlssdQueueRendezvous
