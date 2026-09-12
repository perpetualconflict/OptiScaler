#pragma once
#include <stdint.h>

// Versioned extension: do not change the original DlssdRuntimeFrame ABI.
// One lease protects the runtime's single staging/descriptor/HIP allocation set.
struct DlssdFrameTag
{
    uint32_t size = sizeof(DlssdFrameTag);
    uint32_t version = 1;
    uint64_t receiptId = 0;
    uint64_t sourceFrame = 0;
    uint32_t handleId = 0;
    uint32_t featureId = 13;
    uint32_t resetRequested = 0;
    uint32_t inputSubmissionVerified = 0;
};

enum DlssdLeaseRetirement : uint32_t
{
    DlssdLease_InputCancelled = 1u,
    DlssdLease_InputCompleted = 2u,
    DlssdLease_OutputNotPublished = 4u,
    DlssdLease_OutputCompleted = 8u,
};

struct DlssdReadyOutput
{
    uint32_t size = sizeof(DlssdReadyOutput);
    uint32_t version = 1;
    uint64_t receiptId = 0;
    uint64_t sourceFrame = 0;
    uint32_t handleId = 0;
    uint32_t featureId = 13;
    void* resource = nullptr; // AddRef'd ID3D12Resource; caller releases it.
    uint32_t state = 0;
    uint32_t reserved = 0;
};

using PFN_DlssdRuntime_BeginFrameLease = uint32_t(__cdecl*)(const DlssdFrameTag*);
using PFN_DlssdRuntime_VerifyFrameInput = uint32_t(__cdecl*)(const DlssdFrameTag*);
using PFN_DlssdRuntime_GetReadyOutput = uint32_t(__cdecl*)(DlssdReadyOutput*);
using PFN_DlssdRuntime_EndFrameLease = uint32_t(__cdecl*)(uint64_t, uint32_t);
