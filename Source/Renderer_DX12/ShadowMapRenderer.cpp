#include "pch.h"
#include "ShadowMapRenderer.h"

#include "d3dx12.h"

#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool ShadowMapRenderer::Initialize()
{
    mLastError.clear();
    mIsInitialized = false;

    if (!CreateResources())
        return false;
    if (!CreatePipeline())
        return false;

    mIsInitialized = true;
    return true;
}

void ShadowMapRenderer::BeginShadowPass(
    ID3D12GraphicsCommandList* commandList,
    const DirectX::XMFLOAT3&   sunDir,
    float                       sceneBoundRadius)
{
    if (!commandList || !mIsInitialized) return;

    // -----------------------------------------------------------------------
    // Compute the orthographic light-space view-projection matrix.
    // We position the light camera far enough back that the entire scene sphere
    // fits inside the frustum, then use an ortho projection sized to the sphere.
    // -----------------------------------------------------------------------
    const XMVECTOR lightDir = XMVector3Normalize(
        XMVectorSet(sunDir.x, sunDir.y, sunDir.z, 0.0f));
    const XMVECTOR sceneCenter = XMVectorZero();

    // Place the shadow camera behind the scene along the sun direction.
    // The distance ensures the near/far planes bracket the entire scene.
    const float backDist = sceneBoundRadius * 2.0f;
    const XMVECTOR eyePos = XMVectorAdd(sceneCenter,
        XMVectorScale(lightDir, -backDist));

    // Choose an up vector that is not parallel to lightDir.
    XMVECTOR up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    if (std::fabsf(XMVectorGetZ(lightDir)) > 0.99f)
        up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    const XMMATRIX view = XMMatrixLookAtLH(eyePos, sceneCenter, up);

    // Ortho frustum tightly bounds the scene sphere.
    const float halfSize = sceneBoundRadius;
    const float nearZ    = 0.0f;
    const float farZ     = backDist + sceneBoundRadius;
    const XMMATRIX proj  = XMMatrixOrthographicLH(
        halfSize * 2.0f, halfSize * 2.0f, nearZ, farZ);

    // Store pre-transposed (row-major) matrix for HLSL cbuffer upload.
    XMStoreFloat4x4(&mLightViewProjection, XMMatrixTranspose(view * proj));

    // -----------------------------------------------------------------------
    // Transition shadow texture from SRV → DSV and clear it.
    // -----------------------------------------------------------------------
    const auto toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
        mShadowDepthTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList->ResourceBarrier(1, &toDepthWrite);

    commandList->ClearDepthStencilView(
        mDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    commandList->OMSetRenderTargets(0, nullptr, FALSE, &mDsvHandle);

    // Shadow viewport covers the full shadow map texture.
    const D3D12_VIEWPORT vp =
    {
        0.0f, 0.0f,
        static_cast<float>(kShadowMapSize),
        static_cast<float>(kShadowMapSize),
        0.0f, 1.0f
    };
    const D3D12_RECT scissor = { 0, 0,
        static_cast<LONG>(kShadowMapSize),
        static_cast<LONG>(kShadowMapSize) };
    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &scissor);
}

void ShadowMapRenderer::EndShadowPass(ID3D12GraphicsCommandList* commandList)
{
    if (!commandList || !mIsInitialized) return;

    // Transition back to SRV so the main pass can sample the shadow map.
    const auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        mShadowDepthTexture.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toSrv);
}

void ShadowMapRenderer::Shutdown()
{
    mShadowDepthTexture.Reset();
    mDsvHeap.Reset();
    mDsvHandle  = {};
    mShadowSrvCpu = {};
    mShadowSrvGpu = {};
    mRootSignature.Reset();
    mPipelineState.Reset();
    mVertexShader = DX12Shader{};
    mIsInitialized = false;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool ShadowMapRenderer::CreateResources()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "ShadowMapRenderer: DX12 device is null.";
        return false;
    }

    // Create the shadow depth texture using a typeless format so we can
    // create both a DSV (kShadowDepthFormat) and an SRV (kShadowResourceFormat).
    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type                 = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeap.CreationNodeMask     = 1;
    defaultHeap.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = kShadowMapSize;
    texDesc.Height             = kShadowMapSize;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = kShadowTypelessFormat;
    texDesc.SampleDesc.Count   = 1;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format               = kShadowDepthFormat;
    clearVal.DepthStencil.Depth   = 1.0f;
    clearVal.DepthStencil.Stencil = 0;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        // Initial state is SRV so the first barrier in BeginShadowPass transitions correctly.
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clearVal,
        IID_PPV_ARGS(&mShadowDepthTexture))))
    {
        mLastError = "ShadowMapRenderer: Failed to create shadow depth texture.";
        return false;
    }
    mShadowDepthTexture->SetName(L"ShadowDepthTexture");

    // DSV heap for the depth-only pass.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap))))
    {
        mLastError = "ShadowMapRenderer: Failed to create DSV heap.";
        return false;
    }

    mDsvHandle = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format        = kShadowDepthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(mShadowDepthTexture.Get(), &dsvDesc, mDsvHandle);

    // Allocate an SRV slot in the shared shader-visible heap.
    if (!DX12Context_AllocateSrvDescriptor(&mShadowSrvCpu, &mShadowSrvGpu))
    {
        mLastError = "ShadowMapRenderer: Failed to allocate shadow SRV descriptor.";
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format                    = kShadowResourceFormat;
    srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels       = 1;
    device->CreateShaderResourceView(mShadowDepthTexture.Get(), &srvDesc, mShadowSrvCpu);

    return true;
}

bool ShadowMapRenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "ShadowMapRenderer: DX12 device is null.";
        return false;
    }

    // Compile the depth-only vertex shader (no pixel shader needed).
    const ShaderCompileRequest vsReq
    {
        L"Shaders\\ShadowDepth.hlsl",
        L"VSMain",
        L"vs_5_0",
        ShaderStage::Vertex
    };
    if (!mVertexShader.Compile(vsReq))
    {
        mLastError = std::string("ShadowMapRenderer: VS compile failed: ")
            + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // Root signature: single root CBV at slot 0 (b0, vertex visibility) for gLightMVP.
    D3D12_ROOT_PARAMETER rootParam{};
    rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0; // b0
    rootParam.Descriptor.RegisterSpace  = 0;
    rootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters   = &rootParam;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "ShadowMapRenderer: D3D12SerializeRootSignature failed.";
        return false;
    }
    if (FAILED(device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "ShadowMapRenderer: CreateRootSignature failed.";
        return false;
    }

    // Input layout must match the full mesh vertex layout (POSITION, NORMAL, TEXCOORD, COLOR)
    // even though the shader only reads POSITION — the driver still validates the IA layout.
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS             = mVertexShader.GetBytecode();
    // No pixel shader — depth-only pass.
    psoDesc.SampleMask     = UINT_MAX;
    psoDesc.NumRenderTargets = 0;   // no colour target
    psoDesc.DSVFormat      = kShadowDepthFormat;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Disable blending (no RT).
    psoDesc.BlendState.IndependentBlendEnable  = FALSE;
    psoDesc.BlendState.AlphaToCoverageEnable   = FALSE;

    // Back-face culling (front faces cast shadows, consistent with the main pass).
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;
    // Slope-scaled depth bias reduces shadow acne without requiring a large constant bias.
    psoDesc.RasterizerState.DepthBias             = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias  = 1.0f;
    psoDesc.RasterizerState.DepthBiasClamp        = 0.01f;

    // Standard depth write.
    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable  = FALSE;

    psoDesc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "ShadowMapRenderer: CreateGraphicsPipelineState failed.";
        return false;
    }

    return true;
}
