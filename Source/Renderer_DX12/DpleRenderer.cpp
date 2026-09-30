#include "pch.h"
#include "DpleRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

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
    // Must match every image DPLE can be handed (scene colour, TAA, SMAA, DLSS and FSR
    // outputs): the result is copied straight back over it.
    constexpr DXGI_FORMAT ImageFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // The guide carries linear depth that the passes reconstruct world positions from, so
    // it stays 32-bit; fp16 would put steps of centimetres into a 6 cm AO radius at range.
    constexpr DXGI_FORMAT GuideFormat         = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr DXGI_FORMAT OcclusionFormat     = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT ContactShadowFormat = DXGI_FORMAT_R16_FLOAT;

    constexpr D3D12_RESOURCE_STATES SrvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES UavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    constexpr UINT kGroupSize = 8;
    constexpr UINT kRootConstantCount = 8;

    constexpr UINT kFlagAmbientOcclusion = 0x1u;
    constexpr UINT kFlagContactShadows   = 0x2u;
    constexpr UINT kFlagTemporal         = 0x4u;
    constexpr UINT kFlagMaterialResponse = 0x8u;
    constexpr UINT kFlagMicroSpecular    = 0x10u;

    UINT Groups(UINT size)
    {
        return ((std::max)(size, 1u) + kGroupSize - 1) / kGroupSize;
    }

    UINT HalfOf(UINT size)
    {
        return (std::max)((size + 1) / 2, 1u);
    }

    XMFLOAT4X4 Transposed(const XMFLOAT4X4& matrix)
    {
        XMFLOAT4X4 result;
        XMStoreFloat4x4(&result, XMMatrixTranspose(XMLoadFloat4x4(&matrix)));
        return result;
    }
}

bool DpleRenderer::EnsureInitialized()
{
    if (mInitFailed)
        return false;
    if (mPipelinesCreated && mDescriptorsAllocated)
        return true;

    try
    {
        if (!mPipelinesCreated)
        {
            //                 pass                    shader                      SRV tables
            if (!CreatePass(mPreparePass,          L"Dple_Prepare.hlsl",       4)) throw std::runtime_error(mLastError);
            if (!CreatePass(mAmbientOcclusionPass, L"Dple_MultiscaleAO.hlsl",  5)) throw std::runtime_error(mLastError);
            if (!CreatePass(mContactShadowPass,    L"Dple_ContactShadow.hlsl", 5)) throw std::runtime_error(mLastError);
            if (!CreatePass(mTemporalPass,         L"Dple_Temporal.hlsl",      9)) throw std::runtime_error(mLastError);
            if (!CreatePass(mDenoisePass,          L"Dple_Denoise.hlsl",       6)) throw std::runtime_error(mLastError);
            if (!CreatePass(mCompositePass,        L"Dple_Composite.hlsl",     7)) throw std::runtime_error(mLastError);
            if (!CreatePass(mDownsamplePass,       L"Dple_Downsample.hlsl",    5)) throw std::runtime_error(mLastError);
            if (!CreatePass(mDetailPass,           L"Dple_Detail.hlsl",        8)) throw std::runtime_error(mLastError);

            ID3D12Device* device = DX12Context_GetDevice();
            D3D12_HEAP_PROPERTIES uploadHeap{};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            const CD3DX12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(kCbStride) * kFramesInFlight);
            DX12_THROW_IF_FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&mConstantBuffer)));
            DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCb)));

            mPipelinesCreated = true;
        }

        if (!AllocateDescriptors())
            throw std::runtime_error(mLastError);
        return true;
    }
    catch (const std::exception& ex)
    {
        // CreatePass and AllocateDescriptors rethrow their own, already-prefixed message.
        if (mLastError != ex.what())
            mLastError = std::string("DpleRenderer: ") + ex.what();
        mInitFailed = true;
        return false;
    }
}

bool DpleRenderer::CreatePass(Pass& pass, const wchar_t* shaderFile, UINT srvTables)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "DpleRenderer: null device.";
        return false;
    }

    const ShaderCompileRequest request
    {
        std::wstring(L"Shaders\\") + shaderFile,
        L"CSMain",
        L"cs_5_0",
        ShaderStage::Compute
    };

    if (!pass.Shader.Compile(request))
    {
        mLastError = "DpleRenderer: failed to compile ";
        for (const wchar_t* c = shaderFile; *c; ++c)
            mLastError += static_cast<char>(*c);
        mLastError += ": ";
        mLastError += pass.Shader.GetLastErrorMessage() ? pass.Shader.GetLastErrorMessage() : "unknown";
        return false;
    }

    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(srvTables + 1);
    std::vector<D3D12_ROOT_PARAMETER> params;
    params.reserve(3 + srvTables);

    D3D12_ROOT_PARAMETER cbv{};
    cbv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cbv.Descriptor.ShaderRegister = 0;
    cbv.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params.push_back(cbv);

    D3D12_ROOT_PARAMETER constants{};
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants.ShaderRegister = 1;
    constants.Constants.Num32BitValues = kRootConstantCount;
    constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params.push_back(constants);

    for (UINT i = 0; i <= srvTables; ++i)
    {
        const bool isUav = (i == srvTables);
        D3D12_DESCRIPTOR_RANGE& range = ranges[i];
        range.RangeType = isUav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = isUav ? 0 : i;
        range.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(param);
    }

    // Clamp matters for the history and upsample taps: a lookup just off screen must not
    // wrap round and fetch the opposite edge.
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (UINT i = 0; i < 2; ++i)
    {
        samplers[i].Filter = (i == 0) ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(params.size());
    rsDesc.pParameters = params.data();
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "DpleRenderer: root signature serialization failed";
        if (errors)
            mLastError += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    }
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&pass.RootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = pass.RootSignature.Get();
    psoDesc.CS = pass.Shader.GetBytecode();
    DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pass.Pipeline)));

    pass.SrvTables = srvTables;
    return true;
}

bool DpleRenderer::AllocateDescriptors()
{
    // The shared heap is a bump allocator that never frees: allocate once, and let resizes
    // rewrite the same slots.
    if (mDescriptorsAllocated)
        return true;

    auto allocate = [this](D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
    {
        if (!DX12Context_AllocateSrvDescriptor(&cpu, &gpu))
        {
            mLastError = "DpleRenderer: the shared descriptor heap is full.";
            return false;
        }
        return true;
    };

    auto allocateTexture = [&](Texture& texture, bool needsSrv)
    {
        if (needsSrv && !allocate(texture.SrvCpu, texture.Srv))
            return false;
        return allocate(texture.UavCpu, texture.Uav);
    };

    for (UINT i = 0; i < 2; ++i)
    {
        if (!allocateTexture(mGuide[i], true) || !allocateTexture(mOcclusion[i], true))
            return false;
    }
    for (Texture* texture : { &mAmbientOcclusion, &mContactShadow, &mDenoiseTemp, &mDenoised, &mComposite })
    {
        if (!allocateTexture(*texture, true))
            return false;
    }
    if (!allocateTexture(mDetail, false))
        return false;
    for (Texture& level : mDetailLevels)
    {
        if (!allocateTexture(level, true))
            return false;
    }
    for (UINT i = 0; i < kFramesInFlight; ++i)
    {
        if (!allocate(mImageSrvCpu[i], mImageSrv[i]))
            return false;
    }

    mDescriptorsAllocated = true;
    return true;
}

void DpleRenderer::CreateTexture(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name)
{
    ID3D12Device* device = DX12Context_GetDevice();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

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

    texture.Resource.Reset();
    texture.State = SrvState;
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, texture.State, nullptr, IID_PPV_ARGS(&texture.Resource)));
    texture.Resource->SetName(name);
    texture.Width = width;
    texture.Height = height;

    if (texture.SrvCpu.ptr != 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture.Resource.Get(), &srvDesc, texture.SrvCpu);
    }

    if (texture.UavCpu.ptr != 0)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(texture.Resource.Get(), nullptr, &uavDesc, texture.UavCpu);
    }
}

bool DpleRenderer::EnsureSize(UINT outputWidth, UINT outputHeight, UINT workWidth, UINT workHeight)
{
    const bool outputChanged = outputWidth != mOutputWidth || outputHeight != mOutputHeight;
    const bool workChanged = workWidth != mWorkWidth || workHeight != mWorkHeight;
    if (!outputChanged && !workChanged)
        return true;

    try
    {
        // Frames still in flight may be reading the textures being replaced.
        DX12Context_WaitForGPU();

        if (workChanged)
        {
            for (UINT i = 0; i < 2; ++i)
            {
                CreateTexture(mGuide[i], workWidth, workHeight, GuideFormat, L"DPLE Guide");
                CreateTexture(mOcclusion[i], workWidth, workHeight, OcclusionFormat, L"DPLE Occlusion");
            }
            CreateTexture(mAmbientOcclusion, workWidth, workHeight, OcclusionFormat, L"DPLE Multiscale AO");
            CreateTexture(mContactShadow, workWidth, workHeight, ContactShadowFormat, L"DPLE Contact Shadow");
            CreateTexture(mDenoiseTemp, workWidth, workHeight, OcclusionFormat, L"DPLE Denoise H");
            CreateTexture(mDenoised, workWidth, workHeight, OcclusionFormat, L"DPLE Denoise V");
            mWorkWidth = workWidth;
            mWorkHeight = workHeight;
            // History is at the old resolution; reprojecting it would sample garbage.
            mHistoryValid = false;
        }

        if (outputChanged)
        {
            CreateTexture(mComposite, outputWidth, outputHeight, ImageFormat, L"DPLE Composite");
            CreateTexture(mDetail, outputWidth, outputHeight, ImageFormat, L"DPLE Detail");
            UINT levelWidth = outputWidth;
            UINT levelHeight = outputHeight;
            for (Texture& level : mDetailLevels)
            {
                levelWidth = HalfOf(levelWidth);
                levelHeight = HalfOf(levelHeight);
                CreateTexture(level, levelWidth, levelHeight, ImageFormat, L"DPLE Detail Level");
            }
            mOutputWidth = outputWidth;
            mOutputHeight = outputHeight;
        }

        mPendingBarriers.clear();
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("DpleRenderer: could not create textures: ") + ex.what();
        mInitFailed = true;
        return false;
    }
}

void DpleRenderer::Require(Texture& texture, D3D12_RESOURCE_STATES state)
{
    if (texture.State == state)
        return;
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(texture.Resource.Get(), texture.State, state));
    texture.State = state;
}

void DpleRenderer::FlushBarriers(ID3D12GraphicsCommandList* commandList)
{
    // Every flush sits between two passes; one global UAV barrier covers any pass that
    // keeps writing the same target as the one before it.
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(nullptr));
    commandList->ResourceBarrier(static_cast<UINT>(mPendingBarriers.size()), mPendingBarriers.data());
    mPendingBarriers.clear();
}

void DpleRenderer::Bind(
    ID3D12GraphicsCommandList* commandList,
    const Pass& pass,
    D3D12_GPU_VIRTUAL_ADDRESS constants,
    const PassConstants& passConstants,
    std::initializer_list<D3D12_GPU_DESCRIPTOR_HANDLE> srvs,
    D3D12_GPU_DESCRIPTOR_HANDLE uav)
{
    static_assert(sizeof(PassConstants) == kRootConstantCount * sizeof(UINT), "root constant count mismatch");

    commandList->SetComputeRootSignature(pass.RootSignature.Get());
    commandList->SetPipelineState(pass.Pipeline.Get());

    UINT parameter = 0;
    commandList->SetComputeRootConstantBufferView(parameter++, constants);
    commandList->SetComputeRoot32BitConstants(parameter++, kRootConstantCount, &passConstants, 0);
    for (const D3D12_GPU_DESCRIPTOR_HANDLE& srv : srvs)
        commandList->SetComputeRootDescriptorTable(parameter++, srv);
    commandList->SetComputeRootDescriptorTable(parameter, uav);
}

float DpleRenderer::ComputeDetailMotionScale(const DpleSettings& settings, const FrameInputs& inputs) const
{
    const float suppression = std::clamp(settings.DetailMotionSuppression, 0.0f, 1.0f);
    if (suppression <= 0.0f || !mHasPreviousCamera || inputs.DeltaSeconds <= 1.0e-6f)
        return 1.0f;

    const XMVECTOR previous = XMVector3Normalize(XMLoadFloat3(&mPrevCameraForward));
    const XMVECTOR current = XMVector3Normalize(XMLoadFloat3(&inputs.CameraForward));
    const float cosAngle = std::clamp(XMVectorGetX(XMVector3Dot(previous, current)), -1.0f, 1.0f);

    // Degrees per second, so the effect does not come and go with frame rate. Full
    // suppression at a brisk 60 deg/s pan. Camera translation is deliberately left out:
    // its screen-space effect depends on depth, which is exactly what splits the frame.
    const float angularSpeed = XMConvertToDegrees(std::acos(cosAngle)) / inputs.DeltaSeconds;
    const float motion = std::clamp(angularSpeed / 60.0f, 0.0f, 1.0f);
    return 1.0f - motion * suppression;
}

bool DpleRenderer::Dispatch(ID3D12GraphicsCommandList* commandList, const DpleSettings& settings, const FrameInputs& inputs)
{
    if (commandList == nullptr || inputs.Image == nullptr)
        return false;

    // Every input must be bound; a missing one would leave a stale descriptor in a table.
    if (inputs.DepthSrv.ptr == 0 || inputs.NormalSrv.ptr == 0 || inputs.MaterialSrv.ptr == 0
        || inputs.AlbedoSrv.ptr == 0 || inputs.GBufferWidth == 0 || inputs.GBufferHeight == 0)
        return false;

    const D3D12_RESOURCE_DESC imageDesc = inputs.Image->GetDesc();
    if (imageDesc.Format != ImageFormat || imageDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || imageDesc.SampleDesc.Count != 1)
    {
        mLastError = "DpleRenderer: the image to enhance is not a single-sampled R16G16B16A16_FLOAT texture.";
        return false;
    }

    if (!EnsureInitialized())
        return false;

    const UINT outputWidth = static_cast<UINT>(imageDesc.Width);
    const UINT outputHeight = imageDesc.Height;
    const int divisor = std::clamp(settings.WorkingResolutionDivisor, 1, 4);
    const UINT workWidth = (std::max)((outputWidth + divisor - 1) / divisor, 1u);
    const UINT workHeight = (std::max)((outputHeight + divisor - 1) / divisor, 1u);

    if (!EnsureSize(outputWidth, outputHeight, workWidth, workHeight))
        return false;

    const UINT cur = mBufferIndex;
    const UINT prev = 1 - mBufferIndex;

    const int debugView = std::clamp(settings.DebugView, 0, static_cast<int>(DpleDebugView::Count) - 1);

    // A camera that jumped - a cut, a teleport, a level load - leaves history describing
    // another place. Disocclusion would catch most of it, but not all.
    bool cameraCut = !mHasPreviousCamera;
    if (mHasPreviousCamera)
    {
        const XMVECTOR forwardDot = XMVector3Dot(
            XMVector3Normalize(XMLoadFloat3(&mPrevCameraForward)), XMVector3Normalize(XMLoadFloat3(&inputs.CameraForward)));
        const XMVECTOR moved = XMVector3Length(XMVectorSubtract(XMLoadFloat3(&inputs.CameraPosition), XMLoadFloat3(&mPrevCameraPosition)));
        cameraCut = XMVectorGetX(forwardDot) < 0.85f || XMVectorGetX(moved) > 5.0f;
    }

    const bool temporal = settings.EnableTemporalReconstruction;
    const bool historyUsable = temporal && mHistoryValid && !cameraCut;
    const int denoiseRadius = settings.EnableSpatialDenoise ? std::clamp(settings.SpatialDenoiseRadius, 0, 4) : 0;
    // The detail pass multiplies whatever is on screen, which would include a debug view.
    const bool detail = settings.EnableDetailEnhancement && debugView == 0
        && (settings.FineDetailStrength > 1.0e-4f || settings.StructureStrength > 1.0e-4f);

    // ---------------------------------------------------------------- constants
    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;
    ++mFrameIndex;
    {
        DpleCbData cb{};

        const XMMATRIX viewProjection = XMLoadFloat4x4(&inputs.ViewProjection);
        XMFLOAT4X4 inverseViewProjection;
        XMStoreFloat4x4(&inverseViewProjection, XMMatrixInverse(nullptr, viewProjection));

        cb.InvViewProj = Transposed(inverseViewProjection);
        cb.ViewProj = Transposed(inputs.ViewProjection);
        cb.ViewProjNoJitter = Transposed(inputs.ViewProjectionNoJitter);
        cb.PrevViewProjNoJitter = Transposed(mHasPreviousCamera ? mPrevViewProjNoJitter : inputs.ViewProjectionNoJitter);

        XMStoreFloat3(&cb.CameraForward, XMVector3Normalize(XMLoadFloat3(&inputs.CameraForward)));
        cb.CameraPos = inputs.CameraPosition;
        XMStoreFloat3(&cb.SunDirection, XMVector3Normalize(XMLoadFloat3(&inputs.SunDirection)));
        cb.SunWeight = std::clamp(inputs.SunWeight, 0.0f, 1.0f);
        cb.ProjScaleX = inputs.ProjectionNoJitter._11;
        cb.ProjScaleY = inputs.ProjectionNoJitter._22;

        const float gbufferWidth = static_cast<float>(inputs.GBufferWidth);
        const float gbufferHeight = static_cast<float>(inputs.GBufferHeight);
        // The jitter shifts NDC by (2x/w, 2y/h) with +Y up, i.e. UV by (x/w, -y/h). Only an
        // image that has had it resolved out needs it put back to find its G-Buffer texel.
        cb.JitterUV = inputs.ImageIsJitterResolved
            ? XMFLOAT2(inputs.JitterX / gbufferWidth, -inputs.JitterY / gbufferHeight)
            : XMFLOAT2(0.0f, 0.0f);
        cb.GBufferSize = XMFLOAT2(gbufferWidth, gbufferHeight);
        cb.InvGBufferSize = XMFLOAT2(1.0f / gbufferWidth, 1.0f / gbufferHeight);
        cb.OutputSize = XMFLOAT2(static_cast<float>(outputWidth), static_cast<float>(outputHeight));
        cb.InvOutputSize = XMFLOAT2(1.0f / outputWidth, 1.0f / outputHeight);
        cb.InvWorkSize = XMFLOAT2(1.0f / workWidth, 1.0f / workHeight);
        cb.WorkSizeX = workWidth;
        cb.WorkSizeY = workHeight;
        cb.FrameIndex = mFrameIndex;
        cb.DebugView = static_cast<UINT>(debugView);

        cb.Flags = (settings.EnableAmbientOcclusion ? kFlagAmbientOcclusion : 0u)
                 | (settings.EnableContactShadows ? kFlagContactShadows : 0u)
                 | (temporal ? kFlagTemporal : 0u)
                 | (settings.EnableMaterialResponse ? kFlagMaterialResponse : 0u)
                 | (settings.EnableMicroSpecular ? kFlagMicroSpecular : 0u);
        cb.HistoryValid = historyUsable ? 1u : 0u;
        cb.ContactShadowSteps = static_cast<UINT>(std::clamp(settings.ContactShadowSteps, 4, 32));
        cb.DenoiseRadius = denoiseRadius;

        // Radii are ordered rather than trusted: micro larger than contact would make the
        // bands overlap and the intensities stop meaning what their names say.
        cb.MicroRadius = (std::max)(settings.MicroAORadius, 0.005f);
        cb.ContactRadius = (std::max)(settings.ContactAORadius, cb.MicroRadius * 1.5f);
        cb.BroadRadius = (std::max)(settings.BroadAORadius, cb.ContactRadius * 1.5f);
        cb.AOBias = (std::max)(settings.AOBias, 0.0f);
        cb.AOPower = (std::max)(settings.AOPower, 0.05f);
        cb.MicroAOIntensity = (std::max)(settings.MicroAOIntensity, 0.0f);
        cb.ContactAOIntensity = (std::max)(settings.ContactAOIntensity, 0.0f);
        cb.BroadAOIntensity = (std::max)(settings.BroadAOIntensity, 0.0f);
        cb.MaxCombinedAO = std::clamp(settings.MaxCombinedAO, 0.0f, 1.0f);

        cb.ContactShadowLength = (std::max)(settings.ContactShadowLength, 0.01f);
        cb.ContactShadowThickness = (std::max)(settings.ContactShadowThickness, 0.001f);
        cb.ContactShadowIntensity = std::clamp(settings.ContactShadowIntensity, 0.0f, 1.0f);

        cb.SkinAOScale = (std::max)(settings.SkinAOScale, 0.0f);
        cb.SkinWarmth = std::clamp(settings.SkinWarmth, 0.0f, 1.0f);
        cb.FoliageAOScale = (std::max)(settings.FoliageAOScale, 0.0f);
        cb.FoliageSaturation = std::clamp(settings.FoliageSaturation, 0.0f, 1.0f);
        cb.WetRoughnessThreshold = std::clamp(settings.WetRoughnessThreshold, 0.0f, 0.5f);
        cb.WetResponseStrength = std::clamp(settings.WetResponseStrength, 0.0f, 1.0f);

        cb.SpecularOcclusionStrength = std::clamp(settings.SpecularOcclusionStrength, 0.0f, 1.0f);
        cb.MicroSpecularStrength = (std::max)(settings.MicroSpecularStrength, 0.0f);
        cb.MicroSpecularRoughnessMax = std::clamp(settings.MicroSpecularRoughnessMax, 0.05f, 1.0f);
        cb.MicroSpecularDetailScale = std::clamp(settings.MicroSpecularDetailScale, 0.5f, 4.0f);

        // Min must not exceed max, or the lerp between them runs backwards and brightly lit
        // surfaces get treated as the most indirect thing on screen.
        cb.IndirectFractionMin = std::clamp(settings.IndirectFractionMin, 0.0f, 1.0f);
        cb.IndirectFractionMax = std::clamp(settings.IndirectFractionMax, cb.IndirectFractionMin, 1.0f);
        cb.DirectLightWeight = std::clamp(settings.DirectLightWeight, 0.0f, 1.0f);

        // Moving must never be allowed above stable: that makes history stickiest exactly
        // where reprojection is least reliable - the recipe for ghost trails.
        cb.HistoryWeightStable = std::clamp(settings.HistoryWeightStable, 0.0f, 0.98f);
        cb.HistoryWeightMoving = std::clamp(settings.HistoryWeightMoving, 0.0f, cb.HistoryWeightStable);
        cb.DisocclusionDepthTolerance = std::clamp(settings.DisocclusionDepthTolerance, 0.005f, 0.5f);
        cb.NeighborhoodClampScale = std::clamp(settings.NeighborhoodClampScale, 0.5f, 4.0f);
        cb.DenoiseDepthTolerance = cb.DisocclusionDepthTolerance;

        cb.FineDetailStrength = std::clamp(settings.FineDetailStrength, 0.0f, 1.0f);
        cb.StructureStrength = std::clamp(settings.StructureStrength, 0.0f, 1.0f);
        cb.MaxLuminanceChange = std::clamp(settings.MaxLuminanceChange, 0.0f, 1.0f);
        cb.DetailMotionScale = ComputeDetailMotionScale(settings, inputs);

        // Offsets as dxc reports them for DpleConstants; a mismatch shifts every value after it.
        static_assert(offsetof(DpleCbData, CameraPos) == 256, "DpleCbData out of step with Dple_Common.hlsli");
        static_assert(offsetof(DpleCbData, WorkSizeX) == 352, "DpleCbData out of step with Dple_Common.hlsli");
        static_assert(offsetof(DpleCbData, MicroRadius) == 384, "DpleCbData out of step with Dple_Common.hlsli");
        static_assert(offsetof(DpleCbData, MaxLuminanceChange) == 512, "DpleCbData out of step with Dple_Common.hlsli");
        static_assert(sizeof(DpleCbData) == 528, "DpleCbData out of step with Dple_Common.hlsli");
        std::memcpy(mMappedCb + static_cast<size_t>(mFrameSlot) * kCbStride, &cb, sizeof(cb));
    }
    const D3D12_GPU_VIRTUAL_ADDRESS constants =
        mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(mFrameSlot) * kCbStride;

    // The image's SRV is rewritten every frame (the image changes with the AA mode), into
    // this frame's own slot so a frame still in flight keeps the view it was recorded with.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = ImageFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        DX12Context_GetDevice()->CreateShaderResourceView(inputs.Image, &srvDesc, mImageSrvCpu[mFrameSlot]);
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE imageSrv = mImageSrv[mFrameSlot];

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    const PassConstants noPassConstants{};
    const D3D12_GPU_DESCRIPTOR_HANDLE depth = inputs.DepthSrv;
    const D3D12_GPU_DESCRIPTOR_HANDLE normal = inputs.NormalSrv;
    const D3D12_GPU_DESCRIPTOR_HANDLE material = inputs.MaterialSrv;
    const D3D12_GPU_DESCRIPTOR_HANDLE albedo = inputs.AlbedoSrv;

    // The image is only read from compute until the copy-back at the end.
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
        inputs.Image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, SrvState));

    // ---------------------------------------------------------------- prepare
    Require(mGuide[cur], UavState);
    FlushBarriers(commandList);
    Bind(commandList, mPreparePass, constants, noPassConstants, { depth, normal, material, albedo }, mGuide[cur].Uav);
    commandList->Dispatch(Groups(workWidth), Groups(workHeight), 1);

    // ---------------------------------------------------------------- AO + contact shadows
    // Both run whether or not they are enabled: disabled, they write "fully visible", which
    // costs less than the branches downstream would.
    Require(mGuide[cur], SrvState);
    Require(mAmbientOcclusion, UavState);
    Require(mContactShadow, UavState);
    FlushBarriers(commandList);

    Bind(commandList, mAmbientOcclusionPass, constants, noPassConstants,
         { depth, normal, material, albedo, mGuide[cur].Srv }, mAmbientOcclusion.Uav);
    commandList->Dispatch(Groups(workWidth), Groups(workHeight), 1);

    Bind(commandList, mContactShadowPass, constants, noPassConstants,
         { depth, normal, material, albedo, mGuide[cur].Srv }, mContactShadow.Uav);
    commandList->Dispatch(Groups(workWidth), Groups(workHeight), 1);

    // ---------------------------------------------------------------- temporal + pack
    // Last frame's halves are bound even when history is not used - the shader ignores
    // them then, but a table must never hold a stale descriptor.
    Require(mAmbientOcclusion, SrvState);
    Require(mContactShadow, SrvState);
    Require(mOcclusion[prev], SrvState);
    Require(mGuide[prev], SrvState);
    Require(mOcclusion[cur], UavState);
    FlushBarriers(commandList);

    Bind(commandList, mTemporalPass, constants, noPassConstants,
         { depth, normal, material, albedo, mAmbientOcclusion.Srv, mContactShadow.Srv, mGuide[cur].Srv,
           mOcclusion[prev].Srv, mGuide[prev].Srv },
         mOcclusion[cur].Uav);
    commandList->Dispatch(Groups(workWidth), Groups(workHeight), 1);

    // ---------------------------------------------------------------- spatial denoise
    // Reads the temporal result but never writes it: what accumulates frame to frame must
    // stay unfiltered, or the blur compounds into its own history.
    Texture* occlusionForComposite = &mOcclusion[cur];
    Require(mOcclusion[cur], SrvState);
    if (denoiseRadius > 0)
    {
        PassConstants horizontal{};
        horizontal.StepDirectionX = 1;
        PassConstants vertical{};
        vertical.StepDirectionY = 1;

        Require(mDenoiseTemp, UavState);
        FlushBarriers(commandList);
        Bind(commandList, mDenoisePass, constants, horizontal,
             { depth, normal, material, albedo, mOcclusion[cur].Srv, mGuide[cur].Srv }, mDenoiseTemp.Uav);
        commandList->Dispatch(Groups(workWidth), Groups(workHeight), 1);

        Require(mDenoiseTemp, SrvState);
        Require(mDenoised, UavState);
        FlushBarriers(commandList);
        Bind(commandList, mDenoisePass, constants, vertical,
             { depth, normal, material, albedo, mDenoiseTemp.Srv, mGuide[cur].Srv }, mDenoised.Uav);
        commandList->Dispatch(Groups(workWidth), Groups(workHeight), 1);

        Require(mDenoised, SrvState);
        occlusionForComposite = &mDenoised;
    }

    // ---------------------------------------------------------------- composite
    Require(mComposite, UavState);
    FlushBarriers(commandList);
    Bind(commandList, mCompositePass, constants, noPassConstants,
         { depth, normal, material, albedo, imageSrv, occlusionForComposite->Srv, mGuide[cur].Srv },
         mComposite.Uav);
    commandList->Dispatch(Groups(outputWidth), Groups(outputHeight), 1);

    Texture* result = &mComposite;

    // ---------------------------------------------------------------- detail enhancement
    if (detail)
    {
        // The bands come from the image as it arrived, which is still untouched in the
        // caller's texture: DPLE has only written its own targets so far.
        D3D12_GPU_DESCRIPTOR_HANDLE source = imageSrv;
        UINT sourceWidth = outputWidth;
        UINT sourceHeight = outputHeight;
        for (Texture& level : mDetailLevels)
        {
            Require(level, UavState);
            FlushBarriers(commandList);

            PassConstants downsample{};
            downsample.DestWidth = level.Width;
            downsample.DestHeight = level.Height;
            downsample.SourceWidth = static_cast<float>(sourceWidth);
            downsample.SourceHeight = static_cast<float>(sourceHeight);
            downsample.SourceInvWidth = 1.0f / static_cast<float>(sourceWidth);
            downsample.SourceInvHeight = 1.0f / static_cast<float>(sourceHeight);

            Bind(commandList, mDownsamplePass, constants, downsample,
                 { depth, normal, material, albedo, source }, level.Uav);
            commandList->Dispatch(Groups(level.Width), Groups(level.Height), 1);

            Require(level, SrvState);
            source = level.Srv;
            sourceWidth = level.Width;
            sourceHeight = level.Height;
        }

        Require(mComposite, SrvState);
        Require(mDetail, UavState);
        FlushBarriers(commandList);

        // Quarter resolution carries the fine band, sixteenth the structural one. The half
        // level is almost identical to full resolution, so a band taken from it would be
        // pure sampling noise.
        Bind(commandList, mDetailPass, constants, noPassConstants,
             { depth, normal, material, albedo, mComposite.Srv, imageSrv, mDetailLevels[1].Srv, mDetailLevels[3].Srv },
             mDetail.Uav);
        commandList->Dispatch(Groups(outputWidth), Groups(outputHeight), 1);

        result = &mDetail;
    }

    // ---------------------------------------------------------------- copy back
    Require(*result, D3D12_RESOURCE_STATE_COPY_SOURCE);
    mPendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
        inputs.Image, SrvState, D3D12_RESOURCE_STATE_COPY_DEST));
    FlushBarriers(commandList);

    commandList->CopyResource(inputs.Image, result->Resource.Get());

    const auto restoreImage = CD3DX12_RESOURCE_BARRIER::Transition(
        inputs.Image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &restoreImage);

    // ---------------------------------------------------------------- history
    if (temporal)
    {
        mHistoryValid = true;
        mBufferIndex = prev;
    }
    else
    {
        mHistoryValid = false;
    }

    mPrevViewProjNoJitter = inputs.ViewProjectionNoJitter;
    mPrevCameraPosition = inputs.CameraPosition;
    mPrevCameraForward = inputs.CameraForward;
    mHasPreviousCamera = true;
    return true;
}

void DpleRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }
    mConstantBuffer.Reset();

    for (Pass* pass : { &mPreparePass, &mAmbientOcclusionPass, &mContactShadowPass, &mTemporalPass,
                        &mDenoisePass, &mCompositePass, &mDownsamplePass, &mDetailPass })
    {
        pass->Pipeline.Reset();
        pass->RootSignature.Reset();
    }

    for (Texture* texture : { &mGuide[0], &mGuide[1], &mOcclusion[0], &mOcclusion[1], &mAmbientOcclusion,
                              &mContactShadow, &mDenoiseTemp, &mDenoised, &mComposite, &mDetail })
    {
        texture->Resource.Reset();
    }
    for (Texture& level : mDetailLevels)
        level.Resource.Reset();

    // Descriptor slots stay allocated: the shared heap never frees, so a later dispatch
    // rewrites the same slots rather than leaking new ones.
    mPendingBarriers.clear();
    mPipelinesCreated = false;
    mInitFailed = false;
    mOutputWidth = mOutputHeight = 0;
    mWorkWidth = mWorkHeight = 0;
    InvalidateHistory();
}
