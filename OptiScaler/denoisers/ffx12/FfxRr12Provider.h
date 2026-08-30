#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <d3d12.h>

namespace FfxRr12
{
enum class Signal : uint32_t
{
    AmbientOcclusion = 1u << 0,
    DirectDiffuse = 1u << 1,
    DirectSpecular = 1u << 2,
    DominantLightVisibility = 1u << 3,
    IndirectDiffuse = 1u << 4,
    IndirectSpecular = 1u << 5,
    SpecularOcclusion = 1u << 6,
};

using SignalMask = uint32_t;

constexpr SignalMask ToMask(Signal signal) { return static_cast<SignalMask>(signal); }

enum class ResourceState : uint8_t
{
    ComputeRead,
    PixelComputeRead,
    UnorderedAccess,
};

struct Resource
{
    ID3D12Resource* resource = nullptr;
    ResourceState state = ResourceState::ComputeRead;
};

struct Dimensions
{
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Float2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Matrix4x4
{
    // Row-major storage, row-vector multiplication, as required by FSR-RR 1.2.
    std::array<float, 16> values {};
};

struct Bounds
{
    float min = 0.0f;
    float max = 0.0f;
};

struct ContextDescription
{
    ID3D12Device* device = nullptr;
    Dimensions maxRenderSize {};
    SignalMask signals = 0;
    SignalMask checkerboardSignals = 0;
    bool enableDebugging = false;
    bool enableValidation = false;
};

struct SignalDescription
{
    Signal type = Signal::AmbientOcclusion;
    Resource input {};
    Resource output { nullptr, ResourceState::UnorderedAccess };
    uint32_t checkerboardOrigin = 0;

    // Used only by DominantLightVisibility.
    Float3 lightDirection {};
    Float3 lightEmission {};
    float lightAngularRadius = 0.0f;
};

struct DispatchDescription
{
    ID3D12GraphicsCommandList* commandList = nullptr;
    Resource linearDepth {};
    Resource motionVectors {};
    Resource normals {};
    Resource specularAlbedo {};
    Resource diffuseAlbedo {};
    Float3 motionVectorScale { 1.0f, 1.0f, 1.0f };
    Float2 jitterPixels {};
    Float3 cameraPositionDelta {};
    Matrix4x4 view {};
    Matrix4x4 projection {};
    Bounds linearDepthBounds {};
    Dimensions renderSize {};
    uint32_t frameIndex = 0;
    bool resetHistory = false;
    bool albedoIsLinear = true;
    std::span<const SignalDescription> signals {};
};

struct ProviderVersion
{
    uint64_t id = 0;
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    std::string name;

    bool IsAtLeast(uint32_t requiredMajor, uint32_t requiredMinor, uint32_t requiredPatch) const;
};

class Provider
{
  public:
    Provider();
    ~Provider();

    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    bool Load();
    bool QueryVersions(ID3D12Device* device);
    bool CreateContext(const ContextDescription& description);
    void DestroyContext();
    bool ConfigureFloat(uint64_t key, float value);
    bool Dispatch(const DispatchDescription& description);

    bool IsLoaded() const;
    bool HasContext() const;
    const ProviderVersion& SelectedVersion() const;
    const std::vector<ProviderVersion>& Versions() const;
    const std::string& LastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace FfxRr12
