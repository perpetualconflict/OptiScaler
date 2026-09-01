#include "pch.h"

#include "NgxFeatureTrace.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <denoisers/RrInputRegistry.h>

#include <sha1/sha1.hpp>

#include <d3d12.h>
#include <nvsdk_ngx_params.h>

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
};

constexpr std::array SuperSamplingResources = {
    SuperSamplingResource { "Color", "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y" },
    SuperSamplingResource { "Output", "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y" },
    SuperSamplingResource { "Depth", "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y" },
    SuperSamplingResource { "MotionVectors", "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y" },
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

struct HandleState
{
    uint32_t evaluateCount = 0;
    std::string lastSignature;
};

std::mutex StateMutex;
std::unordered_map<uint32_t, HandleState> Handles;
std::unordered_set<uint32_t> LoggedRequirements;
bool LoggedJoinDisclaimer = false;
bool LoggedStreamline = false;
bool LoggedDlss = false;
bool LoggedDlssd = false;

bool LoggingEnabled() { return Config::Instance()->FSRRLogInputs.value_or_default(); }

void LogJoinDisclaimerOnce()
{
    if (LoggedJoinDisclaimer)
        return;
    LoggedJoinDisclaimer = true;
    LOG_INFO("NGX feature-trace: raw ID3D12Resource identities and NGX keys are the upper join only; "
             "this log does not prove descriptor, packed-argument, or kernel conversion");
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

void AppendResource(std::ostringstream& stream, std::ostringstream& signature, const NVSDK_NGX_Parameter& parameters,
                    const SuperSamplingResource& definition)
{
    stream << "\n  " << definition.ngxKey << '=';
    ID3D12Resource* resource = nullptr;
    if (!TryGetResource(parameters, definition.ngxKey, resource))
    {
        stream << "missing";
        signature << '0';
        return;
    }

    const auto description = resource->GetDesc();
    uint32_t baseX = 0;
    uint32_t baseY = 0;
    if (definition.baseXKey)
        parameters.Get(definition.baseXKey, &baseX);
    if (definition.baseYKey)
        parameters.Get(definition.baseYKey, &baseY);

    stream << "resource=0x" << std::hex << reinterpret_cast<uintptr_t>(resource) << std::dec << ' '
           << static_cast<uint32_t>(description.Format) << ' ' << description.Width << 'x' << description.Height
           << " base=(" << baseX << ',' << baseY << ") flags=0x" << std::hex
           << static_cast<uint32_t>(description.Flags) << std::dec;
    signature << '1' << reinterpret_cast<uintptr_t>(resource) << static_cast<uint32_t>(description.Format) << '@'
              << description.Width << 'x' << description.Height << '+' << baseX << ',' << baseY;
}

std::string DescribeSuperSampling(uint32_t handleId, uint32_t evaluateCount, const NVSDK_NGX_Parameter& parameters,
                                  std::string& signature)
{
    std::ostringstream stream;
    std::ostringstream signatureStream;
    stream << "handle=" << handleId << " eval=" << evaluateCount;
    AppendScalarDump(stream, parameters);
    signatureStream << handleId << ':';
    for (const auto& definition : SuperSamplingResources)
        AppendResource(stream, signatureStream, parameters, definition);
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
    LogVendorIdentity(featureId);

    if (handleId != 0)
        Handles[handleId] = {};

    std::ostringstream stream;
    stream << Header("create", featureId, handleId, 0, commandList) << " result=0x" << std::hex << result << std::dec;
    if (parameters != nullptr)
    {
        if (featureId == NVSDK_NGX_Feature_SuperSampling)
        {
            std::string signature;
            stream << '\n' << DescribeSuperSampling(handleId, 0, *parameters, signature);
        }
        else
        {
            const auto snapshot = RayReconstruction::CaptureInputs(handleId, 0, *parameters);
            stream << '\n' << snapshot.Describe();
        }
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
    LogVendorIdentity(featureId);

    auto& handle = Handles[handleId];
    handle.evaluateCount++;

    std::string signature;
    std::string body;
    if (featureId == NVSDK_NGX_Feature_SuperSampling)
        body = DescribeSuperSampling(handleId, handle.evaluateCount, *parameters, signature);
    else
    {
        const auto snapshot = RayReconstruction::CaptureInputs(handleId, handle.evaluateCount, *parameters);
        body = snapshot.Describe();
        signature = snapshot.Signature();
        for (const auto& input : snapshot.resources)
        {
            signature.push_back(':');
            signature += std::to_string(reinterpret_cast<uintptr_t>(input.resource));
        }
    }

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
    }

    LOG_INFO("NGX feature-trace release feature={} {} handle={} evals={}", static_cast<uint32_t>(featureId),
             FeatureName(featureId), handleId, evaluateCount);
}
} // namespace NgxFeatureTrace
