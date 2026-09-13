#include "pch.h"
#include "VolumetricFogRenderer.h"

#include "d3dx12.h"

#include <algorithm>
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
    constexpr UINT kInjectGroupX = 8;
    constexpr UINT kInjectGroupY = 8;
    constexpr UINT kAccumulateGroupX = 8;
    constexpr UINT kAccumulateGroupY = 8;
    constexpr DXGI_FORMAT kFogFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr UINT kMinFroxelTileSize = 4;
    constexpr UINT kMaxFroxelTileSize = 32;
    constexpr UINT kInternalMaxFroxelTileSize = 64;
    constexpr UINT kMinDepthSlices = 8;
    constexpr UINT kMaxDepthSlices = 128;
    constexpr uint64_t kMaxTotalFroxels = 4ull * 1024ull * 1024ull;

    struct FogGridDimensions
    {
        UINT TileSize = 8;
        UINT Width = 1;
        UINT Height = 1;
        UINT DepthSlices = 64;
    };

    FogGridDimensions ComputeFogGridDimensions(UINT sceneWidth, UINT sceneHeight, const VolumetricFogSettings& settings)
    {
        FogGridDimensions dimensions;
        dimensions.TileSize = static_cast<UINT>(std::clamp(settings.FroxelTileSize, static_cast<int>(kMinFroxelTileSize), static_cast<int>(kMaxFroxelTileSize)));
        dimensions.DepthSlices = static_cast<UINT>(std::clamp(settings.DepthSlices, static_cast<int>(kMinDepthSlices), static_cast<int>(kMaxDepthSlices)));

        const auto recomputeResolution = [&dimensions, sceneWidth, sceneHeight]()
        {
            dimensions.Width = (((sceneWidth + dimensions.TileSize - 1u) / dimensions.TileSize) > 1u) ? ((sceneWidth + dimensions.TileSize - 1u) / dimensions.TileSize) : 1u;
            dimensions.Height = (((sceneHeight + dimensions.TileSize - 1u) / dimensions.TileSize) > 1u) ? ((sceneHeight + dimensions.TileSize - 1u) / dimensions.TileSize) : 1u;
        };

        const auto totalFroxels = [&dimensions]() -> uint64_t
        {
            return static_cast<uint64_t>(dimensions.Width) * static_cast<uint64_t>(dimensions.Height) * static_cast<uint64_t>(dimensions.DepthSlices);
        };

        recomputeResolution();

        while (totalFroxels() > kMaxTotalFroxels && dimensions.DepthSlices > kMinDepthSlices)
        {
            dimensions.DepthSlices = (dimensions.DepthSlices > 64) ? 64u : (dimensions.DepthSlices > 32 ? 32u : kMinDepthSlices);
        }

        while (totalFroxels() > kMaxTotalFroxels && dimensions.TileSize < kInternalMaxFroxelTileSize)
        {
            dimensions.TileSize = (dimensions.TileSize < 8u) ? 8u : (dimensions.TileSize < 16u ? 16u : (dimensions.TileSize < 32u ? 32u : kInternalMaxFroxelTileSize));
            recomputeResolution();
        }

        return dimensions;
    }

    bool AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu, std::string& err)
    {
        if (!DX12Context_AllocateSrvDescriptor(&cpu, &gpu))
        {
            err = "VolumetricFogRenderer: ran out of shared SRV/UAV descriptor heap slots.";
            return false;
        }
        return true;
    }

    bool CreateTexture3D(
        ID3D12Device* device,
        UINT width,
        UINT height,
        UINT depth,
        DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState,
        const wchar_t* name,
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
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&outResource))))
        {
            return false;
        }

        outResource->SetName(name);
        return true;
    }

    bool CompileComputePso(
        ID3D12Device* device,
        ID3D12RootSignature* rootSignature,
        const wchar_t* filePath,
        const wchar_t* entryPoint,
        ComPtr<ID3D12PipelineState>& outPso,
        std::string& err)
    {
        ShaderCompileRequest request{};
        request.FilePath = filePath;
        request.EntryPoint = entryPoint;
        request.TargetProfile = L"cs_6_5";
        request.Stage = ShaderStage::Compute;

        DX12Shader shader;
        if (!shader.Compile(request))
        {
            err = std::string("VolumetricFogRenderer: shader compile failed: ")
                + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSignature;
        psoDesc.CS = shader.GetBytecode();
        if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso))))
        {
            err = "VolumetricFogRenderer: CreateComputePipelineState failed.";
            return false;
        }

        return true;
    }
}

bool VolumetricFogRenderer::Initialize(UINT sceneWidth, UINT sceneHeight)
{
    if (sceneWidth == 0 || sceneHeight == 0)
        return false;

    mLastError.clear();

    if (!mInjectRootSignature && !CreateRootSignatures())
        return false;
    if (!mInjectPipelineState && !CreatePipelines())
        return false;
    if (!mConstantBuffer && !CreateConstantBuffer())
        return false;

    mSceneWidth = sceneWidth;
    mSceneHeight = sceneHeight;

    if (!CreateResolutionResources(VolumetricFogSettings{}))
        return false;
    if (!CreateDescriptors())
        return false;

    mIsInitialized = true;
    return true;
}

bool VolumetricFogRenderer::EnsureSize(UINT sceneWidth, UINT sceneHeight)
{
    if (sceneWidth == mSceneWidth && sceneHeight == mSceneHeight && mIsInitialized)
        return true;
    return Initialize(sceneWidth, sceneHeight);
}

bool VolumetricFogRenderer::EnsureSize(UINT sceneWidth, UINT sceneHeight, const VolumetricFogSettings& settings)
{
    const FogGridDimensions dimensions = ComputeFogGridDimensions(sceneWidth, sceneHeight, settings);
    const UINT desiredWidth = dimensions.Width;
    const UINT desiredHeight = dimensions.Height;
    const UINT desiredSlices = dimensions.DepthSlices;

    if (mIsInitialized
        && sceneWidth == mSceneWidth
        && sceneHeight == mSceneHeight
        && desiredWidth == mFroxelWidth
        && desiredHeight == mFroxelHeight
        && desiredSlices == mDepthSlices)
    {
        return true;
    }

    if (sceneWidth == 0 || sceneHeight == 0)
        return false;

    if (!mInjectRootSignature && !CreateRootSignatures())
        return false;
    if (!mInjectPipelineState && !CreatePipelines())
        return false;
    if (!mConstantBuffer && !CreateConstantBuffer())
        return false;

    if (mIsInitialized && !DX12Context_WaitForGPU())
        return false;

    mSceneWidth = sceneWidth;
    mSceneHeight = sceneHeight;

    if (!CreateResolutionResources(settings))
        return false;
    if (!CreateDescriptors())
        return false;

    mIsInitialized = true;
    return true;
}

void VolumetricFogRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedConstants)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedConstants = nullptr;
    }

    mLightingVolume.Reset();
    mIntegratedFog.Reset();
    mConstantBuffer.Reset();
    mInjectPipelineState.Reset();
    mAccumulatePipelineState.Reset();
    mInjectRootSignature.Reset();
    mAccumulateRootSignature.Reset();
    mIsInitialized = false;
}

void VolumetricFogRenderer::Dispatch(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
    const VolumetricFogSettings& settings,
    const float viewProjInv[16],
    const float currViewProj[16],
    const float cameraPos[3],
    float nearPlane,
    float farPlane,
    float sunDirX,
    float sunDirY,
    float sunDirZ,
    float sunColorR,
    float sunColorG,
    float sunColorB,
    float skyColorR,
    float skyColorG,
    float skyColorB,
    const FogPointLight* pointLights,
    uint32_t numPointLights)
{
    if (cmdList == nullptr || !mIsInitialized || !mConstantBuffer || !mLightingVolume || !mIntegratedFog)
        return;

    UploadConstants(
        settings,
        viewProjInv,
        currViewProj,
        cameraPos,
        nearPlane,
        farPlane,
        sunDirX,
        sunDirY,
        sunDirZ,
        sunColorR,
        sunColorG,
        sunColorB,
        skyColorR,
        skyColorG,
        skyColorB,
        pointLights,
        numPointLights);

    ID3D12DescriptorHeap* descriptorHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
    if (descriptorHeaps[0] != nullptr)
        cmdList->SetDescriptorHeaps(1, descriptorHeaps);

    D3D12_RESOURCE_BARRIER toUav[2];
    toUav[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mLightingVolume.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    toUav[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mIntegratedFog.Get(),
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(2, toUav);

    cmdList->SetComputeRootSignature(mInjectRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    cmdList->SetPipelineState(mInjectPipelineState.Get());
    cmdList->SetComputeRootDescriptorTable(1, sceneDepthSrv);
    cmdList->SetComputeRootDescriptorTable(2, mLightingUavGpu);
    cmdList->Dispatch(
        (mFroxelWidth + kInjectGroupX - 1) / kInjectGroupX,
        (mFroxelHeight + kInjectGroupY - 1) / kInjectGroupY,
        mDepthSlices);

    D3D12_RESOURCE_BARRIER lightingUavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mLightingVolume.Get());
    cmdList->ResourceBarrier(1, &lightingUavBarrier);
    D3D12_RESOURCE_BARRIER lightingToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        mLightingVolume.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &lightingToSrv);

    cmdList->SetComputeRootSignature(mAccumulateRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
    cmdList->SetPipelineState(mAccumulatePipelineState.Get());
    cmdList->SetComputeRootDescriptorTable(1, mLightingSrvGpu);
    cmdList->SetComputeRootDescriptorTable(2, mIntegratedUavGpu);
    cmdList->Dispatch(
        (mFroxelWidth + kAccumulateGroupX - 1) / kAccumulateGroupX,
        (mFroxelHeight + kAccumulateGroupY - 1) / kAccumulateGroupY,
        1);

    D3D12_RESOURCE_BARRIER integratedUavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mIntegratedFog.Get());
    cmdList->ResourceBarrier(1, &integratedUavBarrier);
    D3D12_RESOURCE_BARRIER integratedToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        mIntegratedFog.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &integratedToSrv);
}

bool VolumetricFogRenderer::CreateRootSignatures()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "VolumetricFogRenderer: no D3D12 device.";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE injectRanges[2]{};
    injectRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    injectRanges[0].NumDescriptors = 1;
    injectRanges[0].BaseShaderRegister = 0;
    injectRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    injectRanges[1].NumDescriptors = 1;
    injectRanges[1].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER injectParams[3]{};
    injectParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    injectParams[0].Descriptor.ShaderRegister = 0;
    injectParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 2; ++i)
    {
        injectParams[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        injectParams[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        injectParams[i + 1].DescriptorTable.pDescriptorRanges = &injectRanges[i];
        injectParams[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC injectDesc{};
    injectDesc.NumParameters = 3;
    injectDesc.pParameters = injectParams;
    injectDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&injectDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors))
        || FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&mInjectRootSignature))))
    {
        mLastError = "VolumetricFogRenderer: failed to create injection root signature.";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE accumulateRanges[2]{};
    accumulateRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    accumulateRanges[0].NumDescriptors = 1;
    accumulateRanges[0].BaseShaderRegister = 0;
    accumulateRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    accumulateRanges[1].NumDescriptors = 1;
    accumulateRanges[1].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER accumulateParams[3]{};
    accumulateParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    accumulateParams[0].Descriptor.ShaderRegister = 0;
    accumulateParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 2; ++i)
    {
        accumulateParams[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        accumulateParams[i + 1].DescriptorTable.NumDescriptorRanges = 1;
        accumulateParams[i + 1].DescriptorTable.pDescriptorRanges = &accumulateRanges[i];
        accumulateParams[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC accumulateDesc{};
    accumulateDesc.NumParameters = 3;
    accumulateDesc.pParameters = accumulateParams;
    accumulateDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    blob.Reset();
    errors.Reset();
    if (FAILED(D3D12SerializeRootSignature(&accumulateDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors))
        || FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&mAccumulateRootSignature))))
    {
        mLastError = "VolumetricFogRenderer: failed to create accumulation root signature.";
        return false;
    }

    return true;
}

bool VolumetricFogRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "VolumetricFogRenderer: no D3D12 device.";
        return false;
    }

    if (!CompileComputePso(device, mInjectRootSignature.Get(), L"Data/Shaders/VolumetricFogInject.hlsl", L"CSMain", mInjectPipelineState, mLastError))
        return false;
    if (!CompileComputePso(device, mAccumulateRootSignature.Get(), L"Data/Shaders/VolumetricFogAccumulate.hlsl", L"CSMain", mAccumulatePipelineState, mLastError))
        return false;

    return true;
}

bool VolumetricFogRenderer::CreateConstantBuffer()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(FogConstants);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mConstantBuffer))))
    {
        mLastError = "VolumetricFogRenderer: failed to create constant buffer.";
        return false;
    }

    if (FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedConstants))))
    {
        mLastError = "VolumetricFogRenderer: failed to map constant buffer.";
        return false;
    }

    return true;
}

bool VolumetricFogRenderer::CreateResolutionResources(const VolumetricFogSettings& settings)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    const FogGridDimensions dimensions = ComputeFogGridDimensions(mSceneWidth, mSceneHeight, settings);
    mFroxelWidth = dimensions.Width;
    mFroxelHeight = dimensions.Height;
    mDepthSlices = dimensions.DepthSlices;

    if (!CreateTexture3D(device, mFroxelWidth, mFroxelHeight, mDepthSlices, kFogFormat, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"VolumetricFog_LightingVolume", mLightingVolume))
    {
        mLastError = "VolumetricFogRenderer: failed to create lighting volume.";
        return false;
    }

    if (!CreateTexture3D(device, mFroxelWidth, mFroxelHeight, mDepthSlices, kFogFormat, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"VolumetricFog_IntegratedFog", mIntegratedFog))
    {
        mLastError = "VolumetricFogRenderer: failed to create integrated fog volume.";
        return false;
    }

    return true;
}

bool VolumetricFogRenderer::CreateDescriptors()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    if (!mDescriptorsAllocated)
    {
        if (!AllocateDescriptor(mLightingSrvCpu, mLightingSrvGpu, mLastError)) return false;
        if (!AllocateDescriptor(mLightingUavCpu, mLightingUavGpu, mLastError)) return false;
        if (!AllocateDescriptor(mIntegratedSrvCpu, mIntegratedSrvGpu, mLastError)) return false;
        if (!AllocateDescriptor(mIntegratedUavCpu, mIntegratedUavGpu, mLastError)) return false;
        mDescriptorsAllocated = true;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = kFogFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srvDesc.Texture3D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = kFogFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uavDesc.Texture3D.WSize = mDepthSlices;

    device->CreateShaderResourceView(mLightingVolume.Get(), &srvDesc, mLightingSrvCpu);
    device->CreateUnorderedAccessView(mLightingVolume.Get(), nullptr, &uavDesc, mLightingUavCpu);
    device->CreateShaderResourceView(mIntegratedFog.Get(), &srvDesc, mIntegratedSrvCpu);
    device->CreateUnorderedAccessView(mIntegratedFog.Get(), nullptr, &uavDesc, mIntegratedUavCpu);
    return true;
}

void VolumetricFogRenderer::UploadConstants(
    const VolumetricFogSettings& settings,
    const float viewProjInv[16],
    const float currViewProj[16],
    const float cameraPos[3],
    float nearPlane,
    float farPlane,
    float sunDirX,
    float sunDirY,
    float sunDirZ,
    float sunColorR,
    float sunColorG,
    float sunColorB,
    float skyColorR,
    float skyColorG,
    float skyColorB,
    const FogPointLight* pointLights,
    uint32_t numPointLights)
{
    FogConstants constants{};
    const auto maxu = [](uint32_t a, uint32_t b) -> uint32_t { return a > b ? a : b; };
    const auto maxi = [](int a, int b) -> int { return a > b ? a : b; };
    const auto maxf = [](float a, float b) -> float { return a > b ? a : b; };
    const FogGridDimensions dimensions = ComputeFogGridDimensions(mSceneWidth, mSceneHeight, settings);

    constants.FrameWidth = mSceneWidth;
    constants.FrameHeight = mSceneHeight;

    constants.FroxelWidth = maxu(1u, dimensions.Width);
    constants.FroxelHeight = maxu(1u, dimensions.Height);
    constants.DepthSlices = dimensions.DepthSlices;
    constants.DebugView = static_cast<uint32_t>(maxi(0, settings.DebugView));
    constants.NearPlane = maxf(0.001f, nearPlane);
    constants.FarPlane = maxf(constants.NearPlane + 0.001f, farPlane);
    constants.StartDistance = maxf(constants.NearPlane, settings.StartDistance);
    constants.MaxDistance = maxf(constants.StartDistance + 0.001f, settings.MaxDistance);
    constants.Density = maxf(0.0f, settings.Density);
    constants.Anisotropy = std::clamp(settings.Anisotropy, -0.95f, 0.95f);
    constants.BaseHeight = settings.BaseHeight;
    constants.HeightFalloff = maxf(0.0f, settings.HeightFalloff);

    constants.FogColor[0] = settings.ColorR;
    constants.FogColor[1] = settings.ColorG;
    constants.FogColor[2] = settings.ColorB;

    constants.EmissiveColor[0] = settings.EmissiveColorR;
    constants.EmissiveColor[1] = settings.EmissiveColorG;
    constants.EmissiveColor[2] = settings.EmissiveColorB;
    constants.EmissiveIntensity = maxf(0.0f, settings.EmissiveIntensity);

    constants.CameraPos[0] = cameraPos[0];
    constants.CameraPos[1] = cameraPos[1];
    constants.CameraPos[2] = cameraPos[2];

    constants.SunDir[0] = sunDirX;
    constants.SunDir[1] = sunDirY;
    constants.SunDir[2] = sunDirZ;

    constants.SunColor[0] = sunColorR;
    constants.SunColor[1] = sunColorG;
    constants.SunColor[2] = sunColorB;

    constants.SkyColor[0] = skyColorR;
    constants.SkyColor[1] = skyColorG;
    constants.SkyColor[2] = skyColorB;

    constants.NumPointLights = (std::min)(numPointLights, kMaxPointLights);
    for (uint32_t i = 0; i < constants.NumPointLights; ++i)
    {
        constants.PointLights[i] = pointLights[i];
    }

    std::memcpy(constants.ViewProjInv, viewProjInv, sizeof(constants.ViewProjInv));
    std::memcpy(constants.CurrViewProj, currViewProj, sizeof(constants.CurrViewProj));
    std::memcpy(mMappedConstants, &constants, sizeof(constants));

    mFroxelWidth = constants.FroxelWidth;
    mFroxelHeight = constants.FroxelHeight;
    mDepthSlices = constants.DepthSlices;
}
