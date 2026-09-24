#include "pch.h"
#include "System/DataFiles.h"
#include <filesystem>
#include "VolumetricCloudRenderer.h"

#include "d3dx12.h"
#include "..\Ptero-Engine\RenderInterfaces.h"

#include <algorithm>
#include <cmath>
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
    constexpr DXGI_FORMAT kNoiseFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kCloudColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kCloudDepthFormat = DXGI_FORMAT_R32_FLOAT;
    constexpr UINT kTraceGroupSize = 8;
    constexpr UINT kBakeGroupSize = 8;
    constexpr float kPi = 3.14159265359f;

    std::wstring GetShaderDirectory()
    {
        // The repository's Data folder, or a packaged game's virtual one (DataFiles.h).
        const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
        return dataDirectory.empty() ? std::wstring(L"Data\\Shaders\\") : (dataDirectory / L"Shaders").wstring() + L"\\";
    }

    // Colour and depth targets are always used together, so they are tracked and
    // transitioned as a pair.
    void TransitionPair(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* colorResource,
        ID3D12Resource* depthResource,
        D3D12_RESOURCE_STATES& currentState,
        D3D12_RESOURCE_STATES targetState)
    {
        if (colorResource == nullptr || depthResource == nullptr || currentState == targetState)
            return;

        const D3D12_RESOURCE_BARRIER barriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(colorResource, currentState, targetState),
            CD3DX12_RESOURCE_BARRIER::Transition(depthResource, currentState, targetState)
        };
        commandList->ResourceBarrier(2, barriers);
        currentState = targetState;
    }

    bool CreateTexture2D(
        ID3D12Device* device,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState,
        const wchar_t* debugName,
        ComPtr<ID3D12Resource>& outResource)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&outResource))))
        {
            return false;
        }

        outResource->SetName(debugName);
        return true;
    }

    bool CreateTexture3D(
        ID3D12Device* device,
        UINT width,
        UINT height,
        UINT depth,
        DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState,
        const wchar_t* debugName,
        ComPtr<ID3D12Resource>& outResource)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = static_cast<UINT16>(depth);
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&outResource))))
        {
            return false;
        }

        outResource->SetName(debugName);
        return true;
    }

    bool CreateUploadBuffer(
        ID3D12Device* device,
        UINT64 sizeInBytes,
        const wchar_t* debugName,
        ComPtr<ID3D12Resource>& outResource)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeInBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&outResource))))
        {
            return false;
        }

        outResource->SetName(debugName);
        return true;
    }

    D3D12_STATIC_SAMPLER_DESC MakeStaticSampler(
        UINT shaderRegister,
        D3D12_TEXTURE_ADDRESS_MODE addressMode,
        D3D12_SHADER_VISIBILITY visibility)
    {
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = addressMode;
        sampler.AddressV = addressMode;
        sampler.AddressW = addressMode;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = shaderRegister;
        sampler.ShaderVisibility = visibility;
        return sampler;
    }

    bool CompileComputePipeline(
        ID3D12Device* device,
        ID3D12RootSignature* rootSignature,
        const std::wstring& filePath,
        const wchar_t* entryPoint,
        const std::wstring& includeDirectory,
        ComPtr<ID3D12PipelineState>& outPipeline,
        std::string& outError)
    {
        ShaderCompileRequest request{};
        request.FilePath = filePath;
        request.EntryPoint = entryPoint;
        request.TargetProfile = L"cs_6_5";
        request.Stage = ShaderStage::Compute;
        request.IncludeDirectories.push_back(includeDirectory);

        DX12Shader shader;
        if (!shader.Compile(request))
        {
            outError = std::string("VolumetricCloudRenderer: compute shader compile failed: ")
                + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSignature;
        psoDesc.CS = shader.GetBytecode();
        if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPipeline))))
        {
            outError = "VolumetricCloudRenderer: CreateComputePipelineState failed.";
            return false;
        }

        return true;
    }

    bool SerializeAndCreateRootSignature(
        ID3D12Device* device,
        const D3D12_ROOT_SIGNATURE_DESC& desc,
        const wchar_t* debugName,
        ComPtr<ID3D12RootSignature>& outRootSignature,
        std::string& outError)
    {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
        {
            outError = "VolumetricCloudRenderer: D3D12SerializeRootSignature failed";
            if (errors)
                outError += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }

        if (FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&outRootSignature))))
        {
            outError = "VolumetricCloudRenderer: CreateRootSignature failed.";
            return false;
        }

        outRootSignature->SetName(debugName);
        return true;
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

UINT VolumetricCloudRenderer::ComputeTraceExtent(UINT sceneExtent, int resolutionDivisor)
{
    const UINT divisor = static_cast<UINT>(std::clamp(resolutionDivisor, 1, 4));
    return (std::max)(1u, (sceneExtent + divisor - 1u) / divisor);
}

bool VolumetricCloudRenderer::Initialize(UINT sceneWidth, UINT sceneHeight, DXGI_FORMAT sceneColorFormat)
{
    if (sceneWidth == 0 || sceneHeight == 0)
        return false;

    mLastError.clear();

    if (!mBakeRootSignature && !CreateRootSignatures())
        return false;
    if (!mRaymarchPipeline && !CreatePipelines(sceneColorFormat))
        return false;
    if (!mConstantBuffer && !CreateConstantBuffers())
        return false;
    if (!mBaseShapeNoise && !CreateNoiseResources())
        return false;
    if (!CreateNoiseDescriptors())
        return false;

    mSceneWidth = sceneWidth;
    mSceneHeight = sceneHeight;

    if (!CreateResolutionResources(VolumetricCloudSettings{}))
        return false;
    if (!CreateResolutionDescriptors())
        return false;

    mIsInitialized = true;
    return true;
}

bool VolumetricCloudRenderer::EnsureSize(UINT sceneWidth, UINT sceneHeight, const VolumetricCloudSettings& settings)
{
    if (sceneWidth == 0 || sceneHeight == 0)
        return false;

    const UINT divisor = static_cast<UINT>(std::clamp(settings.ResolutionDivisor, 1, 4));
    const UINT desiredTraceWidth = ComputeTraceExtent(sceneWidth, settings.ResolutionDivisor);
    const UINT desiredTraceHeight = ComputeTraceExtent(sceneHeight, settings.ResolutionDivisor);

    if (mIsInitialized
        && sceneWidth == mSceneWidth
        && sceneHeight == mSceneHeight
        && desiredTraceWidth == mTraceWidth
        && desiredTraceHeight == mTraceHeight
        && divisor == mResolutionDivisor)
    {
        return true;
    }

    if (!mIsInitialized)
        return false;

    if (!DX12Context_WaitForGPU())
        return false;

    mSceneWidth = sceneWidth;
    mSceneHeight = sceneHeight;

    ReleaseResolutionResources();

    if (!CreateResolutionResources(settings))
        return false;
    if (!CreateResolutionDescriptors())
        return false;

    mHistoryValid = false;
    return true;
}

void VolumetricCloudRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedConstants)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedConstants = nullptr;
    }
    if (mNoiseConstantBuffer && mMappedNoiseConstants)
    {
        mNoiseConstantBuffer->Unmap(0, nullptr);
        mMappedNoiseConstants = nullptr;
    }

    ReleaseResolutionResources();

    mBaseShapeNoise.Reset();
    mDetailNoise.Reset();
    mCurlNoise.Reset();
    mWeatherMap.Reset();

    mConstantBuffer.Reset();
    mNoiseConstantBuffer.Reset();

    mBaseShapePipeline.Reset();
    mDetailPipeline.Reset();
    mCurlPipeline.Reset();
    mWeatherPipeline.Reset();
    mRaymarchPipeline.Reset();
    mReconstructPipeline.Reset();
    mCompositePipeline.Reset();

    mBakeRootSignature.Reset();
    mRaymarchRootSignature.Reset();
    mReconstructRootSignature.Reset();
    mCompositeRootSignature.Reset();

    mIsInitialized = false;
    mNoiseBaked = false;
    mHistoryValid = false;
    mDispatchedThisFrame = false;
    mBakedWeatherSeed = INT32_MIN;
}

void VolumetricCloudRenderer::ReleaseResolutionResources()
{
    mTraceColor.Reset();
    mTraceDepth.Reset();
    for (UINT i = 0; i < kHistoryBufferCount; ++i)
    {
        mHistoryColor[i].Reset();
        mHistoryDepth[i].Reset();
    }
}

// ---------------------------------------------------------------------------
// Root signatures and pipelines
// ---------------------------------------------------------------------------

bool VolumetricCloudRenderer::CreateRootSignatures()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "VolumetricCloudRenderer: no D3D12 device.";
        return false;
    }

    // ---- Noise bake: b0 + one 3D UAV + one 2D UAV --------------------------
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 1;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT i = 0; i < 2; ++i)
        {
            params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
            params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
            params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 3;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        if (!SerializeAndCreateRootSignature(device, desc, L"VolumetricCloud_BakeRS", mBakeRootSignature, mLastError))
            return false;
    }

    const D3D12_STATIC_SAMPLER_DESC computeSamplers[] =
    {
        MakeStaticSampler(0, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_SHADER_VISIBILITY_ALL),
        MakeStaticSampler(1, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL)
    };

    // ---- Raymarch: b0 + noise SRVs (t0-t3) + scene depth (t4) + UAVs -------
    {
        D3D12_DESCRIPTOR_RANGE ranges[3]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 4;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[2].NumDescriptors = 2;
        ranges[2].BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT i = 0; i < 3; ++i)
        {
            params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
            params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
            params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 4;
        desc.pParameters = params;
        desc.NumStaticSamplers = static_cast<UINT>(std::size(computeSamplers));
        desc.pStaticSamplers = computeSamplers;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        if (!SerializeAndCreateRootSignature(device, desc, L"VolumetricCloud_RaymarchRS", mRaymarchRootSignature, mLastError))
            return false;
    }

    // ---- Reconstruct: b0 + trace/history SRVs (t4-t7) + output UAVs --------
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4;
        ranges[0].BaseShaderRegister = 4;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (UINT i = 0; i < 2; ++i)
        {
            params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
            params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
            params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 3;
        desc.pParameters = params;
        desc.NumStaticSamplers = static_cast<UINT>(std::size(computeSamplers));
        desc.pStaticSamplers = computeSamplers;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        if (!SerializeAndCreateRootSignature(device, desc, L"VolumetricCloud_ReconstructRS", mReconstructRootSignature, mLastError))
            return false;
    }

    // ---- Composite: b0 + resolved cloud colour/depth (t8-t9) ---------------
    {
        const D3D12_STATIC_SAMPLER_DESC pixelSamplers[] =
        {
            MakeStaticSampler(0, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_SHADER_VISIBILITY_PIXEL),
            MakeStaticSampler(1, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_PIXEL)
        };

        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        range.BaseShaderRegister = 8;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.NumStaticSamplers = static_cast<UINT>(std::size(pixelSamplers));
        desc.pStaticSamplers = pixelSamplers;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        if (!SerializeAndCreateRootSignature(device, desc, L"VolumetricCloud_CompositeRS", mCompositeRootSignature, mLastError))
            return false;
    }

    return true;
}

bool VolumetricCloudRenderer::CreatePipelines(DXGI_FORMAT sceneColorFormat)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "VolumetricCloudRenderer: no D3D12 device.";
        return false;
    }

    const std::wstring shaderDirectory = GetShaderDirectory();
    const std::wstring noisePath = shaderDirectory + L"VolumetricCloudNoise.hlsl";
    const std::wstring renderPath = shaderDirectory + L"VolumetricCloudRender.hlsl";
    const std::wstring reconstructPath = shaderDirectory + L"VolumetricCloudReconstruct.hlsl";
    const std::wstring compositePath = shaderDirectory + L"VolumetricCloudComposite.hlsl";

    if (!CompileComputePipeline(device, mBakeRootSignature.Get(), noisePath, L"CSBaseShape", shaderDirectory, mBaseShapePipeline, mLastError))
        return false;
    if (!CompileComputePipeline(device, mBakeRootSignature.Get(), noisePath, L"CSDetail", shaderDirectory, mDetailPipeline, mLastError))
        return false;
    if (!CompileComputePipeline(device, mBakeRootSignature.Get(), noisePath, L"CSCurl", shaderDirectory, mCurlPipeline, mLastError))
        return false;
    if (!CompileComputePipeline(device, mBakeRootSignature.Get(), noisePath, L"CSWeather", shaderDirectory, mWeatherPipeline, mLastError))
        return false;
    if (!CompileComputePipeline(device, mRaymarchRootSignature.Get(), renderPath, L"CSMain", shaderDirectory, mRaymarchPipeline, mLastError))
        return false;
    if (!CompileComputePipeline(device, mReconstructRootSignature.Get(), reconstructPath, L"CSMain", shaderDirectory, mReconstructPipeline, mLastError))
        return false;

    DX12Shader vertexShader;
    DX12Shader pixelShader;
    {
        ShaderCompileRequest request{};
        request.FilePath = compositePath;
        request.EntryPoint = L"VSMain";
        request.TargetProfile = L"vs_6_5";
        request.Stage = ShaderStage::Vertex;
        request.IncludeDirectories.push_back(shaderDirectory);
        if (!vertexShader.Compile(request))
        {
            mLastError = std::string("VolumetricCloudRenderer: composite VS compile failed: ")
                + (vertexShader.GetLastErrorMessage() ? vertexShader.GetLastErrorMessage() : "unknown");
            return false;
        }
    }
    {
        ShaderCompileRequest request{};
        request.FilePath = compositePath;
        request.EntryPoint = L"PSMain";
        request.TargetProfile = L"ps_6_5";
        request.Stage = ShaderStage::Pixel;
        request.IncludeDirectories.push_back(shaderDirectory);
        if (!pixelShader.Compile(request))
        {
            mLastError = std::string("VolumetricCloudRenderer: composite PS compile failed: ")
                + (pixelShader.GetLastErrorMessage() ? pixelShader.GetLastErrorMessage() : "unknown");
            return false;
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mCompositeRootSignature.Get();
    psoDesc.VS = vertexShader.GetBytecode();
    psoDesc.PS = pixelShader.GetBytecode();
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = sceneColorFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.InputLayout = { nullptr, 0 };

    // scene * srcAlpha (transmittance) + luminance.  Destination alpha is left
    // untouched so the deferred lighting pass's coverage mask survives.
    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.BlendEnable = TRUE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = blend;

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mCompositePipeline))))
    {
        mLastError = "VolumetricCloudRenderer: composite CreateGraphicsPipelineState failed.";
        return false;
    }
    mCompositePipeline->SetName(L"VolumetricCloud_CompositePSO");

    return true;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

bool VolumetricCloudRenderer::CreateConstantBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (!CreateUploadBuffer(device, sizeof(CloudConstants), L"VolumetricCloud_Constants", mConstantBuffer))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the constant buffer.";
        return false;
    }
    if (FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedConstants))))
    {
        mLastError = "VolumetricCloudRenderer: failed to map the constant buffer.";
        return false;
    }

    // One 256 byte aligned slot per bake kernel so all four dispatches can share
    // a command list without overwriting each other's constants.
    if (!CreateUploadBuffer(device, sizeof(NoiseConstants) * NoiseBakeSlot_Count, L"VolumetricCloud_NoiseConstants", mNoiseConstantBuffer))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the noise constant buffer.";
        return false;
    }
    if (FAILED(mNoiseConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedNoiseConstants))))
    {
        mLastError = "VolumetricCloudRenderer: failed to map the noise constant buffer.";
        return false;
    }

    return true;
}

bool VolumetricCloudRenderer::CreateNoiseResources()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    mNoiseState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (!CreateTexture3D(device, kBaseShapeSize, kBaseShapeSize, kBaseShapeSize, kNoiseFormat,
        mNoiseState, L"VolumetricCloud_BaseShapeNoise", mBaseShapeNoise))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the base shape noise volume.";
        return false;
    }

    if (!CreateTexture3D(device, kDetailSize, kDetailSize, kDetailSize, kNoiseFormat,
        mNoiseState, L"VolumetricCloud_DetailNoise", mDetailNoise))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the detail noise volume.";
        return false;
    }

    if (!CreateTexture2D(device, kCurlSize, kCurlSize, kNoiseFormat,
        mNoiseState, L"VolumetricCloud_CurlNoise", mCurlNoise))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the curl noise texture.";
        return false;
    }

    if (!CreateTexture2D(device, kWeatherSize, kWeatherSize, kNoiseFormat,
        mNoiseState, L"VolumetricCloud_WeatherMap", mWeatherMap))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the weather map.";
        return false;
    }

    return true;
}

bool VolumetricCloudRenderer::CreateResolutionResources(const VolumetricCloudSettings& settings)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    mResolutionDivisor = static_cast<UINT>(std::clamp(settings.ResolutionDivisor, 1, 4));
    mTraceWidth = ComputeTraceExtent(mSceneWidth, settings.ResolutionDivisor);
    mTraceHeight = ComputeTraceExtent(mSceneHeight, settings.ResolutionDivisor);

    mTraceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!CreateTexture2D(device, mTraceWidth, mTraceHeight, kCloudColorFormat, mTraceState,
        L"VolumetricCloud_TraceColor", mTraceColor))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the trace colour target.";
        return false;
    }
    if (!CreateTexture2D(device, mTraceWidth, mTraceHeight, kCloudDepthFormat, mTraceState,
        L"VolumetricCloud_TraceDepth", mTraceDepth))
    {
        mLastError = "VolumetricCloudRenderer: failed to create the trace depth target.";
        return false;
    }

    for (UINT i = 0; i < kHistoryBufferCount; ++i)
    {
        mHistoryState[i] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (!CreateTexture2D(device, mSceneWidth, mSceneHeight, kCloudColorFormat, mHistoryState[i],
            L"VolumetricCloud_HistoryColor", mHistoryColor[i]))
        {
            mLastError = "VolumetricCloudRenderer: failed to create a cloud history colour buffer.";
            return false;
        }
        if (!CreateTexture2D(device, mSceneWidth, mSceneHeight, kCloudDepthFormat, mHistoryState[i],
            L"VolumetricCloud_HistoryDepth", mHistoryDepth[i]))
        {
            mLastError = "VolumetricCloudRenderer: failed to create a cloud history depth buffer.";
            return false;
        }
    }

    mHistoryValid = false;
    return true;
}

bool VolumetricCloudRenderer::CreateNoiseDescriptors()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (!mNoiseDescriptorsAllocated)
    {
        for (UINT i = 0; i < NoiseBakeSlot_Count; ++i)
        {
            if (!DX12Context_AllocateSrvDescriptor(&mBakeUavCpu[i], &mBakeUavGpu[i]))
            {
                mLastError = "VolumetricCloudRenderer: ran out of shared descriptor heap slots (noise UAVs).";
                return false;
            }
        }

        // t0..t3 must be contiguous for the raymarch descriptor table.
        for (UINT i = 0; i < 4; ++i)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
            if (!DX12Context_AllocateSrvDescriptor(&mNoiseSrvCpu[i], &gpuHandle))
            {
                mLastError = "VolumetricCloudRenderer: ran out of shared descriptor heap slots (noise SRVs).";
                return false;
            }
            if (i == 0)
                mNoiseSrvTableGpu = gpuHandle;
        }

        mNoiseDescriptorsAllocated = true;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC volumeUav{};
    volumeUav.Format = kNoiseFormat;
    volumeUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    volumeUav.Texture3D.WSize = kBaseShapeSize;
    device->CreateUnorderedAccessView(mBaseShapeNoise.Get(), nullptr, &volumeUav, mBakeUavCpu[NoiseBakeSlot_BaseShape]);

    volumeUav.Texture3D.WSize = kDetailSize;
    device->CreateUnorderedAccessView(mDetailNoise.Get(), nullptr, &volumeUav, mBakeUavCpu[NoiseBakeSlot_Detail]);

    D3D12_UNORDERED_ACCESS_VIEW_DESC textureUav{};
    textureUav.Format = kNoiseFormat;
    textureUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(mCurlNoise.Get(), nullptr, &textureUav, mBakeUavCpu[NoiseBakeSlot_Curl]);
    device->CreateUnorderedAccessView(mWeatherMap.Get(), nullptr, &textureUav, mBakeUavCpu[NoiseBakeSlot_Weather]);

    D3D12_SHADER_RESOURCE_VIEW_DESC volumeSrv{};
    volumeSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    volumeSrv.Format = kNoiseFormat;
    volumeSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    volumeSrv.Texture3D.MipLevels = 1;
    device->CreateShaderResourceView(mBaseShapeNoise.Get(), &volumeSrv, mNoiseSrvCpu[0]);
    device->CreateShaderResourceView(mDetailNoise.Get(), &volumeSrv, mNoiseSrvCpu[1]);

    D3D12_SHADER_RESOURCE_VIEW_DESC textureSrv{};
    textureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    textureSrv.Format = kNoiseFormat;
    textureSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    textureSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mCurlNoise.Get(), &textureSrv, mNoiseSrvCpu[2]);
    device->CreateShaderResourceView(mWeatherMap.Get(), &textureSrv, mNoiseSrvCpu[3]);

    return true;
}

bool VolumetricCloudRenderer::CreateResolutionDescriptors()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (!mResolutionDescriptorsAllocated)
    {
        const auto allocateBlock = [this](D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandles, UINT count, D3D12_GPU_DESCRIPTOR_HANDLE& outTableStart) -> bool
        {
            for (UINT i = 0; i < count; ++i)
            {
                D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
                if (!DX12Context_AllocateSrvDescriptor(&cpuHandles[i], &gpuHandle))
                {
                    mLastError = "VolumetricCloudRenderer: ran out of shared descriptor heap slots.";
                    return false;
                }
                if (i == 0)
                    outTableStart = gpuHandle;
            }
            return true;
        };

        if (!allocateBlock(mTraceUavCpu, 2, mTraceUavTableGpu))
            return false;

        for (UINT i = 0; i < kHistoryBufferCount; ++i)
        {
            if (!allocateBlock(mReconstructSrvCpu[i], 4, mReconstructSrvTableGpu[i]))
                return false;
        }
        for (UINT i = 0; i < kHistoryBufferCount; ++i)
        {
            if (!allocateBlock(mReconstructUavCpu[i], 2, mReconstructUavTableGpu[i]))
                return false;
        }
        for (UINT i = 0; i < kHistoryBufferCount; ++i)
        {
            if (!allocateBlock(mCompositeSrvCpu[i], 2, mCompositeSrvTableGpu[i]))
                return false;
        }

        mResolutionDescriptorsAllocated = true;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC colorUav{};
    colorUav.Format = kCloudColorFormat;
    colorUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_UNORDERED_ACCESS_VIEW_DESC depthUav{};
    depthUav.Format = kCloudDepthFormat;
    depthUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_SHADER_RESOURCE_VIEW_DESC colorSrv{};
    colorSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    colorSrv.Format = kCloudColorFormat;
    colorSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    colorSrv.Texture2D.MipLevels = 1;

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Format = kCloudDepthFormat;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Texture2D.MipLevels = 1;

    device->CreateUnorderedAccessView(mTraceColor.Get(), nullptr, &colorUav, mTraceUavCpu[0]);
    device->CreateUnorderedAccessView(mTraceDepth.Get(), nullptr, &depthUav, mTraceUavCpu[1]);

    for (UINT i = 0; i < kHistoryBufferCount; ++i)
    {
        device->CreateShaderResourceView(mTraceColor.Get(), &colorSrv, mReconstructSrvCpu[i][0]);
        device->CreateShaderResourceView(mTraceDepth.Get(), &depthSrv, mReconstructSrvCpu[i][1]);
        device->CreateShaderResourceView(mHistoryColor[i].Get(), &colorSrv, mReconstructSrvCpu[i][2]);
        device->CreateShaderResourceView(mHistoryDepth[i].Get(), &depthSrv, mReconstructSrvCpu[i][3]);

        device->CreateUnorderedAccessView(mHistoryColor[i].Get(), nullptr, &colorUav, mReconstructUavCpu[i][0]);
        device->CreateUnorderedAccessView(mHistoryDepth[i].Get(), nullptr, &depthUav, mReconstructUavCpu[i][1]);

        device->CreateShaderResourceView(mHistoryColor[i].Get(), &colorSrv, mCompositeSrvCpu[i][0]);
        device->CreateShaderResourceView(mHistoryDepth[i].Get(), &depthSrv, mCompositeSrvCpu[i][1]);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Noise bake
// ---------------------------------------------------------------------------

void VolumetricCloudRenderer::BakeNoise(ID3D12GraphicsCommandList* commandList, const VolumetricCloudSettings& settings)
{
    const bool weatherDirty =
        settings.WeatherSeed != mBakedWeatherSeed
        || settings.WeatherCellSize != mBakedWeatherCellSize
        || settings.WeatherCoverageBias != mBakedWeatherCoverageBias
        || settings.WeatherTypeBias != mBakedWeatherTypeBias;

    if (mNoiseBaked && !weatherDirty)
        return;

    const bool bakeShapes = !mNoiseBaked;

    ID3D12Resource* const bakedResources[] =
    {
        mBaseShapeNoise.Get(), mDetailNoise.Get(), mCurlNoise.Get(), mWeatherMap.Get()
    };

    if (mNoiseState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER toUav[4]{};
        for (UINT i = 0; i < 4; ++i)
        {
            toUav[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                bakedResources[i], mNoiseState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        commandList->ResourceBarrier(4, toUav);
        mNoiseState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    NoiseConstants slots[NoiseBakeSlot_Count]{};
    slots[NoiseBakeSlot_BaseShape].TargetSize = kBaseShapeSize;
    slots[NoiseBakeSlot_Detail].TargetSize = kDetailSize;
    slots[NoiseBakeSlot_Curl].TargetSize = kCurlSize;
    slots[NoiseBakeSlot_Weather].TargetSize = kWeatherSize;
    slots[NoiseBakeSlot_Weather].Seed = static_cast<uint32_t>(settings.WeatherSeed);
    slots[NoiseBakeSlot_Weather].WeatherCellSize = (std::max)(settings.WeatherCellSize, 0.25f);
    slots[NoiseBakeSlot_Weather].WeatherCoverageBias = std::clamp(settings.WeatherCoverageBias, -1.0f, 1.0f);
    slots[NoiseBakeSlot_Weather].WeatherTypeBias = std::clamp(settings.WeatherTypeBias, -1.0f, 1.0f);
    std::memcpy(mMappedNoiseConstants, slots, sizeof(slots));

    const D3D12_GPU_VIRTUAL_ADDRESS noiseCbBase = mNoiseConstantBuffer->GetGPUVirtualAddress();
    commandList->SetComputeRootSignature(mBakeRootSignature.Get());

    const auto dispatchBake = [&](
        NoiseBakeSlot slot,
        ID3D12PipelineState* pipeline,
        D3D12_GPU_DESCRIPTOR_HANDLE volumeUav,
        D3D12_GPU_DESCRIPTOR_HANDLE textureUav,
        UINT sizeX,
        UINT sizeY,
        UINT sizeZ)
    {
        commandList->SetComputeRootConstantBufferView(0, noiseCbBase + static_cast<UINT64>(slot) * sizeof(NoiseConstants));
        commandList->SetComputeRootDescriptorTable(1, volumeUav);
        commandList->SetComputeRootDescriptorTable(2, textureUav);
        commandList->SetPipelineState(pipeline);
        commandList->Dispatch(
            (sizeX + kBakeGroupSize - 1) / kBakeGroupSize,
            (sizeY + kBakeGroupSize - 1) / kBakeGroupSize,
            (sizeZ + kBakeGroupSize - 1) / kBakeGroupSize);
    };

    if (bakeShapes)
    {
        dispatchBake(NoiseBakeSlot_BaseShape, mBaseShapePipeline.Get(),
            mBakeUavGpu[NoiseBakeSlot_BaseShape], mBakeUavGpu[NoiseBakeSlot_Curl],
            kBaseShapeSize, kBaseShapeSize, kBaseShapeSize);

        dispatchBake(NoiseBakeSlot_Detail, mDetailPipeline.Get(),
            mBakeUavGpu[NoiseBakeSlot_Detail], mBakeUavGpu[NoiseBakeSlot_Curl],
            kDetailSize, kDetailSize, kDetailSize);

        dispatchBake(NoiseBakeSlot_Curl, mCurlPipeline.Get(),
            mBakeUavGpu[NoiseBakeSlot_BaseShape], mBakeUavGpu[NoiseBakeSlot_Curl],
            kCurlSize, kCurlSize, 1);
    }

    dispatchBake(NoiseBakeSlot_Weather, mWeatherPipeline.Get(),
        mBakeUavGpu[NoiseBakeSlot_BaseShape], mBakeUavGpu[NoiseBakeSlot_Weather],
        kWeatherSize, kWeatherSize, 1);

    D3D12_RESOURCE_BARRIER uavBarriers[4]{};
    for (UINT i = 0; i < 4; ++i)
        uavBarriers[i] = CD3DX12_RESOURCE_BARRIER::UAV(bakedResources[i]);
    commandList->ResourceBarrier(4, uavBarriers);

    D3D12_RESOURCE_BARRIER toSrv[4]{};
    for (UINT i = 0; i < 4; ++i)
    {
        toSrv[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            bakedResources[i],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    commandList->ResourceBarrier(4, toSrv);
    mNoiseState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    mNoiseBaked = true;
    mBakedWeatherSeed = settings.WeatherSeed;
    mBakedWeatherCellSize = settings.WeatherCellSize;
    mBakedWeatherCoverageBias = settings.WeatherCoverageBias;
    mBakedWeatherTypeBias = settings.WeatherTypeBias;
    mHistoryValid = false;
}

// ---------------------------------------------------------------------------
// Per-frame constants
// ---------------------------------------------------------------------------

void VolumetricCloudRenderer::UploadConstants(
    const VolumetricCloudSettings& settings,
    const float viewProjInv[16],
    const float prevViewProj[16],
    const float cameraPos[3],
    const float sunDirection[3],
    const float sunColor[3],
    const float skyColor[3])
{
    CloudConstants constants{};

    constants.TraceResolution[0] = mTraceWidth;
    constants.TraceResolution[1] = mTraceHeight;
    constants.FullResolution[0] = mSceneWidth;
    constants.FullResolution[1] = mSceneHeight;
    constants.InvTraceResolution[0] = 1.0f / static_cast<float>((std::max)(mTraceWidth, 1u));
    constants.InvTraceResolution[1] = 1.0f / static_cast<float>((std::max)(mTraceHeight, 1u));
    constants.InvFullResolution[0] = 1.0f / static_cast<float>((std::max)(mSceneWidth, 1u));
    constants.InvFullResolution[1] = 1.0f / static_cast<float>((std::max)(mSceneHeight, 1u));

    constants.FrameIndex = mFrameIndex;
    constants.ResolutionDivisor = mResolutionDivisor;
    constants.TemporalEnabled = settings.TemporalUpsampling ? 1u : 0u;
    constants.DebugView = static_cast<uint32_t>(std::clamp(settings.DebugView, 0, 4));

    constants.CameraPos[0] = cameraPos[0];
    constants.CameraPos[1] = cameraPos[1];
    constants.CameraPos[2] = cameraPos[2];
    constants.TimeSeconds = mElapsedSeconds;

    constants.SunDirection[0] = sunDirection[0];
    constants.SunDirection[1] = sunDirection[1];
    constants.SunDirection[2] = sunDirection[2];
    constants.SunIntensityScale = (std::max)(settings.SunIntensityScale, 0.0f);

    constants.SunColor[0] = sunColor[0];
    constants.SunColor[1] = sunColor[1];
    constants.SunColor[2] = sunColor[2];
    constants.AmbientIntensityScale = (std::max)(settings.AmbientIntensityScale, 0.0f);

    constants.SkyColor[0] = skyColor[0];
    constants.SkyColor[1] = skyColor[1];
    constants.SkyColor[2] = skyColor[2];
    constants.GroundBounceScale = std::clamp(settings.GroundBounceScale, 0.0f, 2.0f);

    constants.ScatteringAlbedo[0] = std::clamp(settings.ScatteringColorR, 0.0f, 1.0f);
    constants.ScatteringAlbedo[1] = std::clamp(settings.ScatteringColorG, 0.0f, 1.0f);
    constants.ScatteringAlbedo[2] = std::clamp(settings.ScatteringColorB, 0.0f, 1.0f);
    constants.ExtinctionScale = (std::max)(settings.ExtinctionScale, 1.0e-4f);

    std::memcpy(constants.ViewProjInv, viewProjInv, sizeof(constants.ViewProjInv));
    std::memcpy(constants.PrevViewProj, prevViewProj, sizeof(constants.PrevViewProj));

    const float planetRadius = (std::max)(settings.PlanetRadiusKm, 1.0f) * 1000.0f;
    const float layerBottom = (std::max)(settings.LayerBottomMeters, 1.0f);
    const float layerThickness = (std::max)(settings.LayerThicknessMeters, 100.0f);
    constants.PlanetRadius = planetRadius;
    constants.LayerBottomRadius = planetRadius + layerBottom;
    constants.LayerTopRadius = planetRadius + layerBottom + layerThickness;
    constants.LayerThickness = layerThickness;

    constants.Coverage = std::clamp(settings.Coverage, 0.0f, 1.0f);
    constants.CloudType = std::clamp(settings.CloudType, 0.0f, 1.0f);
    constants.DensityScale = (std::max)(settings.Density, 0.0f);
    constants.BaseNoiseFrequency = 1.0f / (std::max)(settings.BaseNoiseScaleMeters, 100.0f);

    constants.DetailNoiseFrequency = 1.0f / (std::max)(settings.DetailNoiseScaleMeters, 10.0f);
    constants.DetailStrength = std::clamp(settings.DetailStrength, 0.0f, 1.0f);
    constants.CurlStrength = (std::max)(settings.CurlStrength, 0.0f);
    constants.AnvilBias = std::clamp(settings.AnvilBias, 0.0f, 1.0f);

    constants.WeatherFrequency = 1.0f / (std::max)(settings.WeatherScaleMeters, 1000.0f);
    constants.WeatherOffsetX = 0.0f;
    constants.WeatherOffsetY = 0.0f;
    constants.CloudTopOffset = settings.CloudTopOffsetMeters;

    constants.WindOffset[0] = mWindOffset[0];
    constants.WindOffset[1] = mWindOffset[1];
    constants.WindOffset[2] = mWindOffset[2];
    constants.WindSkew = settings.WindSkew;

    constants.DetailWindOffset[0] = mDetailWindOffset[0];
    constants.DetailWindOffset[1] = mDetailWindOffset[1];
    constants.DetailWindOffset[2] = mDetailWindOffset[2];

    const float windRadians = settings.WindDirectionDegrees * (kPi / 180.0f);
    constants.WindDirectionX = std::cos(windRadians);
    constants.WindDirectionY = std::sin(windRadians);

    constants.PhaseG0 = std::clamp(settings.PhaseG0, -0.99f, 0.99f);
    constants.PhaseG1 = std::clamp(settings.PhaseG1, -0.99f, 0.99f);
    constants.PhaseBlend = std::clamp(settings.PhaseBlend, 0.0f, 1.0f);

    constants.PowderStrength = std::clamp(settings.PowderStrength, 0.0f, 1.0f);
    constants.MsOctaves = static_cast<uint32_t>(std::clamp(settings.MultiScatterOctaves, 1, 4));
    constants.MsScatterFalloff = std::clamp(settings.MsScatterFalloff, 0.0f, 1.0f);
    constants.MsExtinctionFalloff = std::clamp(settings.MsExtinctionFalloff, 0.0f, 1.0f);
    constants.MsPhaseFalloff = std::clamp(settings.MsPhaseFalloff, 0.0f, 1.0f);

    constants.MaxSteps = static_cast<uint32_t>(std::clamp(settings.MaxSteps, 16, 256));
    constants.LightSteps = static_cast<uint32_t>(std::clamp(settings.LightSteps, 1, 6));
    constants.LightMarchDistance = (std::max)(settings.LightMarchDistanceMeters, 10.0f);

    constants.MaxTraceDistance = (std::max)(settings.MaxTraceDistanceMeters, 1000.0f);
    constants.DistanceFadeStart = std::clamp(
        settings.DistanceFadeStartMeters, 100.0f, constants.MaxTraceDistance - 1.0f);
    constants.DetailFadeDistance = (std::max)(settings.DetailFadeDistanceMeters, 100.0f);
    constants.TemporalBlend = std::clamp(settings.TemporalBlend, 0.0f, 0.98f);

    constants.ShadowStepGrowth = std::clamp(settings.ShadowStepGrowth, 1.0f, 3.0f);
    constants.HistoryValid = mHistoryValid ? 1.0f : 0.0f;
    constants.ConeSpread = std::clamp(settings.ShadowConeSpread, 0.0f, 1.0f);

    std::memcpy(mMappedConstants, &constants, sizeof(constants));
}

// ---------------------------------------------------------------------------
// Dispatch and composite
// ---------------------------------------------------------------------------

void VolumetricCloudRenderer::Dispatch(
    ID3D12GraphicsCommandList* commandList,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
    const VolumetricCloudSettings& settings,
    const float viewProjInv[16],
    const float prevViewProj[16],
    const float cameraPos[3],
    const float sunDirection[3],
    const float sunColor[3],
    const float skyColor[3],
    float deltaTimeSeconds,
    bool resetHistory)
{
    mDispatchedThisFrame = false;

    if (commandList == nullptr || !mIsInitialized || !mTraceColor || sceneDepthSrv.ptr == 0)
        return;

    if (resetHistory)
        mHistoryValid = false;

    // Advance the wind so the layer drifts and evolves.  Detail advects faster
    // than the base shape, which is what makes cloud interiors churn.
    const float clampedDelta = std::clamp(deltaTimeSeconds, 0.0f, 0.25f);
    mElapsedSeconds += clampedDelta;

    const float windRadians = settings.WindDirectionDegrees * (kPi / 180.0f);
    const float windX = std::cos(windRadians);
    const float windY = std::sin(windRadians);
    const float baseTravel = settings.WindSpeed * clampedDelta;
    const float detailTravel = baseTravel * settings.DetailWindSpeedScale;

    // Sampling positions move against the wind so the clouds themselves move with it.
    mWindOffset[0] -= windX * baseTravel;
    mWindOffset[1] -= windY * baseTravel;
    mWindOffset[2] -= baseTravel * 0.1f;
    mDetailWindOffset[0] -= windX * detailTravel;
    mDetailWindOffset[1] -= windY * detailTravel;
    mDetailWindOffset[2] -= detailTravel * 0.2f;

    std::swap(mHistoryReadIndex, mHistoryWriteIndex);

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    if (heaps[0] != nullptr)
        commandList->SetDescriptorHeaps(1, heaps);

    BakeNoise(commandList, settings);
    UploadConstants(settings, viewProjInv, prevViewProj, cameraPos, sunDirection, sunColor, skyColor);

    const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = mConstantBuffer->GetGPUVirtualAddress();

    // ---- Raymarch at reduced resolution ------------------------------------
    TransitionPair(commandList, mTraceColor.Get(), mTraceDepth.Get(), mTraceState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetComputeRootSignature(mRaymarchRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->SetComputeRootDescriptorTable(1, mNoiseSrvTableGpu);
    commandList->SetComputeRootDescriptorTable(2, sceneDepthSrv);
    commandList->SetComputeRootDescriptorTable(3, mTraceUavTableGpu);
    commandList->SetPipelineState(mRaymarchPipeline.Get());
    commandList->Dispatch(
        (mTraceWidth + kTraceGroupSize - 1) / kTraceGroupSize,
        (mTraceHeight + kTraceGroupSize - 1) / kTraceGroupSize,
        1);

    {
        D3D12_RESOURCE_BARRIER uavBarriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(mTraceColor.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(mTraceDepth.Get())
        };
        commandList->ResourceBarrier(2, uavBarriers);
    }

    mTraceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    TransitionPair(commandList, mTraceColor.Get(), mTraceDepth.Get(), mTraceState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // ---- Temporal reconstruction to full resolution ------------------------
    const uint32_t readIndex = mHistoryReadIndex;
    const uint32_t writeIndex = mHistoryWriteIndex;

    TransitionPair(commandList, mHistoryColor[readIndex].Get(), mHistoryDepth[readIndex].Get(),
        mHistoryState[readIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionPair(commandList, mHistoryColor[writeIndex].Get(), mHistoryDepth[writeIndex].Get(),
        mHistoryState[writeIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetComputeRootSignature(mReconstructRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, constantsAddress);
    commandList->SetComputeRootDescriptorTable(1, mReconstructSrvTableGpu[readIndex]);
    commandList->SetComputeRootDescriptorTable(2, mReconstructUavTableGpu[writeIndex]);
    commandList->SetPipelineState(mReconstructPipeline.Get());
    commandList->Dispatch(
        (mSceneWidth + kTraceGroupSize - 1) / kTraceGroupSize,
        (mSceneHeight + kTraceGroupSize - 1) / kTraceGroupSize,
        1);

    {
        D3D12_RESOURCE_BARRIER uavBarriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::UAV(mHistoryColor[writeIndex].Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(mHistoryDepth[writeIndex].Get())
        };
        commandList->ResourceBarrier(2, uavBarriers);
    }

    // The composite pass reads these from a pixel shader.
    TransitionPair(commandList, mHistoryColor[writeIndex].Get(), mHistoryDepth[writeIndex].Get(),
        mHistoryState[writeIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    ++mFrameIndex;
    mHistoryValid = true;
    mDispatchedThisFrame = true;
}

void VolumetricCloudRenderer::Composite(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
    UINT viewportWidth,
    UINT viewportHeight)
{
    if (commandList == nullptr || !mIsInitialized || !mDispatchedThisFrame || !mCompositePipeline)
        return;

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    if (heaps[0] != nullptr)
        commandList->SetDescriptorHeaps(1, heaps);

    const D3D12_VIEWPORT viewport =
    {
        0.0f, 0.0f,
        static_cast<float>(viewportWidth),
        static_cast<float>(viewportHeight),
        0.0f, 1.0f
    };
    const D3D12_RECT scissor =
    {
        0, 0,
        static_cast<LONG>(viewportWidth),
        static_cast<LONG>(viewportHeight)
    };

    commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    commandList->SetGraphicsRootSignature(mCompositeRootSignature.Get());
    commandList->SetGraphicsRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(1, mCompositeSrvTableGpu[mHistoryWriteIndex]);
    commandList->SetPipelineState(mCompositePipeline.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}

D3D12_GPU_DESCRIPTOR_HANDLE VolumetricCloudRenderer::GetResolvedCloudSrv() const
{
    if (!mIsInitialized || !mDispatchedThisFrame)
        return D3D12_GPU_DESCRIPTOR_HANDLE{};
    return mCompositeSrvTableGpu[mHistoryWriteIndex];
}
