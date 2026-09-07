#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Must match ZLUDA_RR_Investigation/native/dlssd_translated_runtime.h.
enum DlssdRuntimeSemantic : uint32_t
{
    DlssdRuntimeSemantic_Color = 0,
    DlssdRuntimeSemantic_Output = 1,
    DlssdRuntimeSemantic_Depth = 2,
    DlssdRuntimeSemantic_MotionVectors = 3,
    DlssdRuntimeSemantic_Normals = 4,
    DlssdRuntimeSemantic_DiffuseAlbedo = 5,
    DlssdRuntimeSemantic_SpecularAlbedo = 6,
    DlssdRuntimeSemantic_SpecularHitDistance = 7,
    DlssdRuntimeSemantic_ColorBeforeParticles = 8,
    DlssdRuntimeSemantic_BiasMask = 9,
    DlssdRuntimeSemantic_SssGuide = 10,
};

struct DlssdRuntimeResource
{
    void* resource;
    uint32_t semantic;
    uint32_t subrectX;
    uint32_t subrectY;
    uint32_t sourceState;
    uint32_t sourceStateValid;
    uint32_t copyWidth;
    uint32_t copyHeight;
    const char* ngxKey;
};

enum DlssdRuntimeFrameFlags : uint32_t
{
    DlssdRuntimeFrame_PublishOutput = 1u << 0,
    DlssdRuntimeFrame_RequireValidation = 1u << 1,
    DlssdRuntimeFrame_CopyOnCallerList = 1u << 2,
    DlssdRuntimeFrame_SkipInputCopy = 1u << 3,
};

struct DlssdRuntimeFrame
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t reset;
    float jitterX;
    float jitterY;
    float motionScaleX;
    float motionScaleY;
    float preExposure;
    float exposureScale;
    float sharpness;
    const float* worldToView;
    const float* viewToClip;
    void* commandList;
    const DlssdRuntimeResource* resources;
    uint32_t resourceCount;
    const float* invViewProjection;
    const float* clipToPrevClip;
    uint32_t flags;
    float frameTimeDelta;
    uint32_t indicatorInvertX;
    uint32_t indicatorInvertY;
    uint32_t depthType;
    uint32_t roughnessMode;
    uint32_t denoiseMode;
    uint32_t perfQuality;
    uint32_t liveScalars;
};

using PFN_DlssdRuntime_Attach = uint32_t(__cdecl*)(void*, const wchar_t*, const wchar_t*, const wchar_t*,
                                                   const wchar_t*, uint32_t, uint32_t, uint32_t, uint32_t);
using PFN_DlssdRuntime_PrepareAttach = uint32_t(__cdecl*)(void*, const wchar_t*, const wchar_t*, const wchar_t*,
                                                          const wchar_t*, uint32_t, uint32_t, uint32_t, uint32_t);
using PFN_DlssdRuntime_FinishAttach = uint32_t(__cdecl*)();
using PFN_DlssdRuntime_CreateProgress = uint32_t(__cdecl*)(uint32_t*);
using PFN_DlssdRuntime_Evaluate = uint32_t(__cdecl*)(const DlssdRuntimeFrame*);
using PFN_DlssdRuntime_Release = uint32_t(__cdecl*)();
using PFN_DlssdRuntime_LastError = const char*(__cdecl*)();
using PFN_DlssdRuntime_Log = void(__cdecl*)(const char*);
using PFN_DlssdRuntime_SetLog = void(__cdecl*)(PFN_DlssdRuntime_Log);

#ifdef __cplusplus
}
#endif
