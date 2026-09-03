#pragma once

#include <cstdint>

#include <nvsdk_ngx_defs.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

// Opt-in diagnostic for whether Cyberpunk (or another title) consumes the
// feature-13 DLSS-D output on the same command list passed to
// NVSDK_NGX_D3D12_EvaluateFeature, or only after a later queue submission.
// Disabled by default. Does not enable the FG/Hudfix descriptor tracker.
namespace DlssdOutputHazardTrace
{
bool Enabled();
void InstallForDevice(ID3D12Device* device);
void OnCreate(uint32_t handleId, NVSDK_NGX_Feature featureId, ID3D12GraphicsCommandList* commandList);
void OnRelease(uint32_t handleId);
void Shutdown();

class ScopedEvaluate
{
  public:
    ScopedEvaluate(uint32_t handleId, NVSDK_NGX_Feature featureId, ID3D12GraphicsCommandList* commandList,
                   const NVSDK_NGX_Parameter* parameters);
    ~ScopedEvaluate();

    ScopedEvaluate(const ScopedEvaluate&) = delete;
    ScopedEvaluate& operator=(const ScopedEvaluate&) = delete;

  private:
    uint32_t _handleId = 0;
    NVSDK_NGX_Feature _featureId {};
    ID3D12GraphicsCommandList* _commandList = nullptr;
    bool _active = false;
};
} // namespace DlssdOutputHazardTrace
