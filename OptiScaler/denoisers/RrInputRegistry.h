#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <d3d12.h>
#include <nvsdk_ngx_params.h>

namespace RayReconstruction
{
enum class InputSemantic : uint8_t
{
    Color,
    Output,
    Depth,
    MotionVectors,
    HighResolutionDepth,
    SpecularMotionVectors,
    Normals,
    Roughness,
    DiffuseAlbedo,
    SpecularAlbedo,
    DiffuseHitDistance,
    SpecularHitDistance,
    DiffuseRayDirection,
    SpecularRayDirection,
    DiffuseRayDirectionHitDistance,
    SpecularRayDirectionHitDistance,
    ReflectedAlbedo,
    ColorBeforeParticles,
    ColorAfterParticles,
    ColorBeforeTransparency,
    ColorAfterTransparency,
    ColorBeforeFog,
    ColorAfterFog,
    TransparencyLayer,
    TransparencyLayerOpacity,
    TransparencyMask,
    ResponsivityMask,
    DisocclusionMask,
    BiasMask,
    Alpha,
    OutputAlpha,
    Exposure,
    ParticleMask,
    AnimatedTextureMask,
    PositionViewSpace,
    RayTracingHitDistance,
    ReflectionMotionVectors,
    ScreenSpaceSubsurfaceScatteringGuide,
    ColorBeforeScreenSpaceSubsurfaceScattering,
    ColorAfterScreenSpaceSubsurfaceScattering,
    ScreenSpaceRefractionGuide,
    ColorBeforeScreenSpaceRefraction,
    ColorAfterScreenSpaceRefraction,
    DepthOfFieldGuide,
    ColorBeforeDepthOfField,
    ColorAfterDepthOfField,
};

struct InputDefinition
{
    InputSemantic semantic;
    const char* ngxKey;
    const char* name;
    bool canonicalBase;
};

struct ResourceInput
{
    const InputDefinition* definition = nullptr;
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_DESC description {};
    uint32_t subrectBaseX = 0;
    uint32_t subrectBaseY = 0;

    bool IsPresent() const { return resource != nullptr; }
};

enum class ValidationSeverity : uint8_t
{
    Info,
    Warning,
    Error,
};

struct ValidationIssue
{
    ValidationSeverity severity = ValidationSeverity::Info;
    std::string code;
    std::string message;
};

struct ValidationReport
{
    std::vector<ValidationIssue> issues;

    bool HasErrors() const;
    std::string Summary() const;
};

struct InputSnapshot
{
    uint32_t handleId = 0;
    uint32_t frameIndex = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
    uint32_t reset = 0;
    std::optional<uint32_t> depthType;
    std::optional<uint32_t> roughnessMode;
    std::optional<uint32_t> denoiseMode;
    std::optional<uint32_t> perfQuality;
    std::optional<uint32_t> featureCreateFlags;
    std::optional<uint32_t> indicatorInvertX;
    std::optional<uint32_t> indicatorInvertY;
    std::optional<float> sharpness;
    std::optional<float> preExposure;
    std::optional<float> exposureScale;
    std::array<float, 16> worldToView {};
    std::array<float, 16> viewToClip {};
    bool hasWorldToView = false;
    bool hasViewToClip = false;
    std::vector<ResourceInput> resources;

    const ResourceInput* Find(InputSemantic semantic) const;
    ValidationReport ValidateInventory() const;
    std::string Signature() const;
    std::string Describe() const;
};

std::span<const InputDefinition> InputCatalog();
InputSnapshot CaptureInputs(uint32_t handleId, uint32_t frameIndex, const NVSDK_NGX_Parameter& parameters);
const char* ToString(InputSemantic semantic);
} // namespace RayReconstruction
