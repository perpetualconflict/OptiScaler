#define MainRS \
    "RootFlags(0)," \
    "CBV(b0)," \
    "DescriptorTable(SRV(t0, numDescriptors = 5))," \
    "DescriptorTable(UAV(u0, numDescriptors = 1))"

Texture2D<float4> InDiffuse : register(t0);
Texture2D<float4> InSpecular : register(t1);
Texture2D<float4> InResidual : register(t2);
Texture2D<float4> InNoisyDiffuse : register(t3);
Texture2D<float4> InNoisySpecular : register(t4);
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
    float3 diffuse = InDiffuse[pixel].rgb;
    float3 specular = InSpecular[pixel].rgb;
    float3 color = diffuse + specular + residual.rgb;
    if (DebugOutput == 1)
        color = diffuse;
    else if (DebugOutput == 2)
        color = specular;
    else if (DebugOutput == 3)
        color = residual.rgb;
    else if (DebugOutput == 4)
        color = InNoisyDiffuse[pixel].rgb;
    else if (DebugOutput == 5)
        color = InNoisySpecular[pixel].rgb;
    OutColor[pixel] = float4(max(color, 0.0f), residual.a);
}
