#pragma once

#include "../RrProfile.h"

#include <array>
#include <cstdint>
#include <memory>

#include <d3d12.h>

namespace RayReconstruction
{
enum class CanonicalizationFlag : uint32_t
{
    HardwareDepth = 1u << 0,
    PackedRoughness = 1u << 1,
    ViewSpaceNormals = 1u << 2,
    FlipMotionVectors = 1u << 3,
    HasDiffuseHitDistance = 1u << 4,
    HasSpecularHitDistance = 1u << 5,
    DiffuseHitDistanceInAlpha = 1u << 6,
    SpecularHitDistanceInAlpha = 1u << 7,
    HasPreviousLinearDepth = 1u << 8,
    HasSssGuide = 1u << 9,
    HasBiasMask = 1u << 10,
    HasColorBeforeParticles = 1u << 11,
};

constexpr uint32_t ToFlag(CanonicalizationFlag flag) { return static_cast<uint32_t>(flag); }

struct CanonicalizationDescription
{
    ID3D12GraphicsCommandList* commandList = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t flags = 0;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
    float linearDepthMin = 0.0f;
    float linearDepthMax = 0.0f;
    std::array<float, 16> inverseView {};
    std::array<float, 16> inverseProjection {};
    std::array<float, 16> previousView {};

    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* normals = nullptr;
    ID3D12Resource* roughness = nullptr;
    ID3D12Resource* diffuseAlbedo = nullptr;
    ID3D12Resource* specularAlbedo = nullptr;
    ID3D12Resource* diffuseHitDistance = nullptr;
    ID3D12Resource* specularHitDistance = nullptr;
    ID3D12Resource* sssGuide = nullptr;
    ID3D12Resource* biasMask = nullptr;
    ID3D12Resource* colorBeforeParticles = nullptr;
    std::array<std::array<uint32_t, 2>, 12> inputSubrectBases {};
};

class CanonicalizerDx12
{
  public:
    explicit CanonicalizerDx12(ID3D12Device* device);
    ~CanonicalizerDx12();

    CanonicalizerDx12(const CanonicalizerDx12&) = delete;
    CanonicalizerDx12& operator=(const CanonicalizerDx12&) = delete;

    bool IsReady() const;
    const std::string& LastError() const;

    bool Resize(uint32_t width, uint32_t height);
    bool Convert(const CanonicalizationDescription& description);
    bool PrepareSignalOutputs(ID3D12GraphicsCommandList* commandList, FfxRr12::SignalMask signals);
    bool Compose(ID3D12GraphicsCommandList* commandList, FfxRr12::SignalMask signals,
                 RecompositionMode recompositionMode, float depthDeltaCurrentColorScale,
                 float depthDeltaCurrentColorStrength, uint32_t debugOutput = 0);

    ID3D12Resource* LinearDepth() const;
    ID3D12Resource* MotionVectors() const;
    ID3D12Resource* Normals() const;
    ID3D12Resource* DiffuseAlbedo() const;
    ID3D12Resource* SpecularAlbedo() const;
    ID3D12Resource* SignalInput(FfxRr12::Signal signal) const;
    ID3D12Resource* SignalOutput(FfxRr12::Signal signal) const;
    ID3D12Resource* ComposedColor() const;

    void SetComposedColorState(D3D12_RESOURCE_STATES state);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace RayReconstruction
