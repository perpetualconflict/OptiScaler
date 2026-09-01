#pragma once

#include <cstdint>

#include <nvsdk_ngx_defs.h>

struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

namespace NgxFeatureTrace
{
const char* FeatureName(NVSDK_NGX_Feature featureId);

void LogRequirements(NVSDK_NGX_Feature featureId, bool supported, uint32_t featureSupportedCode);
void LogCreate(NVSDK_NGX_Feature featureId, uint32_t handleId, const NVSDK_NGX_Parameter* parameters,
               ID3D12GraphicsCommandList* commandList, uint32_t result);
void LogEvaluate(NVSDK_NGX_Feature featureId, uint32_t handleId, const NVSDK_NGX_Parameter* parameters,
                 ID3D12GraphicsCommandList* commandList);
void LogRelease(NVSDK_NGX_Feature featureId, uint32_t handleId);
} // namespace NgxFeatureTrace
