#include "pch.h"
#include "GlassRenderer.h"

#include "d3dx12.h"

#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
}

namespace
{
    constexpr DXGI_FORMAT kGlassFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    bool MakeTexture2D(
        ID3D12Device* device,
        UINT width,
        UINT height,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState,
        const wchar_t* name,
        ComPtr<ID3D12Resource>& out)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kGlassFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;

        if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&out))))
        {
            return false;
        }

        out->SetName(name);
        return true;
    }
}

bool GlassRenderer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();

    if (!mRootSignature && !CreatePipeline())
        return false;

    if (!mConstantBuffer)
    {
        ID3D12Device* device = DX12Context_GetDevice();
        if (!device)
        {
            mLastError = "GlassRenderer: null device.";
            return false;
        }

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(GlassConstants);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&mConstantBuffer))))
        {
            mLastError = "GlassRenderer: failed to create constant buffer.";
            return false;
        }

        if (FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedConstants))))
        {
            mLastError = "GlassRenderer: failed to map constant buffer.";
            return false;
        }
    }

    if (!CreateResolutionResources(width, height))
        return false;

    mWidth = width;
    mHeight = height;
    mIsInitialized = true;
    return true;
}

bool GlassRenderer::EnsureSize(UINT width, UINT height)
{
    if (!mIsInitialized)
        return Initialize(width, height);

    if (width == mWidth && height == mHeight)
        return true;

    return CreateResolutionResources(width, height);
}

bool GlassRenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "GlassRenderer: null device.";
        return false;
    }

    const ShaderCompileRequest request
    {
        L"Shaders\\GlassComposite.hlsl",
        L"CSMain",
        L"cs_6_5",
        ShaderStage::Compute
    };
    if (!mComputeShader.Compile(request))
    {
        mLastError = std::string("GlassRenderer: shader compile failed: ")
            + (mComputeShader.GetLastErrorMessage() ? mComputeShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[12]{};
    for (UINT i = 0; i < 11; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = (i == 10) ? 32 : 1;
        ranges[i].BaseShaderRegister = i;
        ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }
    ranges[11].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[11].NumDescriptors = 1;
    ranges[11].BaseShaderRegister = 0;
    ranges[11].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[13]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 12; ++i)
    {
        params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(std::size(params));
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "GlassRenderer: failed to serialize root signature.";
        return false;
    }

    if (FAILED(device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "GlassRenderer: failed to create root signature.";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = mRootSignature.Get();
    pso.CS = mComputeShader.GetBytecode();
    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "GlassRenderer: failed to create compute PSO.";
        return false;
    }

    return true;
}

bool GlassRenderer::CreateResolutionResources(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    DX12Context_WaitForGPU();
    mOutputTexture.Reset();

    if (!MakeTexture2D(
        device,
        width,
        height,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        L"GlassComposite_Output",
        mOutputTexture))
    {
        mLastError = "GlassRenderer: failed to create output texture.";
        return false;
    }

    if (!CreateDescriptors())
        return false;

    mWidth = width;
    mHeight = height;
    return true;
}

bool GlassRenderer::CreateDescriptors()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    if (!mDescriptorsAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputSrvCpu, &mOutputSrvGpu))
        {
            mLastError = "GlassRenderer: failed to allocate output SRV descriptor.";
            return false;
        }
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUavCpu, &mOutputUavGpu))
        {
            mLastError = "GlassRenderer: failed to allocate output UAV descriptor.";
            return false;
        }
        mDescriptorsAllocated = true;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kGlassFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srv, mOutputSrvCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = kGlassFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(mOutputTexture.Get(), nullptr, &uav, mOutputUavCpu);

    return true;
}

void GlassRenderer::Dispatch(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12Resource* sceneColorResource,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneColorSrv,
    D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferAlbedoSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferMaterialSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE tlasSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE vertexSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE indexSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE instanceInfoSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE materialRangeSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE baseTextureTableSrv,
    const float invViewProj[16],
    const float viewProj[16],
    const float cameraPos[3],
    uint32_t frameIndex,
    bool tlasReady)
{
    if (!mIsInitialized || !commandList || !sceneColorResource || !mMappedConstants)
        return;

    GlassConstants constants{};
    constants.FrameWidth = mWidth;
    constants.FrameHeight = mHeight;
    constants.FrameIndex = frameIndex;
    constants.TlasReady = tlasReady ? 1u : 0u;
    std::memcpy(constants.InvViewProj, invViewProj, sizeof(float) * 16);
    std::memcpy(constants.ViewProj, viewProj, sizeof(float) * 16);
    std::memcpy(constants.CameraPos, cameraPos, sizeof(float) * 3);
    std::memcpy(mMappedConstants, &constants, sizeof(constants));

    D3D12_RESOURCE_BARRIER pre[2]{};
    pre[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    pre[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(2, pre);

    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    if (heap)
        commandList->SetDescriptorHeaps(1, &heap);

    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(1, sceneColorSrv);
    commandList->SetComputeRootDescriptorTable(2, gbufferAlbedoSrv);
    commandList->SetComputeRootDescriptorTable(3, gbufferNormalSrv);
    commandList->SetComputeRootDescriptorTable(4, gbufferMaterialSrv);
    commandList->SetComputeRootDescriptorTable(5, depthSrv);
    commandList->SetComputeRootDescriptorTable(6, tlasSrv);
    commandList->SetComputeRootDescriptorTable(7, vertexSrv);
    commandList->SetComputeRootDescriptorTable(8, indexSrv);
    commandList->SetComputeRootDescriptorTable(9, instanceInfoSrv);
    commandList->SetComputeRootDescriptorTable(10, materialRangeSrv);
    commandList->SetComputeRootDescriptorTable(11, baseTextureTableSrv);
    commandList->SetComputeRootDescriptorTable(12, mOutputUavGpu);

    commandList->Dispatch((mWidth + 7u) / 8u, (mHeight + 7u) / 8u, 1);

    auto uav = CD3DX12_RESOURCE_BARRIER::UAV(mOutputTexture.Get());
    commandList->ResourceBarrier(1, &uav);

    D3D12_RESOURCE_BARRIER copyPre[2]{};
    copyPre[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyPre[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->ResourceBarrier(2, copyPre);
    commandList->CopyResource(sceneColorResource, mOutputTexture.Get());

    D3D12_RESOURCE_BARRIER post[2]{};
    post[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    post[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        sceneColorResource,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(2, post);
}

void GlassRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedConstants)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedConstants = nullptr;
    }
    mConstantBuffer.Reset();
    mOutputTexture.Reset();
    mRootSignature.Reset();
    mPipelineState.Reset();
    mComputeShader = DX12Shader{};
    mIsInitialized = false;
}
