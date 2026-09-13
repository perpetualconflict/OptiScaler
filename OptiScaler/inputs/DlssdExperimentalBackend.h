#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12Resource;
struct NVSDK_NGX_Parameter;
namespace DlssdOutputHazardTrace { struct PublicationPlan; }

namespace RayReconstruction
{
struct InputSnapshot;
}

// Opt-in experimental translated DLSS-D path at the NGX feature-13 seam.
// After the load window, prepare (hooks + staging) runs on the NGX
// evaluate call so device hooks are not installed from a worker.
// nvcuda load, HIP adapter select, Init, CreateFeature, and sidecar
// EvaluateFeature run on one persistent owner thread. slEvaluateFeature
// must return immediately; it never waits on CreateFeature. Input
// copies and a generation receipt stay on the caller list. Its exact queue
// submission schedules the owner; Present is not submission evidence.
// The HIP owner writes private output. An opt-in matching-frame queue splice
// can copy validated output before the original consumer suffix; unknown
// boundaries or a late/failed owner keep the recorded FSR-RR fallback.
// Unsupported extents, fingerprints,
// missing inputs, or empty hip-only fallbacks fail closed.
// This is not same-command-list HIP insertion.
namespace DlssdExperimentalBackend
{
enum class Decision
{
    NotEnabled,
    Rejected,
    Failed,
    Unpublished,
    Dispatched
};

bool Enabled();
Decision Evaluate(uint32_t handleId, const RayReconstruction::InputSnapshot& snapshot,
                  ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters);
void NotifyFrameSubmitted();
using ExecuteLists = void (*)(ID3D12CommandQueue*, unsigned int, ID3D12CommandList* const*);
bool ExecuteMatchingSubmission(ID3D12CommandQueue* queue, unsigned int count, ID3D12CommandList* const* lists,
                               ExecuteLists execute, const DlssdOutputHazardTrace::PublicationPlan& plan);
void NotifyCommandListReset(ID3D12GraphicsCommandList* list);
// Barrier-observed input states for Execute-time capture (resource states
// are uint32_t D3D12_RESOURCE_STATES to keep this header API-light).
void NoteResourceState(ID3D12Resource* resource, uint32_t stateAfter);
// First hooked Execute after a lease: record sidecar copies plus the color
// probe onto the owned list and submit them on the submitting queue, then
// hand verified leases to the owner. Fail-closed; never blocks the caller.
void TryOwnedCaptureOnExecute(ID3D12CommandQueue* queue);
void Release(uint32_t handleId);
} // namespace DlssdExperimentalBackend
