#include "pch.h"
#include "DecalRenderer.h"

#include "d3dx12.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ---------------------------------------------------------------------------
// Buffer helper (identical pattern to PointLightRenderer)
// ---------------------------------------------------------------------------
namespace
{
    static bool CreateAndUploadBuffer(
        ID3D12Device*              device,
        ID3D12GraphicsCommandList* commandList,
        const void*                initialData,
        UINT64                     dataSize,
        D3D12_RESOURCE_STATES      finalState,
        ComPtr<ID3D12Resource>&    outDefault,
        ComPtr<ID3D12Resource>&    outUpload)
    {
        auto heapDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto heapUpload  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto bufDesc     = CD3DX12_RESOURCE_DESC::Buffer(dataSize);

        if (FAILED(device->CreateCommittedResource(
            &heapDefault, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outDefault))))
            return false;

        if (FAILED(device->CreateCommittedResource(
            &heapUpload, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outUpload))))
            return false;

        D3D12_SUBRESOURCE_DATA sub{};
        sub.pData      = initialData;
        sub.RowPitch   = static_cast<LONG_PTR>(dataSize);
        sub.SlicePitch = sub.RowPitch;
        UpdateSubresources(commandList, outDefault.Get(), outUpload.Get(), 0, 0, 1, &sub);

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            outDefault.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
        commandList->ResourceBarrier(1, &barrier);

        return true;
    }
} // namespace

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

bool DecalRenderer::Initialize(
    ID3D12GraphicsCommandList* commandList,
    DXGI_FORMAT                colorFormat,
    DXGI_FORMAT                depthFormat)
{
    if (mPipelineReady) return true;

    if (!CreateBoxMesh(commandList))   return false;
    if (!CreateArrowMesh(commandList)) return false;
    if (!CreatePipeline(colorFormat, depthFormat)) return false;

    mPipelineReady = true;
    return true;
}

// ---------------------------------------------------------------------------
// Box: unit wireframe box with extents [-0.5, +0.5] on each axis (12 edges)
// ---------------------------------------------------------------------------

bool DecalRenderer::CreateBoxMesh(ID3D12GraphicsCommandList* commandList)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    const std::array<XMFLOAT3, 8> verts = {{
        { -0.5f, -0.5f, -0.5f },
        {  0.5f, -0.5f, -0.5f },
        {  0.5f,  0.5f, -0.5f },
        { -0.5f,  0.5f, -0.5f },
        { -0.5f, -0.5f,  0.5f },
        {  0.5f, -0.5f,  0.5f },
        {  0.5f,  0.5f,  0.5f },
        { -0.5f,  0.5f,  0.5f },
    }};

    // 12 edges as a line-list (24 indices)
    const std::array<std::uint16_t, 24> indices = {{
        0,1, 1,2, 2,3, 3,0,   // back face  (z = -0.5)
        4,5, 5,6, 6,7, 7,4,   // front face (z = +0.5)
        0,4, 1,5, 2,6, 3,7    // connecting edges
    }};

    mBoxIndexCount = static_cast<UINT>(indices.size());

    if (!CreateAndUploadBuffer(device, commandList,
        verts.data(), sizeof(verts),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        mBoxVertexBuffer, mBoxVertexUpload))
        return false;

    if (!CreateAndUploadBuffer(device, commandList,
        indices.data(), sizeof(indices),
        D3D12_RESOURCE_STATE_INDEX_BUFFER,
        mBoxIndexBuffer, mBoxIndexUpload))
        return false;

    mBoxVBView.BufferLocation = mBoxVertexBuffer->GetGPUVirtualAddress();
    mBoxVBView.SizeInBytes    = static_cast<UINT>(sizeof(verts));
    mBoxVBView.StrideInBytes  = sizeof(XMFLOAT3);

    mBoxIBView.BufferLocation = mBoxIndexBuffer->GetGPUVirtualAddress();
    mBoxIBView.SizeInBytes    = static_cast<UINT>(sizeof(indices));
    mBoxIBView.Format         = DXGI_FORMAT_R16_UINT;

    return true;
}

// ---------------------------------------------------------------------------
// Arrow: shaft along -Z from origin, with four arrowhead fins at the tip.
// The decal projects in -Z local space to match the box front face.
// ---------------------------------------------------------------------------

bool DecalRenderer::CreateArrowMesh(ID3D12GraphicsCommandList* commandList)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    const std::array<XMFLOAT3, 6> verts = {{
        {  0.0f,  0.0f,  0.0f },   // 0: shaft base
        {  0.0f,  0.0f, -1.0f },   // 1: tip
        {  0.2f,  0.0f, -0.7f },   // 2: fin +X
        { -0.2f,  0.0f, -0.7f },   // 3: fin -X
        {  0.0f,  0.2f, -0.7f },   // 4: fin +Y
        {  0.0f, -0.2f, -0.7f },   // 5: fin -Y
    }};

    const std::array<std::uint16_t, 10> indices = {{
        0, 1,        // shaft
        1, 2, 1, 3,  // horizontal fins
        1, 4, 1, 5   // vertical fins
    }};

    mArrowIndexCount = static_cast<UINT>(indices.size());

    if (!CreateAndUploadBuffer(device, commandList,
        verts.data(), sizeof(verts),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        mArrowVertexBuffer, mArrowVertexUpload))
        return false;

    if (!CreateAndUploadBuffer(device, commandList,
        indices.data(), sizeof(indices),
        D3D12_RESOURCE_STATE_INDEX_BUFFER,
        mArrowIndexBuffer, mArrowIndexUpload))
        return false;

    mArrowVBView.BufferLocation = mArrowVertexBuffer->GetGPUVirtualAddress();
    mArrowVBView.SizeInBytes    = static_cast<UINT>(sizeof(verts));
    mArrowVBView.StrideInBytes  = sizeof(XMFLOAT3);

    mArrowIBView.BufferLocation = mArrowIndexBuffer->GetGPUVirtualAddress();
    mArrowIBView.SizeInBytes    = static_cast<UINT>(sizeof(indices));
    mArrowIBView.Format         = DXGI_FORMAT_R16_UINT;

    return true;
}

// ---------------------------------------------------------------------------
// Pipeline (root sig + PSO) – mirrors PointLightRenderer::CreatePipeline
// ---------------------------------------------------------------------------

bool DecalRenderer::CreatePipeline(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "DecalRenderer: no D3D12 device for pipeline creation.";
        return false;
    }

    // Compile vertex and pixel shaders from Decal.hlsl.
    const ShaderCompileRequest vsReq{ L"Shaders\\Decal.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    const ShaderCompileRequest psReq{ L"Shaders\\Decal.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel  };

    if (!mVertexShader.Compile(vsReq))
    {
        mLastError = std::string("DecalRenderer VS compile failed: ")
                   + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }
    if (!mPixelShader.Compile(psReq))
    {
        mLastError = std::string("DecalRenderer PS compile failed: ")
                   + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // Root signature: single root CBV at b0 visible to all stages.
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0; // b0
    rootParam.Descriptor.RegisterSpace  = 0;
    rootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = 1;
    rsDesc.pParameters       = &rootParam;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers   = nullptr;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "DecalRenderer: D3D12SerializeRootSignature failed.";
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "DecalRenderer: CreateRootSignature failed.";
        return false;
    }

    // Input layout: float3 POSITION only.
    D3D12_INPUT_ELEMENT_DESC inputElement{};
    inputElement.SemanticName         = "POSITION";
    inputElement.SemanticIndex        = 0;
    inputElement.Format               = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElement.InputSlot            = 0;
    inputElement.AlignedByteOffset    = 0;
    inputElement.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    inputElement.InstanceDataStepRate = 0;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature        = mRootSignature.Get();
    pso.VS                    = mVertexShader.GetBytecode();
    pso.PS                    = mPixelShader.GetBytecode();
    pso.InputLayout           = { &inputElement, 1 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = colorFormat;
    pso.DSVFormat             = depthFormat;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    pso.RasterizerState.FillMode              = D3D12_FILL_MODE_WIREFRAME;
    pso.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable       = TRUE;

    // Depth: test but don't write so the gizmo is occluded by geometry without z-fighting.
    pso.DepthStencilState.DepthEnable    = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.DepthStencilState.StencilEnable  = FALSE;

    // Additive blending so the gizmo tints the scene without fully overwriting it.
    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable           = TRUE;
    rtBlend.SrcBlend              = D3D12_BLEND_ONE;
    rtBlend.DestBlend             = D3D12_BLEND_ONE;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[0] = rtBlend;

    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "DecalRenderer: CreateGraphicsPipelineState failed.";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Constant buffer
// ---------------------------------------------------------------------------

bool DecalRenderer::EnsureConstantBuffer(std::size_t requiredCount)
{
    // Each decal issues two draws (box + arrow) so we need 2x slots.
    const std::size_t slotCount = requiredCount * 2;

    if (mConstantBuffer && mCBCapacity >= slotCount)
        return true;

    if (mConstantBuffer && mMappedCB)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCB = nullptr;
    }
    mConstantBuffer.Reset();

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    std::size_t newCapacity = 1;
    while (newCapacity < slotCount)
        newCapacity <<= 1;

    const UINT64 cbSize = static_cast<UINT64>(newCapacity) * sizeof(GizmoConstants);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type             = D3D12_HEAP_TYPE_UPLOAD;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask  = 1;
    auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

    if (FAILED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mConstantBuffer))))
        return false;

    if (FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCB))))
    {
        mConstantBuffer.Reset();
        return false;
    }

    mCBCapacity = newCapacity;
    return true;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void DecalRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const std::vector<Entity>& entities,
    const XMMATRIX&            viewProjection)
{
    if (!mPipelineReady || mBoxIndexCount == 0 || mArrowIndexCount == 0) return;

    // Count decals this frame.
    std::size_t decalCount = 0;
    for (const Entity& e : entities)
    {
        if (e.Decal.has_value())
            ++decalCount;
    }

    if (decalCount == 0) return;
    if (!EnsureConstantBuffer(decalCount)) return;

    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

    const D3D12_GPU_VIRTUAL_ADDRESS cbBase = mConstantBuffer->GetGPUVirtualAddress();

    std::size_t drawIndex = 0;
    for (const Entity& e : entities)
    {
        if (!e.Decal.has_value()) continue;

        const DecalComponent& dc = *e.Decal;

        const XMMATRIX rotM   = XMMatrixRotationRollPitchYaw(
            e.Transform.Rotation.x,
            e.Transform.Rotation.y,
            e.Transform.Rotation.z);
        const XMMATRIX transM = XMMatrixTranslation(
            e.Transform.Position.x,
            e.Transform.Position.y,
            e.Transform.Position.z);

        // Box scaled to decal extents.
        const XMMATRIX boxModel = XMMatrixScaling(dc.SizeX, dc.SizeY, dc.SizeZ) * rotM * transM;
        const XMMATRIX boxMVP   = XMMatrixTranspose(boxModel * viewProjection);

        // Push the facing arrow slightly out in front of the decal volume so it reads clearly in 3D.
        const float arrowScale = (std::max)(0.05f, (std::min)({ dc.SizeX, dc.SizeY, dc.SizeZ }) * 0.35f);
        const XMMATRIX arrowModel =
            XMMatrixScaling(arrowScale, arrowScale, arrowScale) *
            XMMatrixTranslation(0.0f, 0.0f, -0.5f * dc.SizeZ - (arrowScale * 0.25f)) *
            rotM * transM;
        const XMMATRIX arrowMVP   = XMMatrixTranspose(arrowModel * viewProjection);

        const std::size_t backFaceSlot  = drawIndex * 4;
        const std::size_t frontFaceSlot = drawIndex * 4 + 1;
        const std::size_t sideFaceSlot  = drawIndex * 4 + 2;
        const std::size_t arrowSlot     = drawIndex * 4 + 3;

        GizmoConstants& backFaceCB = mMappedCB[backFaceSlot];
        XMStoreFloat4x4(&backFaceCB.MVP, boxMVP);
        backFaceCB.Color = XMFLOAT4(0.12f, 0.35f, 0.48f, 1.0f);

        GizmoConstants& frontFaceCB = mMappedCB[frontFaceSlot];
        XMStoreFloat4x4(&frontFaceCB.MVP, boxMVP);
        frontFaceCB.Color = XMFLOAT4(0.45f, 0.92f, 1.0f, 1.0f);

        GizmoConstants& sideFaceCB = mMappedCB[sideFaceSlot];
        XMStoreFloat4x4(&sideFaceCB.MVP, boxMVP);
        sideFaceCB.Color = XMFLOAT4(0.24f, 0.68f, 0.86f, 1.0f);

        GizmoConstants& arrowCB = mMappedCB[arrowSlot];
        XMStoreFloat4x4(&arrowCB.MVP, arrowMVP);
        arrowCB.Color = XMFLOAT4(1.0f, 0.65f, 0.1f, 1.0f);

        // Draw back face edges.
        commandList->SetGraphicsRootConstantBufferView(0, cbBase + backFaceSlot * sizeof(GizmoConstants));
        commandList->IASetVertexBuffers(0, 1, &mBoxVBView);
        commandList->IASetIndexBuffer(&mBoxIBView);
        commandList->DrawIndexedInstanced(8, 1, 0, 0, 0);

        // Draw front face edges brighter.
        commandList->SetGraphicsRootConstantBufferView(0, cbBase + frontFaceSlot * sizeof(GizmoConstants));
        commandList->DrawIndexedInstanced(8, 1, 8, 0, 0);

        // Draw connecting side edges.
        commandList->SetGraphicsRootConstantBufferView(0, cbBase + sideFaceSlot * sizeof(GizmoConstants));
        commandList->DrawIndexedInstanced(8, 1, 16, 0, 0);

        // Draw arrow.
        commandList->SetGraphicsRootConstantBufferView(0, cbBase + arrowSlot * sizeof(GizmoConstants));
        commandList->IASetVertexBuffers(0, 1, &mArrowVBView);
        commandList->IASetIndexBuffer(&mArrowIBView);
        commandList->DrawIndexedInstanced(mArrowIndexCount, 1, 0, 0, 0);

        ++drawIndex;
    }
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void DecalRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCB)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCB = nullptr;
    }
    mConstantBuffer.Reset();
    mCBCapacity = 0;

    mBoxVertexBuffer.Reset();
    mBoxVertexUpload.Reset();
    mBoxIndexBuffer.Reset();
    mBoxIndexUpload.Reset();

    mArrowVertexBuffer.Reset();
    mArrowVertexUpload.Reset();
    mArrowIndexBuffer.Reset();
    mArrowIndexUpload.Reset();

    mRootSignature.Reset();
    mPipelineState.Reset();
    mVertexShader = DX12Shader{};
    mPixelShader  = DX12Shader{};
    mPipelineReady = false;
}
