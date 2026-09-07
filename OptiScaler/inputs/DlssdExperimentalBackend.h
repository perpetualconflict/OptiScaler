#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

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
// copies stay on the caller list. Present only signals a submitted
// frame. The HIP chain does not write the game output. FSR-RR remains
// the presented picture. Dispatched is reserved for a later
// copy-in/output validation gate. Unsupported extents, fingerprints,
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
void Release(uint32_t handleId);
} // namespace DlssdExperimentalBackend
