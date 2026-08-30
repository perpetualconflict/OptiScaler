#define MainRS \
    "RootFlags(0)," \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors = 11))," \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"

Texture2D<float4> InDiffuse : register(t0);
Texture2D<float4> InSpecular : register(t1);
Texture2D<float4> InResidual : register(t2);
Texture2D<float4> InNoisyDiffuse : register(t3);
Texture2D<float4> InNoisySpecular : register(t4);
Texture2D<float4> InDiffuseAlbedo : register(t5);
Texture2D<float4> InSpecularAlbedo : register(t6);
Texture2D<float4> InMotion : register(t7);
Texture2D<float4> InSssGuide : register(t8);
Texture2D<float4> InBiasMask : register(t9);
Texture2D<float4> InColorBeforeParticles : register(t10);
RWTexture2D<float4> OutColor : register(u0);

cbuffer Constants : register(b0)
{
    uint DebugOutput;
    uint UseDepthDeltaCurrentColor;
    float DepthDeltaCurrentColorScale;
    float DepthDeltaCurrentColorStrength;
};

float3 DiagnosticOverlay(float3 sceneColor, float3 diagnosticTint, float amount)
{
    static const float3 LuminanceWeights = float3(0.2126f, 0.7152f, 0.0722f);
    float sceneLuminance = dot(max(sceneColor, 0.0f), LuminanceWeights);
    float tintLuminance = max(dot(diagnosticTint, LuminanceWeights), 0.001f);
    float3 luminanceMatchedTint = diagnosticTint * (max(sceneLuminance, 0.02f) / tintLuminance);
    return lerp(sceneColor, luminanceMatchedTint, 0.85f * saturate(amount));
}

[RootSignature(MainRS)]
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    uint width;
    uint height;
    OutColor.GetDimensions(width, height);
    if (pixel.x >= width || pixel.y >= height)
        return;

    float4 residual = InResidual[pixel];
    float3 diffuseAlbedo = InDiffuseAlbedo[pixel].rgb;
    float3 specularAlbedo = InSpecularAlbedo[pixel].rgb;
    float3 diffuse = InDiffuse[pixel].rgb * diffuseAlbedo;
    float3 specular = InSpecular[pixel].rgb * specularAlbedo;
    float3 noisyDiffuse = InNoisyDiffuse[pixel].rgb * diffuseAlbedo;
    float3 noisySpecular = InNoisySpecular[pixel].rgb * specularAlbedo;
    float3 currentColor = noisyDiffuse + noisySpecular + residual.rgb;
    float3 denoisedColor = diffuse + specular + residual.rgb;
    float3 color = denoisedColor;
    float sssGuide = InSssGuide[pixel].r;
    float sssAmount = saturate(log2(1.0f + abs(sssGuide) * 64.0f) * 0.25f);
    float biasMask = saturate(InBiasMask[pixel].r);
    float4 colorBeforeParticles = InColorBeforeParticles[pixel];
    if (UseDepthDeltaCurrentColor != 0)
    {
        float depthRisk = saturate(log2(1.0f + abs(InMotion[pixel].z)) * DepthDeltaCurrentColorScale);
        color = lerp(color, currentColor, DepthDeltaCurrentColorStrength * depthRisk);
    }
    if (DebugOutput == 1)
        color = diffuse;
    else if (DebugOutput == 2)
        color = specular;
    else if (DebugOutput == 3)
        color = residual.rgb;
    else if (DebugOutput == 4)
        color = noisyDiffuse;
    else if (DebugOutput == 5)
        color = noisySpecular;
    else if (DebugOutput == 6)
    {
        float2 motionPixels = InMotion[pixel].xy * float2(width, height);
        float motionLength = length(motionPixels);
        float2 direction = motionPixels / max(motionLength, 0.0001f);
        float3 horizontalTint = direction.x >= 0.0f ? float3(1.0f, 0.05f, 0.05f) : float3(0.05f, 1.0f, 1.0f);
        float3 verticalTint = direction.y >= 0.0f ? float3(0.05f, 1.0f, 0.05f) : float3(1.0f, 0.05f, 1.0f);
        float directionWeight = max(abs(direction.x) + abs(direction.y), 0.0001f);
        float3 directionTint =
            (abs(direction.x) * horizontalTint + abs(direction.y) * verticalTint) / directionWeight;
        color = DiagnosticOverlay(color, directionTint, motionLength * 0.5f);
    }
    else if (DebugOutput == 7)
    {
        float depthDelta = InMotion[pixel].z;
        float3 deltaTint = depthDelta >= 0.0f ? float3(1.0f, 0.05f, 0.05f) : float3(0.05f, 0.25f, 1.0f);
        float deltaMagnitude = log2(1.0f + abs(depthDelta)) * 0.5f;
        color = DiagnosticOverlay(color, deltaTint, deltaMagnitude);
    }
    else if (DebugOutput == 8)
    {
        // Match the depth-delta overlay's replacement strength so this is a causal A/B: pixels that
        // were diagnostic tint in mode 7 instead receive the non-temporal, reconstructed current color.
        float depthRisk = saturate(log2(1.0f + abs(InMotion[pixel].z)) * 0.5f);
        color = lerp(denoisedColor, currentColor, 0.85f * depthRisk);
    }
    else if (DebugOutput == 9)
    {
        float3 sssTint = sssGuide >= 0.0f ? float3(1.0f, 0.35f, 0.05f) : float3(0.05f, 0.4f, 1.0f);
        color = DiagnosticOverlay(color, sssTint, sssAmount);
    }
    else if (DebugOutput == 10)
        color = DiagnosticOverlay(color, float3(0.1f, 1.0f, 0.25f), biasMask);
    else if (DebugOutput == 11)
    {
        static const float3 LuminanceWeights = float3(0.2126f, 0.7152f, 0.0722f);
        float sceneLuminance = max(dot(max(currentColor, 0.0f), LuminanceWeights), 0.02f);
        float particleLuminance = dot(abs(colorBeforeParticles.rgb), LuminanceWeights);
        float particleAmount = max(colorBeforeParticles.a, saturate(particleLuminance / sceneLuminance));
        float3 particleTint = particleLuminance > 0.001f
                                  ? abs(colorBeforeParticles.rgb)
                                  : float3(1.0f, 0.55f, 0.05f);
        color = DiagnosticOverlay(color, particleTint, particleAmount);
    }
    else if (DebugOutput == 12)
        color = lerp(color, currentColor, sssAmount);
    else if (DebugOutput == 13)
        color = lerp(color, currentColor, biasMask);
    else if (DebugOutput == 14)
        color = (1.0f - colorBeforeParticles.a) * color + colorBeforeParticles.rgb;
    else if (DebugOutput == 15)
        color = denoisedColor;
    else if (DebugOutput == 16)
        color = currentColor;
    else if (DebugOutput == 17)
        color = lerp(color, denoisedColor, sssAmount);
    else if (DebugOutput == 18)
        color = lerp(color, denoisedColor, biasMask);
    else if (DebugOutput == 19)
    {
        float residualMagnitude = length(residual.rgb) / max(length(currentColor), 0.02f);
        color = DiagnosticOverlay(color, float3(1.0f, 0.05f, 0.7f), residualMagnitude);
    }
    else if (DebugOutput == 20)
        color = max(color - residual.rgb * sssAmount, 0.0f);
    OutColor[pixel] = float4(max(color, 0.0f), residual.a);
}
