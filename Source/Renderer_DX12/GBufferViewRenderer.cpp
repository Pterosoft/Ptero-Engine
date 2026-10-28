#include "pch.h"
#include "GBufferViewRenderer.h"

#include <cstring>

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
    // Display data, not radiance: eight bits a channel is all the viewport shows anyway.
    constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool GBufferViewRenderer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();

    try
    {
        if (!mIsInitialized && !CreatePipeline())
            return false;

        if ((!mIsInitialized || width != mWidth || height != mHeight) && !CreateTextures(width, height))
            return false;

        mWidth = width;
        mHeight = height;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("GBufferViewRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void GBufferViewRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }

    mConstantBuffer.Reset();
    mOutputTexture.Reset();
    mRtvHeap.Reset();
    mPipelineState.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
}

bool GBufferViewRenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "GBufferViewRenderer: null device.";
        return false;
    }

    const ShaderCompileRequest vsReq{ L"Shaders\\GBufferView.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    const ShaderCompileRequest psReq{ L"Shaders\\GBufferView.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };
    if (!mVertexShader.Compile(vsReq) || !mPixelShader.Compile(psReq))
    {
        const char* error = mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage()
                                                                : mPixelShader.GetLastErrorMessage();
        mLastError = std::string("GBufferViewRenderer: shader compile failed: ") + (error ? error : "unknown");
        return false;
    }

    // One table per G-Buffer target: they come from the shared heap, wherever the
    // lighting pass allocated them, except the two principled targets, which are
    // allocated together and bind as one.
    D3D12_DESCRIPTOR_RANGE ranges[5]{};
    const UINT registers[5] = { 0, 1, 2, 3, 5 };
    const UINT counts[5]    = { 1, 1, 1, 2, 1 };
    D3D12_ROOT_PARAMETER params[6]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT i = 0; i < 5; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = counts[i];
        ranges[i].BaseShaderRegister = registers[i];
        params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(std::size(params));
    rsDesc.pParameters = params;

    ComPtr<ID3DBlob> serialized, errors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mRootSignature.Get();
    psoDesc.VS                    = mVertexShader.GetBytecode();
    psoDesc.PS                    = mPixelShader.GetBytecode();
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = kOutputFormat;
    psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.DepthStencilState.DepthEnable    = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.InputLayout = { nullptr, 0 };
    DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)));

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = sizeof(ViewConstants);
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mConstantBuffer)));
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mMappedCb));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 1;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    return true;
}

bool GBufferViewRenderer::CreateTextures(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    if (!mSrvSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputSrvCpu, &mOutputSrvGpu))
        {
            mLastError = "GBufferViewRenderer: failed to allocate an SRV slot.";
            return false;
        }
        mSrvSlotAllocated = true;
        mOutputTextureId = static_cast<UiTextureID>(mOutputSrvGpu.ptr);
    }

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
    desc.Format = kOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = kOutputFormat;

    DX12Context_WaitForGPU();
    mOutputTexture.Reset();
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear, IID_PPV_ARGS(&mOutputTexture)));

    device->CreateRenderTargetView(mOutputTexture.Get(), nullptr, mRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = kOutputFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputSrvCpu);

    return true;
}

void GBufferViewRenderer::Render(
    ID3D12GraphicsCommandList*  commandList,
    GBufferView                 view,
    const GBufferSrvs&          gbuffer,
    D3D12_GPU_DESCRIPTOR_HANDLE motionVectorSrv,
    const DirectX::XMFLOAT4X4&  invViewProjTransposed,
    const DirectX::XMFLOAT3&    cameraPosition)
{
    if (!mIsInitialized || !commandList || gbuffer.Albedo.ptr == 0 || gbuffer.Normal.ptr == 0
        || gbuffer.Material.ptr == 0 || gbuffer.Principled0.ptr == 0)
        return;

    ViewConstants constants{};
    constants.InvViewProj = invViewProjTransposed;
    constants.CameraPos   = cameraPosition;
    constants.View        = static_cast<int>(view);
    constants.Width       = mWidth;
    constants.Height      = mHeight;
    constants.HasMotion   = motionVectorSrv.ptr != 0 ? 1u : 0u;
    std::memcpy(mMappedCb, &constants, sizeof(constants));

    auto toTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &toTarget);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    // Every SRV bound below lives in the shared heap.
    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->SetGraphicsRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(1, gbuffer.Albedo);
    commandList->SetGraphicsRootDescriptorTable(2, gbuffer.Normal);
    commandList->SetGraphicsRootDescriptorTable(3, gbuffer.Material);
    commandList->SetGraphicsRootDescriptorTable(4, gbuffer.Principled0);
    // Without motion vectors the slot still needs a valid descriptor; the shader does
    // not read it (HasMotion is 0).
    commandList->SetGraphicsRootDescriptorTable(5, motionVectorSrv.ptr != 0 ? motionVectorSrv : gbuffer.Albedo);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShaderResource);
}
