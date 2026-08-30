#include "pch.h"

#include "FfxRr12Provider.h"

#include "Util.h"
#include "proxies/KernelBase_Proxy.h"

#include "../../../external/FidelityFX-SDK-v2/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "../../../external/FidelityFX-SDK-v2/Kits/FidelityFX/denoisers/include/ffx_denoiser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <tuple>

namespace FfxRr12
{
namespace
{
constexpr SignalMask AllSignals = (1u << 7) - 1u;

uint32_t ToFfxState(ResourceState state)
{
    switch (state)
    {
    case ResourceState::ComputeRead:
        return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    case ResourceState::PixelComputeRead:
        return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    case ResourceState::UnorderedAccess:
        return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    return FFX_API_RESOURCE_STATE_COMMON;
}

FfxApiResource ToFfxResource(const Resource& resource)
{
    return ffxApiGetResourceDX12(resource.resource, ToFfxState(resource.state));
}

FfxApiFloatCoords3D ToFfxFloat3(const Float3& value) { return { value.x, value.y, value.z }; }

FfxApiMatrix4x4 ToFfxMatrix(const Matrix4x4& matrix)
{
    FfxApiMatrix4x4 result {};
    static_assert(sizeof(result) == sizeof(matrix.values));
    std::memcpy(&result, matrix.values.data(), sizeof(result));
    return result;
}

bool ParseVersion(std::string_view name, ProviderVersion& version)
{
    for (size_t offset = 0; offset < name.size(); ++offset)
    {
        if (!std::isdigit(static_cast<unsigned char>(name[offset])))
            continue;

        uint32_t major = 0;
        uint32_t minor = 0;
        uint32_t patch = 0;
        if (sscanf_s(name.data() + offset, "%u.%u.%u", &major, &minor, &patch) == 3)
        {
            version.major = major;
            version.minor = minor;
            version.patch = patch;
            return true;
        }
    }

    return false;
}

const char* ReturnCodeName(ffxReturnCode_t code)
{
    switch (code)
    {
    case FFX_API_RETURN_OK:
        return "OK";
    case FFX_API_RETURN_ERROR:
        return "error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
        return "unknown descriptor type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
        return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER:
        return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY:
        return "memory error";
    case FFX_API_RETURN_ERROR_PARAMETER:
        return "invalid parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
        return "provider does not support descriptor";
    default:
        return "unknown error";
    }
}

std::string DescribeResult(ffxReturnCode_t code)
{
    return std::string(ReturnCodeName(code)) + " (" + std::to_string(code) + ")";
}

void AppendHeader(ffxDispatchDescHeader*& tail, ffxDispatchDescHeader& next)
{
    tail->pNext = &next;
    tail = &next;
}
} // namespace

struct Provider::Impl
{
    HMODULE module = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxQuery query = nullptr;
    ffxContext context = nullptr;
    ContextDescription contextDescription {};
    ProviderVersion selectedVersion {};
    std::vector<ProviderVersion> versions;
    std::string lastError;

    bool Fail(std::string message)
    {
        lastError = std::move(message);
        LOG_ERROR("FSR-RR 1.2: {}", lastError);
        return false;
    }

    bool ValidateContextDescription(const ContextDescription& description)
    {
        if (description.device == nullptr)
            return Fail("context creation rejected: device is null");
        if (description.maxRenderSize.width == 0 || description.maxRenderSize.height == 0)
            return Fail("context creation rejected: maximum render size is empty");
        if (description.signals == 0 || (description.signals & ~AllSignals) != 0)
            return Fail("context creation rejected: signal mask is empty or contains unknown bits");
        if ((description.checkerboardSignals & ~description.signals) != 0)
            return Fail("context creation rejected: checkerboard signals are not a subset of active signals");
        return true;
    }

    bool ValidateDispatch(const DispatchDescription& description)
    {
        if (context == nullptr)
            return Fail("dispatch rejected: context is not initialized");
        if (description.commandList == nullptr)
            return Fail("dispatch rejected: command list is null");
        if (description.renderSize.width == 0 || description.renderSize.height == 0 ||
            description.renderSize.width > contextDescription.maxRenderSize.width ||
            description.renderSize.height > contextDescription.maxRenderSize.height)
            return Fail("dispatch rejected: render size is empty or exceeds the context maximum");
        if (description.linearDepth.resource == nullptr || description.motionVectors.resource == nullptr ||
            description.normals.resource == nullptr)
            return Fail("dispatch rejected: canonical depth, motion, or normal resource is missing");
        if (!std::isfinite(description.linearDepthBounds.min) || !std::isfinite(description.linearDepthBounds.max) ||
            description.linearDepthBounds.min < 0.0f ||
            description.linearDepthBounds.min > description.linearDepthBounds.max)
            return Fail("dispatch rejected: linear depth bounds are invalid");

        SignalMask dispatchSignals = 0;
        for (const auto& signal : description.signals)
        {
            const SignalMask bit = ToMask(signal.type);
            if ((bit & AllSignals) == 0 || (dispatchSignals & bit) != 0)
                return Fail("dispatch rejected: signal list contains an unknown or duplicate signal");
            if (signal.input.resource == nullptr || signal.output.resource == nullptr)
                return Fail("dispatch rejected: an active signal is missing its input or output resource");
            if (signal.checkerboardOrigin > 1)
                return Fail("dispatch rejected: checkerboard origin must be zero or one");
            if ((contextDescription.checkerboardSignals & bit) == 0 && signal.checkerboardOrigin != 0)
                return Fail("dispatch rejected: checkerboard origin was supplied for a non-checkerboard signal");
            dispatchSignals |= bit;
        }

        if (dispatchSignals != contextDescription.signals)
            return Fail("dispatch rejected: signal descriptors do not exactly match the context signal mask");

        constexpr SignalMask DiffuseSignals = ToMask(Signal::DirectDiffuse) | ToMask(Signal::IndirectDiffuse) |
                                              ToMask(Signal::DominantLightVisibility);
        constexpr SignalMask SpecularSignals = ToMask(Signal::DirectSpecular) | ToMask(Signal::IndirectSpecular) |
                                               ToMask(Signal::DominantLightVisibility);
        if ((dispatchSignals & DiffuseSignals) != 0 && description.diffuseAlbedo.resource == nullptr)
            return Fail("dispatch rejected: diffuse albedo is required by the active signal mask");
        if ((dispatchSignals & SpecularSignals) != 0 && description.specularAlbedo.resource == nullptr)
            return Fail("dispatch rejected: specular albedo is required by the active signal mask");

        return true;
    }
};

bool ProviderVersion::IsAtLeast(uint32_t requiredMajor, uint32_t requiredMinor, uint32_t requiredPatch) const
{
    return std::tie(major, minor, patch) >= std::tie(requiredMajor, requiredMinor, requiredPatch);
}

Provider::Provider() : _impl(std::make_unique<Impl>()) {}

Provider::~Provider() { DestroyContext(); }

bool Provider::Load()
{
    if (_impl->module != nullptr)
        return true;

    HMODULE memoryModule = nullptr;
    HMODULE loadedModule = nullptr;
    Util::LoadProxyLibrary(L"amd_fidelityfx_denoiser_dx12.dll",
                           Config::Instance()->MainDllPath.value_or(L""),
                           Config::Instance()->FfxDx12RRPath.value_or(L""), &memoryModule, &loadedModule);
    _impl->module = loadedModule != nullptr ? loadedModule : memoryModule;
    if (_impl->module == nullptr)
        return _impl->Fail("provider load failed: amd_fidelityfx_denoiser_dx12.dll was not found");

    _impl->configure = reinterpret_cast<PfnFfxConfigure>(
        KernelBaseProxy::GetProcAddress_()(_impl->module, "ffxConfigure"));
    _impl->createContext = reinterpret_cast<PfnFfxCreateContext>(
        KernelBaseProxy::GetProcAddress_()(_impl->module, "ffxCreateContext"));
    _impl->destroyContext = reinterpret_cast<PfnFfxDestroyContext>(
        KernelBaseProxy::GetProcAddress_()(_impl->module, "ffxDestroyContext"));
    _impl->dispatch =
        reinterpret_cast<PfnFfxDispatch>(KernelBaseProxy::GetProcAddress_()(_impl->module, "ffxDispatch"));
    _impl->query = reinterpret_cast<PfnFfxQuery>(KernelBaseProxy::GetProcAddress_()(_impl->module, "ffxQuery"));

    if (_impl->configure == nullptr || _impl->createContext == nullptr || _impl->destroyContext == nullptr ||
        _impl->dispatch == nullptr || _impl->query == nullptr)
        return _impl->Fail("provider load failed: one or more required FFX API exports are missing");

    _impl->lastError.clear();
    LOG_INFO("FSR-RR 1.2: provider exports loaded");
    return true;
}

bool Provider::QueryVersions(ID3D12Device* device)
{
    if (!Load())
        return false;
    if (device == nullptr)
        return _impl->Fail("version query rejected: device is null");

    uint64_t versionCount = 0;
    ffxQueryDescGetVersions queryDescription {};
    queryDescription.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    queryDescription.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    queryDescription.device = device;
    queryDescription.outputCount = &versionCount;

    auto result = _impl->query(nullptr, &queryDescription.header);
    if (result != FFX_API_RETURN_OK || versionCount == 0)
        return _impl->Fail("version query failed: " + DescribeResult(result));

    std::vector<uint64_t> ids(versionCount);
    std::vector<const char*> names(versionCount);
    queryDescription.versionIds = ids.data();
    queryDescription.versionNames = names.data();
    result = _impl->query(nullptr, &queryDescription.header);
    if (result != FFX_API_RETURN_OK)
        return _impl->Fail("version enumeration failed: " + DescribeResult(result));

    _impl->versions.clear();
    _impl->selectedVersion = {};
    for (uint64_t index = 0; index < versionCount; ++index)
    {
        ProviderVersion version {};
        version.id = ids[index];
        version.name = names[index] != nullptr ? names[index] : "<unnamed>";
        if (!ParseVersion(version.name, version))
        {
            LOG_WARN("FSR-RR 1.2: ignoring provider with unparseable version name '{}'", version.name);
            continue;
        }

        _impl->versions.push_back(version);
        if (version.IsAtLeast(1, 2, 0) &&
            (!_impl->selectedVersion.IsAtLeast(1, 2, 0) ||
             std::tie(version.major, version.minor, version.patch) >
                 std::tie(_impl->selectedVersion.major, _impl->selectedVersion.minor,
                          _impl->selectedVersion.patch)))
            _impl->selectedVersion = version;
    }

    if (!_impl->selectedVersion.IsAtLeast(1, 2, 0))
        return _impl->Fail("no compatible FSR Ray Regeneration 1.2 provider was reported");

    _impl->lastError.clear();
    LOG_INFO("FSR-RR 1.2: selected provider '{}' (id 0x{:X})", _impl->selectedVersion.name,
             _impl->selectedVersion.id);
    return true;
}

bool Provider::CreateContext(const ContextDescription& description)
{
    DestroyContext();
    if (!_impl->ValidateContextDescription(description) || !QueryVersions(description.device))
        return false;

    ffxOverrideVersion versionOverride {};
    versionOverride.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    versionOverride.versionId = _impl->selectedVersion.id;

    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.header.pNext = &versionOverride.header;
    backend.device = description.device;

    ffxCreateContextDescDenoiser denoiser {};
    denoiser.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    denoiser.header.pNext = &backend.header;
    denoiser.version = FFX_DENOISER_VERSION;
    denoiser.maxRenderSize = { description.maxRenderSize.width, description.maxRenderSize.height };
    denoiser.signalFlags = description.signals;
    denoiser.checkerboardSignalFlags = description.checkerboardSignals;
    denoiser.flags = (description.enableDebugging ? FFX_DENOISER_ENABLE_DEBUGGING : 0u) |
                     (description.enableValidation ? FFX_DENOISER_ENABLE_VALIDATION : 0u);

    const auto result = _impl->createContext(&_impl->context, &denoiser.header, nullptr);
    if (result != FFX_API_RETURN_OK)
    {
        _impl->context = nullptr;
        return _impl->Fail("context creation failed: " + DescribeResult(result));
    }

    _impl->contextDescription = description;
    _impl->lastError.clear();
    LOG_INFO("FSR-RR 1.2: context created for {}x{}, signals=0x{:02X}, checkerboard=0x{:02X}",
             description.maxRenderSize.width, description.maxRenderSize.height, description.signals,
             description.checkerboardSignals);
    return true;
}

void Provider::DestroyContext()
{
    if (_impl->context == nullptr)
        return;

    const auto result = _impl->destroyContext(&_impl->context, nullptr);
    if (result != FFX_API_RETURN_OK)
        _impl->Fail("context destruction failed: " + DescribeResult(result));
    _impl->context = nullptr;
    _impl->contextDescription = {};
}

bool Provider::ConfigureFloat(uint64_t key, float value)
{
    if (_impl->context == nullptr)
        return _impl->Fail("configuration rejected: context is not initialized");
    if (!std::isfinite(value))
        return _impl->Fail("configuration rejected: value is not finite");

    ffxConfigureDescDenoiserKeyValue description {};
    description.header.type = FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE;
    description.key = key;
    description.count = 1;
    description.data = &value;
    const auto result = _impl->configure(&_impl->context, &description.header);
    if (result != FFX_API_RETURN_OK)
        return _impl->Fail("configuration failed for key " + std::to_string(key) + ": " + DescribeResult(result));
    return true;
}

bool Provider::Dispatch(const DispatchDescription& description)
{
    if (!_impl->ValidateDispatch(description))
        return false;

    ffxDispatchDescDenoiser base {};
    base.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    base.commandList = description.commandList;
    base.linearDepth = ToFfxResource(description.linearDepth);
    base.motionVectors = ToFfxResource(description.motionVectors);
    base.normals = ToFfxResource(description.normals);
    base.specularAlbedo = ToFfxResource(description.specularAlbedo);
    base.diffuseAlbedo = ToFfxResource(description.diffuseAlbedo);
    base.motionVectorScale = ToFfxFloat3(description.motionVectorScale);
    base.jitterOffsets = { description.jitterPixels.x, description.jitterPixels.y };
    base.cameraPositionDelta = ToFfxFloat3(description.cameraPositionDelta);
    base.view = ToFfxMatrix(description.view);
    base.projection = ToFfxMatrix(description.projection);
    base.linearDepthBounds = { description.linearDepthBounds.min, description.linearDepthBounds.max };
    base.renderSize = { description.renderSize.width, description.renderSize.height };
    base.frameIndex = description.frameIndex;
    base.flags = (description.resetHistory ? FFX_DENOISER_DISPATCH_RESET : 0u) |
                 (description.albedoIsLinear ? FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO : 0u);

    ffxDispatchDescDenoiserAmbientOcclusion ambientOcclusion {};
    ffxDispatchDescDenoiserDirectDiffuse directDiffuse {};
    ffxDispatchDescDenoiserDirectSpecular directSpecular {};
    ffxDispatchDescDenoiserDominantLight dominantLight {};
    ffxDispatchDescDenoiserIndirectDiffuse indirectDiffuse {};
    ffxDispatchDescDenoiserIndirectSpecular indirectSpecular {};
    ffxDispatchDescDenoiserSpecularOcclusion specularOcclusion {};
    ffxDispatchDescHeader* tail = &base.header;

    const auto setSignal = [](FfxApiDenoiserSignal& target, const SignalDescription& source)
    {
        target.input = ToFfxResource(source.input);
        target.output = ToFfxResource(source.output);
        target.checkerboardOrigin = source.checkerboardOrigin;
    };

    for (const auto& signal : description.signals)
    {
        switch (signal.type)
        {
        case Signal::AmbientOcclusion:
            ambientOcclusion.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION;
            setSignal(ambientOcclusion.signal, signal);
            AppendHeader(tail, ambientOcclusion.header);
            break;
        case Signal::DirectDiffuse:
            directDiffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
            setSignal(directDiffuse.signal, signal);
            AppendHeader(tail, directDiffuse.header);
            break;
        case Signal::DirectSpecular:
            directSpecular.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
            setSignal(directSpecular.signal, signal);
            AppendHeader(tail, directSpecular.header);
            break;
        case Signal::DominantLightVisibility:
            dominantLight.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT;
            setSignal(dominantLight.signal, signal);
            dominantLight.direction = ToFfxFloat3(signal.lightDirection);
            dominantLight.emission = ToFfxFloat3(signal.lightEmission);
            dominantLight.angularRadius = signal.lightAngularRadius;
            AppendHeader(tail, dominantLight.header);
            break;
        case Signal::IndirectDiffuse:
            indirectDiffuse.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE;
            setSignal(indirectDiffuse.signal, signal);
            AppendHeader(tail, indirectDiffuse.header);
            break;
        case Signal::IndirectSpecular:
            indirectSpecular.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR;
            setSignal(indirectSpecular.signal, signal);
            AppendHeader(tail, indirectSpecular.header);
            break;
        case Signal::SpecularOcclusion:
            specularOcclusion.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION;
            setSignal(specularOcclusion.signal, signal);
            AppendHeader(tail, specularOcclusion.header);
            break;
        }
    }

    const auto result = _impl->dispatch(&_impl->context, &base.header);
    if (result != FFX_API_RETURN_OK)
        return _impl->Fail("dispatch failed: " + DescribeResult(result));

    _impl->lastError.clear();
    return true;
}

bool Provider::IsLoaded() const { return _impl->module != nullptr && _impl->createContext != nullptr; }

bool Provider::HasContext() const { return _impl->context != nullptr; }

const ProviderVersion& Provider::SelectedVersion() const { return _impl->selectedVersion; }

const std::vector<ProviderVersion>& Provider::Versions() const { return _impl->versions; }

const std::string& Provider::LastError() const { return _impl->lastError; }
} // namespace FfxRr12
