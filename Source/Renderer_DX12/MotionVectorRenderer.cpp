#include "pch.h"
#include "MotionVectorRenderer.h"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
}

namespace
{
    constexpr DXGI_FORMAT MotionVectorFormat = DXGI_FORMAT_R16G16_FLOAT;
}

bool MotionVectorRenderer::Initialize(UINT width, UINT height)
{
    if (mInitialized)
    {
        return EnsureSize(width, height);
    }

    if (!CreatePipeline())
    {
        return false;
    }

    if (!CreateOutput(width, height))
    {
        return false;
    }

    mInitialized = true;
    return true;
}

void MotionVectorRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedConstants != nullptr)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedConstants = nullptr;
    }

    mConstantBuffer.Reset();
    mOutputTexture.Reset();
    mRtvHeap.Reset();
    mRootSignature.Reset();
    mPipelineState.Reset();
    mGpuMeshes.clear();
    mWidth = 0;
    mHeight = 0;
    mInitialized = false;
}

bool MotionVectorRenderer::EnsureSize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
    {
        return false;
    }

    if (mOutputTexture && mWidth == width && mHeight == height)
    {
        return true;
    }

    return CreateOutput(width, height);
}

void MotionVectorRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const std::unordered_map<std::size_t, XMFLOAT4X4>& previousTransforms,
    const XMFLOAT4X4& currentViewProjection,
    const XMFLOAT4X4& previousViewProjection,
    bool resetHistory)
{
    if (!mInitialized || !commandList || !mEntities || mEntities->empty() || !mOutputTexture)
    {
        return;
    }

    std::size_t meshCount = 0;
    for (const Entity& entity : *mEntities)
    {
        if (entity.HasMeshComponent() && entity.Mesh.has_value() && entity.Mesh->MeshAsset)
        {
            ++meshCount;
        }
    }

    if (meshCount == 0 || !EnsureConstantBuffer(meshCount))
    {
        return;
    }

    mClearedThisFrame = false;

    const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &toRenderTarget);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    commandList->ClearRenderTargetView(mRtvHandle, clearColor, 0, nullptr);
    commandList->OMSetRenderTargets(1, &mRtvHandle, FALSE, nullptr);
    mClearedThisFrame = true;

    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const XMMATRIX currVP = XMLoadFloat4x4(&currentViewProjection);
    const XMMATRIX prevVP = XMLoadFloat4x4(&previousViewProjection);

    std::size_t cbSlot = 0;
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& entity = (*mEntities)[i];
        if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset)
        {
            continue;
        }

        const Mesh* mesh = entity.Mesh->MeshAsset.get();
        if (!EnsureMesh(commandList, mesh))
        {
            ++cbSlot;
            continue;
        }

        auto gpuMeshIt = mGpuMeshes.find(mesh);
        if (gpuMeshIt == mGpuMeshes.end() || gpuMeshIt->second.IndexCount == 0)
        {
            ++cbSlot;
            continue;
        }

        XMMATRIX model = entity.Transform.GetTransform();
        XMMATRIX prevModel = model;
        const auto prevTransformIt = previousTransforms.find(i);
        if (!resetHistory && prevTransformIt != previousTransforms.end())
        {
            prevModel = XMLoadFloat4x4(&prevTransformIt->second);
        }

        const XMMATRIX currMvp = XMMatrixTranspose(model * currVP);
        const XMMATRIX prevMvp = XMMatrixTranspose(prevModel * prevVP);
        XMStoreFloat4x4(&mMappedConstants[cbSlot].CurrentModelViewProjection, currMvp);
        XMStoreFloat4x4(&mMappedConstants[cbSlot].PreviousModelViewProjection, prevMvp);

        commandList->SetGraphicsRootConstantBufferView(
            0,
            mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(cbSlot) * sizeof(MotionVectorConstants));

        commandList->IASetVertexBuffers(0, 1, &gpuMeshIt->second.VertexView);
        commandList->IASetIndexBuffer(&gpuMeshIt->second.IndexView);
        commandList->DrawIndexedInstanced(gpuMeshIt->second.IndexCount, 1, 0, 0, 0);
        ++cbSlot;
    }

    const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShaderResource);
}

bool MotionVectorRenderer::BeginExternalPass(ID3D12GraphicsCommandList* commandList)
{
    if (!mInitialized || commandList == nullptr || !mOutputTexture)
        return false;

    const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &toRenderTarget);

    // Render() clears when it has meshes to draw, but it bails out early on an
    // empty scene.  Clearing here in that case keeps the target from carrying
    // last frame's vectors into the temporal resolve.
    if (!mClearedThisFrame)
    {
        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        commandList->ClearRenderTargetView(mRtvHandle, clearColor, 0, nullptr);
        mClearedThisFrame = true;
    }

    commandList->OMSetRenderTargets(1, &mRtvHandle, FALSE, nullptr);

    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    return true;
}

void MotionVectorRenderer::EndExternalPass(ID3D12GraphicsCommandList* commandList)
{
    if (commandList == nullptr || !mOutputTexture)
        return;

    const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShaderResource);

    // The next frame's Render() resets this; clearing it here as well keeps an
    // external-only frame from being mistaken for an already-cleared one.
    mClearedThisFrame = false;
}

void MotionVectorRenderer::ResetHistory()
{
}

bool MotionVectorRenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "MotionVectorRenderer: DX12 device is null.";
        return false;
    }

    const ShaderCompileRequest vsRequest
    {
        L"Shaders\\MotionVectors.hlsl",
        L"VSMain",
        L"vs_5_0",
        ShaderStage::Vertex
    };
    const ShaderCompileRequest psRequest
    {
        L"Shaders\\MotionVectors.hlsl",
        L"PSMain",
        L"ps_5_0",
        ShaderStage::Pixel
    };

    if (!mVertexShader.Compile(vsRequest))
    {
        mLastError = std::string("Motion vector VS compile failed: ") + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    if (!mPixelShader.Compile(psRequest))
    {
        mLastError = std::string("Motion vector PS compile failed: ") + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.Descriptor.RegisterSpace = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> rootSignatureErrors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSignature,
        &rootSignatureErrors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0,
        serializedRootSignature->GetBufferPointer(),
        serializedRootSignature->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = mVertexShader.GetBytecode();
    psoDesc.PS = mPixelShader.GetBytecode();
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = MotionVectorFormat;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.RasterizerState.MultisampleEnable = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };

    DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)));
    return true;
}

bool MotionVectorRenderer::CreateOutput(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "MotionVectorRenderer: DX12 device is null.";
        return false;
    }

    DX12Context_WaitForGPU();
    mOutputTexture.Reset();
    mRtvHeap.Reset();

    if (!mOutputSrvAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputSrvCpu, &mOutputSrvGpu))
        {
            mLastError = "MotionVectorRenderer: failed to allocate output SRV descriptor.";
            return false;
        }
        mOutputTextureId = static_cast<UiTextureID>(mOutputSrvGpu.ptr);
        mOutputSrvAllocated = true;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));
    mRtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = MotionVectorFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = MotionVectorFormat;

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearValue,
        IID_PPV_ARGS(&mOutputTexture)));

    device->CreateRenderTargetView(mOutputTexture.Get(), nullptr, mRtvHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = MotionVectorFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputSrvCpu);

    mWidth = width;
    mHeight = height;
    return true;
}

bool MotionVectorRenderer::EnsureConstantBuffer(std::size_t requiredCount)
{
    if (requiredCount == 0)
    {
        return true;
    }

    if (mConstantBuffer && requiredCount <= mConstantBufferCapacity)
    {
        return true;
    }

    if (mConstantBuffer && mMappedConstants != nullptr)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedConstants = nullptr;
    }

    const std::size_t newCapacity = (std::max)(requiredCount, mConstantBufferCapacity == 0 ? requiredCount : mConstantBufferCapacity * 2);
    const UINT64 bufferSize = static_cast<UINT64>(newCapacity * sizeof(MotionVectorConstants));

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bufferSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mConstantBuffer)));

    void* mapped = nullptr;
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mapped));
    mMappedConstants = reinterpret_cast<MotionVectorConstants*>(mapped);
    mConstantBufferCapacity = newCapacity;
    return true;
}

bool MotionVectorRenderer::EnsureMesh(ID3D12GraphicsCommandList* commandList, const Mesh* mesh)
{
    // One entry per asset, so an entry can never come to describe a different
    // mesh than the one it was built from, and nothing ever has to be erased to
    // correct it while the GPU may still be reading it.
    if (mGpuMeshes.find(mesh) != mGpuMeshes.end())
    {
        return true;
    }

    if (mesh == nullptr || mesh->GetVertices().empty() || mesh->GetIndices().empty())
    {
        return false;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    GpuMesh gpuMesh{};
    gpuMesh.SourceMesh = mesh;

    const UINT64 vbSize = mesh->GetVertices().size() * sizeof(Vertex);
    const UINT64 ibSize = mesh->GetIndices().size() * sizeof(std::uint32_t);

    if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, gpuMesh.VertexBuffer)
        || !CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, gpuMesh.VertexUpload)
        || !CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, gpuMesh.IndexBuffer)
        || !CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, gpuMesh.IndexUpload))
    {
        return false;
    }

    void* mapped = nullptr;
    DX12_THROW_IF_FAILED(gpuMesh.VertexUpload->Map(0, nullptr, &mapped));
    std::memcpy(mapped, mesh->GetVertices().data(), static_cast<size_t>(vbSize));
    gpuMesh.VertexUpload->Unmap(0, nullptr);

    DX12_THROW_IF_FAILED(gpuMesh.IndexUpload->Map(0, nullptr, &mapped));
    std::memcpy(mapped, mesh->GetIndices().data(), static_cast<size_t>(ibSize));
    gpuMesh.IndexUpload->Unmap(0, nullptr);

    commandList->CopyBufferRegion(gpuMesh.VertexBuffer.Get(), 0, gpuMesh.VertexUpload.Get(), 0, vbSize);
    commandList->CopyBufferRegion(gpuMesh.IndexBuffer.Get(), 0, gpuMesh.IndexUpload.Get(), 0, ibSize);

    D3D12_RESOURCE_BARRIER barriers[2] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(gpuMesh.VertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        CD3DX12_RESOURCE_BARRIER::Transition(gpuMesh.IndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER)
    };
    commandList->ResourceBarrier(2, barriers);

    gpuMesh.VertexView.BufferLocation = gpuMesh.VertexBuffer->GetGPUVirtualAddress();
    gpuMesh.VertexView.StrideInBytes = sizeof(Vertex);
    gpuMesh.VertexView.SizeInBytes = static_cast<UINT>(vbSize);

    gpuMesh.IndexView.BufferLocation = gpuMesh.IndexBuffer->GetGPUVirtualAddress();
    gpuMesh.IndexView.Format = DXGI_FORMAT_R32_UINT;
    gpuMesh.IndexView.SizeInBytes = static_cast<UINT>(ibSize);
    gpuMesh.IndexCount = static_cast<UINT>(mesh->GetIndices().size());

    mGpuMeshes[mesh] = std::move(gpuMesh);
    return true;
}

bool MotionVectorRenderer::CreateCommittedBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = heapType;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return SUCCEEDED(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        nullptr,
        IID_PPV_ARGS(&outResource)));
}