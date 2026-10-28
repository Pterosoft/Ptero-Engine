#include "pch.h"
#include "AnalyticEdgeAARenderer.h"

#include <cmath>
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

// ---------------------------------------------------------------------------
// AnalyticEdge
// ---------------------------------------------------------------------------

bool AnalyticEdge::IsSupported()
{
    // Asked once: the answer cannot change for the device's lifetime, and every
    // G-Buffer pipeline must agree on it.
    static int supported = -1;
    if (supported >= 0)
        return supported != 0;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_1 };
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
    const bool shaderModelOk =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))
        && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_1;
    const bool barycentricsOk =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3)))
        && options3.BarycentricsSupported;

    supported = (shaderModelOk && barycentricsOk) ? 1 : 0;
    OutputDebugStringA(supported
        ? "AnalyticEdge: SM 6.1 barycentrics supported; the G-Buffer writes triangle edges.\n"
        : "AnalyticEdge: SM 6.1 barycentrics unsupported; Analytic Edge AA is unavailable.\n");
    return supported != 0;
}

ShaderCompileRequest AnalyticEdge::MakeGBufferPixelShaderRequest(const wchar_t* filePath, const wchar_t* entryPoint)
{
    ShaderCompileRequest request{ filePath, entryPoint, L"ps_5_0", ShaderStage::Pixel };
    if (IsSupported())
    {
        request.TargetProfile = L"ps_6_1";
        request.Defines.push_back(L"PTERO_ANALYTIC_EDGES=1");
    }
    return request;
}

// ---------------------------------------------------------------------------
// AnalyticEdgeAARenderer
// ---------------------------------------------------------------------------

bool AnalyticEdgeAARenderer::Initialize(UINT width, UINT height, DXGI_FORMAT sceneColorFormat)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();
    try
    {
        if (!AnalyticEdge::IsSupported())
        {
            mLastError = "AnalyticEdgeAARenderer: the GPU does not support SM 6.1 barycentrics.";
            return false;
        }

        mSceneColorFormat = sceneColorFormat;
        if (!mPipelineState && !CreatePipeline())
            return false;
        if (!CreateSceneCopy(width, height))
            return false;

        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("AnalyticEdgeAARenderer::Initialize: ") + ex.what();
        return false;
    }
}

bool AnalyticEdgeAARenderer::Resize(UINT width, UINT height)
{
    if (!mIsInitialized)
        return false;
    if (width == mWidth && height == mHeight)
        return true;

    try
    {
        return CreateSceneCopy(width, height);
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("AnalyticEdgeAARenderer::Resize: ") + ex.what();
        mIsInitialized = false;
        return false;
    }
}

void AnalyticEdgeAARenderer::Shutdown()
{
    mSceneCopy.Reset();
    mPipelineState.Reset();
    mRootSignature.Reset();
    mVertexShader = DX12Shader{};
    mPixelShader = DX12Shader{};
    // The SRV slot stays allocated: the shared heap never frees, and Initialize reuses it.
    mWidth = 0;
    mHeight = 0;
    mIsInitialized = false;
}

bool AnalyticEdgeAARenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "AnalyticEdgeAARenderer: null device.";
        return false;
    }

    const ShaderCompileRequest vsRequest{ L"Shaders\\AnalyticEdgeAA.hlsl", L"VSMain", L"vs_6_0", ShaderStage::Vertex };
    const ShaderCompileRequest psRequest{ L"Shaders\\AnalyticEdgeAA.hlsl", L"PSMain", L"ps_6_0", ShaderStage::Pixel };
    if (!mVertexShader.Compile(vsRequest) || !mPixelShader.Compile(psRequest))
    {
        const char* vsError = mVertexShader.GetLastErrorMessage();
        const char* psError = mPixelShader.GetLastErrorMessage();
        mLastError = std::string("AnalyticEdgeAARenderer: shader compile failed: ")
            + (vsError ? vsError : (psError ? psError : "unknown"));
        return false;
    }

    // b0: root constants. t0 scene copy, t1 G-Buffer normal + depth, t2 G-Buffer edges:
    // one table each, since the three live at unrelated places in the shared heap.
    D3D12_DESCRIPTOR_RANGE srvRanges[3]{};
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT i = 0; i < 3; ++i)
    {
        srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors = 1;
        srvRanges[i].BaseShaderRegister = i;
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[1 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(std::size(params));
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "AnalyticEdgeAARenderer: D3D12SerializeRootSignature failed.";
        return false;
    }
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = mVertexShader.GetBytecode();
    psoDesc.PS = mPixelShader.GetBytecode();
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mSceneColorFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
    blend.BlendEnable = FALSE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)));
    return true;
}

bool AnalyticEdgeAARenderer::CreateSceneCopy(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (mSceneCopySrvCpu.ptr == 0
        && !DX12Context_AllocateSrvDescriptor(&mSceneCopySrvCpu, &mSceneCopySrvGpu))
    {
        mLastError = "AnalyticEdgeAARenderer: SRV heap full.";
        return false;
    }

    if (mSceneCopy)
    {
        DX12Context_WaitForGPU();
        mSceneCopy.Reset();
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
    desc.Format = mSceneColorFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&mSceneCopy)));
    mSceneCopy->SetName(L"AnalyticEdgeAA_SceneCopy");

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = mSceneColorFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mSceneCopy.Get(), &srvDesc, mSceneCopySrvCpu);

    mWidth = width;
    mHeight = height;
    return true;
}

void AnalyticEdgeAARenderer::Apply(
    ID3D12GraphicsCommandList*  commandList,
    ID3D12Resource*             sceneColor,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
    D3D12_GPU_DESCRIPTOR_HANDLE normalDepthSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE edgeSrv,
    float                       projectionZ33,
    const AnalyticEdgeAASettings& settings)
{
    if (!mIsInitialized || commandList == nullptr || sceneColor == nullptr
        || normalDepthSrv.ptr == 0 || edgeSrv.ptr == 0)
        return;

    // Snapshot the lit scene, so every pixel reads its neighbours before any is rewritten.
    {
        const D3D12_RESOURCE_BARRIER toCopy[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(sceneColor,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mSceneCopy.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        commandList->ResourceBarrier(2, toCopy);
        commandList->CopyResource(mSceneCopy.Get(), sceneColor);

        const D3D12_RESOURCE_BARRIER fromCopy[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(sceneColor,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(mSceneCopy.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(2, fromCopy);
    }

    Constants constants{};
    constants.Width = mWidth;
    constants.Height = mHeight;
    constants.DepthThreshold = settings.DepthThreshold;
    constants.NormalThreshold = (settings.CreaseAngleDegrees >= 180.0f)
        ? -2.0f
        : std::cos(settings.CreaseAngleDegrees * 3.14159265358979f / 180.0f);
    constants.ProjectionZ33 = projectionZ33;
    constants.DebugView = static_cast<UINT>(settings.DebugView);

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
    commandList->SetGraphicsRootDescriptorTable(1, mSceneCopySrvGpu);
    commandList->SetGraphicsRootDescriptorTable(2, normalDepthSrv);
    commandList->SetGraphicsRootDescriptorTable(3, edgeSrv);

    const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}
