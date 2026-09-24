#include "VideoTexture.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    // One more than the engine's frames in flight: a slot is written again only after
    // the frame that last read it has certainly retired.
    constexpr int kUploadRingSize = 4;
    constexpr int kRetireFrames = 4;

    const char* kShader = R"(
cbuffer Fit : register(b0) { float2 uvScale; float2 uvOffset; };
Texture2D picture : register(t0);
SamplerState pictureSampler : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID)
{
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
// The sampler's border is opaque black, which is what paints the letterbox bars.
float4 PS(V v) : SV_Target { return float4(picture.SampleLevel(pictureSampler, v.uv * uvScale + uvOffset, 0).rgb, 1); }
)";

    D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }
}

struct VideoTexture::Impl
{
    ComPtr<ID3D12Device> Device;
    ComPtr<ID3D12RootSignature> RootSignature;
    ComPtr<ID3D12PipelineState> Pipeline;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kDescriptorCount> CpuHandles{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kDescriptorCount> GpuHandles{};
    int DescriptorSlot = 0;

    ComPtr<ID3D12Resource> Texture;
    std::array<ComPtr<ID3D12Resource>, kUploadRingSize> UploadBuffers;
    std::array<std::uint8_t*, kUploadRingSize> UploadMapped{};
    int UploadSlot = 0;
    UINT RowPitch = 0;
    int Width = 0;
    int Height = 0;
    std::uint64_t UploadedSerial = 0;
    bool HasPicture = false;

    struct Retired
    {
        ComPtr<ID3D12Resource> Resource;
        int FramesLeft = 0;
    };
    std::vector<Retired> RetiredResources;

    void Retire(ComPtr<ID3D12Resource>& resource)
    {
        if (resource)
        {
            RetiredResources.push_back({ resource, kRetireFrames });
            resource.Reset();
        }
    }

    bool CreatePipeline(DXGI_FORMAT format)
    {
        ComPtr<ID3DBlob> vs, ps, errors;
        if (FAILED(D3DCompile(kShader, std::strlen(kShader), "VideoTexture", nullptr, nullptr, "VS", "vs_5_0", 0, 0,
                              &vs, &errors)) ||
            FAILED(D3DCompile(kShader, std::strlen(kShader), "VideoTexture", nullptr, nullptr, "PS", "ps_5_0", 0, 0,
                              &ps, &errors)))
        {
            return false;
        }

        D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                                      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = { 0, 0, 4 };
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = { 1, &range };
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{ 2, parameters, 1, &sampler,
                                        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        ComPtr<ID3DBlob> signature;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)) ||
            FAILED(Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                               IID_PPV_ARGS(&RootSignature))))
        {
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
        state.pRootSignature = RootSignature.Get();
        state.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        state.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        auto& blend = state.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        state.RasterizerState.DepthClipEnable = TRUE;
        state.DepthStencilState.DepthEnable = FALSE;
        state.DepthStencilState.StencilEnable = FALSE;
        state.SampleMask = UINT_MAX;
        state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets = 1;
        state.RTVFormats[0] = format;
        state.SampleDesc.Count = 1;
        return SUCCEEDED(Device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&Pipeline)));
    }

    bool EnsureSize(int width, int height)
    {
        if (Texture && width == Width && height == Height)
        {
            return true;
        }

        Retire(Texture);
        for (int i = 0; i < kUploadRingSize; ++i)
        {
            UploadMapped[i] = nullptr;
            Retire(UploadBuffers[i]);
        }

        HasPicture = false;
        Width = width;
        Height = height;
        RowPitch = (static_cast<UINT>(width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                   ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

        D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = static_cast<UINT64>(width);
        textureDesc.Height = static_cast<UINT>(height);
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (FAILED(Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&Texture))))
        {
            Width = Height = 0;
            return false;
        }

        Texture->SetName(L"VideoTexture");

        D3D12_HEAP_PROPERTIES uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = static_cast<UINT64>(RowPitch) * static_cast<UINT64>(height);
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (int i = 0; i < kUploadRingSize; ++i)
        {
            void* mapped = nullptr;
            D3D12_RANGE noRead{ 0, 0 };
            if (FAILED(Device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&UploadBuffers[i]))) ||
                FAILED(UploadBuffers[i]->Map(0, &noRead, &mapped)))
            {
                Retire(Texture);
                Width = Height = 0;
                return false;
            }

            UploadBuffers[i]->SetName(L"VideoTextureUpload");
            UploadMapped[i] = static_cast<std::uint8_t*>(mapped);
        }

        // A fresh slot, so a frame still in flight keeps reading the old texture through
        // the old slot.
        DescriptorSlot = (DescriptorSlot + 1) % kDescriptorCount;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        Device->CreateShaderResourceView(Texture.Get(), &srv, CpuHandles[DescriptorSlot]);
        return true;
    }
};

VideoTexture::VideoTexture()
    : mImpl(std::make_unique<Impl>())
{
}

VideoTexture::~VideoTexture()
{
    Shutdown();
}

bool VideoTexture::Initialize(ID3D12Device* device,
                              const D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandles,
                              const D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandles,
                              DXGI_FORMAT renderTargetFormat)
{
    Shutdown();
    if (device == nullptr || cpuHandles == nullptr || gpuHandles == nullptr)
    {
        return false;
    }

    Impl& d = *mImpl;
    d.Device = device;
    for (int i = 0; i < kDescriptorCount; ++i)
    {
        d.CpuHandles[i] = cpuHandles[i];
        d.GpuHandles[i] = gpuHandles[i];
    }

    if (!d.CreatePipeline(renderTargetFormat))
    {
        Shutdown();
        return false;
    }

    return true;
}

void VideoTexture::Shutdown()
{
    Impl& d = *mImpl;
    d.RetiredResources.clear();
    d.Texture.Reset();
    for (int i = 0; i < kUploadRingSize; ++i)
    {
        d.UploadMapped[i] = nullptr;
        d.UploadBuffers[i].Reset();
    }

    d.Pipeline.Reset();
    d.RootSignature.Reset();
    d.Device.Reset();
    d.Width = d.Height = 0;
    d.UploadedSerial = 0;
    d.HasPicture = false;
}

bool VideoTexture::IsInitialized() const
{
    return mImpl->Pipeline != nullptr;
}

void VideoTexture::BeginFrame()
{
    auto& retired = mImpl->RetiredResources;
    for (auto& entry : retired)
    {
        --entry.FramesLeft;
    }

    retired.erase(std::remove_if(retired.begin(), retired.end(), [](const Impl::Retired& r) { return r.FramesLeft <= 0; }),
                  retired.end());
}

bool VideoTexture::Upload(ID3D12GraphicsCommandList* commandList, const VideoPlayer::Frame& frame)
{
    Impl& d = *mImpl;
    if (!d.Device || commandList == nullptr || frame.Pixels == nullptr || frame.Width <= 0 || frame.Height <= 0)
    {
        return true;
    }

    if (d.HasPicture && frame.Serial == d.UploadedSerial && frame.Width == d.Width && frame.Height == d.Height)
    {
        return true;
    }

    if (!d.EnsureSize(frame.Width, frame.Height))
    {
        return false;
    }

    d.UploadSlot = (d.UploadSlot + 1) % kUploadRingSize;
    std::uint8_t* destination = d.UploadMapped[d.UploadSlot];
    const size_t rowBytes = static_cast<size_t>(frame.Width) * 4;
    for (int y = 0; y < frame.Height; ++y)
    {
        std::memcpy(destination + static_cast<size_t>(y) * d.RowPitch,
                    frame.Pixels + static_cast<size_t>(y) * static_cast<size_t>(frame.Pitch), rowBytes);
    }

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = d.UploadBuffers[d.UploadSlot].Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    source.PlacedFootprint.Footprint.Width = static_cast<UINT>(frame.Width);
    source.PlacedFootprint.Footprint.Height = static_cast<UINT>(frame.Height);
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = d.RowPitch;

    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = d.Texture.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;

    D3D12_RESOURCE_BARRIER toCopy = Transition(d.Texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->ResourceBarrier(1, &toCopy);
    commandList->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER toRead = Transition(d.Texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toRead);

    d.UploadedSerial = frame.Serial;
    d.HasPicture = true;
    return true;
}

bool VideoTexture::HasPicture() const
{
    return mImpl->HasPicture;
}

void VideoTexture::Clear()
{
    mImpl->HasPicture = false;
    mImpl->UploadedSerial = 0;
}

void VideoTexture::Draw(ID3D12GraphicsCommandList* commandList, unsigned targetWidth, unsigned targetHeight,
                        Fit fit) const
{
    const Impl& d = *mImpl;
    if (commandList == nullptr || !d.Pipeline || !d.HasPicture || targetWidth == 0 || targetHeight == 0)
    {
        return;
    }

    // Maps target UV to picture UV. Letterbox scales the UV range up on the axis with
    // spare room, so part of it falls outside [0,1] and samples the black border.
    const float targetAspect = static_cast<float>(targetWidth) / static_cast<float>(targetHeight);
    const float pictureAspect = static_cast<float>(d.Width) / static_cast<float>(d.Height);
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (fit != Fit::Stretch)
    {
        const float ratio = targetAspect / pictureAspect;
        const bool widerTarget = ratio > 1.0f;
        if ((fit == Fit::Letterbox) == widerTarget)
        {
            scaleX = ratio;
        }
        else
        {
            scaleY = 1.0f / ratio;
        }
    }

    const float constants[4] = { scaleX, scaleY, 0.5f - 0.5f * scaleX, 0.5f - 0.5f * scaleY };

    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetPipelineState(d.Pipeline.Get());
    commandList->SetGraphicsRootSignature(d.RootSignature.Get());
    commandList->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
    commandList->SetGraphicsRootDescriptorTable(1, d.GpuHandles[d.DescriptorSlot]);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}

std::uint64_t VideoTexture::GetGpuDescriptor() const
{
    return mImpl->HasPicture ? mImpl->GpuHandles[mImpl->DescriptorSlot].ptr : 0;
}

int VideoTexture::GetWidth() const
{
    return mImpl->Width;
}

int VideoTexture::GetHeight() const
{
    return mImpl->Height;
}
