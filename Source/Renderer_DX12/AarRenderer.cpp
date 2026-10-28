#include "pch.h"
#include "AarRenderer.h"
#include "AnalyticEdgeAARenderer.h"

#include <algorithm>
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

namespace
{
    constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullscreenPipelineDesc(
        ID3D12RootSignature* rootSignature,
        D3D12_SHADER_BYTECODE vertexShader,
        D3D12_SHADER_BYTECODE pixelShader,
        DXGI_FORMAT targetFormat)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignature;
        desc.VS = vertexShader;
        desc.PS = pixelShader;
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = targetFormat;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
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
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        return desc;
    }

    ComPtr<ID3D12Resource> MakeRenderTarget(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name)
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
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource)));
        resource->SetName(name);
        return resource;
    }
}

void AarRenderer::QueryRenderSize(UINT outputWidth, UINT outputHeight, int mode, UINT& renderWidth, UINT& renderHeight)
{
    const float scale = AarSettings::RenderScale(mode);
    renderWidth = (std::max)(1u, (std::min)(outputWidth, static_cast<UINT>(std::lround(static_cast<float>(outputWidth) * scale))));
    renderHeight = (std::max)(1u, (std::min)(outputHeight, static_cast<UINT>(std::lround(static_cast<float>(outputHeight) * scale))));
}

bool AarRenderer::EnsureSize(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight)
{
    if (renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0)
        return false;

    mLastError.clear();
    try
    {
        if (!AnalyticEdge::IsSupported())
        {
            mLastError = "AarRenderer: the GPU does not support SM 6.1 barycentrics, which the G-Buffer edges come from.";
            return false;
        }
        if (!mReconstructPipeline && !CreatePipelines())
            return false;

        if (mOutputTexture && renderWidth == mRenderWidth && renderHeight == mRenderHeight
            && outputWidth == mOutputWidth && outputHeight == mOutputHeight)
            return true;

        return CreateTextures(renderWidth, renderHeight, outputWidth, outputHeight);
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("AarRenderer: ") + ex.what();
        ReleaseResources();
        return false;
    }
}

void AarRenderer::ReleaseResources()
{
    if (mOutputTexture || mGatedEdges)
        DX12Context_WaitForGPU();
    mOutputTexture.Reset();
    mGatedEdges.Reset();
    mRenderWidth = mRenderHeight = mOutputWidth = mOutputHeight = 0;
}

void AarRenderer::Shutdown()
{
    mOutputTexture.Reset();
    mGatedEdges.Reset();
    mRtvHeap.Reset();
    mGatePipeline.Reset();
    mReconstructPipeline.Reset();
    mRootSignature.Reset();
    mVertexShader = DX12Shader{};
    mGateShader = DX12Shader{};
    mReconstructShader = DX12Shader{};
    // The SRV slots stay allocated: the shared heap never frees, and EnsureSize reuses them.
    mRenderWidth = mRenderHeight = mOutputWidth = mOutputHeight = 0;
}

bool AarRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "AarRenderer: null device.";
        return false;
    }

    const wchar_t* file = L"Shaders\\AnalyticAwareReconstruction.hlsl";
    const ShaderCompileRequest vsRequest{ file, L"VSMain", L"vs_6_0", ShaderStage::Vertex };
    const ShaderCompileRequest gateRequest{ file, L"AarGatePS", L"ps_6_0", ShaderStage::Pixel };
    const ShaderCompileRequest reconstructRequest{ file, L"AarReconstructPS", L"ps_6_0", ShaderStage::Pixel };
    if (!mVertexShader.Compile(vsRequest) || !mGateShader.Compile(gateRequest) || !mReconstructShader.Compile(reconstructRequest))
    {
        const char* error = mVertexShader.GetLastErrorMessage();
        if (!error) error = mGateShader.GetLastErrorMessage();
        if (!error) error = mReconstructShader.GetLastErrorMessage();
        mLastError = std::string("AarRenderer: shader compile failed: ") + (error ? error : "unknown");
        return false;
    }

    // b0: root constants. t0 scene colour, t1 G-Buffer normal + depth, t2 edges: one
    // table each, since they live at unrelated places in the shared heap.
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
        mLastError = "AarRenderer: D3D12SerializeRootSignature failed.";
        return false;
    }
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

    const D3D12_GRAPHICS_PIPELINE_STATE_DESC gateDesc = FullscreenPipelineDesc(
        mRootSignature.Get(), mVertexShader.GetBytecode(), mGateShader.GetBytecode(), AnalyticEdge::kGBufferFormat);
    DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&gateDesc, IID_PPV_ARGS(&mGatePipeline)));

    const D3D12_GRAPHICS_PIPELINE_STATE_DESC reconstructDesc = FullscreenPipelineDesc(
        mRootSignature.Get(), mVertexShader.GetBytecode(), mReconstructShader.GetBytecode(), kOutputFormat);
    DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&reconstructDesc, IID_PPV_ARGS(&mReconstructPipeline)));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 2;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));
    const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mGatedEdgesRtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    mOutputRtv = mGatedEdgesRtv;
    mOutputRtv.ptr += rtvStride;
    return true;
}

bool AarRenderer::CreateTextures(UINT renderWidth, UINT renderHeight, UINT outputWidth, UINT outputHeight)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if ((mGatedEdgesSrvCpu.ptr == 0 && !DX12Context_AllocateSrvDescriptor(&mGatedEdgesSrvCpu, &mGatedEdgesSrvGpu))
        || (mOutputSrvCpu.ptr == 0 && !DX12Context_AllocateSrvDescriptor(&mOutputSrvCpu, &mOutputSrvGpu)))
    {
        mLastError = "AarRenderer: SRV heap full.";
        return false;
    }

    // The old textures may still be in flight.
    if (mOutputTexture || mGatedEdges)
        DX12Context_WaitForGPU();
    mGatedEdges = MakeRenderTarget(device, renderWidth, renderHeight, AnalyticEdge::kGBufferFormat, L"AAR_GatedEdges");
    mOutputTexture = MakeRenderTarget(device, outputWidth, outputHeight, kOutputFormat, L"AAR_Output");

    device->CreateRenderTargetView(mGatedEdges.Get(), nullptr, mGatedEdgesRtv);
    device->CreateRenderTargetView(mOutputTexture.Get(), nullptr, mOutputRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Format = AnalyticEdge::kGBufferFormat;
    device->CreateShaderResourceView(mGatedEdges.Get(), &srvDesc, mGatedEdgesSrvCpu);
    srvDesc.Format = kOutputFormat;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputSrvCpu);

    mRenderWidth = renderWidth;
    mRenderHeight = renderHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    return true;
}

void AarRenderer::Dispatch(
    ID3D12GraphicsCommandList*  commandList,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneColorSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE normalDepthSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE edgeSrv,
    float                       projectionZ33,
    const AarSettings&          settings)
{
    if (commandList == nullptr || !mOutputTexture || !mGatedEdges || !mReconstructPipeline
        || sceneColorSrv.ptr == 0 || normalDepthSrv.ptr == 0 || edgeSrv.ptr == 0)
        return;

    Constants constants{};
    constants.RenderWidth = mRenderWidth;
    constants.RenderHeight = mRenderHeight;
    constants.OutputWidth = mOutputWidth;
    constants.OutputHeight = mOutputHeight;
    constants.DepthThreshold = settings.DepthThreshold;
    constants.NormalThreshold = (settings.CreaseAngleDegrees >= 180.0f)
        ? -2.0f
        : std::cos(settings.CreaseAngleDegrees * 3.14159265358979f / 180.0f);
    constants.ProjectionZ33 = projectionZ33;
    constants.DepthSigma = settings.DepthSigma;
    constants.NormalPower = settings.NormalPower;
    constants.DebugView = static_cast<UINT>(settings.DebugView);

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
    commandList->SetGraphicsRootDescriptorTable(1, sceneColorSrv);
    commandList->SetGraphicsRootDescriptorTable(2, normalDepthSrv);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // --- Gate (render size) ---------------------------------------------------------
    {
        const auto toTarget = CD3DX12_RESOURCE_BARRIER::Transition(mGatedEdges.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->ResourceBarrier(1, &toTarget);

        const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mRenderWidth), static_cast<float>(mRenderHeight), 0.0f, 1.0f };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mRenderWidth), static_cast<LONG>(mRenderHeight) };
        commandList->SetPipelineState(mGatePipeline.Get());
        commandList->SetGraphicsRootDescriptorTable(3, edgeSrv);
        commandList->OMSetRenderTargets(1, &mGatedEdgesRtv, FALSE, nullptr);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->DrawInstanced(3, 1, 0, 0);
    }

    // --- Reconstruct (output size) ------------------------------------------------------
    {
        const D3D12_RESOURCE_BARRIER barriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mGatedEdges.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        commandList->ResourceBarrier(2, barriers);

        const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mOutputWidth), static_cast<float>(mOutputHeight), 0.0f, 1.0f };
        const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mOutputWidth), static_cast<LONG>(mOutputHeight) };
        commandList->SetPipelineState(mReconstructPipeline.Get());
        commandList->SetGraphicsRootDescriptorTable(3, mGatedEdgesSrvGpu);
        commandList->OMSetRenderTargets(1, &mOutputRtv, FALSE, nullptr);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        commandList->DrawInstanced(3, 1, 0, 0);

        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toRead);
    }
}
