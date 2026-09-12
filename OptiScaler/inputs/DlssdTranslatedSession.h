#pragma once

#include <cstdint>

#include "../../native/dlssd_frame_lease.h"

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

namespace RayReconstruction
{
struct InputSnapshot;
}

namespace DlssdTranslatedSession
{
bool EnsurePrepared(ID3D12Device* device, uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth,
                    uint32_t outputHeight, const char** error);
bool StartFinishAttach(const char** error);
bool FinishAttachOnOwnerThread(const char** error);
void BindCudaOnThisThread();
void AbandonHungCreate();
bool IsAttached();
bool IsPrepared();
bool IsCreating();
long long CreatingElapsedMs();
// Late-abandon cleanup, called only when no thread is inside the runtime
// (the hung owner has exited). Runs the normal sidecar release for a
// previously abandoned prepare so bridge registrations do not leak into
// process detach. Poison stays, so this never re-arms attach.
bool ReleaseAbandonedLate();
// Host create-watchdog progress: allocation heartbeats plus translated
// functions created. Returns false when the loaded sidecar predates the
// progress export; counters are zero then.
bool CreateProgress(uint32_t* allocHeartbeats, uint32_t* functionsCreated);
bool AttachLoadBypassActive();
HMODULE LoadSidecarNvapiForAttach();
HMODULE NvapiHandleForAttach();
bool BeginFrameLease(const DlssdFrameTag& tag, const char** error);
bool VerifyFrameInput(const DlssdFrameTag& tag, const char** error);
bool GetReadyOutput(DlssdReadyOutput& output, const char** error);
bool EndFrameLease(uint64_t receiptId, uint32_t retirement, const char** error);
bool Evaluate(ID3D12GraphicsCommandList* commandList, const RayReconstruction::InputSnapshot& snapshot,
              NVSDK_NGX_Parameter* parameters, bool publishOutput, const char** error,
              uint32_t runtimeFlags = 0);
void Release();

struct ScopedAttachLoadBypass
{
    ScopedAttachLoadBypass();
    ~ScopedAttachLoadBypass();

    ScopedAttachLoadBypass(const ScopedAttachLoadBypass&) = delete;
    ScopedAttachLoadBypass& operator=(const ScopedAttachLoadBypass&) = delete;
};
}
