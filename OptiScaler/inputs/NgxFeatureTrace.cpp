#include "pch.h"

#include "NgxFeatureTrace.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <denoisers/RrInputRegistry.h>

#include <sha1/sha1.hpp>

#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace NgxFeatureTrace
{
namespace
{
struct SuperSamplingResource
{
    const char* ngxKey;
    const char* baseXKey;
    const char* baseYKey;
    bool required = false;
};

constexpr std::array SuperSamplingResources = {
    SuperSamplingResource { "Color", "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y", true },
    SuperSamplingResource { "Output", "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y", true },
    SuperSamplingResource { "Depth", "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y", true },
    SuperSamplingResource { "MotionVectors", "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y", true },
    SuperSamplingResource { "TransparencyMask", "DLSS.Input.Translucency.Subrect.Base.X",
                            "DLSS.Input.Translucency.Subrect.Base.Y" },
    SuperSamplingResource { "ExposureTexture", nullptr, nullptr },
    SuperSamplingResource { "DLSS.Input.Bias.Current.Color.Mask", "DLSS.Input.Bias.Current.Color.Subrect.Base.X",
                            "DLSS.Input.Bias.Current.Color.Subrect.Base.Y" },
    SuperSamplingResource { "AnimatedTextureMask", nullptr, nullptr },
    SuperSamplingResource { "RayTracingHitDistance", nullptr, nullptr },
    SuperSamplingResource { "MotionVectorsReflection", nullptr, nullptr },
    SuperSamplingResource { "IsParticleMask", nullptr, nullptr },
};

constexpr std::array SharedJoinKeys = {
    "Color", "Output", "Depth", "MotionVectors", "DLSS.Input.Bias.Current.Color.Mask",
};

constexpr std::array ScalarUIntKeys = {
    "Width",
    "Height",
    "OutWidth",
    "OutHeight",
    "DLSS.Render.Subrect.Dimensions.Width",
    "DLSS.Render.Subrect.Dimensions.Height",
    "PerfQualityValue",
    "DLSS.Feature.Create.Flags",
    "CreationNodeMask",
    "VisibilityNodeMask",
    "FreeMemOnReleaseFeature",
    "Reset",
    "DLSS.Use.HW.Depth",
    "DLSS.Roughness.Mode",
    "DLSS.Denoise.Mode",
    "DLSS.Indicator.Invert.X.Axis",
    "DLSS.Indicator.Invert.Y.Axis",
    "RTXValue",
};

constexpr std::array ScalarFloatKeys = {
    "Jitter.Offset.X", "Jitter.Offset.Y", "MV.Scale.X", "MV.Scale.Y",
    "Sharpness",       "DLSS.Pre.Exposure", "DLSS.Exposure.Scale",
};

struct ResourceIdentity
{
    uintptr_t pointer = 0;
    uint64_t gpuVa = 0;
    bool present = false;
};

struct JoinSnapshot
{
    uint32_t handleId = 0;
    bool valid = false;
    std::array<ResourceIdentity, SharedJoinKeys.size()> resources {};
};

struct HandleState
{
    NVSDK_NGX_Feature featureId {};
    uint32_t evaluateCount = 0;
    std::string lastSignature;
};

std::mutex StateMutex;
std::unordered_map<uint32_t, HandleState> Handles;
std::unordered_map<uintptr_t, uint64_t> AllocationSizes;
std::unordered_set<uint32_t> LoggedRequirements;
JoinSnapshot SuperSamplingJoin;
JoinSnapshot RayReconstructionJoin;
bool LoggedJoinDisclaimer = false;
bool LoggedResourceStateNote = false;
bool LoggedStreamline = false;
bool LoggedDlss = false;
bool LoggedDlssd = false;

bool LoggingEnabled() { return Config::Instance()->FSRRLogInputs.value_or_default(); }

void LogJoinDisclaimerOnce()
{
    if (LoggedJoinDisclaimer)
        return;
    LoggedJoinDisclaimer = true;
    LOG_INFO("NGX feature-trace: raw ID3D12Resource identities, GPU VA, and GetDesc fields are the upper join only; "
             "this log does not prove descriptor, packed-argument, or kernel conversion");
}

void LogResourceStateNoteOnce()
{
    if (LoggedResourceStateNote)
        return;
    LoggedResourceStateNote = true;
    LOG_INFO("NGX feature-trace: D3D12 resource state is not present on the NGX parameter table; "
             "Streamline cachedStates are not hooked");
}

void LogBinaryOnce(bool& logged, const char* label, const std::optional<std::wstring>& pathValue)
{
    if (logged)
        return;
    logged = true;
    if (!pathValue)
    {
        LOG_WARN("NGX feature-trace could not resolve {}", label);
        return;
    }

    const std::filesystem::path path(*pathValue);
    version_t version;
    const bool hasVersion = Util::GetFileVersion(path.wstring(), &version, nullptr);
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        LOG_WARN("NGX feature-trace could not open {}: {}", label, path.string());
        return;
    }

    SHA1 checksum;
    checksum.update(file);
    LOG_INFO("NGX feature-trace {}: path={} version={}.{}.{} bytes={} sha1={}", label, path.string(),
             hasVersion ? version.major : 0, hasVersion ? version.minor : 0, hasVersion ? version.patch : 0,
             sizeError ? 0 : fileSize, checksum.final());
}

void LogVendorIdentity(NVSDK_NGX_Feature featureId)
{
    if (!LoggedStreamline)
    {
        LoggedStreamline = true;
        const auto& version = State::Instance().streamlineVersion;
        LOG_INFO("NGX feature-trace Streamline version: {}.{}.{}", version.major, version.minor, version.patch);
    }

    if (featureId == NVSDK_NGX_Feature_SuperSampling)
        LogBinaryOnce(LoggedDlss, "nvngx_dlss.dll", State::Instance().NVNGX_DLSS_Path);
    else if (featureId == NVSDK_NGX_Feature_RayReconstruction)
        LogBinaryOnce(LoggedDlssd, "nvngx_dlssd.dll", State::Instance().NVNGX_DLSSD_Path);
}

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

uint64_t QueryGpuVa(ID3D12Resource* resource)
{
    if (resource == nullptr)
        return 0;
    return resource->GetGPUVirtualAddress();
}

uint64_t QueryAllocationSize(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& description)
{
    if (resource == nullptr)
        return 0;

    const uintptr_t key = reinterpret_cast<uintptr_t>(resource);
    if (const auto it = AllocationSizes.find(key); it != AllocationSizes.end())
        return it->second;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || !device)
    {
        AllocationSizes.emplace(key, 0);
        return 0;
    }

    const D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &description);
    const uint64_t size = (info.SizeInBytes == UINT64_MAX) ? 0 : info.SizeInBytes;
    AllocationSizes.emplace(key, size);
    return size;
}

int SharedJoinIndex(const char* ngxKey)
{
    for (size_t index = 0; index < SharedJoinKeys.size(); ++index)
    {
        if (std::strcmp(SharedJoinKeys[index], ngxKey) == 0)
            return static_cast<int>(index);
    }
    return -1;
}

JoinSnapshot& JoinFor(NVSDK_NGX_Feature featureId)
{
    return featureId == NVSDK_NGX_Feature_SuperSampling ? SuperSamplingJoin : RayReconstructionJoin;
}

void ClearJoin(NVSDK_NGX_Feature featureId) { JoinFor(featureId) = {}; }

void RememberJoinResource(JoinSnapshot& join, const char* ngxKey, const ResourceIdentity& identity)
{
    const int index = SharedJoinIndex(ngxKey);
    if (index < 0)
        return;
    join.resources[static_cast<size_t>(index)] = identity;
}

void AppendMissingResource(std::ostringstream& stream, std::ostringstream& signature, const char* ngxKey,
                           JoinSnapshot* join)
{
    stream << "missing GPU VA=0x0";
    signature << ngxKey << "=0/va0;";
    if (join != nullptr)
        RememberJoinResource(*join, ngxKey, {});
}

void AppendPresentResource(std::ostringstream& stream, std::ostringstream& signature, const char* ngxKey,
                           ID3D12Resource* resource, uint32_t baseX, uint32_t baseY, JoinSnapshot* join)
{
    const auto description = resource->GetDesc();
    const uint64_t gpuVa = QueryGpuVa(resource);
    const uint64_t allocationSize = QueryAllocationSize(resource, description);
    const uintptr_t pointer = reinterpret_cast<uintptr_t>(resource);

    stream << "resource=0x" << std::hex << pointer << " GPU VA=0x" << gpuVa << std::dec << ' '
           << static_cast<uint32_t>(description.Format) << ' ' << description.Width << 'x' << description.Height
           << " base=(" << baseX << ',' << baseY << ") GetDesc dim=" << static_cast<uint32_t>(description.Dimension)
           << " width=" << description.Width << " height=" << description.Height
           << " depth=" << description.DepthOrArraySize << " mips=" << description.MipLevels
           << " format=" << static_cast<uint32_t>(description.Format) << " flags=0x" << std::hex
           << static_cast<uint32_t>(description.Flags) << std::dec << " samples=" << description.SampleDesc.Count;
    if (allocationSize != 0)
        stream << " alloc=" << allocationSize;

    signature << ngxKey << "=1/ptr" << pointer << "/va" << gpuVa << "/dim" << static_cast<uint32_t>(description.Dimension)
              << '/' << description.Width << 'x' << description.Height << 'x' << description.DepthOrArraySize << "/mips"
              << description.MipLevels << "/fmt" << static_cast<uint32_t>(description.Format) << "/flags"
              << static_cast<uint32_t>(description.Flags) << "/samples" << description.SampleDesc.Count << '+' << baseX
              << ',' << baseY << ';';

    if (join != nullptr)
        RememberJoinResource(*join, ngxKey, ResourceIdentity { pointer, gpuVa, true });
}

void AppendResource(std::ostringstream& stream, std::ostringstream& signature, const NVSDK_NGX_Parameter& parameters,
                    const SuperSamplingResource& definition, JoinSnapshot* join)
{
    stream << "\n  " << definition.ngxKey << '=';
    ID3D12Resource* resource = nullptr;
    if (!TryGetResource(parameters, definition.ngxKey, resource))
    {
        AppendMissingResource(stream, signature, definition.ngxKey, join);
        return;
    }

    uint32_t baseX = 0;
    uint32_t baseY = 0;
    if (definition.baseXKey)
        parameters.Get(definition.baseXKey, &baseX);
    if (definition.baseYKey)
        parameters.Get(definition.baseYKey, &baseY);
    AppendPresentResource(stream, signature, definition.ngxKey, resource, baseX, baseY, join);
}

void AppendOptionalTagSummary(std::ostringstream& stream, std::ostringstream& signature, std::string present,
                              std::string missing)
{
    if (present.empty())
        present = "none";
    if (missing.empty())
        missing = "none";
    stream << "\n  optionalTags present=" << present << " missing=" << missing;
    signature << "opt=" << present << '/' << missing << ';';
}

void AppendEvalScalars(std::ostringstream& stream, std::ostringstream& signature, const NVSDK_NGX_Parameter& parameters)
{
    uint32_t reset = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    uint32_t quality = 0;
    uint32_t createFlags = 0;
    uint32_t roughnessMode = 0;
    const bool hasReset = TryGetUInt(parameters, "Reset", reset);
    const bool hasWidth = TryGetUInt(parameters, "Width", width);
    const bool hasHeight = TryGetUInt(parameters, "Height", height);
    const bool hasRenderWidth = TryGetUInt(parameters, "DLSS.Render.Subrect.Dimensions.Width", renderWidth);
    const bool hasRenderHeight = TryGetUInt(parameters, "DLSS.Render.Subrect.Dimensions.Height", renderHeight);
    const bool hasOutWidth = TryGetUInt(parameters, "OutWidth", outWidth);
    const bool hasOutHeight = TryGetUInt(parameters, "OutHeight", outHeight);
    const bool hasQuality = TryGetUInt(parameters, "PerfQualityValue", quality);
    const bool hasCreateFlags = TryGetUInt(parameters, "DLSS.Feature.Create.Flags", createFlags);
    const bool hasRoughness = TryGetUInt(parameters, "DLSS.Roughness.Mode", roughnessMode);
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    const bool hasJitterX = parameters.Get("Jitter.Offset.X", &jitterX) == NVSDK_NGX_Result_Success;
    const bool hasJitterY = parameters.Get("Jitter.Offset.Y", &jitterY) == NVSDK_NGX_Result_Success;
    const bool autoExposure =
        hasCreateFlags && (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

    stream << "\n  evalScalars Reset=" << (hasReset ? std::to_string(reset) : "missing") << " jitter=("
           << (hasJitterX ? std::to_string(jitterX) : "missing") << ','
           << (hasJitterY ? std::to_string(jitterY) : "missing") << ") render="
           << (hasRenderWidth ? std::to_string(renderWidth) : (hasWidth ? std::to_string(width) : "missing")) << 'x'
           << (hasRenderHeight ? std::to_string(renderHeight) : (hasHeight ? std::to_string(height) : "missing"))
           << " output=" << (hasOutWidth ? std::to_string(outWidth) : "missing") << 'x'
           << (hasOutHeight ? std::to_string(outHeight) : "missing")
           << " quality=" << (hasQuality ? std::to_string(quality) : "missing")
           << " autoExposure=" << (hasCreateFlags ? (autoExposure ? "1" : "0") : "missing")
           << " roughnessMode=" << (hasRoughness ? std::to_string(roughnessMode) : "missing")
           << " createFlags=" << (hasCreateFlags ? std::to_string(createFlags) : "missing");

    // Jitter changes every frame; keep it out of the stable-inventory signature.
    signature << "reset=" << (hasReset ? reset : 99) << ";render="
              << (hasRenderWidth ? renderWidth : width) << 'x' << (hasRenderHeight ? renderHeight : height)
              << ";output=" << outWidth << 'x' << outHeight << ";quality=" << (hasQuality ? quality : 99)
              << ";ae=" << (hasCreateFlags ? (autoExposure ? 1 : 0) : 9)
              << ";rough=" << (hasRoughness ? roughnessMode : 99) << ";flags=" << createFlags << ';';
}

void AppendScalarDump(std::ostringstream& stream, const NVSDK_NGX_Parameter& parameters)
{
    for (const char* key : ScalarUIntKeys)
    {
        uint32_t value = 0;
        if (parameters.Get(key, &value) == NVSDK_NGX_Result_Success)
            stream << ' ' << key << '=' << value;
    }

    for (const char* key : ScalarFloatKeys)
    {
        float value = 0.0f;
        if (parameters.Get(key, &value) == NVSDK_NGX_Result_Success)
            stream << ' ' << key << '=' << value;
    }

    for (const char* key : { "WorldToViewMatrix", "ViewToClipMatrix" })
    {
        void* pointer = nullptr;
        if (parameters.Get(key, &pointer) != NVSDK_NGX_Result_Success || pointer == nullptr)
        {
            stream << ' ' << key << "=missing";
            continue;
        }

        std::array<float, 16> matrix {};
        std::memcpy(matrix.data(), pointer, sizeof(matrix));
        stream << ' ' << key << '=';
        for (size_t index = 0; index < matrix.size(); ++index)
        {
            if (index != 0)
                stream << ',';
            stream << matrix[index];
        }
    }
}

void AppendShareNotes(std::ostringstream& stream)
{
    stream << "\n  share/alias";
    if (!SuperSamplingJoin.valid && !RayReconstructionJoin.valid)
    {
        stream << " SuperSampling=missing RayReconstruction=missing";
        return;
    }
    if (!SuperSamplingJoin.valid)
    {
        stream << " SuperSampling=missing RayReconstruction handle=" << RayReconstructionJoin.handleId;
        return;
    }
    if (!RayReconstructionJoin.valid)
    {
        stream << " SuperSampling handle=" << SuperSamplingJoin.handleId << " RayReconstruction=missing";
        return;
    }

    stream << " SuperSampling handle=" << SuperSamplingJoin.handleId << " RayReconstruction handle="
           << RayReconstructionJoin.handleId;
    for (size_t index = 0; index < SharedJoinKeys.size(); ++index)
    {
        const auto& superSampling = SuperSamplingJoin.resources[index];
        const auto& rayReconstruction = RayReconstructionJoin.resources[index];
        stream << "\n    " << SharedJoinKeys[index] << ':';
        if (!superSampling.present && !rayReconstruction.present)
        {
            stream << " both-missing";
            continue;
        }
        if (!superSampling.present || !rayReconstruction.present)
        {
            stream << " SuperSampling=" << (superSampling.present ? "present" : "missing")
                   << " RayReconstruction=" << (rayReconstruction.present ? "present" : "missing");
            continue;
        }

        const bool samePointer = superSampling.pointer == rayReconstruction.pointer;
        const bool sameVa = superSampling.gpuVa != 0 && superSampling.gpuVa == rayReconstruction.gpuVa;
        stream << " SuperSampling ptr=0x" << std::hex << superSampling.pointer << " GPU VA=0x" << superSampling.gpuVa
               << " RayReconstruction ptr=0x" << rayReconstruction.pointer << " GPU VA=0x" << rayReconstruction.gpuVa
               << std::dec << " pointer=" << (samePointer ? "same" : "different") << " va="
               << (sameVa ? "same" : ((superSampling.gpuVa == 0 && rayReconstruction.gpuVa == 0) ? "both-zero"
                                                                                                 : "different"));
    }
}

std::string DescribeSuperSampling(uint32_t handleId, uint32_t evaluateCount, const NVSDK_NGX_Parameter& parameters,
                                  std::string& signature)
{
    std::ostringstream stream;
    std::ostringstream signatureStream;
    JoinSnapshot& join = SuperSamplingJoin;
    join = {};
    join.handleId = handleId;
    join.valid = true;

    stream << "handle=" << handleId << " eval=" << evaluateCount;
    AppendScalarDump(stream, parameters);
    AppendEvalScalars(stream, signatureStream, parameters);
    signatureStream << handleId << ':';

    std::string presentTags;
    std::string missingTags;
    for (const auto& definition : SuperSamplingResources)
    {
        AppendResource(stream, signatureStream, parameters, definition, &join);
        if (definition.required)
            continue;
        ID3D12Resource* resource = nullptr;
        const bool present = TryGetResource(parameters, definition.ngxKey, resource);
        auto& tags = present ? presentTags : missingTags;
        if (!tags.empty())
            tags += ',';
        tags += definition.ngxKey;
    }
    AppendOptionalTagSummary(stream, signatureStream, presentTags, missingTags);
    AppendShareNotes(stream);
    signature = signatureStream.str();
    return stream.str();
}

std::string DescribeRayReconstruction(uint32_t handleId, uint32_t evaluateCount, const NVSDK_NGX_Parameter& parameters,
                                      std::string& signature)
{
    const auto snapshot = RayReconstruction::CaptureInputs(handleId, evaluateCount, parameters);
    std::ostringstream stream;
    std::ostringstream signatureStream;
    JoinSnapshot& join = RayReconstructionJoin;
    join = {};
    join.handleId = handleId;
    join.valid = true;

    stream << "handle=" << snapshot.handleId << " frame=" << snapshot.frameIndex << " render=" << snapshot.renderWidth
           << 'x' << snapshot.renderHeight << " output=" << snapshot.outputWidth << 'x' << snapshot.outputHeight
           << " jitter=(" << snapshot.jitterX << ',' << snapshot.jitterY << ") mvScale=(" << snapshot.motionScaleX
           << ',' << snapshot.motionScaleY << ") reset=" << snapshot.reset
           << " depthType=" << (snapshot.depthType ? std::to_string(*snapshot.depthType) : "missing")
           << " roughnessMode=" << (snapshot.roughnessMode ? std::to_string(*snapshot.roughnessMode) : "missing")
           << " denoiseMode=" << (snapshot.denoiseMode ? std::to_string(*snapshot.denoiseMode) : "missing")
           << " quality=" << (snapshot.perfQuality ? std::to_string(*snapshot.perfQuality) : "missing")
           << " createFlags=" << (snapshot.featureCreateFlags ? std::to_string(*snapshot.featureCreateFlags) : "missing")
           << " sharpness=" << (snapshot.sharpness ? std::to_string(*snapshot.sharpness) : "missing")
           << " preExposure=" << (snapshot.preExposure ? std::to_string(*snapshot.preExposure) : "missing")
           << " exposureScale=" << (snapshot.exposureScale ? std::to_string(*snapshot.exposureScale) : "missing")
           << " indicatorInvert=("
           << (snapshot.indicatorInvertX ? std::to_string(*snapshot.indicatorInvertX) : "missing") << ','
           << (snapshot.indicatorInvertY ? std::to_string(*snapshot.indicatorInvertY) : "missing") << ')';
    AppendEvalScalars(stream, signatureStream, parameters);
    signatureStream << snapshot.Signature();

    std::string presentTags;
    std::string missingTags;
    for (const auto& input : snapshot.resources)
    {
        stream << "\n  " << input.definition->ngxKey << '=';
        if (!input.IsPresent())
        {
            AppendMissingResource(stream, signatureStream, input.definition->ngxKey, &join);
        }
        else
        {
            AppendPresentResource(stream, signatureStream, input.definition->ngxKey, input.resource,
                                  input.subrectBaseX, input.subrectBaseY, &join);
        }

        if (input.definition->canonicalBase)
            continue;
        auto& tags = input.IsPresent() ? presentTags : missingTags;
        if (!tags.empty())
            tags += ',';
        tags += input.definition->ngxKey;
    }
    AppendOptionalTagSummary(stream, signatureStream, presentTags, missingTags);
    AppendShareNotes(stream);
    signature = signatureStream.str();
    return stream.str();
}

std::string Header(const char* phase, NVSDK_NGX_Feature featureId, uint32_t handleId, uint32_t evaluateCount,
                   ID3D12GraphicsCommandList* commandList)
{
    std::ostringstream stream;
    stream << "NGX feature-trace " << phase << " feature=" << static_cast<uint32_t>(featureId) << ' '
           << FeatureName(featureId) << " handle=" << handleId;
    if (evaluateCount != 0)
        stream << " eval=" << evaluateCount;
    if (commandList != nullptr)
        stream << " cmdList=0x" << std::hex << reinterpret_cast<uintptr_t>(commandList) << std::dec;
    return stream.str();
}

HandleState& EnsureHandle(uint32_t handleId, NVSDK_NGX_Feature featureId)
{
    auto& handle = Handles[handleId];
    if (handle.featureId != featureId && handle.evaluateCount != 0)
    {
        LOG_WARN("NGX feature-trace handle={} changed from feature {} to {}; treating as a new handle", handleId,
                 static_cast<uint32_t>(handle.featureId), static_cast<uint32_t>(featureId));
        handle = {};
    }
    handle.featureId = featureId;
    return handle;
}
} // namespace

const char* FeatureName(NVSDK_NGX_Feature featureId)
{
    switch (featureId)
    {
    case NVSDK_NGX_Feature_SuperSampling:
        return "SuperSampling";
    case NVSDK_NGX_Feature_RayReconstruction:
        return "RayReconstruction";
    case NVSDK_NGX_Feature_FrameGeneration:
        return "FrameGeneration";
    default:
        return "Other";
    }
}

void LogRequirements(NVSDK_NGX_Feature featureId, bool supported, uint32_t featureSupportedCode)
{
    if (!LoggingEnabled())
        return;

    std::lock_guard lock(StateMutex);
    if (!LoggedRequirements.insert(static_cast<uint32_t>(featureId)).second)
        return;

    LOG_INFO("NGX feature-trace requirements feature={} {} supported={} code={}", static_cast<uint32_t>(featureId),
             FeatureName(featureId), supported ? 1 : 0, featureSupportedCode);
}

void LogCreate(NVSDK_NGX_Feature featureId, uint32_t handleId, const NVSDK_NGX_Parameter* parameters,
               ID3D12GraphicsCommandList* commandList, uint32_t result)
{
    if (!LoggingEnabled())
        return;
    if (featureId != NVSDK_NGX_Feature_SuperSampling && featureId != NVSDK_NGX_Feature_RayReconstruction)
        return;

    std::lock_guard lock(StateMutex);
    LogJoinDisclaimerOnce();
    LogResourceStateNoteOnce();
    LogVendorIdentity(featureId);

    if (handleId != 0)
        Handles[handleId] = HandleState { featureId, 0, {} };

    std::ostringstream stream;
    stream << Header("create", featureId, handleId, 0, commandList) << " result=0x" << std::hex << result << std::dec;
    if (parameters != nullptr)
    {
        std::string signature;
        if (featureId == NVSDK_NGX_Feature_SuperSampling)
            stream << '\n' << DescribeSuperSampling(handleId, 0, *parameters, signature);
        else
            stream << '\n' << DescribeRayReconstruction(handleId, 0, *parameters, signature);
    }

    LOG_INFO("{}", stream.str());
}

void LogEvaluate(NVSDK_NGX_Feature featureId, uint32_t handleId, const NVSDK_NGX_Parameter* parameters,
                 ID3D12GraphicsCommandList* commandList)
{
    if (!LoggingEnabled() || parameters == nullptr)
        return;
    if (featureId != NVSDK_NGX_Feature_SuperSampling && featureId != NVSDK_NGX_Feature_RayReconstruction)
        return;

    std::lock_guard lock(StateMutex);
    LogJoinDisclaimerOnce();
    LogResourceStateNoteOnce();
    LogVendorIdentity(featureId);

    auto& handle = EnsureHandle(handleId, featureId);
    handle.evaluateCount++;

    std::string signature;
    const std::string body = featureId == NVSDK_NGX_Feature_SuperSampling
                                 ? DescribeSuperSampling(handleId, handle.evaluateCount, *parameters, signature)
                                 : DescribeRayReconstruction(handleId, handle.evaluateCount, *parameters, signature);

    if (handle.evaluateCount != 1 && signature == handle.lastSignature)
        return;

    handle.lastSignature = std::move(signature);
    LOG_INFO("{}\n{}", Header("evaluate", featureId, handleId, handle.evaluateCount, commandList), body);
}

void LogRelease(NVSDK_NGX_Feature featureId, uint32_t handleId)
{
    if (!LoggingEnabled())
        return;
    if (featureId != NVSDK_NGX_Feature_SuperSampling && featureId != NVSDK_NGX_Feature_RayReconstruction)
        return;

    uint32_t evaluateCount = 0;
    {
        std::lock_guard lock(StateMutex);
        if (const auto it = Handles.find(handleId); it != Handles.end())
        {
            evaluateCount = it->second.evaluateCount;
            Handles.erase(it);
        }
        if (JoinFor(featureId).handleId == handleId)
            ClearJoin(featureId);
    }

    LOG_INFO("NGX feature-trace release feature={} {} handle={} evals={}", static_cast<uint32_t>(featureId),
             FeatureName(featureId), handleId, evaluateCount);
}
} // namespace NgxFeatureTrace
