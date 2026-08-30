#define MainRS \
    "RootFlags(0)," \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors = 8))," \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"

Texture2D<float4> InDiffuse : register(t0);
Texture2D<float4> InSpecular : register(t1);
Texture2D<float4> InResidual : register(t2);
Texture2D<float4> InNoisyDiffuse : register(t3);
Texture2D<float4> InNoisySpecular : register(t4);
Texture2D<float4> InDiffuseAlbedo : register(t5);
Texture2D<float4> InSpecularAlbedo : register(t6);
Texture2D<float4> InMotion : register(t7);
RWTexture2D<float4> OutColor : register(u0);

cbuffer Constants : register(b0)
{
    uint DebugOutput;
    uint3 Padding;
};

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
    float3 diffuse = InDiffuse[pixel].rgb * InDiffuseAlbedo[pixel].rgb;
    float3 specular = InSpecular[pixel].rgb * InSpecularAlbedo[pixel].rgb;
    float3 color = diffuse + specular + residual.rgb;
    if (DebugOutput == 1)
        color = diffuse;
    else if (DebugOutput == 2)
        color = specular;
    else if (DebugOutput == 3)
        color = residual.rgb;
    else if (DebugOutput == 4)
        color = InNoisyDiffuse[pixel].rgb * InDiffuseAlbedo[pixel].rgb;
    else if (DebugOutput == 5)
        color = InNoisySpecular[pixel].rgb * InSpecularAlbedo[pixel].rgb;
    else if (DebugOutput == 6)
    {
        float2 motionPixels = InMotion[pixel].xy * float2(width, height);
        color = float3(0.5f + 0.05f * motionPixels.x, 0.5f + 0.05f * motionPixels.y, 0.5f);
    }
    else if (DebugOutput == 7)
    {
        float depthDelta = InMotion[pixel].z;
        color = float3(0.5f + 0.1f * depthDelta, 0.5f - 0.1f * depthDelta, 0.5f);
    }
    OutColor[pixel] = float4(max(color, 0.0f), residual.a);
}
