#include "pch.h"

#include "RrInputRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <utility>

namespace RayReconstruction
{
namespace
{
constexpr std::array<InputDefinition, 46> Catalog = { {
    { InputSemantic::Color, "Color", "noisy composite color", true },
    { InputSemantic::Output, "Output", "upscaled output", true },
    { InputSemantic::Depth, "Depth", "depth", true },
    { InputSemantic::MotionVectors, "MotionVectors", "motion vectors", true },
    { InputSemantic::HighResolutionDepth, "DepthHighRes", "high-resolution depth", false },
    { InputSemantic::SpecularMotionVectors, "GBuffer.SpecularMvec", "specular motion vectors", false },
    { InputSemantic::Normals, "GBuffer.Normals", "normals", true },
    { InputSemantic::Roughness, "GBuffer.Roughness", "roughness", false },
    { InputSemantic::DiffuseAlbedo, "DLSS.Input.DiffuseAlbedo", "diffuse albedo", false },
    { InputSemantic::SpecularAlbedo, "DLSS.Input.SpecularAlbedo", "specular albedo", false },
    { InputSemantic::DiffuseHitDistance, "DLSSD.DiffuseHitDistance", "diffuse hit distance", false },
    { InputSemantic::SpecularHitDistance, "DLSSD.SpecularHitDistance", "specular hit distance", false },
    { InputSemantic::DiffuseRayDirection, "DLSSD.DiffuseRayDirection", "diffuse ray direction", false },
    { InputSemantic::SpecularRayDirection, "DLSSD.SpecularRayDirection", "specular ray direction", false },
    { InputSemantic::DiffuseRayDirectionHitDistance, "DLSSD.DiffuseRayDirectionHitDistance",
      "diffuse ray direction and hit distance", false },
    { InputSemantic::SpecularRayDirectionHitDistance, "DLSSD.SpecularRayDirectionHitDistance",
      "specular ray direction and hit distance", false },
    { InputSemantic::ReflectedAlbedo, "DLSSD.ReflectedAlbedo", "reflected albedo", false },
    { InputSemantic::ColorBeforeParticles, "DLSSD.ColorBeforeParticles", "color before particles", false },
    { InputSemantic::ColorAfterParticles, "DLSSD.ColorAfterParticles", "color after particles", false },
    { InputSemantic::ColorBeforeTransparency, "DLSSD.ColorBeforeTransparency", "color before transparency", false },
    { InputSemantic::ColorAfterTransparency, "DLSSD.ColorAfterTransparency", "color after transparency", false },
    { InputSemantic::ColorBeforeFog, "DLSSD.ColorBeforeFog", "color before fog", false },
    { InputSemantic::ColorAfterFog, "DLSSD.ColorAfterFog", "color after fog", false },
    { InputSemantic::TransparencyLayer, "DLSS.TransparencyLayer", "transparency layer", false },
    { InputSemantic::TransparencyLayerOpacity, "DLSS.TransparencyLayerOpacity", "transparency layer opacity", false },
    { InputSemantic::TransparencyMask, "TransparencyMask", "transparency hint", false },
    { InputSemantic::ResponsivityMask, "DLSSD.ResponsivityMask", "responsivity mask", false },
    { InputSemantic::DisocclusionMask, "DLSS.DisocclusionMask", "disocclusion mask", false },
    { InputSemantic::BiasMask, "DLSS.Input.Bias.Current.Color.Mask", "current color bias mask", false },
    { InputSemantic::Alpha, "DLSSD.Alpha", "input alpha", false },
    { InputSemantic::OutputAlpha, "DLSSD.OutputAlpha", "output alpha", false },
    { InputSemantic::Exposure, "ExposureTexture", "exposure", false },
    { InputSemantic::ParticleMask, "IsParticleMask", "particle mask", false },
    { InputSemantic::AnimatedTextureMask, "AnimatedTextureMask", "animated texture mask", false },
    { InputSemantic::PositionViewSpace, "Position.ViewSpace", "view-space position", false },
    { InputSemantic::RayTracingHitDistance, "RayTracingHitDistance", "generic ray-tracing hit distance", false },
    { InputSemantic::ReflectionMotionVectors, "MotionVectorsReflection", "reflection motion vectors", false },
    { InputSemantic::ScreenSpaceSubsurfaceScatteringGuide, "DLSSD.ScreenSpaceSubsurfaceScatteringGuide",
      "screen-space subsurface scattering guide", false },
    { InputSemantic::ColorBeforeScreenSpaceSubsurfaceScattering,
      "DLSSD.ColorBeforeScreenSpaceSubsurfaceScattering", "color before screen-space subsurface scattering", false },
    { InputSemantic::ColorAfterScreenSpaceSubsurfaceScattering,
      "DLSSD.ColorAfterScreenSpaceSubsurfaceScattering", "color after screen-space subsurface scattering", false },
    { InputSemantic::ScreenSpaceRefractionGuide, "DLSSD.ScreenSpaceRefractionGuide",
      "screen-space refraction guide", false },
    { InputSemantic::ColorBeforeScreenSpaceRefraction, "DLSSD.ColorBeforeScreenSpaceRefraction",
      "color before screen-space refraction", false },
    { InputSemantic::ColorAfterScreenSpaceRefraction, "DLSSD.ColorAfterScreenSpaceRefraction",
      "color after screen-space refraction", false },
    { InputSemantic::DepthOfFieldGuide, "DLSSD.DepthOfFieldGuide", "depth-of-field guide", false },
    { InputSemantic::ColorBeforeDepthOfField, "DLSSD.ColorBeforeDepthOfField", "color before depth of field", false },
    { InputSemantic::ColorAfterDepthOfField, "DLSSD.ColorAfterDepthOfField", "color after depth of field", false },
} };

bool TryGetResource(const NVSDK_NGX_Parameter& parameters, const char* key, ID3D12Resource*& resource)
{
    resource = nullptr;
    if (parameters.Get(key, &resource) == NVSDK_NGX_Result_Success && resource != nullptr)
        return true;

    void* pointer = nullptr;
    if (parameters.Get(key, &pointer) == NVSDK_NGX_Result_Success && pointer != nullptr)
    {
        resource = static_cast<ID3D12Resource*>(pointer);
        return true;
    }

    return false;
}

bool TryGetUInt(const NVSDK_NGX_Parameter& parameters, const char* key, uint32_t& value)
{
    return parameters.Get(key, &value) == NVSDK_NGX_Result_Success;
}

bool TryGetFloat(const NVSDK_NGX_Parameter& parameters, const char* key, float& value)
{
    return parameters.Get(key, &value) == NVSDK_NGX_Result_Success;
}

void CaptureMatrix(const NVSDK_NGX_Parameter& parameters, const char* key, std::array<float, 16>& matrix, bool& present)
{
    void* pointer = nullptr;
    present = parameters.Get(key, &pointer) == NVSDK_NGX_Result_Success && pointer != nullptr;
    if (present)
        std::memcpy(matrix.data(), pointer, sizeof(matrix));
}

bool IsTexture2D(const D3D12_RESOURCE_DESC& description)
{
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.DepthOrArraySize == 1 &&
           description.SampleDesc.Count == 1;
}

std::pair<const char*, const char*> SubrectKeys(InputSemantic semantic)
{
    switch (semantic)
    {
    case InputSemantic::Color:
        return { "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y" };
    case InputSemantic::Output:
        return { "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y" };
    case InputSemantic::Depth:
        return { "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y" };
    case InputSemantic::MotionVectors:
        return { "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y" };
    case InputSemantic::Normals:
        return { "DLSS.Input.Normals.Subrect.Base.X", "DLSS.Input.Normals.Subrect.Base.Y" };
    case InputSemantic::Roughness:
        return { "DLSS.Input.Roughness.Subrect.Base.X", "DLSS.Input.Roughness.Subrect.Base.Y" };
    case InputSemantic::DiffuseAlbedo:
        return { "DLSS.Input.DiffuseAlbedo.Subrect.Base.X", "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y" };
    case InputSemantic::SpecularAlbedo:
        return { "DLSS.Input.SpecularAlbedo.Subrect.Base.X", "DLSS.Input.SpecularAlbedo.Subrect.Base.Y" };
    case InputSemantic::BiasMask:
        return { "DLSS.Input.Bias.Current.Color.Subrect.Base.X",
                 "DLSS.Input.Bias.Current.Color.Subrect.Base.Y" };
    case InputSemantic::TransparencyMask:
        return { "DLSS.Input.Translucency.Subrect.Base.X", "DLSS.Input.Translucency.Subrect.Base.Y" };
    case InputSemantic::DisocclusionMask:
        return { "DLSS.DisocclusionMask.Subrect.Base.X", "DLSS.DisocclusionMask.Subrect.Base.Y" };
    case InputSemantic::TransparencyLayer:
        return { "DLSS.TransparencyLayer.Subrect.Base.X", "DLSS.TransparencyLayer.Subrect.Base.Y" };
    case InputSemantic::Exposure:
    case InputSemantic::HighResolutionDepth:
    case InputSemantic::SpecularMotionVectors:
    case InputSemantic::ParticleMask:
    case InputSemantic::AnimatedTextureMask:
    case InputSemantic::PositionViewSpace:
    case InputSemantic::RayTracingHitDistance:
    case InputSemantic::ReflectionMotionVectors:
        return { nullptr, nullptr };
    default:
        break;
    }

    for (const auto& definition : Catalog)
    {
        if (definition.semantic != semantic)
            continue;
        // All remaining normalized DLSSD resources follow this public NGX naming convention.
        static thread_local std::string x;
        static thread_local std::string y;
        x = std::string(definition.ngxKey) + ".Subrect.Base.X";
        y = std::string(definition.ngxKey) + ".Subrect.Base.Y";
        return { x.c_str(), y.c_str() };
    }
    return { nullptr, nullptr };
}
} // namespace

bool ValidationReport::HasErrors() const
{
    return std::ranges::any_of(issues,
                               [](const ValidationIssue& issue) { return issue.severity == ValidationSeverity::Error; });
}

std::string ValidationReport::Summary() const
{
    std::ostringstream stream;
    for (size_t index = 0; index < issues.size(); ++index)
    {
        if (index != 0)
            stream << "; ";
        stream << issues[index].code << ": " << issues[index].message;
    }
    return stream.str();
}

const ResourceInput* InputSnapshot::Find(InputSemantic semantic) const
{
    const auto result = std::ranges::find_if(resources, [semantic](const ResourceInput& input)
                                              { return input.definition->semantic == semantic; });
    return result == resources.end() ? nullptr : &*result;
}

ValidationReport InputSnapshot::ValidateInventory() const
{
    ValidationReport report;
    const auto error = [&report](std::string code, std::string message)
    { report.issues.push_back({ ValidationSeverity::Error, std::move(code), std::move(message) }); };
    const auto warning = [&report](std::string code, std::string message)
    { report.issues.push_back({ ValidationSeverity::Warning, std::move(code), std::move(message) }); };

    if (renderWidth == 0 || renderHeight == 0)
        error("render_size", "NGX did not provide a non-zero render size");
    if (outputWidth == 0 || outputHeight == 0)
        error("output_size", "NGX did not provide a non-zero output size");
    if (!hasWorldToView || !hasViewToClip)
        error("camera_matrices", "world-to-view or view-to-clip matrix is missing");

    for (const auto& input : resources)
    {
        if (input.definition->canonicalBase && !input.IsPresent())
        {
            error("missing_" + std::string(ToString(input.definition->semantic)),
                  std::string(input.definition->name) + " was not supplied");
            continue;
        }

        if (!input.IsPresent())
            continue;
        const auto invalid = [&](std::string code, std::string message)
        {
            if (input.definition->canonicalBase)
                error(std::move(code), std::move(message));
            else
                warning(std::move(code), std::move(message));
        };
        if (!IsTexture2D(input.description))
            invalid("resource_shape", std::string(input.definition->name) + " is not a single-sample 2D texture");
        if (input.description.Format == DXGI_FORMAT_UNKNOWN)
            invalid("resource_format", std::string(input.definition->name) + " has an unknown DXGI format");

        if (input.definition->semantic != InputSemantic::Output && input.definition->semantic != InputSemantic::Exposure &&
            renderWidth != 0 && renderHeight != 0 &&
            (static_cast<uint64_t>(input.subrectBaseX) + renderWidth > input.description.Width ||
             static_cast<uint64_t>(input.subrectBaseY) + renderHeight > input.description.Height))
            invalid("resource_extent", std::string(input.definition->name) + " does not contain the render subrect");
    }

    if (!depthType)
        warning("depth_type", "DLSS.Use.HW.Depth was not supplied; a profile must specify the depth convention");
    if (!roughnessMode)
        warning("roughness_mode", "DLSS.Roughness.Mode was not supplied; a profile must specify roughness packing");
    if ((sharpness && !std::isfinite(*sharpness)) || (preExposure && !std::isfinite(*preExposure)) ||
        (exposureScale && !std::isfinite(*exposureScale)))
        warning("exposure_metadata", "sharpness, pre-exposure, or exposure scale contains a non-finite value");

    return report;
}

std::string InputSnapshot::Signature() const
{
    std::ostringstream stream;
    stream << handleId << ':' << renderWidth << 'x' << renderHeight << "->" << outputWidth << 'x'
           << outputHeight << ":d" << depthType.value_or(99) << ":r" << roughnessMode.value_or(99) << ":n"
           << denoiseMode.value_or(99) << ":q" << perfQuality.value_or(99) << ":f"
           << featureCreateFlags.value_or(0) << ':';
    for (const auto& input : resources)
    {
        stream << (input.IsPresent() ? '1' : '0');
        if (input.IsPresent())
            stream << static_cast<uint32_t>(input.description.Format) << '@' << input.description.Width << 'x'
                   << input.description.Height << '+' << input.subrectBaseX << ',' << input.subrectBaseY;
        stream << ',';
    }
    return stream.str();
}

std::string InputSnapshot::Describe() const
{
    std::ostringstream stream;
    stream << "handle=" << handleId << " frame=" << frameIndex << " render=" << renderWidth << 'x' << renderHeight
           << " output=" << outputWidth << 'x' << outputHeight << " jitter=(" << jitterX << ',' << jitterY
           << ") mvScale=(" << motionScaleX << ',' << motionScaleY << ") reset=" << reset
           << " depthType=" << (depthType ? std::to_string(*depthType) : "missing")
           << " roughnessMode=" << (roughnessMode ? std::to_string(*roughnessMode) : "missing")
           << " denoiseMode=" << (denoiseMode ? std::to_string(*denoiseMode) : "missing")
           << " quality=" << (perfQuality ? std::to_string(*perfQuality) : "missing")
           << " createFlags=" << (featureCreateFlags ? std::to_string(*featureCreateFlags) : "missing")
           << " sharpness=" << (sharpness ? std::to_string(*sharpness) : "missing")
           << " preExposure=" << (preExposure ? std::to_string(*preExposure) : "missing")
           << " exposureScale=" << (exposureScale ? std::to_string(*exposureScale) : "missing")
           << " indicatorInvert=(" << (indicatorInvertX ? std::to_string(*indicatorInvertX) : "missing") << ','
           << (indicatorInvertY ? std::to_string(*indicatorInvertY) : "missing") << ')';

    for (const auto& input : resources)
    {
        stream << "\n  " << input.definition->ngxKey << '=';
        if (!input.IsPresent())
        {
            stream << "missing";
            continue;
        }
        stream << static_cast<uint32_t>(input.description.Format) << ' ' << input.description.Width << 'x'
               << input.description.Height << " base=(" << input.subrectBaseX << ',' << input.subrectBaseY
               << ") flags=0x" << std::hex
               << static_cast<uint32_t>(input.description.Flags) << std::dec;
    }
    return stream.str();
}

std::span<const InputDefinition> InputCatalog() { return Catalog; }

InputSnapshot CaptureInputs(uint32_t handleId, uint32_t frameIndex, const NVSDK_NGX_Parameter& parameters)
{
    InputSnapshot snapshot;
    snapshot.handleId = handleId;
    snapshot.frameIndex = frameIndex;

    TryGetUInt(parameters, "DLSS.Render.Subrect.Dimensions.Width", snapshot.renderWidth);
    TryGetUInt(parameters, "DLSS.Render.Subrect.Dimensions.Height", snapshot.renderHeight);
    if (snapshot.renderWidth == 0)
        TryGetUInt(parameters, "Width", snapshot.renderWidth);
    if (snapshot.renderHeight == 0)
        TryGetUInt(parameters, "Height", snapshot.renderHeight);
    TryGetUInt(parameters, "OutWidth", snapshot.outputWidth);
    TryGetUInt(parameters, "OutHeight", snapshot.outputHeight);
    parameters.Get("Jitter.Offset.X", &snapshot.jitterX);
    parameters.Get("Jitter.Offset.Y", &snapshot.jitterY);
    parameters.Get("MV.Scale.X", &snapshot.motionScaleX);
    parameters.Get("MV.Scale.Y", &snapshot.motionScaleY);
    TryGetUInt(parameters, "Reset", snapshot.reset);

    uint32_t value = 0;
    if (TryGetUInt(parameters, "DLSS.Use.HW.Depth", value))
        snapshot.depthType = value;
    if (TryGetUInt(parameters, "DLSS.Roughness.Mode", value))
        snapshot.roughnessMode = value;
    if (TryGetUInt(parameters, "DLSS.Denoise.Mode", value))
        snapshot.denoiseMode = value;
    if (TryGetUInt(parameters, "PerfQualityValue", value))
        snapshot.perfQuality = value;
    if (TryGetUInt(parameters, "DLSS.Feature.Create.Flags", value))
        snapshot.featureCreateFlags = value;
    if (TryGetUInt(parameters, "DLSS.Indicator.Invert.X.Axis", value))
        snapshot.indicatorInvertX = value;
    if (TryGetUInt(parameters, "DLSS.Indicator.Invert.Y.Axis", value))
        snapshot.indicatorInvertY = value;

    float floatValue = 0.0f;
    if (TryGetFloat(parameters, "Sharpness", floatValue))
        snapshot.sharpness = floatValue;
    if (TryGetFloat(parameters, "DLSS.Pre.Exposure", floatValue))
        snapshot.preExposure = floatValue;
    if (TryGetFloat(parameters, "DLSS.Exposure.Scale", floatValue))
        snapshot.exposureScale = floatValue;

    CaptureMatrix(parameters, "WorldToViewMatrix", snapshot.worldToView, snapshot.hasWorldToView);
    CaptureMatrix(parameters, "ViewToClipMatrix", snapshot.viewToClip, snapshot.hasViewToClip);

    snapshot.resources.reserve(Catalog.size());
    for (const auto& definition : Catalog)
    {
        ResourceInput input;
        input.definition = &definition;
        if (TryGetResource(parameters, definition.ngxKey, input.resource))
        {
            input.description = input.resource->GetDesc();
            const auto [baseXKey, baseYKey] = SubrectKeys(definition.semantic);
            if (baseXKey)
                TryGetUInt(parameters, baseXKey, input.subrectBaseX);
            if (baseYKey)
                TryGetUInt(parameters, baseYKey, input.subrectBaseY);
        }
        snapshot.resources.push_back(input);
    }

    return snapshot;
}

const char* ToString(InputSemantic semantic)
{
    for (const auto& definition : Catalog)
        if (definition.semantic == semantic)
            return definition.ngxKey;
    return "unknown";
}
} // namespace RayReconstruction
