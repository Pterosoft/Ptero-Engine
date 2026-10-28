// PathTracer.cpp
// Path tracer, reference and real time. See PathTracer.h for each mode's frame
// sequence and PathTracer_Common.hlsli for the integrator.

#include "pch.h"
#include "PathTracer.h"
#include "TimeOfDaySettings.h"   // kSunAngularRadiusRad

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

static const wchar_t* kReferenceShader = L"Data/Shaders/PathTracer.hlsl";
static const wchar_t* kRealTimeShader  = L"Data/Shaders/PathTracer_RealTime.hlsl";
static const wchar_t* kCompositeShader = L"Data/Shaders/PathTracer_Composite.hlsl";

namespace
{
    constexpr UINT        kGroupSize          = 8;
    constexpr DXGI_FORMAT kAccumulationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    // Must match the scene colour target, which the background and output are
    // copied from and to.
    constexpr DXGI_FORMAT kSceneColorFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kRadianceFormat     = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Mirrors PT_FLAG_* in PathTracer_Common.hlsli.
    constexpr uint32_t kFlagAlphaTest   = 1u;
    constexpr uint32_t kFlagSoftShadows = 2u;
    constexpr uint32_t kFlagTrace       = 4u;
    constexpr uint32_t kFlagOneLight    = 8u;
    constexpr uint32_t kFlagDenoised    = 16u;
    constexpr uint32_t kFlagScreenCache = 32u;
    constexpr uint32_t kFlagTerrainShadows = 64u;
    constexpr uint32_t kFlagDisneyBrdf  = 128u;
    constexpr uint32_t kFlagSubsurface  = 256u;
    constexpr uint32_t kFlagSubsurfaceMask = 512u;
    constexpr uint32_t kFlagVolumetricFog = 1024u;

    // Root parameters: one table per binding, so each can come from wherever its
    // owner keeps it. Every pass binds against this one signature.
    enum : UINT
    {
        kRootConstants = 0,
        kRootTlas,              // t0
        kRootVertices,          // t1
        kRootIndices,           // t2
        kRootInstanceInfo,      // t3
        kRootMaterialRanges,    // t4
        kRootMaterialSurfaces,  // t5
        kRootBackground,        // t6
        kRootTerrainTrace,      // t7
        kRootTextures,          // t0..t1023, space 1
        kRootGBufferAlbedo,     // t39
        kRootGBufferNormal,     // t40
        kRootGBufferMaterial,   // t41
        kRootDenoisedDiffuse,   // t43
        kRootDenoisedSpecular,  // t44
        kRootPrevDiffuse,       // t45
        kRootPrevViewZ,         // t46
        kRootGBufferPrincipled, // t47..t48
        kRootVolumetricFog,     // t49
        kRootAccumulation,      // u0
        kRootOutput,            // u1
        kRootDiffuseIn,         // u2
        kRootSpecularIn,        // u3
        kRootNormalRoughness,   // u4
        kRootViewZ,             // u5
        kRootMotion,            // u6
        kRootParameterCount
    };
    constexpr UINT kMaterialTextureCount = 1024;   // RtGlobalIllumination::kMaxRtSurfaceTextures

    bool AllocDesc(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu, std::string& err)
    {
        if (!DX12Context_AllocateSrvDescriptor(&cpu, &gpu))
        {
            err = "PathTracer: ran out of SRV descriptor heap slots.";
            return false;
        }
        return true;
    }

    ComPtr<ID3D12Resource> MakeTexture2D(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT format,
                                         D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, LPCWSTR name)
    {
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = flags;
        ComPtr<ID3D12Resource> res;
        DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)));
        res->SetName(name);
        return res;
    }

    bool CompilePipeline(ID3D12Device* dev, ID3D12RootSignature* rootSignature, const wchar_t* file,
                         ComPtr<ID3D12PipelineState>& out, const wchar_t* name, std::string& err)
    {
        ShaderCompileRequest req{};
        req.FilePath = file;
        req.EntryPoint = L"CSMain";
        req.TargetProfile = L"cs_6_5";
        req.Stage = ShaderStage::Compute;

        DX12Shader shader;
        if (!shader.Compile(req))
        {
            err = std::string("PathTracer: shader compile failed ('")
                + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "?") + "')";
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSignature;
        psoDesc.CS = shader.GetBytecode();
        if (FAILED(dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&out))))
        {
            err = "PathTracer: CreateComputePipelineState failed.";
            return false;
        }
        out->SetName(name);
        return true;
    }

    void StoreTransposed(float (&dst)[16], DirectX::FXMMATRIX m)
    {
        DirectX::XMFLOAT4X4 transposed;
        DirectX::XMStoreFloat4x4(&transposed, DirectX::XMMatrixTranspose(m));
        std::memcpy(dst, &transposed, sizeof(dst));
    }

    // FNV-1a, fed field by field.
    struct SignatureHasher
    {
        std::uint64_t Value = 1469598103934665603ull;

        void Bytes(const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                Value ^= bytes[i];
                Value *= 1099511628211ull;
            }
        }

        template <typename T>
        void Add(const T& value) { Bytes(&value, sizeof(T)); }
    };
}

bool PathTracer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();
    try
    {
        if (!mRootSignature && !CreateRootSignature()) { mInitFailed = true; return false; }
        if (!mPipeline && !CreatePipelines())          { mInitFailed = true; return false; }

        if (!mConstantBuffer)
        {
            ID3D12Device* dev = DX12Context_GetDevice();
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
            const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(kConstantStride * kFramesInFlight);
            DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&mConstantBuffer)));
            mConstantBuffer->SetName(L"PathTracer_Constants");
            void* mapped = nullptr;
            DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mapped));
            mMappedConstants = static_cast<uint8_t*>(mapped);
        }

        if (!mIsInitialized || width != mWidth || height != mHeight)
        {
            mWidth = width;
            mHeight = height;
            if (!CreateResolutionResources()) { mInitFailed = true; return false; }
            mSampleCount = 0;
        }

        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("PathTracer::Initialize: ") + ex.what();
        mInitFailed = true;
        return false;
    }
}

bool PathTracer::EnsureSize(UINT width, UINT height)
{
    if (mIsInitialized && width == mWidth && height == mHeight)
        return true;
    return Initialize(width, height);
}

void PathTracer::Shutdown()
{
    mDenoiser.Shutdown();
    if (mConstantBuffer && mMappedConstants)
        mConstantBuffer->Unmap(0, nullptr);
    mMappedConstants = nullptr;
    mConstantBuffer.Reset();
    for (ComPtr<ID3D12Resource>* resource : {
            std::addressof(mAccumulation), std::addressof(mBackground), std::addressof(mOutput),
            std::addressof(mDiffuseIn), std::addressof(mSpecularIn), std::addressof(mNormalRoughness),
            std::addressof(mViewZ), std::addressof(mMotion), std::addressof(mDiffuseOut), std::addressof(mSpecularOut),
            std::addressof(mPrevViewZ) })
    {
        resource->Reset();
    }
    mScreenCacheValid = false;
    for (auto& retired : mRetired)
        retired.clear();
    mPipeline.Reset();
    mPipelineRealTime.Reset();
    mPipelineComposite.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
    mRealTimeReady = false;
    mRealTimeFailed = false;
    mSampleCount = 0;
    // The descriptor slots stay allocated: the shared heap cannot take them back,
    // and a later Initialize rewrites them in place.
}

bool PathTracer::CreateRootSignature()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev)
    {
        mLastError = "PathTracer: no D3D12 device.";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[kRootParameterCount]{};
    for (UINT i = kRootTlas; i <= kRootTerrainTrace; ++i)
        ranges[i] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i - kRootTlas, 0, 0 };   // t0..t7
    // The material textures are one range, which is how the shaders declare them, in
    // their own register space so its size cannot collide with anything else.
    ranges[kRootTextures]         = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kMaterialTextureCount, 0, 1, 0 };   // t0..t1023, space 1
    ranges[kRootGBufferAlbedo]    = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 39, 0, 0 };
    ranges[kRootGBufferNormal]    = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 40, 0, 0 };
    ranges[kRootGBufferMaterial]  = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 41, 0, 0 };
    ranges[kRootDenoisedDiffuse]  = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 43, 0, 0 };
    ranges[kRootDenoisedSpecular] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 44, 0, 0 };
    ranges[kRootPrevDiffuse]      = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 45, 0, 0 };
    ranges[kRootPrevViewZ]        = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 46, 0, 0 };
    ranges[kRootGBufferPrincipled] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, PrincipledGBuffer::kTargetCount, 47, 0, 0 };
    ranges[kRootVolumetricFog]    = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 49, 0, 0 };
    for (UINT i = kRootAccumulation; i < kRootParameterCount; ++i)
        ranges[i] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, i - kRootAccumulation, 0, 0 };   // u0..u6

    D3D12_ROOT_PARAMETER params[kRootParameterCount]{};
    params[kRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kRootConstants].Descriptor.ShaderRegister = 0;
    params[kRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = kRootTlas; i < kRootParameterCount; ++i)
    {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable = { 1, &ranges[i] };
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // The fog volume: clamped, or its far slice wraps onto its near one.
    D3D12_STATIC_SAMPLER_DESC clampSampler = sampler;
    clampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    clampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    clampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    clampSampler.ShaderRegister = 1;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = { sampler, clampSampler };

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = kRootParameterCount;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    rsDesc.pStaticSamplers = samplers;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialized, &errors)))
    {
        mLastError = "PathTracer: D3D12SerializeRootSignature failed";
        if (errors)
            mLastError += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                        IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "PathTracer: CreateRootSignature failed.";
        return false;
    }
    mRootSignature->SetName(L"PathTracer_RootSignature");
    return true;
}

bool PathTracer::CreatePipelines()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    return CompilePipeline(dev, mRootSignature.Get(), kReferenceShader, mPipeline, L"PathTracer_Reference", mLastError)
        && CompilePipeline(dev, mRootSignature.Get(), kRealTimeShader, mPipelineRealTime, L"PathTracer_RealTime", mLastError)
        && CompilePipeline(dev, mRootSignature.Get(), kCompositeShader, mPipelineComposite, L"PathTracer_Composite", mLastError);
}

bool PathTracer::CreateResolutionResources()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev)
        return false;

    // Frames already recorded may still name the old textures; they die when the
    // ring comes back round, as RtGlobalIllumination retires its buffers.
    for (ComPtr<ID3D12Resource>* resource : { std::addressof(mAccumulation), std::addressof(mBackground), std::addressof(mOutput) })
    {
        if (*resource)
            mRetired[mRetireSlot].push_back(std::move(*resource));
    }

    mAccumulation = MakeTexture2D(dev, mWidth, mHeight, kAccumulationFormat,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"PathTracer_Accumulation");
    mBackground = MakeTexture2D(dev, mWidth, mHeight, kSceneColorFormat,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"PathTracer_Background");
    mOutput = MakeTexture2D(dev, mWidth, mHeight, kSceneColorFormat,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE, L"PathTracer_Output");

    if (!mDescriptorsAllocated)
    {
        if (!AllocDesc(mAccumulationUavCpu, mAccumulationUavGpu, mLastError)) return false;
        if (!AllocDesc(mBackgroundSrvCpu, mBackgroundSrvGpu, mLastError)) return false;
        if (!AllocDesc(mOutputUavCpu, mOutputUavGpu, mLastError)) return false;
        mDescriptorsAllocated = true;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = kAccumulationFormat;
    dev->CreateUnorderedAccessView(mAccumulation.Get(), nullptr, &uav, mAccumulationUavCpu);
    uav.Format = kSceneColorFormat;
    dev->CreateUnorderedAccessView(mOutput.Get(), nullptr, &uav, mOutputUavCpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = kSceneColorFormat;
    srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(mBackground.Get(), &srv, mBackgroundSrvCpu);

    // The real-time targets follow the size, if they exist yet.
    if (mRealTimeReady)
    {
        mRealTimeReady = false;
        mRealTimeFailed = false;
        if (!EnsureRealTimeResources())
            return false;
    }
    return true;
}

bool PathTracer::EnsureRealTimeResources()
{
    if (mRealTimeReady)
        return true;
    if (mRealTimeFailed)
        return false;
    // Set again on success below; any early return leaves it latched.
    mRealTimeFailed = true;

    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev)
        return false;

    try
    {
        for (ComPtr<ID3D12Resource>* resource : {
                std::addressof(mDiffuseIn), std::addressof(mSpecularIn), std::addressof(mNormalRoughness),
                std::addressof(mViewZ), std::addressof(mMotion), std::addressof(mDiffuseOut), std::addressof(mSpecularOut),
                std::addressof(mPrevViewZ) })
        {
            if (*resource)
                mRetired[mRetireSlot].push_back(std::move(*resource));
        }
        mScreenCacheValid = false;

        constexpr D3D12_RESOURCE_FLAGS uavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr D3D12_RESOURCE_STATES uavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        mDiffuseIn       = MakeTexture2D(dev, mWidth, mHeight, kRadianceFormat, uavFlag, uavState, L"PathTracer_NrdDiffuseIn");
        mSpecularIn      = MakeTexture2D(dev, mWidth, mHeight, kRadianceFormat, uavFlag, uavState, L"PathTracer_NrdSpecularIn");
        // TYPELESS: NRD reads it as R10G10B10A2_UNORM, which has no typed UAV store,
        // so the shader writes the same bits through an R10G10B10A2_UINT view.
        mNormalRoughness = MakeTexture2D(dev, mWidth, mHeight, DXGI_FORMAT_R10G10B10A2_TYPELESS, uavFlag, uavState, L"PathTracer_NrdNormalRoughness");
        mViewZ           = MakeTexture2D(dev, mWidth, mHeight, DXGI_FORMAT_R32_FLOAT, uavFlag, uavState, L"PathTracer_NrdViewZ");
        mMotion          = MakeTexture2D(dev, mWidth, mHeight, DXGI_FORMAT_R16G16_FLOAT, uavFlag, uavState, L"PathTracer_NrdMotion");
        mDiffuseOut      = MakeTexture2D(dev, mWidth, mHeight, kRadianceFormat, uavFlag, uavState, L"PathTracer_NrdDiffuseOut");
        mSpecularOut     = MakeTexture2D(dev, mWidth, mHeight, kRadianceFormat, uavFlag, uavState, L"PathTracer_NrdSpecularOut");
        mPrevViewZ       = MakeTexture2D(dev, mWidth, mHeight, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"PathTracer_PrevViewZ");
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("PathTracer: real-time targets: ") + ex.what();
        return false;
    }

    if (!mRealTimeDescriptorsAllocated)
    {
        for (ViewSlot* slot : { &mDiffuseInUav, &mSpecularInUav, &mNormalRoughnessUav, &mViewZUav, &mMotionUav,
                                &mDiffuseOutSrv, &mSpecularOutSrv, &mPrevViewZSrv })
        {
            if (!AllocDesc(slot->Cpu, slot->Gpu, mLastError))
                return false;
        }
        mRealTimeDescriptorsAllocated = true;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = kRadianceFormat;
    dev->CreateUnorderedAccessView(mDiffuseIn.Get(), nullptr, &uav, mDiffuseInUav.Cpu);
    dev->CreateUnorderedAccessView(mSpecularIn.Get(), nullptr, &uav, mSpecularInUav.Cpu);
    uav.Format = DXGI_FORMAT_R10G10B10A2_UINT;
    dev->CreateUnorderedAccessView(mNormalRoughness.Get(), nullptr, &uav, mNormalRoughnessUav.Cpu);
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    dev->CreateUnorderedAccessView(mViewZ.Get(), nullptr, &uav, mViewZUav.Cpu);
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    dev->CreateUnorderedAccessView(mMotion.Get(), nullptr, &uav, mMotionUav.Cpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = kRadianceFormat;
    srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(mDiffuseOut.Get(), &srv, mDiffuseOutSrv.Cpu);
    dev->CreateShaderResourceView(mSpecularOut.Get(), &srv, mSpecularOutSrv.Cpu);
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    dev->CreateShaderResourceView(mPrevViewZ.Get(), &srv, mPrevViewZSrv.Cpu);

    NrdInitTextures nrd{};
    nrd.diffRadianceHitDist = mDiffuseIn.Get();
    nrd.motionVectors       = mMotion.Get();
    nrd.normalRoughness     = mNormalRoughness.Get();
    nrd.viewZ               = mViewZ.Get();
    nrd.outDiffRadiance     = mDiffuseOut.Get();
    nrd.specRadianceHitDist = mSpecularIn.Get();
    nrd.outSpecRadiance     = mSpecularOut.Get();
    nrd.diffRadianceHitDistFmt = kRadianceFormat;
    nrd.motionVectorsFmt       = DXGI_FORMAT_R16G16_FLOAT;
    nrd.normalRoughnessFmt     = DXGI_FORMAT_R10G10B10A2_UNORM;
    nrd.viewZFmt               = DXGI_FORMAT_R32_FLOAT;
    nrd.outDiffRadianceFmt     = kRadianceFormat;
    nrd.specRadianceHitDistFmt = kRadianceFormat;
    nrd.outSpecRadianceFmt     = kRadianceFormat;
    if (!mDenoiser.Initialize(mWidth, mHeight, nrd))
    {
        mLastError = std::string("PathTracer: NRD init failed: ")
                   + (mDenoiser.GetLastError() ? mDenoiser.GetLastError() : "unknown");
        return false;
    }
    mDenoiserHistoryFrames = -1;   // retune on the next frame
    mDenoiserHistoryReset = true;
    mRealTimeReady = true;
    mRealTimeFailed = false;
    return true;
}

PathTracer::InputSignature PathTracer::ComputeInputSignature(const FrameInputs& in) const
{
    const PathTracerSettings& s = *in.Settings;
    InputSignature sig;

    SignatureHasher scene;
    scene.Add(in.SceneSignature);
    sig.Group[kSigScene] = scene.Value;

    SignatureHasher camera;
    camera.Add(in.ViewProjection);
    camera.Add(in.CameraPosition);
    camera.Add(mWidth);
    camera.Add(mHeight);
    sig.Group[kSigCamera] = camera.Value;

    SignatureHasher sunSky;
    sunSky.Add(in.SunDirection);
    sunSky.Add(in.SunColor);
    sunSky.Add(in.SkyAmbient);
    sunSky.Add(in.Sky);
    sunSky.Add(in.PreExposure);
    sig.Group[kSigSunSky] = sunSky.Value;

    SignatureHasher lights;
    const int numLights = (std::clamp)(in.NumLights, 0, static_cast<int>(DeferredLightingPass::kMaxPointLights));
    lights.Add(numLights);
    for (int i = 0; in.Lights != nullptr && i < numLights; ++i)
    {
        // Not the shadow-map slot the scene renderer writes into _Pad0: the path
        // tracer traces its own shadows, and slots move as the shadow pass reshuffles.
        DeferredLightingPass::PointLightGpu light = in.Lights[i];
        light._Pad0 = 0.0f;
        lights.Add(light);
    }
    sig.Group[kSigLights] = lights.Value;

    // Not SamplesPerFrame or MaxSamples: they change how fast and how far the
    // image refines, not what it converges to.
    SignatureHasher settings;
    settings.Add(s.MaxBounces);
    settings.Add(in.DisneyBrdf);
    settings.Add(in.Subsurface);
    settings.Add(in.Subsurface ? in.SubsurfaceTransmission : 0.0f);
    settings.Add(s.IndirectClamp);
    settings.Add(s.EmissiveIntensity);
    settings.Add(s.GroundAlbedo);
    settings.Add(s.SoftShadows);
    settings.Add(s.TerrainDisplacementShadows);
    settings.Add(s.AlphaTest);
    settings.Add(s.DebugView);
    settings.Add(in.SubsurfaceMask);
    sig.Group[kSigSettings] = settings.Value;
    return sig;
}

PathTracerConstants PathTracer::BuildConstants(const FrameInputs& in) const
{
    using namespace DirectX;
    const PathTracerSettings& settings = *in.Settings;

    PathTracerConstants constants{};
    constants.Width = mWidth;
    constants.Height = mHeight;
    const int numLights = (std::clamp)(in.NumLights, 0, static_cast<int>(DeferredLightingPass::kMaxPointLights));
    constants.NumLights = in.Lights != nullptr ? static_cast<uint32_t>(numLights) : 0u;

    // Traced values are pre-exposed like the GI passes', so a night scene keeps
    // daylight magnitudes; everything entered in raster units is scaled here.
    const float preExposure = in.PreExposure > 0.0f ? in.PreExposure : 1.0f;
    constants.IndirectClamp = (std::max)(settings.IndirectClamp, 0.0f) * preExposure;
    constants.EmissiveScale = (std::max)(settings.EmissiveIntensity, 0.0f) * preExposure;
    constants.SunCosAngularRadius = std::cos(kSunAngularRadiusRad);
    constants.InvPreExposure = 1.0f / preExposure;
    constants.ScreenCacheScale = 1.0f;

    StoreTransposed(constants.ViewProjInv, XMMatrixInverse(nullptr, XMLoadFloat4x4(&in.ViewProjection)));

    constants.CameraPos[0] = in.CameraPosition.x;
    constants.CameraPos[1] = in.CameraPosition.y;
    constants.CameraPos[2] = in.CameraPosition.z;
    constants.DebugView = settings.DebugView;

    constants.SunDir[0] = in.SunDirection.x;
    constants.SunDir[1] = in.SunDirection.y;
    constants.SunDir[2] = in.SunDirection.z;
    constants.GroundAlbedo = (std::clamp)(settings.GroundAlbedo, 0.0f, 1.0f);
    constants.SunColor[0] = in.SunColor.x * preExposure;
    constants.SunColor[1] = in.SunColor.y * preExposure;
    constants.SunColor[2] = in.SunColor.z * preExposure;
    constants.SkyAmbient[0] = in.SkyAmbient.x * preExposure;
    constants.SkyAmbient[1] = in.SkyAmbient.y * preExposure;
    constants.SkyAmbient[2] = in.SkyAmbient.z * preExposure;
    constants.SssTransmission = (std::max)(in.SubsurfaceTransmission, 0.0f);

    for (uint32_t i = 0; i < constants.NumLights; ++i)
    {
        constants.Lights[i] = in.Lights[i];
        constants.Lights[i].Color.x *= preExposure;
        constants.Lights[i].Color.y *= preExposure;
        constants.Lights[i].Color.z *= preExposure;
    }
    constants.Sky = in.Sky;

    StoreTransposed(constants.ViewProjInvJittered, XMMatrixInverse(nullptr, XMLoadFloat4x4(&in.JitteredViewProjection)));
    StoreTransposed(constants.CurrViewProj, XMLoadFloat4x4(&in.ViewProjection));
    StoreTransposed(constants.PrevViewProj, XMLoadFloat4x4(&in.PrevViewProjection));
    StoreTransposed(constants.WorldToView, XMLoadFloat4x4(&in.WorldToView));
    constants.FogStartDistance = in.FogStartDistance;
    constants.FogMaxDistance = in.FogMaxDistance;
    return constants;
}

D3D12_GPU_VIRTUAL_ADDRESS PathTracer::CommitConstants(const PathTracerConstants& constants)
{
    mConstantSlot = (mConstantSlot + 1u) % kFramesInFlight;
    const UINT64 offset = kConstantStride * mConstantSlot;
    std::memcpy(mMappedConstants + offset, &constants, sizeof(constants));
    return mConstantBuffer->GetGPUVirtualAddress() + offset;
}

void PathTracer::BeginOutput(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor)
{
    const D3D12_RESOURCE_BARRIER toCopy[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(sceneColor,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(mBackground.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cmdList->ResourceBarrier(static_cast<UINT>(std::size(toCopy)), toCopy);
    cmdList->CopyResource(mBackground.Get(), sceneColor);

    const D3D12_RESOURCE_BARRIER toTrace[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mBackground.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(mOutput.Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmdList->ResourceBarrier(static_cast<UINT>(std::size(toTrace)), toTrace);
}

void PathTracer::EndOutput(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor)
{
    const D3D12_RESOURCE_BARRIER toCopy[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(mOutput.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(sceneColor,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cmdList->ResourceBarrier(static_cast<UINT>(std::size(toCopy)), toCopy);
    cmdList->CopyResource(sceneColor, mOutput.Get());

    const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(sceneColor,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(1, &toRenderTarget);
}

void PathTracer::BindScene(ID3D12GraphicsCommandList4* cmdList, D3D12_GPU_VIRTUAL_ADDRESS constants, const FrameInputs& in)
{
    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    cmdList->SetDescriptorHeaps(1, &heap);
    cmdList->SetComputeRootSignature(mRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(kRootConstants, constants);
    cmdList->SetComputeRootDescriptorTable(kRootTlas, in.Tlas);
    cmdList->SetComputeRootDescriptorTable(kRootVertices, in.Vertices);
    cmdList->SetComputeRootDescriptorTable(kRootIndices, in.Indices);
    cmdList->SetComputeRootDescriptorTable(kRootInstanceInfo, in.InstanceInfo);
    cmdList->SetComputeRootDescriptorTable(kRootMaterialRanges, in.MaterialRanges);
    cmdList->SetComputeRootDescriptorTable(kRootMaterialSurfaces, in.MaterialSurfaces);
    cmdList->SetComputeRootDescriptorTable(kRootBackground, mBackgroundSrvGpu);
    cmdList->SetComputeRootDescriptorTable(kRootTerrainTrace, in.TerrainTrace);
    cmdList->SetComputeRootDescriptorTable(kRootTextures, in.MaterialTextures);
    cmdList->SetComputeRootDescriptorTable(kRootAccumulation, mAccumulationUavGpu);
    cmdList->SetComputeRootDescriptorTable(kRootOutput, mOutputUavGpu);
    if (in.VolumetricFog.ptr != 0)
        cmdList->SetComputeRootDescriptorTable(kRootVolumetricFog, in.VolumetricFog);
}

void PathTracer::Render(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor, const FrameInputs& in)
{
    if (!mIsInitialized || cmdList == nullptr || sceneColor == nullptr || in.Settings == nullptr || !mMappedConstants)
        return;

    const D3D12_RESOURCE_DESC sceneDesc = sceneColor->GetDesc();
    if (sceneDesc.Width != mWidth || sceneDesc.Height != mHeight || sceneDesc.Format != kSceneColorFormat)
    {
        mLastError = "PathTracer: the scene colour target does not match the path tracer's size or format.";
        return;
    }

    // Retire what the ring's oldest frame was still holding on to.
    mRetireSlot = (mRetireSlot + 1u) % kFramesInFlight;
    mRetired[mRetireSlot].clear();

    // Switching modes starts both over.
    if (in.Settings->Mode != mLastMode)
    {
        mLastMode = in.Settings->Mode;
        mSampleCount = 0;
        mHasInputSignature = false;
        mConsecutiveRestarts = 0;
        mDenoiserHistoryReset = true;
        mScreenCacheValid = false;
    }

    if (in.Settings->Mode == PathTracerMode::RealTime)
        RenderRealTime(cmdList, sceneColor, in);
    else
        RenderReference(cmdList, sceneColor, in);
}

void PathTracer::RenderReference(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor, const FrameInputs& in)
{
    const PathTracerSettings& settings = *in.Settings;

    const InputSignature signature = ComputeInputSignature(in);
    const char* restartReason = nullptr;
    bool cameraMoved = false;
    if (mHasInputSignature)
    {
        static const char* const kGroupNames[kSigGroupCount] = {
            "the ray tracing scene", "the camera", "the sun or sky", "a light", "a path tracer setting" };
        for (int g = 0; g < kSigGroupCount && restartReason == nullptr; ++g)
        {
            if (signature.Group[g] != mInputSignature.Group[g])
                restartReason = kGroupNames[g];
        }
        cameraMoved = signature.Group[kSigCamera] != mInputSignature.Group[kSigCamera];
    }
    if (!mHasInputSignature || restartReason != nullptr)
    {
        mInputSignature = signature;
        mHasInputSignature = true;
        mSampleCount = 0;
    }
    // Only an input that keeps changing on its own is worth reporting; the camera
    // moving under the user's hand is expected to restart it.
    if (restartReason != nullptr && !cameraMoved)
    {
        mConsecutiveRestarts = restartReason == mRestartReason ? mConsecutiveRestarts + 1u : 1u;
        mRestartReason = restartReason;
    }
    else
    {
        mConsecutiveRestarts = 0;
    }

    // Debug views show one deterministic camera ray and are never accumulated.
    const bool debugView = settings.DebugView != 0 || in.SubsurfaceMask;
    const uint32_t maxSamples = static_cast<uint32_t>((std::max)(settings.MaxSamples, 0));
    uint32_t samplesThisFrame = static_cast<uint32_t>((std::clamp)(settings.SamplesPerFrame, 1, 16));
    if (maxSamples > 0)
        samplesThisFrame = mSampleCount >= maxSamples ? 0u : (std::min)(samplesThisFrame, maxSamples - mSampleCount);
    if (debugView)
    {
        samplesThisFrame = 0;
        mSampleCount = 0;
    }
    mConverged = !debugView && samplesThisFrame == 0;

    PathTracerConstants constants = BuildConstants(in);
    constants.FrameSeed = ++mFrameSeed;
    constants.SampleIndex = mSampleCount;
    constants.SamplesPerFrame = samplesThisFrame;
    constants.MaxBounces = static_cast<uint32_t>((std::clamp)(settings.MaxBounces, 0, 32));
    constants.Flags = (samplesThisFrame > 0 ? kFlagTrace : 0u)
                    | (settings.SoftShadows ? kFlagSoftShadows : 0u)
                    | (settings.TerrainDisplacementShadows ? kFlagTerrainShadows : 0u)
                    | (settings.AlphaTest && in.SceneHasAlphaCutout ? kFlagAlphaTest : 0u)
                    | (in.DisneyBrdf ? kFlagDisneyBrdf : 0u)
                    | (in.Subsurface ? kFlagSubsurface : 0u)
                    | (in.SubsurfaceMask ? kFlagSubsurfaceMask : 0u)
                    | (in.VolumetricFog.ptr != 0 ? kFlagVolumetricFog : 0u);
    const D3D12_GPU_VIRTUAL_ADDRESS constantAddress = CommitConstants(constants);

    BeginOutput(cmdList, sceneColor);

    BindScene(cmdList, constantAddress, in);
    // Camera hits on terrain take the G-Buffer's material (PT_GBUFFER_TERRAIN).
    cmdList->SetComputeRootDescriptorTable(kRootGBufferAlbedo, in.GBufferAlbedo);
    cmdList->SetComputeRootDescriptorTable(kRootGBufferNormal, in.GBufferNormal);
    cmdList->SetComputeRootDescriptorTable(kRootGBufferMaterial, in.GBufferMaterial);
    if (in.GBufferPrincipled.ptr != 0)
        cmdList->SetComputeRootDescriptorTable(kRootGBufferPrincipled, in.GBufferPrincipled);
    cmdList->SetPipelineState(mPipeline.Get());
    cmdList->Dispatch((mWidth + kGroupSize - 1) / kGroupSize, (mHeight + kGroupSize - 1) / kGroupSize, 1);
    mSampleCount += samplesThisFrame;

    const auto accumulated = CD3DX12_RESOURCE_BARRIER::UAV(mAccumulation.Get());
    cmdList->ResourceBarrier(1, &accumulated);
    EndOutput(cmdList, sceneColor);
}

void PathTracer::RenderRealTime(ID3D12GraphicsCommandList4* cmdList, ID3D12Resource* sceneColor, const FrameInputs& in)
{
    const PathTracerSettings& settings = *in.Settings;
    mConverged = false;
    mSampleCount = 0;

    if (!EnsureRealTimeResources())
        return;   // the raster image stays; mLastError says why

    // The debug views show whichever signal this gives: denoised, or the raw paths.
    const bool runDenoiser = settings.Denoise;

    // Reading last frame's lighting needs last frame to have produced it (denoised,
    // at this size, since the last restart).
    if (mDenoiserHistoryReset)
        mScreenCacheValid = false;
    const bool useScreenCache = settings.ScreenCache && runDenoiser && mScreenCacheValid;
    const float preExposure = in.PreExposure > 0.0f ? in.PreExposure : 1.0f;

    PathTracerConstants constants = BuildConstants(in);
    constants.FrameSeed = ++mFrameSeed;
    constants.SamplesPerFrame = static_cast<uint32_t>((std::clamp)(settings.RealTimeSamplesPerPixel, 1, 4));
    constants.MaxBounces = static_cast<uint32_t>((std::clamp)(settings.RealTimeMaxBounces, 0, 8));
    // Last frame's values were pre-exposed with last frame's exposure.
    constants.ScreenCacheScale = preExposure / (mPrevPreExposure > 0.0f ? mPrevPreExposure : preExposure);
    // One scene light per vertex: a shadow ray each, however many lights there are.
    constants.Flags = kFlagOneLight
                    | (settings.SoftShadows ? kFlagSoftShadows : 0u)
                    | (settings.TerrainDisplacementShadows ? kFlagTerrainShadows : 0u)
                    | (settings.AlphaTest && in.SceneHasAlphaCutout ? kFlagAlphaTest : 0u)
                    | (runDenoiser ? kFlagDenoised : 0u)
                    | (useScreenCache ? kFlagScreenCache : 0u)
                    | (in.DisneyBrdf ? kFlagDisneyBrdf : 0u)
                    | (in.Subsurface ? kFlagSubsurface : 0u)
                    | (in.SubsurfaceMask ? kFlagSubsurfaceMask : 0u)
                    | (in.VolumetricFog.ptr != 0 ? kFlagVolumetricFog : 0u);
    const D3D12_GPU_VIRTUAL_ADDRESS constantAddress = CommitConstants(constants);
    const UINT groupsX = (mWidth + kGroupSize - 1) / kGroupSize;
    const UINT groupsY = (mHeight + kGroupSize - 1) / kGroupSize;

    BeginOutput(cmdList, sceneColor);

    auto bindRealTime = [&]()
    {
        BindScene(cmdList, constantAddress, in);
        cmdList->SetComputeRootDescriptorTable(kRootGBufferAlbedo, in.GBufferAlbedo);
        cmdList->SetComputeRootDescriptorTable(kRootGBufferNormal, in.GBufferNormal);
        cmdList->SetComputeRootDescriptorTable(kRootGBufferMaterial, in.GBufferMaterial);
        if (in.GBufferPrincipled.ptr != 0)
            cmdList->SetComputeRootDescriptorTable(kRootGBufferPrincipled, in.GBufferPrincipled);
        cmdList->SetComputeRootDescriptorTable(kRootDenoisedDiffuse, mDiffuseOutSrv.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootDenoisedSpecular, mSpecularOutSrv.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootPrevDiffuse, mDiffuseOutSrv.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootPrevViewZ, mPrevViewZSrv.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootDiffuseIn, mDiffuseInUav.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootSpecularIn, mSpecularInUav.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootNormalRoughness, mNormalRoughnessUav.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootViewZ, mViewZUav.Gpu);
        cmdList->SetComputeRootDescriptorTable(kRootMotion, mMotionUav.Gpu);
    };

    // ---- Trace -------------------------------------------------------------
    // NRD's diffuse output still holds last frame's lighting: the screen cache.
    if (useScreenCache)
    {
        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(mDiffuseOut.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &toRead);
    }
    bindRealTime();
    cmdList->SetPipelineState(mPipelineRealTime.Get());
    cmdList->Dispatch(groupsX, groupsY, 1);
    if (useScreenCache)
    {
        const auto toWrite = CD3DX12_RESOURCE_BARRIER::Transition(mDiffuseOut.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &toWrite);
    }
    {
        const D3D12_RESOURCE_BARRIER traced[] = {
            CD3DX12_RESOURCE_BARRIER::UAV(mDiffuseIn.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(mSpecularIn.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(mNormalRoughness.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(mViewZ.Get()),
            CD3DX12_RESOURCE_BARRIER::UAV(mMotion.Get()),
        };
        cmdList->ResourceBarrier(static_cast<UINT>(std::size(traced)), traced);
    }

    // ---- Denoise -----------------------------------------------------------
    if (runDenoiser)
    {
        const int historyFrames = (std::clamp)(settings.DenoiserHistoryFrames, 4, 60);
        if (historyFrames != mDenoiserHistoryFrames)
        {
            // NRD's defaults are tuned for exactly this signal - a path-traced diffuse
            // and specular pair - unlike the RTGI tuning the wrapper carries.
            nrd::RelaxSettings relax{};
            relax.diffuseMaxAccumulatedFrameNum = static_cast<uint32_t>(historyFrames);
            relax.specularMaxAccumulatedFrameNum = static_cast<uint32_t>(historyFrames);
            relax.diffuseMaxFastAccumulatedFrameNum = static_cast<uint32_t>((std::max)(2, historyFrames / 5));
            relax.specularMaxFastAccumulatedFrameNum = static_cast<uint32_t>((std::max)(2, historyFrames / 5));
            relax.enableAntiFirefly = true;
            mDenoiser.SetRelaxSettings(relax);
            mDenoiserHistoryFrames = historyFrames;
        }

        mDenoiser.Denoise(
            cmdList,
            in.ViewToClip.m[0],
            in.PrevViewToClip.m[0],
            in.WorldToView.m[0],
            in.PrevWorldToView.m[0],
            in.CameraJitter,
            in.PrevCameraJitter,
            mDenoiserFrameIndex++,
            mDenoiserHistoryReset,
            in.FrameTimeMs);
        mDenoiserHistoryReset = false;

        // Denoise leaves every external texture as a UAV; the composite reads the outputs.
        const D3D12_RESOURCE_BARRIER toRead[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mDiffuseOut.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mSpecularOut.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cmdList->ResourceBarrier(static_cast<UINT>(std::size(toRead)), toRead);
    }
    else
    {
        // With the denoiser off its history no longer follows the image.
        mDenoiserHistoryReset = true;
    }

    // ---- Compose -----------------------------------------------------------
    // NRD bound its own heap and root signatures; bind everything again.
    bindRealTime();
    cmdList->SetPipelineState(mPipelineComposite.Get());
    cmdList->Dispatch(groupsX, groupsY, 1);

    if (runDenoiser)
    {
        const D3D12_RESOURCE_BARRIER back[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mDiffuseOut.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(mSpecularOut.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cmdList->ResourceBarrier(static_cast<UINT>(std::size(back)), back);

        // This frame's depth, for next frame's cache lookups to validate against.
        const D3D12_RESOURCE_BARRIER toCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mViewZ.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mPrevViewZ.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmdList->ResourceBarrier(static_cast<UINT>(std::size(toCopy)), toCopy);
        cmdList->CopyResource(mPrevViewZ.Get(), mViewZ.Get());
        const D3D12_RESOURCE_BARRIER fromCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mViewZ.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(mPrevViewZ.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        cmdList->ResourceBarrier(static_cast<UINT>(std::size(fromCopy)), fromCopy);
    }
    mScreenCacheValid = runDenoiser;
    mPrevPreExposure = preExposure;

    EndOutput(cmdList, sceneColor);
}
