#pragma once

#include <upscalers/fsr2_212/FSR2Feature_Dx12_212.h>

#include <memory>

class FSRRFeatureDx12 final : public FSR2FeatureDx12_212
{
  public:
    FSRRFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters);
    ~FSRRFeatureDx12() override;

    bool InitInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters) override;

    feature_version Version() override { return { 1, 2, 0 }; }
    Upscaler GetUpscalerType() const override { return Upscaler::DLSSD; }

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
