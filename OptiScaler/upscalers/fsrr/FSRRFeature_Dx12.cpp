#include "pch.h"

#include "FSRRFeature_Dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <denoisers/RrInputRegistry.h>
#include <denoisers/RrProfile.h>
#include <denoisers/dx12/RrCanonicalizerDx12.h>
#include <denoisers/ffx12/FfxRr12Provider.h>

#include <DirectXMath.h>
#include <sha1/sha1.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{
using namespace DirectX;
using namespace RayReconstruction;

void LogDlssdTraceIdentity()
{
    if (!Config::Instance()->FSRRLogInputs.value_or_default())
        return;

    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       const auto& state = State::Instance();
                       LOG_INFO("FSR-RR trace Streamline version: {}.{}.{}", state.streamlineVersion.major,
                                state.streamlineVersion.minor, state.streamlineVersion.patch);
                       if (!state.NVNGX_DLSSD_Path)
                       {
                           LOG_WARN("FSR-RR trace could not resolve the selected nvngx_dlssd.dll path");
                           return;
                       }

                       const std::filesystem::path path(*state.NVNGX_DLSSD_Path);
                       version_t version;
                       const bool hasVersion = Util::GetFileVersion(path.wstring(), &version, nullptr);
                       std::error_code sizeError;
                       const auto fileSize = std::filesystem::file_size(path, sizeError);
                       std::ifstream file(path, std::ios::binary);
                       if (!file)
                       {
                           LOG_WARN("FSR-RR trace could not open selected DLSS-D binary: {}", path.string());
                           return;
                       }
                       SHA1 checksum;
                       checksum.update(file);
                       LOG_INFO("FSR-RR trace DLSS-D binary: path={} version={}.{}.{} bytes={} sha1={}", path.string(),
                                hasVersion ? version.major : 0, hasVersion ? version.minor : 0,
                                hasVersion ? version.patch : 0, sizeError ? 0 : fileSize, checksum.final());
                   });
}

constexpr auto RequiredCompositeSignals =
    FfxRr12::ToMask(FfxRr12::Signal::DirectDiffuse) |
    FfxRr12::ToMask(FfxRr12::Signal::IndirectSpecular);

const ResourceInput* Required(const InputSnapshot& snapshot, InputSemantic semantic, std::string& reason)
{
    const auto* input = snapshot.Find(semantic);
    if (!input || !input->IsPresent())
    {
        reason = std::string(ToString(semantic)) + " is required by the active adapter";
        return nullptr;
    }
    const auto& description = input->description;
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1 ||
        description.SampleDesc.Count != 1 || description.Format == DXGI_FORMAT_UNKNOWN ||
        static_cast<uint64_t>(input->subrectBaseX) + snapshot.renderWidth > description.Width ||
        static_cast<uint64_t>(input->subrectBaseY) + snapshot.renderHeight > description.Height)
    {
        reason = std::string(ToString(semantic)) + " is present but incompatible with the render subrect";
        return nullptr;
    }
    return input;
}

const ResourceInput* OptionalUsable(const InputSnapshot& snapshot, InputSemantic semantic)
{
    std::string ignored;
    return Required(snapshot, semantic, ignored);
}

const char* DebugOutputName(uint32_t output)
{
    static constexpr std::array names = {
        "recomposed_result",
        "denoised_diffuse",
        "denoised_specular",
        "residual",
        "noisy_diffuse",
        "noisy_specular",
        "motion_vectors",
        "depth_delta",
        "depth_risk_current_color",
        "sss_guide_overlay",
        "current_color_bias_overlay",
        "particle_buffer_overlay",
        "sss_current_color_ab",
        "bias_current_color_ab",
        "proof_particle_composite_ab",
        "pure_denoised_composite",
        "full_current_color",
        "sss_denoised_ab",
        "bias_denoised_ab",
        "residual_energy_overlay",
        "sss_residual_suppression_ab",
    };
    return output < names.size() ? names[output] : "unknown";
}

std::array<float, 16> Transpose(const std::array<float, 16>& input)
{
    std::array<float, 16> output {};
    for (size_t row = 0; row < 4; ++row)
        for (size_t column = 0; column < 4; ++column)
            output[row * 4 + column] = input[column * 4 + row];
    return output;
}

std::array<float, 16> ConvertMatrix(const std::array<float, 16>& input, MatrixConversion conversion)
{
    return conversion == MatrixConversion::Transpose ? Transpose(input) : input;
}

XMMATRIX LoadMatrix(const std::array<float, 16>& input)
{
    XMFLOAT4X4 value;
    std::memcpy(&value, input.data(), sizeof(value));
    return XMLoadFloat4x4(&value);
}

std::array<float, 16> StoreMatrix(FXMMATRIX input)
{
    XMFLOAT4X4 value;
    XMStoreFloat4x4(&value, input);
    std::array<float, 16> output {};
    std::memcpy(output.data(), &value, sizeof(value));
    return output;
}

bool Invert(const std::array<float, 16>& input, std::array<float, 16>& inverse)
{
    XMVECTOR determinant;
    const XMMATRIX result = XMMatrixInverse(&determinant, LoadMatrix(input));
    const float value = XMVectorGetX(determinant);
    if (!std::isfinite(value) || std::abs(value) < 1e-8f)
        return false;
    inverse = StoreMatrix(result);
    return std::ranges::all_of(inverse, [](float item) { return std::isfinite(item); });
}

FfxRr12::Float3 CameraPosition(const std::array<float, 16>& inverseView)
{
    return { inverseView[12], inverseView[13], inverseView[14] };
}

class ScopedResourceTransitions
{
  public:
    ScopedResourceTransitions(ID3D12GraphicsCommandList* commandList, const InputSnapshot& snapshot)
        : _commandList(commandList)
    {
        auto& config = *Config::Instance();
        Add(snapshot, InputSemantic::Color, config.ColorResourceBarrier.value_for_config());
        Add(snapshot, InputSemantic::Depth, config.DepthResourceBarrier.value_for_config());
        Add(snapshot, InputSemantic::MotionVectors, config.MVResourceBarrier.value_for_config());
    }

    ~ScopedResourceTransitions()
    {
        for (auto iterator = _resources.rbegin(); iterator != _resources.rend(); ++iterator)
            Barrier(iterator->resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, iterator->originalState);
    }

  private:
    struct Entry
    {
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES originalState;
    };

    void Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (!resource || before == after)
            return;
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _commandList->ResourceBarrier(1, &barrier);
    }

    void Add(const InputSnapshot& snapshot, InputSemantic semantic, std::optional<uint32_t> state)
    {
        if (!state)
            return;
        const auto* input = snapshot.Find(semantic);
        if (!input || !input->IsPresent())
            return;
        const auto original = static_cast<D3D12_RESOURCE_STATES>(*state);
        Barrier(input->resource, original, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _resources.push_back({ input->resource, original });
    }

    ID3D12GraphicsCommandList* _commandList;
    std::vector<Entry> _resources;
};

class ScopedNgxResourceReplacement
{
  public:
    ScopedNgxResourceReplacement(NVSDK_NGX_Parameter* parameters, const char* key, ID3D12Resource* original,
                                 ID3D12Resource* replacement)
        : _parameters(parameters), _key(key), _original(original)
    {
        _parameters->Set(_key, replacement);
    }

    ~ScopedNgxResourceReplacement() { _parameters->Set(_key, _original); }

  private:
    NVSDK_NGX_Parameter* _parameters;
    const char* _key;
    ID3D12Resource* _original;
};
} // namespace

struct FSRRFeatureDx12::Impl
{
    FfxRr12::Provider provider;
    ProfileDatabase profiles;
    const Profile* profile = nullptr;
    std::unique_ptr<CanonicalizerDx12> canonicalizer;
    bool bridgeReady = false;
    bool captureOnly = false;
    bool contextReady = false;
    bool resetHistory = true;
    bool hasPreviousCamera = false;
    bool loggedFirstDispatch = false;
    std::optional<uint32_t> lastDebugOutput;
    uint32_t contextWidth = 0;
    uint32_t contextHeight = 0;
    std::array<float, 16> previousView {};
    FfxRr12::Float3 previousCamera {};
    std::string lastInventorySignature;
    std::string lastFailureKey;
    std::string lastWarningKey;

    void FailOnce(const InputSnapshot* snapshot, const std::string& reason)
    {
        const std::string key = reason + (snapshot ? snapshot->Signature() : "");
        if (key != lastFailureKey)
        {
            LOG_WARN("FSR-RR bypass: {}", reason);
            lastFailureKey = key;
        }
        resetHistory = true;
    }
};

FSRRFeatureDx12::FSRRFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters)
    : IFeature(handleId, parameters), FSR2FeatureDx12_212(handleId, parameters), _impl(std::make_unique<Impl>())
{
}

FSRRFeatureDx12::~FSRRFeatureDx12() = default;

bool FSRRFeatureDx12::EvaluateFallback(ID3D12GraphicsCommandList* commandList,
                                      NVSDK_NGX_Parameter* parameters)
{
    const bool result = FSR2FeatureDx12_212::EvaluateInternal(commandList, parameters);

    auto& changeRequested = State::Instance().changeBackend[Handle()->Id];
    if (changeRequested)
    {
        LOG_WARN("FSR-RR ignored a backend replacement requested by its private FSR 2.1.2 fallback");
        changeRequested = false;
    }

    return result;
}

bool FSRRFeatureDx12::InitInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters)
{
    if (!IFeature::AutoExposure())
        LOG_INFO("FSR-RR is forcing auto exposure for its private FSR 2.1.2 fallback context");

    if (!FSR2FeatureDx12_212::InitInternal(commandList, parameters))
        return false;

    // The pre-device Streamline/NGX advertisement is optimistic. Keep later capability reads honest unless
    // profile, device, provider version, and canonicalizer validation all succeed below.
    parameters->Set("SuperSamplingDenoising.Available", 0);
    parameters->Set("SuperSamplingDenoising.FeatureInitResult", 0);

    if (!Config::Instance()->FSRREnabled.value_or_default())
    {
        LOG_INFO("FSR-RR bridge is disabled; using the FSR 2.1.2 fallback");
        return true;
    }

    LogDlssdTraceIdentity();

    if (Config::Instance()->FSRRCaptureOnly.value_or_default())
    {
        LOG_INFO("FSR-RR capture-only mode armed; inputs will be inventoried without denoiser dispatch");
        parameters->Set("SuperSamplingDenoising.Available", 1);
        parameters->Set("SuperSamplingDenoising.FeatureInitResult",
                        static_cast<uint32_t>(NVSDK_NGX_Result_Success));
        _impl->captureOnly = true;
        return true;
    }

    const auto profilePath =
        std::filesystem::path(Config::Instance()->MainDllPath.value_or(L"")) / L"profiles" / L"fsrr.json";
    if (!_impl->profiles.Load(profilePath))
    {
        _impl->FailOnce(nullptr, "profile database could not be loaded: " + _impl->profiles.LastError());
        return true;
    }

    _impl->profile = _impl->profiles.FindForExecutable(Util::ExePath().string());
    if (!_impl->profile)
    {
        _impl->FailOnce(nullptr, "no executable profile matches " + Util::ExePath().filename().string());
        return true;
    }

    std::string profileReason;
    if (!_impl->profile->IsDispatchable(profileReason))
    {
        _impl->FailOnce(nullptr, "profile '" + _impl->profile->id + "' is not dispatchable: " + profileReason);
        return true;
    }
    if (_impl->profile->signalAdapter != SignalAdapter::CompositeAlbedoSplit ||
        _impl->profile->signals != RequiredCompositeSignals)
    {
        _impl->FailOnce(nullptr,
                        "profile requests a signal adapter or mask not implemented by the initial composition path");
        return true;
    }

    if (!_impl->provider.Load() || !_impl->provider.QueryVersions(Device))
    {
        _impl->FailOnce(nullptr, "FSR-RR 1.2 provider unavailable: " + _impl->provider.LastError());
        return true;
    }

    _impl->canonicalizer = std::make_unique<CanonicalizerDx12>(Device);
    if (!_impl->canonicalizer->IsReady())
    {
        _impl->FailOnce(nullptr, "canonicalizer unavailable: " + _impl->canonicalizer->LastError());
        _impl->canonicalizer.reset();
        return true;
    }

    const auto& version = _impl->provider.SelectedVersion();
    const auto depthDeltaSource =
        _impl->profile->depthDeltaSource == DepthDeltaSource::ReprojectedHistory ? "reprojected_history"
                                                                                : "camera_reprojection";
    const auto recompositionMode =
        _impl->profile->recompositionMode == RecompositionMode::DepthDeltaCurrentColor
            ? "depth_delta_current_color"
            : "denoised";
    LOG_INFO(
        "FSR-RR bridge armed with provider {}.{}.{} ({}) and profile '{}' "
        "(depth_delta_source={}, recomposition={}, depth_delta_current_color_scale={}, "
        "depth_delta_current_color_strength={})",
        version.major, version.minor, version.patch, version.name, _impl->profile->id, depthDeltaSource,
        recompositionMode, _impl->profile->depthDeltaCurrentColorScale,
        _impl->profile->depthDeltaCurrentColorStrength);
    parameters->Set("SuperSamplingDenoising.Available", 1);
    parameters->Set("SuperSamplingDenoising.FeatureInitResult", static_cast<uint32_t>(NVSDK_NGX_Result_Success));
    _impl->bridgeReady = true;
    return true;
}

bool FSRRFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters)
{
    if (!_impl->bridgeReady && !_impl->captureOnly)
        return EvaluateFallback(commandList, parameters);

    const auto snapshot = CaptureInputs(Handle()->Id, static_cast<uint32_t>(_frameCount), *parameters);
    if (Config::Instance()->FSRRLogInputs.value_or_default() &&
        snapshot.Signature() != _impl->lastInventorySignature)
    {
        LOG_INFO("FSR-RR normalized input inventory changed:\n{}", snapshot.Describe());
        _impl->lastInventorySignature = snapshot.Signature();
        _impl->resetHistory = true;
    }

    const auto validation = snapshot.ValidateInventory();
    if (validation.HasErrors())
    {
        _impl->FailOnce(&snapshot, validation.Summary());
        return EvaluateFallback(commandList, parameters);
    }
    const std::string warningKey = validation.Summary() + snapshot.Signature();
    if (!validation.issues.empty() && warningKey != _impl->lastWarningKey)
    {
        LOG_WARN("FSR-RR optional input warning: {}", validation.Summary());
        _impl->lastWarningKey = warningKey;
    }
    else if (validation.issues.empty())
        _impl->lastWarningKey.clear();

    if (_impl->captureOnly)
    {
        _impl->lastFailureKey.clear();
        return EvaluateFallback(commandList, parameters);
    }

    std::string reason;
    const auto* color = Required(snapshot, InputSemantic::Color, reason);
    const auto* depth = Required(snapshot, InputSemantic::Depth, reason);
    const auto* motion = Required(snapshot, InputSemantic::MotionVectors, reason);
    const auto* normals = Required(snapshot, InputSemantic::Normals, reason);
    const auto* diffuseAlbedo = Required(snapshot, InputSemantic::DiffuseAlbedo, reason);
    const auto* specularAlbedo = Required(snapshot, InputSemantic::SpecularAlbedo, reason);
    if (!color || !depth || !motion || !normals || !diffuseAlbedo || !specularAlbedo)
    {
        _impl->FailOnce(&snapshot, reason);
        return EvaluateFallback(commandList, parameters);
    }

    if (!snapshot.depthType ||
        (*snapshot.depthType == 1) != (_impl->profile->depthConvention == DepthConvention::Hardware))
    {
        _impl->FailOnce(&snapshot, "DLSS depth metadata disagrees with the executable profile");
        return EvaluateFallback(commandList, parameters);
    }
    if (!snapshot.roughnessMode)
    {
        _impl->FailOnce(&snapshot, "DLSS roughness mode is missing");
        return EvaluateFallback(commandList, parameters);
    }

    const bool packedRoughness = *snapshot.roughnessMode == 1;
    const auto* roughness = OptionalUsable(snapshot, InputSemantic::Roughness);
    const auto* sssGuide = OptionalUsable(snapshot, InputSemantic::ScreenSpaceSubsurfaceScatteringGuide);
    const auto* biasMask = OptionalUsable(snapshot, InputSemantic::BiasMask);
    const auto* colorBeforeParticles = OptionalUsable(snapshot, InputSemantic::ColorBeforeParticles);
    if (!packedRoughness && !roughness)
    {
        _impl->FailOnce(&snapshot, "unpacked roughness mode requires GBuffer.Roughness");
        return EvaluateFallback(commandList, parameters);
    }

    const auto currentView = ConvertMatrix(snapshot.worldToView, _impl->profile->matrixConversion);
    const auto currentProjection = ConvertMatrix(snapshot.viewToClip, _impl->profile->matrixConversion);
    std::array<float, 16> inverseView {};
    std::array<float, 16> inverseProjection {};
    if (!Invert(currentView, inverseView) || !Invert(currentProjection, inverseProjection))
    {
        _impl->FailOnce(&snapshot, "camera matrices are singular or non-finite");
        return EvaluateFallback(commandList, parameters);
    }

    if (!_impl->contextReady || _impl->contextWidth != snapshot.renderWidth ||
        _impl->contextHeight != snapshot.renderHeight)
    {
        _impl->provider.DestroyContext();
        _impl->contextReady = false;
        if (!_impl->canonicalizer->Resize(snapshot.renderWidth, snapshot.renderHeight))
        {
            _impl->FailOnce(&snapshot, "canonical resource resize failed: " + _impl->canonicalizer->LastError());
            return EvaluateFallback(commandList, parameters);
        }

        FfxRr12::ContextDescription contextDescription;
        contextDescription.device = Device;
        contextDescription.maxRenderSize = { snapshot.renderWidth, snapshot.renderHeight };
        contextDescription.signals = _impl->profile->signals;
        contextDescription.checkerboardSignals = _impl->profile->checkerboardSignals;
        contextDescription.enableDebugging = Config::Instance()->FSRRDebugValidation.value_or_default();
        contextDescription.enableValidation = Config::Instance()->FSRRDebugValidation.value_or_default();
        if (!_impl->provider.CreateContext(contextDescription))
        {
            _impl->FailOnce(&snapshot, "context creation failed: " + _impl->provider.LastError());
            return EvaluateFallback(commandList, parameters);
        }

        _impl->contextReady = true;
        _impl->contextWidth = snapshot.renderWidth;
        _impl->contextHeight = snapshot.renderHeight;
        _impl->hasPreviousCamera = false;
        _impl->resetHistory = true;
    }

    bool rrSucceeded = false;
    const auto currentCamera = CameraPosition(inverseView);
    const auto previousView = _impl->hasPreviousCamera ? _impl->previousView : currentView;
    const FfxRr12::Float3 cameraDelta =
        _impl->hasPreviousCamera
            ? FfxRr12::Float3 { _impl->previousCamera.x - currentCamera.x,
                               _impl->previousCamera.y - currentCamera.y,
                               _impl->previousCamera.z - currentCamera.z }
            : FfxRr12::Float3 {};

    const auto* specularHitDistance = OptionalUsable(snapshot, InputSemantic::SpecularRayDirectionHitDistance);
    bool specularHitInAlpha = specularHitDistance != nullptr;
    if (!specularHitDistance)
    {
        specularHitDistance = OptionalUsable(snapshot, InputSemantic::SpecularHitDistance);
        specularHitInAlpha = false;
    }
    if (!specularHitDistance || !specularHitDistance->IsPresent())
    {
        _impl->FailOnce(&snapshot, "the composite adapter requires a specular hit-distance input");
        return EvaluateFallback(commandList, parameters);
    }

    {
        ScopedResourceTransitions transitions(commandList, snapshot);
        CanonicalizationDescription conversion;
        conversion.commandList = commandList;
        conversion.width = snapshot.renderWidth;
        conversion.height = snapshot.renderHeight;
        conversion.motionScaleX = snapshot.motionScaleX;
        conversion.motionScaleY = snapshot.motionScaleY;
        conversion.linearDepthMin = _impl->profile->linearDepthMin;
        conversion.linearDepthMax = _impl->profile->linearDepthMax;
        conversion.inverseView = inverseView;
        conversion.inverseProjection = inverseProjection;
        conversion.previousView = previousView;
        conversion.color = color->resource;
        conversion.depth = depth->resource;
        conversion.motionVectors = motion->resource;
        conversion.normals = normals->resource;
        conversion.roughness = packedRoughness ? nullptr : roughness->resource;
        conversion.diffuseAlbedo = diffuseAlbedo->resource;
        conversion.specularAlbedo = specularAlbedo->resource;
        conversion.sssGuide = sssGuide ? sssGuide->resource : nullptr;
        conversion.biasMask = biasMask ? biasMask->resource : nullptr;
        conversion.colorBeforeParticles = colorBeforeParticles ? colorBeforeParticles->resource : nullptr;
        const std::array<const ResourceInput*, 12> canonicalInputs = {
            color,
            depth,
            motion,
            normals,
            packedRoughness ? nullptr : roughness,
            diffuseAlbedo,
            specularAlbedo,
            nullptr,
            specularHitDistance,
            sssGuide,
            biasMask,
            colorBeforeParticles,
        };
        for (size_t index = 0; index < canonicalInputs.size(); ++index)
            if (canonicalInputs[index] && canonicalInputs[index]->IsPresent())
                conversion.inputSubrectBases[index] = {
                    canonicalInputs[index]->subrectBaseX,
                    canonicalInputs[index]->subrectBaseY,
                };

        if (_impl->profile->depthConvention == DepthConvention::Hardware)
            conversion.flags |= ToFlag(CanonicalizationFlag::HardwareDepth);
        if (packedRoughness)
            conversion.flags |= ToFlag(CanonicalizationFlag::PackedRoughness);
        if (_impl->profile->normalSpace == NormalSpace::View)
            conversion.flags |= ToFlag(CanonicalizationFlag::ViewSpaceNormals);
        if (_impl->profile->motionVectorDirection == MotionVectorDirection::CurrentMinusPrevious)
            conversion.flags |= ToFlag(CanonicalizationFlag::FlipMotionVectors);
        if (_impl->profile->depthDeltaSource == DepthDeltaSource::ReprojectedHistory &&
            _impl->hasPreviousCamera && !_impl->resetHistory && snapshot.reset == 0)
            conversion.flags |= ToFlag(CanonicalizationFlag::HasPreviousLinearDepth);

        if (specularHitDistance && specularHitDistance->IsPresent())
        {
            conversion.specularHitDistance = specularHitDistance->resource;
            conversion.flags |= ToFlag(CanonicalizationFlag::HasSpecularHitDistance);
            if (specularHitInAlpha)
                conversion.flags |= ToFlag(CanonicalizationFlag::SpecularHitDistanceInAlpha);
        }
        if (sssGuide)
            conversion.flags |= ToFlag(CanonicalizationFlag::HasSssGuide);
        if (biasMask)
            conversion.flags |= ToFlag(CanonicalizationFlag::HasBiasMask);
        if (colorBeforeParticles)
            conversion.flags |= ToFlag(CanonicalizationFlag::HasColorBeforeParticles);

        if (!_impl->canonicalizer->Convert(conversion) ||
            !_impl->canonicalizer->PrepareSignalOutputs(commandList, _impl->profile->signals))
        {
            _impl->FailOnce(&snapshot, "input conversion failed: " + _impl->canonicalizer->LastError());
        }
        else
        {
            std::array<FfxRr12::SignalDescription, 2> signals = {
                FfxRr12::SignalDescription {
                    .type = FfxRr12::Signal::DirectDiffuse,
                    .input = { _impl->canonicalizer->SignalInput(FfxRr12::Signal::DirectDiffuse),
                               FfxRr12::ResourceState::ComputeRead },
                    .output = { _impl->canonicalizer->SignalOutput(FfxRr12::Signal::DirectDiffuse),
                                FfxRr12::ResourceState::UnorderedAccess },
                    .checkerboardOrigin =
                        (_impl->profile->checkerboardSignals &
                         FfxRr12::ToMask(FfxRr12::Signal::DirectDiffuse))
                            ? (snapshot.frameIndex & 1u)
                            : 0u,
                },
                FfxRr12::SignalDescription {
                    .type = FfxRr12::Signal::IndirectSpecular,
                    .input = { _impl->canonicalizer->SignalInput(FfxRr12::Signal::IndirectSpecular),
                               FfxRr12::ResourceState::ComputeRead },
                    .output = { _impl->canonicalizer->SignalOutput(FfxRr12::Signal::IndirectSpecular),
                                FfxRr12::ResourceState::UnorderedAccess },
                    .checkerboardOrigin =
                        (_impl->profile->checkerboardSignals &
                         FfxRr12::ToMask(FfxRr12::Signal::IndirectSpecular))
                            ? (snapshot.frameIndex & 1u)
                            : 0u,
                },
            };

            FfxRr12::DispatchDescription dispatch;
            dispatch.commandList = commandList;
            dispatch.linearDepth = { _impl->canonicalizer->LinearDepth(), FfxRr12::ResourceState::ComputeRead };
            dispatch.motionVectors = { _impl->canonicalizer->MotionVectors(), FfxRr12::ResourceState::ComputeRead };
            dispatch.normals = { _impl->canonicalizer->Normals(), FfxRr12::ResourceState::ComputeRead };
            dispatch.diffuseAlbedo = { _impl->canonicalizer->DiffuseAlbedo(), FfxRr12::ResourceState::ComputeRead };
            dispatch.specularAlbedo = { _impl->canonicalizer->SpecularAlbedo(), FfxRr12::ResourceState::ComputeRead };
            dispatch.motionVectorScale = { 1.0f, 1.0f, 1.0f };
            dispatch.jitterPixels = { snapshot.jitterX, snapshot.jitterY };
            dispatch.cameraPositionDelta = cameraDelta;
            dispatch.view.values = currentView;
            dispatch.projection.values = currentProjection;
            dispatch.linearDepthBounds = { _impl->profile->linearDepthMin, _impl->profile->linearDepthMax };
            dispatch.renderSize = { snapshot.renderWidth, snapshot.renderHeight };
            dispatch.frameIndex = snapshot.frameIndex;
            dispatch.resetHistory = snapshot.reset != 0 || _impl->resetHistory;
            dispatch.albedoIsLinear = true;
            dispatch.signals = signals;

            if (!_impl->provider.Dispatch(dispatch))
                _impl->FailOnce(&snapshot, "provider dispatch failed: " + _impl->provider.LastError());
            else
            {
                const auto debugOutput = static_cast<uint32_t>(
                    std::clamp(Config::Instance()->FSRRDebugOutput.value_or_default(), 0, 20));
                if (!_impl->lastDebugOutput || *_impl->lastDebugOutput != debugOutput)
                {
                    LOG_INFO("FSR-RR debug output changed: handle={}, frame={}, debug_output={} ({})",
                             Handle()->Id, snapshot.frameIndex, debugOutput, DebugOutputName(debugOutput));
                    _impl->lastDebugOutput = debugOutput;
                }
                if (!_impl->canonicalizer->Compose(
                        commandList, _impl->profile->signals,
                        _impl->profile->recompositionMode, _impl->profile->depthDeltaCurrentColorScale,
                        _impl->profile->depthDeltaCurrentColorStrength, debugOutput))
                    _impl->FailOnce(&snapshot, "composition failed: " + _impl->canonicalizer->LastError());
                else
                    rrSucceeded = true;
            }
        }
    }

    if (!rrSucceeded)
        return EvaluateFallback(commandList, parameters);

    _impl->previousView = currentView;
    _impl->previousCamera = currentCamera;
    _impl->hasPreviousCamera = true;
    _impl->resetHistory = false;
    _impl->lastFailureKey.clear();

    if (!_impl->loggedFirstDispatch)
    {
        LOG_INFO(
            "FSR-RR first active dispatch succeeded: handle={}, frame={}, render={}x{}, "
            "signals=direct_diffuse|indirect_specular, debug_output={}",
            Handle()->Id, snapshot.frameIndex, snapshot.renderWidth, snapshot.renderHeight,
            std::clamp(Config::Instance()->FSRRDebugOutput.value_or_default(), 0, 20));
        _impl->loggedFirstDispatch = true;
    }

    ID3D12Resource* originalColor = color->resource;
    auto* composedColor = _impl->canonicalizer->ComposedColor();
    if (Config::Instance()->ColorResourceBarrier.has_value())
    {
        const auto requested =
            static_cast<D3D12_RESOURCE_STATES>(Config::Instance()->ColorResourceBarrier.value());
        ResourceBarrier(commandList, composedColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, requested);
        _impl->canonicalizer->SetComposedColorState(requested);
    }

    ScopedNgxResourceReplacement replaceColor(parameters, NVSDK_NGX_Parameter_Color, originalColor, composedColor);
    const bool result = FSR2FeatureDx12_212::EvaluateInternal(commandList, parameters);
    if (!result)
        _impl->resetHistory = true;
    return result;
}
