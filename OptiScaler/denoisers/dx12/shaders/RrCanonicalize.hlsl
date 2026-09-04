#define MainRS \
    "RootFlags(0)," \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors = 13))," \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_POINT, " \
        "addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP)," \
    "DescriptorTable(UAV(u0, numDescriptors = 11))"

Texture2D<float4> InColor : register(t0);
Texture2D<float> InDepth : register(t1);
Texture2D<float4> InMotion : register(t2);
Texture2D<float4> InNormals : register(t3);
Texture2D<float> InRoughness : register(t4);
Texture2D<float4> InDiffuseAlbedo : register(t5);
Texture2D<float4> InSpecularAlbedo : register(t6);
Texture2D<float4> InDiffuseHitDistance : register(t7);
Texture2D<float4> InSpecularHitDistance : register(t8);
Texture2D<float> InPreviousLinearDepth : register(t9);
Texture2D<float4> InSssGuide : register(t10);
Texture2D<float4> InBiasMask : register(t11);
Texture2D<float4> InColorBeforeParticles : register(t12);
SamplerState PointClampSampler : register(s0);

RWTexture2D<float> OutLinearDepth : register(u0);
RWTexture2D<float4> OutMotion : register(u1);
RWTexture2D<float4> OutNormals : register(u2);
RWTexture2D<float4> OutDiffuseAlbedo : register(u3);
RWTexture2D<float4> OutSpecularAlbedo : register(u4);
RWTexture2D<float4> OutDirectDiffuse : register(u5);
RWTexture2D<float4> OutIndirectSpecular : register(u6);
RWTexture2D<float4> OutResidual : register(u7);
RWTexture2D<float4> OutSssGuide : register(u8);
RWTexture2D<float4> OutBiasMask : register(u9);
RWTexture2D<float4> OutColorBeforeParticles : register(u10);

cbuffer Constants : register(b0)
{
    row_major float4x4 InverseView;
    row_major float4x4 InverseProjection;
    row_major float4x4 PreviousView;
    float4 RenderSize;
    float4 MotionAndDepthBounds;
    uint4 InputBase01;
    uint4 InputBase23;
    uint4 InputBase45;
    uint4 InputBase67;
    uint4 InputBase89;
    uint4 InputBase1011;
    uint Flags;
    float SpecularHoldoutMaxRoughness;
    float2 Padding;
};

static const uint FlagHardwareDepth = 1u << 0;
static const uint FlagPackedRoughness = 1u << 1;
static const uint FlagViewSpaceNormals = 1u << 2;
static const uint FlagFlipMotion = 1u << 3;
static const uint FlagHasDiffuseHitDistance = 1u << 4;
static const uint FlagHasSpecularHitDistance = 1u << 5;
static const uint FlagDiffuseHitDistanceInAlpha = 1u << 6;
static const uint FlagSpecularHitDistanceInAlpha = 1u << 7;
static const uint FlagHasPreviousLinearDepth = 1u << 8;
static const uint FlagHasSssGuide = 1u << 9;
static const uint FlagHasBiasMask = 1u << 10;
static const uint FlagHasColorBeforeParticles = 1u << 11;

float2 OctEncode(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6f);
    float2 encoded = n.xy;
    if (n.z < 0.0f)
        encoded = (1.0f - abs(encoded.yx)) * (encoded.xy >= 0.0f ? 1.0f : -1.0f);
    return encoded * 0.5f + 0.5f;
}

float3 ReconstructViewPosition(uint2 pixel, float depth)
{
    float2 uv = (float2(pixel) + 0.5f) * RenderSize.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 view = mul(float4(ndc, depth, 1.0f), InverseProjection);
    float safeW = abs(view.w) > 1e-6f ? view.w : (view.w < 0.0f ? -1e-6f : 1e-6f);
    return view.xyz / safeW;
}

[RootSignature(MainRS)]
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= (uint)RenderSize.x || pixel.y >= (uint)RenderSize.y)
        return;

    uint2 colorPixel = pixel + InputBase01.xy;
    uint2 depthPixel = pixel + InputBase01.zw;
    uint2 motionPixel = pixel + InputBase23.xy;
    uint2 normalPixel = pixel + InputBase23.zw;
    uint2 roughnessPixel = pixel + InputBase45.xy;
    uint2 diffuseAlbedoPixel = pixel + InputBase45.zw;
    uint2 specularAlbedoPixel = pixel + InputBase67.xy;
    uint2 diffuseHitPixel = pixel + InputBase67.zw;
    uint2 specularHitPixel = pixel + InputBase89.xy;
    uint2 sssGuidePixel = pixel + InputBase89.zw;
    uint2 biasMaskPixel = pixel + InputBase1011.xy;
    uint2 colorBeforeParticlesPixel = pixel + InputBase1011.zw;

    float4 raw = InColor[colorPixel];
    bool rawValid = all(isfinite(raw));
    if (!rawValid)
        raw = 0.0f;

    float inputDepth = InDepth[depthPixel];
    float3 viewPosition;
    float linearDepth;
    if ((Flags & FlagHardwareDepth) != 0)
    {
        viewPosition = ReconstructViewPosition(pixel, inputDepth);
        linearDepth = viewPosition.z;
    }
    else
    {
        linearDepth = inputDepth;
        float2 uv = (float2(pixel) + 0.5f) * RenderSize.zw;
        float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float4 viewRay = mul(float4(ndc, 1.0f, 1.0f), InverseProjection);
        float safeW = abs(viewRay.w) > 1e-6f ? viewRay.w : (viewRay.w < 0.0f ? -1e-6f : 1e-6f);
        viewRay.xyz /= safeW;
        float safeZ = abs(viewRay.z) > 1e-6f ? viewRay.z : (viewRay.z < 0.0f ? -1e-6f : 1e-6f);
        viewPosition = viewRay.xyz * (linearDepth / safeZ);
    }
    bool depthValid = isfinite(linearDepth) && all(isfinite(viewPosition));
    if (!depthValid)
    {
        linearDepth = max(MotionAndDepthBounds.z, 1e-4f);
        viewPosition = float3(0.0f, 0.0f, linearDepth);
    }
    OutLinearDepth[pixel] = linearDepth;

    float3 worldPosition = mul(float4(viewPosition, 1.0f), InverseView).xyz;
    float previousDepth = mul(float4(worldPosition, 1.0f), PreviousView).z;
    float2 motion = InMotion[motionPixel].xy * MotionAndDepthBounds.xy * RenderSize.zw;
    bool motionValid = all(isfinite(motion));
    if (!motionValid)
        motion = 0.0f;
    if ((Flags & FlagFlipMotion) != 0)
        motion = -motion;
    float2 previousUv = (float2(pixel) + 0.5f) * RenderSize.zw + motion;
    bool previousUvValid = all(previousUv >= 0.0f) && all(previousUv <= 1.0f);
    if ((Flags & FlagHasPreviousLinearDepth) != 0 && motionValid && previousUvValid)
    {
        float historyDepth = InPreviousLinearDepth.SampleLevel(PointClampSampler, previousUv, 0.0f);
        if (isfinite(historyDepth))
            previousDepth = historyDepth;
    }
    if (!isfinite(previousDepth))
        previousDepth = linearDepth;
    OutMotion[pixel] = float4(motion, previousDepth - linearDepth, 0.0f);

    float4 sourceNormal = InNormals[normalPixel];
    float normalLengthSquared = dot(sourceNormal.xyz, sourceNormal.xyz);
    bool normalValid = all(isfinite(sourceNormal)) && normalLengthSquared > 1e-8f;
    float3 worldNormal = normalValid ? sourceNormal.xyz * rsqrt(normalLengthSquared) : float3(0.0f, 0.0f, 1.0f);
    if ((Flags & FlagViewSpaceNormals) != 0)
    {
        worldNormal = mul(float4(worldNormal, 0.0f), InverseView).xyz;
        normalLengthSquared = dot(worldNormal, worldNormal);
        normalValid = normalValid && all(isfinite(worldNormal)) && normalLengthSquared > 1e-8f;
        worldNormal = normalValid ? worldNormal * rsqrt(normalLengthSquared) : float3(0.0f, 0.0f, 1.0f);
    }
    float roughness =
        (Flags & FlagPackedRoughness) != 0 ? sourceNormal.a : InRoughness[roughnessPixel];
    bool roughnessValid = isfinite(roughness);
    if (!roughnessValid)
        roughness = 1.0f;
    OutNormals[pixel] = float4(OctEncode(worldNormal), saturate(roughness), 0.0f);

    float3 diffuseAlbedoSample = InDiffuseAlbedo[diffuseAlbedoPixel].rgb;
    float3 specularAlbedoSample = InSpecularAlbedo[specularAlbedoPixel].rgb;
    bool albedoValid = all(isfinite(diffuseAlbedoSample)) && all(isfinite(specularAlbedoSample));
    float3 diffuseAlbedo = albedoValid ? max(diffuseAlbedoSample, 0.0f) : 0.0f;
    float3 specularAlbedo = albedoValid ? max(specularAlbedoSample, 0.0f) : 0.0f;

    // DLSS-RR supplies diffuse albedo and hemispherical specular reflectance. Keep their sum bounded so
    // demodulation remains stable and the same values can be used to recompose FSR-RR's illumination.
    float3 albedoOvershoot = max(diffuseAlbedo + specularAlbedo - 1.0f, 0.0f);
    specularAlbedo = saturate(specularAlbedo - albedoOvershoot);
    diffuseAlbedo = saturate(diffuseAlbedo - max(diffuseAlbedo + specularAlbedo - 1.0f, 0.0f));
    OutDiffuseAlbedo[pixel] = float4(diffuseAlbedo, 0.0f);
    OutSpecularAlbedo[pixel] = float4(specularAlbedo, 0.0f);

    float3 totalAlbedo = diffuseAlbedo + specularAlbedo;
    bool splitValid = rawValid && depthValid && motionValid && normalValid && roughnessValid && albedoValid &&
                      any(totalAlbedo > 1e-4f);
    float3 safeTotalAlbedo = max(totalAlbedo, 1e-4f);
    float3 specularWeight = splitValid ? saturate(specularAlbedo / safeTotalAlbedo) : 0.0f;
    float3 positiveRaw = max(raw.rgb, 0.0f);
    float3 specularColor = splitValid ? positiveRaw * specularWeight : 0.0f;
    float3 diffuseColor = splitValid ? positiveRaw - specularColor : 0.0f;
    float3 specularIllumination = splitValid ? specularColor / max(specularAlbedo, 1e-4f) : 0.0f;
    float3 diffuseIllumination = splitValid ? diffuseColor / max(diffuseAlbedo, 1e-4f) : 0.0f;
    specularIllumination = min(specularIllumination, 65504.0f);
    diffuseIllumination = min(diffuseIllumination, 65504.0f);

    float4 specularHitSample = InSpecularHitDistance[specularHitPixel];
    float specularHitDistance = (Flags & FlagHasSpecularHitDistance) != 0
                                    ? ((Flags & FlagSpecularHitDistanceInAlpha) != 0
                                           ? specularHitSample.a
                                           : specularHitSample.r)
                                    : -1.0f;
    if (!splitValid || !isfinite(specularHitDistance))
        specularHitDistance = -1.0f;
    // The albedo split is lossless, so residual is otherwise ~0 and volumetrics /
    // untrackable specular stay in the denoiser. Hold those pixels out: invalid
    // or non-positive hitT, or roughness at/above the profile threshold.
    bool specularHitUsable = specularHitDistance > 0.0f;
    bool holdOutSpecular = !specularHitUsable ||
                           (SpecularHoldoutMaxRoughness > 0.0f && roughness >= SpecularHoldoutMaxRoughness);
    if (holdOutSpecular)
    {
        specularIllumination = 0.0f;
        specularHitDistance = -1.0f;
    }
    float directDiffuseValidity = splitValid ? 0.0f : -1.0f;
    OutDirectDiffuse[pixel] = float4(diffuseIllumination, directDiffuseValidity);
    OutIndirectSpecular[pixel] = float4(specularIllumination, specularHitDistance);

    float3 remodulated = diffuseIllumination * diffuseAlbedo + specularIllumination * specularAlbedo;
    OutResidual[pixel] = float4(raw.rgb - remodulated, raw.a);

    float sssGuide = 0.0f;
    if ((Flags & FlagHasSssGuide) != 0)
    {
        float value = InSssGuide[sssGuidePixel].r;
        sssGuide = isfinite(value) ? value : 0.0f;
    }
    OutSssGuide[pixel] = float4(sssGuide, 0.0f, 0.0f, 0.0f);

    float biasMask = 0.0f;
    if ((Flags & FlagHasBiasMask) != 0)
    {
        float value = InBiasMask[biasMaskPixel].r;
        biasMask = isfinite(value) ? saturate(value) : 0.0f;
    }
    OutBiasMask[pixel] = float4(biasMask, 0.0f, 0.0f, 0.0f);

    float4 colorBeforeParticles = 0.0f;
    if ((Flags & FlagHasColorBeforeParticles) != 0)
    {
        float4 value = InColorBeforeParticles[colorBeforeParticlesPixel];
        colorBeforeParticles = all(isfinite(value)) ? value : 0.0f;
        colorBeforeParticles.a = saturate(colorBeforeParticles.a);
    }
    OutColorBeforeParticles[pixel] = colorBeforeParticles;
}
