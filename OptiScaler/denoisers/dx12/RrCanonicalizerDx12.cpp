#include "pch.h"

#include "RrCanonicalizerDx12.h"

#include "shaders/RrCanonicalize_Shader.h"
#include "shaders/RrCompose_Shader.h"

#include <State.h>
#include <shaders/Shader_Dx12Utils.h>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace RayReconstruction
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr uint32_t FrameHeapCount = 8;
constexpr D3D12_RESOURCE_STATES ComputeRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES UnorderedAccess = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

uint32_t AlignConstantBuffer(uint32_t size) { return (size + 255u) & ~255u; }

void ThrowIfFailed(HRESULT result, const char* message)
{
    if (FAILED(result))
        throw std::runtime_error(std::string(message) + " (HRESULT " + std::to_string(result) + ")");
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return format;
    }
}

void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES& currentState, D3D12_RESOURCE_STATES requestedState)
{
    if (!resource || currentState == requestedState)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter = requestedState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
    currentState = requestedState;
}

struct Texture
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = ComputeRead;
};

Texture CreateTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport { format };
    ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)),
                  "failed to query FSR-RR canonical format support");
    const auto requiredSupport1 = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
    if ((formatSupport.Support1 & requiredSupport1) != requiredSupport1 ||
        (formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        throw std::runtime_error("device does not support the required FSR-RR canonical texture format " +
                                 std::to_string(static_cast<uint32_t>(format)));

    D3D12_RESOURCE_DESC description {};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProperties {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    Texture texture;
    ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &description, ComputeRead,
                                                   nullptr, IID_PPV_ARGS(&texture.resource)),
                  "failed to create FSR-RR canonical resource");
    texture.resource->SetName(name);
    return texture;
}

void CreateShaderResourceView(ID3D12Device* device, ID3D12Resource* resource,
                              D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC description {};
    description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Texture2D.MipLevels = 1;
    description.Format = resource ? ViewFormat(resource->GetDesc().Format) : DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(resource, &description, handle);
}

void CreateUnorderedAccessView(ID3D12Device* device, ID3D12Resource* resource,
                               D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC description {};
    description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    description.Format = ViewFormat(resource->GetDesc().Format);
    device->CreateUnorderedAccessView(resource, nullptr, &description, handle);
}

void ValidateShaderResource(ID3D12Device* device, ID3D12Resource* resource)
{
    if (!resource)
        return;

    const DXGI_FORMAT format = ViewFormat(resource->GetDesc().Format);
    if (format == DXGI_FORMAT_UNKNOWN)
        throw std::runtime_error("FSR-RR input has no usable shader-resource format");

    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport { format };
    ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)),
                  "failed to query FSR-RR input format support");
    const auto requiredSupport = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
    if ((formatSupport.Support1 & requiredSupport) != requiredSupport)
        throw std::runtime_error("FSR-RR input format is not shader-readable: " +
                                 std::to_string(static_cast<uint32_t>(format)));
}

class ComputePipeline
{
  public:
    void Initialize(ID3D12Device* device, const void* bytecode, size_t bytecodeSize, uint32_t constantSize,
                    uint32_t srvCount, uint32_t uavCount, const wchar_t* constantName)
    {
        _device = device;
        _srvCount = srvCount;
        _uavCount = uavCount;
        _hasConstants = constantSize != 0;

        ThrowIfFailed(device->CreateRootSignature(0, bytecode, bytecodeSize, IID_PPV_ARGS(&_rootSignature)),
                      "failed to create FSR-RR root signature");

        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription {};
        pipelineDescription.pRootSignature = _rootSignature.Get();
        pipelineDescription.CS = { bytecode, bytecodeSize };
        ThrowIfFailed(device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&_pipelineState)),
                      "failed to create FSR-RR compute pipeline");

        _heaps.resize(FrameHeapCount);
        for (auto& heap : _heaps)
            if (!heap.Initialize(device, srvCount, uavCount, 0))
                throw std::runtime_error("failed to create FSR-RR descriptor heap");

        if (!_hasConstants)
            return;

        _constantSlotSize = AlignConstantBuffer(constantSize);
        D3D12_RESOURCE_DESC bufferDescription {};
        bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDescription.Width = static_cast<uint64_t>(_constantSlotSize) * FrameHeapCount;
        bufferDescription.Height = 1;
        bufferDescription.DepthOrArraySize = 1;
        bufferDescription.MipLevels = 1;
        bufferDescription.SampleDesc.Count = 1;
        bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES heapProperties {};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDescription,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&_constantBuffer)),
                      "failed to create FSR-RR constant buffer");
        _constantBuffer->SetName(constantName);
        D3D12_RANGE readRange { 0, 0 };
        ThrowIfFailed(_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&_mappedConstants)),
                      "failed to map FSR-RR constant buffer");
    }

    ~ComputePipeline()
    {
        if (_constantBuffer && _mappedConstants)
            _constantBuffer->Unmap(0, nullptr);
    }

    void Dispatch(ID3D12GraphicsCommandList* commandList, std::span<ID3D12Resource* const> inputs,
                  std::span<ID3D12Resource* const> outputs, uint32_t width, uint32_t height,
                  const void* constants = nullptr, size_t constantSize = 0)
    {
        if (!commandList || inputs.size() != _srvCount || outputs.size() != _uavCount)
            throw std::runtime_error("invalid FSR-RR compute dispatch");

        ScopedSkipHeapCapture skipHeapCapture {};
        const uint32_t frame = _frameIndex++ % FrameHeapCount;
        auto& heap = _heaps[frame];

        for (uint32_t index = 0; index < inputs.size(); ++index)
            CreateShaderResourceView(_device, inputs[index], heap.GetSrvCPU(index));
        for (uint32_t index = 0; index < outputs.size(); ++index)
            CreateUnorderedAccessView(_device, outputs[index], heap.GetUavCPU(index));

        commandList->SetPipelineState(_pipelineState.Get());
        commandList->SetComputeRootSignature(_rootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
        commandList->SetDescriptorHeaps(1, heaps);

        uint32_t rootIndex = 0;
        if (_hasConstants)
        {
            if (!constants || constantSize > _constantSlotSize)
                throw std::runtime_error("invalid FSR-RR constant data");
            const uint32_t offset = frame * _constantSlotSize;
            std::memcpy(_mappedConstants + offset, constants, constantSize);
            commandList->SetComputeRootConstantBufferView(
                rootIndex++, _constantBuffer->GetGPUVirtualAddress() + offset);
        }

        commandList->SetComputeRootDescriptorTable(rootIndex++, heap.GetTableGPUStart());
        CD3DX12_GPU_DESCRIPTOR_HANDLE outputTable = heap.GetTableGPUStart();
        outputTable.Offset(_srvCount,
                           _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        commandList->SetComputeRootDescriptorTable(rootIndex, outputTable);
        commandList->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
    }

  private:
    ID3D12Device* _device = nullptr;
    uint32_t _srvCount = 0;
    uint32_t _uavCount = 0;
    bool _hasConstants = false;
    uint32_t _constantSlotSize = 0;
    uint32_t _frameIndex = 0;
    uint8_t* _mappedConstants = nullptr;
    ComPtr<ID3D12RootSignature> _rootSignature;
    ComPtr<ID3D12PipelineState> _pipelineState;
    ComPtr<ID3D12Resource> _constantBuffer;
    std::vector<FrameDescriptorHeap> _heaps;
};

size_t SignalIndex(FfxRr12::Signal signal)
{
    switch (signal)
    {
    case FfxRr12::Signal::AmbientOcclusion:
        return 0;
    case FfxRr12::Signal::DirectDiffuse:
        return 1;
    case FfxRr12::Signal::DirectSpecular:
        return 2;
    case FfxRr12::Signal::DominantLightVisibility:
        return 3;
    case FfxRr12::Signal::IndirectDiffuse:
        return 4;
    case FfxRr12::Signal::IndirectSpecular:
        return 5;
    case FfxRr12::Signal::SpecularOcclusion:
        return 6;
    }
    throw std::runtime_error("unknown FSR-RR signal");
}

struct alignas(16) ConversionConstants
{
    std::array<float, 16> inverseView {};
    std::array<float, 16> inverseProjection {};
    std::array<float, 16> previousView {};
    std::array<float, 4> renderSize {};
    std::array<float, 4> motionAndDepthBounds {};
    std::array<std::array<uint32_t, 4>, 5> inputBases {};
    uint32_t flags = 0;
    std::array<float, 3> padding {};
};

struct alignas(16) CompositionConstants
{
    uint32_t debugOutput = 0;
    std::array<uint32_t, 3> padding {};
};
} // namespace

struct CanonicalizerDx12::Impl
{
    ID3D12Device* device = nullptr;
    bool ready = false;
    std::string lastError;
    uint32_t width = 0;
    uint32_t height = 0;
    ComputePipeline conversion;
    ComputePipeline composition;

    std::array<Texture, 2> linearDepth;
    uint32_t currentDepthIndex = 0;
    uint32_t depthWriteIndex = 0;
    Texture motionVectors;
    Texture normals;
    Texture diffuseAlbedo;
    Texture specularAlbedo;
    Texture residual;
    Texture composedColor;
    std::array<Texture, 7> signalInputs;
    std::array<Texture, 7> signalOutputs;
    std::array<std::vector<ComPtr<ID3D12Resource>>, FrameHeapCount> retiredResources;
    uint64_t frameSerial = 0;
    uint64_t preparedRetirementSerial = std::numeric_limits<uint64_t>::max();

    std::vector<ComPtr<ID3D12Resource>>& RetirementBucket()
    {
        auto& bucket = retiredResources[frameSerial % FrameHeapCount];
        if (preparedRetirementSerial != frameSerial)
        {
            bucket.clear();
            preparedRetirementSerial = frameSerial;
        }
        return bucket;
    }

    void Retire(Texture& texture)
    {
        if (texture.resource)
            RetirementBucket().push_back(std::move(texture.resource));
        texture.state = ComputeRead;
    }

    void RetireResources()
    {
        for (auto& texture : linearDepth)
            Retire(texture);
        Retire(motionVectors);
        Retire(normals);
        Retire(diffuseAlbedo);
        Retire(specularAlbedo);
        Retire(residual);
        Retire(composedColor);
        for (auto& texture : signalInputs)
            Retire(texture);
        for (auto& texture : signalOutputs)
            Retire(texture);
    }

    void AdvanceFrame()
    {
        ++frameSerial;
        RetirementBucket();
    }
};

CanonicalizerDx12::CanonicalizerDx12(ID3D12Device* device) : _impl(std::make_unique<Impl>())
{
    try
    {
        _impl->device = device;
        _impl->conversion.Initialize(device, RrCanonicalize_cso, sizeof(RrCanonicalize_cso),
                                     sizeof(ConversionConstants), 10, 8, L"FSRR_CanonicalConstants");
        _impl->composition.Initialize(device, RrCompose_cso, sizeof(RrCompose_cso),
                                      sizeof(CompositionConstants), 5, 1, L"FSRR_CompositionConstants");
        _impl->ready = true;
    }
    catch (const std::exception& exception)
    {
        _impl->lastError = exception.what();
        LOG_ERROR("FSR-RR canonicalizer initialization failed: {}", _impl->lastError);
    }
}

CanonicalizerDx12::~CanonicalizerDx12() = default;

bool CanonicalizerDx12::IsReady() const { return _impl->ready; }

const std::string& CanonicalizerDx12::LastError() const { return _impl->lastError; }

bool CanonicalizerDx12::Resize(uint32_t width, uint32_t height)
{
    if (!_impl->ready || width == 0 || height == 0)
        return false;
    if (_impl->width == width && _impl->height == height)
        return true;

    try
    {
        _impl->RetireResources();
        _impl->linearDepth[0] =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R32_FLOAT, L"FSRR_LinearDepth0");
        _impl->linearDepth[1] =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R32_FLOAT, L"FSRR_LinearDepth1");
        _impl->currentDepthIndex = 0;
        _impl->depthWriteIndex = 0;
        _impl->motionVectors =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"FSRR_MotionVectors");
        _impl->normals =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R10G10B10A2_UNORM, L"FSRR_Normals");
        _impl->diffuseAlbedo =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, L"FSRR_DiffuseAlbedo");
        _impl->specularAlbedo =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, L"FSRR_SpecularAlbedo");
        _impl->residual =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"FSRR_Residual");
        _impl->composedColor =
            CreateTexture(_impl->device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, L"FSRR_ComposedColor");

        static constexpr std::array<const wchar_t*, 7> inputNames = {
            L"FSRR_AO_Input",       L"FSRR_DirectDiffuse_Input", L"FSRR_DirectSpecular_Input",
            L"FSRR_Dominant_Input", L"FSRR_IndirectDiffuse_Input", L"FSRR_IndirectSpecular_Input",
            L"FSRR_SpecularOcclusion_Input",
        };
        static constexpr std::array<const wchar_t*, 7> outputNames = {
            L"FSRR_AO_Output",       L"FSRR_DirectDiffuse_Output", L"FSRR_DirectSpecular_Output",
            L"FSRR_Dominant_Output", L"FSRR_IndirectDiffuse_Output", L"FSRR_IndirectSpecular_Output",
            L"FSRR_SpecularOcclusion_Output",
        };
        for (size_t index = 0; index < _impl->signalInputs.size(); ++index)
        {
            _impl->signalInputs[index] =
                CreateTexture(_impl->device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, inputNames[index]);
            _impl->signalOutputs[index] =
                CreateTexture(_impl->device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, outputNames[index]);
        }

        _impl->width = width;
        _impl->height = height;
        LOG_INFO("FSR-RR canonical resources resized to {}x{}", width, height);
        return true;
    }
    catch (const std::exception& exception)
    {
        _impl->lastError = exception.what();
        LOG_ERROR("FSR-RR canonical resource creation failed: {}", _impl->lastError);
        return false;
    }
}

bool CanonicalizerDx12::Convert(const CanonicalizationDescription& description)
{
    if (!_impl->ready || !description.commandList || description.width != _impl->width ||
        description.height != _impl->height)
        return false;

    try
    {
        ConversionConstants constants;
        constants.inverseView = description.inverseView;
        constants.inverseProjection = description.inverseProjection;
        constants.previousView = description.previousView;
        constants.renderSize = { static_cast<float>(description.width), static_cast<float>(description.height),
                                 1.0f / description.width, 1.0f / description.height };
        constants.motionAndDepthBounds = { description.motionScaleX, description.motionScaleY,
                                           description.linearDepthMin, description.linearDepthMax };
        for (size_t pair = 0; pair < 4; ++pair)
        {
            constants.inputBases[pair] = {
                description.inputSubrectBases[pair * 2][0],
                description.inputSubrectBases[pair * 2][1],
                description.inputSubrectBases[pair * 2 + 1][0],
                description.inputSubrectBases[pair * 2 + 1][1],
            };
        }
        constants.inputBases[4] = {
            description.inputSubrectBases[8][0],
            description.inputSubrectBases[8][1],
            0,
            0,
        };
        constants.flags = description.flags;

        const uint32_t previousDepthIndex = 1u - _impl->depthWriteIndex;
        std::array<ID3D12Resource*, 10> inputs = {
            description.color,
            description.depth,
            description.motionVectors,
            description.normals,
            description.roughness,
            description.diffuseAlbedo,
            description.specularAlbedo,
            description.diffuseHitDistance,
            description.specularHitDistance,
            _impl->linearDepth[previousDepthIndex].resource.Get(),
        };
        for (auto* input : inputs)
            ValidateShaderResource(_impl->device, input);

        auto& indirectDiffuse = _impl->signalInputs[SignalIndex(FfxRr12::Signal::IndirectDiffuse)];
        auto& indirectSpecular = _impl->signalInputs[SignalIndex(FfxRr12::Signal::IndirectSpecular)];
        std::array<Texture*, 8> outputTextures = {
            &_impl->linearDepth[_impl->depthWriteIndex], &_impl->motionVectors, &_impl->normals, &_impl->diffuseAlbedo,
            &_impl->specularAlbedo, &indirectDiffuse, &indirectSpecular, &_impl->residual,
        };
        std::array<ID3D12Resource*, 8> outputs {};
        for (size_t index = 0; index < outputTextures.size(); ++index)
        {
            Transition(description.commandList, outputTextures[index]->resource.Get(), outputTextures[index]->state,
                       UnorderedAccess);
            outputs[index] = outputTextures[index]->resource.Get();
        }

        _impl->conversion.Dispatch(description.commandList, inputs, outputs, description.width, description.height,
                                   &constants, sizeof(constants));

        D3D12_RESOURCE_BARRIER uavBarrier {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        for (auto* texture : outputTextures)
        {
            uavBarrier.UAV.pResource = texture->resource.Get();
            description.commandList->ResourceBarrier(1, &uavBarrier);
            Transition(description.commandList, texture->resource.Get(), texture->state, ComputeRead);
        }
        _impl->currentDepthIndex = _impl->depthWriteIndex;
        _impl->depthWriteIndex = 1u - _impl->depthWriteIndex;
        _impl->AdvanceFrame();
        return true;
    }
    catch (const std::exception& exception)
    {
        _impl->lastError = exception.what();
        LOG_ERROR("FSR-RR input canonicalization failed: {}", _impl->lastError);
        return false;
    }
}

bool CanonicalizerDx12::PrepareSignalOutputs(ID3D12GraphicsCommandList* commandList, FfxRr12::SignalMask signals)
{
    if (!commandList)
        return false;

    try
    {
        for (uint32_t bit = 0; bit < 7; ++bit)
        {
            const auto signal = static_cast<FfxRr12::Signal>(1u << bit);
            if ((signals & FfxRr12::ToMask(signal)) == 0)
                continue;
            auto& output = _impl->signalOutputs[SignalIndex(signal)];
            Transition(commandList, output.resource.Get(), output.state, UnorderedAccess);
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        _impl->lastError = exception.what();
        return false;
    }
}

bool CanonicalizerDx12::Compose(ID3D12GraphicsCommandList* commandList, FfxRr12::SignalMask signals,
                                uint32_t debugOutput)
{
    const auto supportedSignals = FfxRr12::ToMask(FfxRr12::Signal::IndirectDiffuse) |
                                  FfxRr12::ToMask(FfxRr12::Signal::IndirectSpecular);
    if (!commandList || signals != supportedSignals)
    {
        _impl->lastError = "the current composition adapter requires indirect diffuse and indirect specular";
        return false;
    }

    try
    {
        auto& diffuse = _impl->signalOutputs[SignalIndex(FfxRr12::Signal::IndirectDiffuse)];
        auto& specular = _impl->signalOutputs[SignalIndex(FfxRr12::Signal::IndirectSpecular)];
        auto& noisyDiffuse = _impl->signalInputs[SignalIndex(FfxRr12::Signal::IndirectDiffuse)];
        auto& noisySpecular = _impl->signalInputs[SignalIndex(FfxRr12::Signal::IndirectSpecular)];
        Transition(commandList, diffuse.resource.Get(), diffuse.state, ComputeRead);
        Transition(commandList, specular.resource.Get(), specular.state, ComputeRead);
        Transition(commandList, _impl->composedColor.resource.Get(), _impl->composedColor.state, UnorderedAccess);

        std::array<ID3D12Resource*, 5> inputs = {
            diffuse.resource.Get(), specular.resource.Get(), _impl->residual.resource.Get(),
            noisyDiffuse.resource.Get(), noisySpecular.resource.Get(),
        };
        std::array<ID3D12Resource*, 1> outputs = { _impl->composedColor.resource.Get() };
        const CompositionConstants constants { std::min(debugOutput, 5u) };
        _impl->composition.Dispatch(commandList, inputs, outputs, _impl->width, _impl->height, &constants,
                                    sizeof(constants));

        D3D12_RESOURCE_BARRIER uavBarrier {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = _impl->composedColor.resource.Get();
        commandList->ResourceBarrier(1, &uavBarrier);
        Transition(commandList, _impl->composedColor.resource.Get(), _impl->composedColor.state, ComputeRead);
        return true;
    }
    catch (const std::exception& exception)
    {
        _impl->lastError = exception.what();
        LOG_ERROR("FSR-RR output composition failed: {}", _impl->lastError);
        return false;
    }
}

ID3D12Resource* CanonicalizerDx12::LinearDepth() const
{
    return _impl->linearDepth[_impl->currentDepthIndex].resource.Get();
}
ID3D12Resource* CanonicalizerDx12::MotionVectors() const { return _impl->motionVectors.resource.Get(); }
ID3D12Resource* CanonicalizerDx12::Normals() const { return _impl->normals.resource.Get(); }
ID3D12Resource* CanonicalizerDx12::DiffuseAlbedo() const { return _impl->diffuseAlbedo.resource.Get(); }
ID3D12Resource* CanonicalizerDx12::SpecularAlbedo() const { return _impl->specularAlbedo.resource.Get(); }

ID3D12Resource* CanonicalizerDx12::SignalInput(FfxRr12::Signal signal) const
{
    return _impl->signalInputs[SignalIndex(signal)].resource.Get();
}

ID3D12Resource* CanonicalizerDx12::SignalOutput(FfxRr12::Signal signal) const
{
    return _impl->signalOutputs[SignalIndex(signal)].resource.Get();
}

ID3D12Resource* CanonicalizerDx12::ComposedColor() const { return _impl->composedColor.resource.Get(); }

void CanonicalizerDx12::SetComposedColorState(D3D12_RESOURCE_STATES state) { _impl->composedColor.state = state; }
} // namespace RayReconstruction
