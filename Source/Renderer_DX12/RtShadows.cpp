// RtShadows.cpp
// Ray-traced sun shadows.
//
// Pass sequence (one root signature for all three):
//   Pass 0  RayGen    t0=GBufferNormal  t1=TLAS            u0=Raw
//   Pass 1  Temporal  t0=GBufferNormal  u0=Raw  u1=PrevHistory  u2=History
//   Pass 2  Spatial   t0=GBufferNormal  u0=Raw  u2=History      u3=Output
//   Pass 3  Local     t0=GBufferNormal  t1=TLAS  space7=light clusters  u5=LocalRaw
//   Pass 4  LocalTemporal  u1=PrevHistory (camera distances)  u5=LocalRaw  u8,u9=PrevLocalHistory  u6,u7=LocalHistory
//   Pass 5  LocalSpatial   t0=GBufferNormal  u6,u7=LocalHistory  u4=Local
// The intermediates never leave the UAV state (the passes read them with Load), so
// only the two outputs are transitioned, to be read by the deferred lighting pass.

#include "pch.h"
#include "RtShadows.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

static const wchar_t* kShaderRayGen   = L"Data/Shaders/RtShadow_RayGen.hlsl";
static const wchar_t* kShaderTemporal = L"Data/Shaders/RtShadow_Temporal.hlsl";
static const wchar_t* kShaderSpatial  = L"Data/Shaders/RtShadow_Spatial.hlsl";
static const wchar_t* kShaderLocal    = L"Data/Shaders/RtShadow_Local.hlsl";
static const wchar_t* kShaderLocalTemporal = L"Data/Shaders/RtShadow_LocalTemporal.hlsl";
static const wchar_t* kShaderLocalSpatial  = L"Data/Shaders/RtShadow_LocalSpatial.hlsl";

namespace
{
    constexpr DXGI_FORMAT kRawFormat     = DXGI_FORMAT_R16G16_FLOAT;
    constexpr DXGI_FORMAT kHistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kOutputFormat  = DXGI_FORMAT_R16_FLOAT;
    constexpr DXGI_FORMAT kLocalFormat   = DXGI_FORMAT_R32G32B32A32_UINT;
    constexpr UINT kGroupSize = 8;
    constexpr UINT kLightClusterRootIndex = 13;

    bool AllocDesc(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu, std::string& err)
    {
        if (!DX12Context_AllocateSrvDescriptor(&cpu, &gpu))
        {
            err = "RtShadows: ran out of SRV descriptor heap slots.";
            return false;
        }
        return true;
    }

    ComPtr<ID3D12Resource> MakeTexture2D(
        ID3D12Device* dev,
        UINT w,
        UINT h,
        DXGI_FORMAT format,
        D3D12_RESOURCE_STATES initialState,
        LPCWSTR name)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> res;
        DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            initialState, nullptr,
            IID_PPV_ARGS(&res)));
        res->SetName(name);
        return res;
    }

    ComPtr<ID3D12Resource> MakeUploadCB(ID3D12Device* dev, UINT64 size, void** outMapped)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> res;
        DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&res)));
        DX12_THROW_IF_FAILED(res->Map(0, nullptr, outMapped));
        res->SetName(L"RtShadow_ConstantBuffer");
        return res;
    }

    bool CompilePSO(
        ID3D12Device* dev,
        ID3D12RootSignature* rootSig,
        const wchar_t* filePath,
        ComPtr<ID3D12PipelineState>& outPSO,
        std::string& err)
    {
        ShaderCompileRequest req{};
        req.FilePath = filePath;
        req.EntryPoint = L"CSMain";
        req.TargetProfile = L"cs_6_5";
        req.Stage = ShaderStage::Compute;

        DX12Shader shader;
        if (!shader.Compile(req))
        {
            err = std::string("RtShadows: shader compile failed ('")
                + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "?") + "')";
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig;
        psoDesc.CS = shader.GetBytecode();
        if (FAILED(dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPSO))))
        {
            err = "RtShadows: CreateComputePipelineState failed.";
            return false;
        }
        return true;
    }
}

bool RtShadows::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();
    try
    {
        if (!mIsInitialized)
        {
            if (!CreateRootSignature()) { mInitFailed = true; return false; }
            if (!CreatePipelines()) { mInitFailed = true; return false; }
        }

        if (!mIsInitialized || width != mWidth || height != mHeight)
        {
            mWidth = width;
            mHeight = height;
            if (!CreateResolutionResources()) { mInitFailed = true; return false; }
            mHistoryValid = false;
        }

        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("RtShadows::Initialize: ") + ex.what();
        mInitFailed = true;
        return false;
    }
}

bool RtShadows::EnsureSize(UINT width, UINT height)
{
    if (width == mWidth && height == mHeight)
        return true;
    return Initialize(width, height);
}

void RtShadows::Shutdown()
{
    if (mMappedCb && mConstantBuffer)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCb = nullptr;
    }

    mConstantBuffer.Reset();
    mRawBuffer.Reset();
    mHistory[0].Reset();
    mHistory[1].Reset();
    mOutputBuffer.Reset();
    mLocalBuffer.Reset();
    mLocalRawBuffer.Reset();
    for (auto& pingPong : mLocalHistory)
        for (auto& half : pingPong)
            half.Reset();
    mRootSignature.Reset();
    mPSO_RayGen.Reset();
    mPSO_Temporal.Reset();
    mPSO_Spatial.Reset();
    mPSO_Local.Reset();
    mPSO_LocalTemporal.Reset();
    mPSO_LocalSpatial.Reset();
    mIsInitialized = false;
    mHistoryValid = false;
    mLocalHistoryValid = false;
    mOutputInSrvState = true;
    mLocalInSrvState = true;
    mLocalWritten = false;
    mDescriptorsAllocated = false;
}

void RtShadows::Dispatch(
    ID3D12GraphicsCommandList4* cmdList,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE tlasSrv,
    const LightClusterBindings& lightClusters,
    const RtShadowSettings& settings,
    const float sunDirection[3],
    const float viewProjInv[16],
    const float prevViewProj[16],
    const float cameraPos[3],
    const float prevCameraPos[3],
    float verticalFovRadians)
{
    if (cmdList == nullptr || !mIsInitialized || mMappedCb == nullptr)
        return;

    RtShadowConstants cb{};
    cb.FrameWidth = mWidth;
    cb.FrameHeight = mHeight;
    cb.FrameIndex = mFrameIndex;
    cb.RaysPerPixel = static_cast<uint32_t>(std::clamp(settings.RaysPerPixel, 1, 8));

    // The lighting pass stores the direction the light travels; rays go the other way.
    float toSun[3] = { -sunDirection[0], -sunDirection[1], -sunDirection[2] };
    const float len = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
    if (len > 1e-6f)
    {
        for (float& c : toSun)
            c /= len;
    }
    else
    {
        toSun[0] = 0.0f; toSun[1] = 0.0f; toSun[2] = 1.0f;
    }
    std::memcpy(cb.SunDirection, toSun, sizeof(toSun));

    constexpr float kDegToRad = 3.14159265358979f / 180.0f;
    cb.SunTanHalfAngle = std::tan(0.5f * std::clamp(settings.SunAngularDiameter, 0.0f, 10.0f) * kDegToRad);
    cb.MaxRayLength = (std::max)(settings.MaxRayLength, 0.01f);
    cb.RayBias = (std::max)(settings.RayBias, 0.0f);
    cb.TemporalBlend = std::clamp(settings.TemporalBlend, 0.02f, 1.0f);
    cb.MaxFilterRadius = std::clamp(settings.MaxFilterRadius, 0, 16);
    std::memcpy(cb.CameraPos, cameraPos, 3 * sizeof(float));
    cb.HistoryValid = mHistoryValid ? 1u : 0u;
    std::memcpy(cb.PrevCameraPos, prevCameraPos, 3 * sizeof(float));
    cb.PixelAngle = verticalFovRadians / static_cast<float>((std::max)(mHeight, 1u));
    std::memcpy(cb.ViewProjInv, viewProjInv, 16 * sizeof(float));
    std::memcpy(cb.PrevViewProj, prevViewProj, 16 * sizeof(float));
    cb.DebugView = settings.DebugView;
    cb.LocalLightSoftness = std::clamp(settings.LocalLightSoftness, 0.0f, 4.0f);

    const bool traceLocal = settings.LocalLights && lightClusters.IsValid();
    if (!traceLocal)
        mLocalHistoryValid = false;
    cb.LocalHistoryValid = mLocalHistoryValid ? 1u : 0u;
    mCbRingSlot = (mCbRingSlot + 1u) % kCbRingSlots;
    const UINT64 cbOffset = UINT64(mCbRingSlot) * sizeof(RtShadowConstants);
    std::memcpy(static_cast<std::byte*>(mMappedCb) + cbOffset, &cb, sizeof(cb));

    {
        ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
        if (heap) cmdList->SetDescriptorHeaps(1, &heap);
    }

    if (mOutputInSrvState)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            mOutputBuffer.Get(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &b);
        mOutputInSrvState = false;
    }
    if (traceLocal && mLocalInSrvState)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            mLocalBuffer.Get(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &b);
        mLocalInSrvState = false;
    }

    const uint32_t cur = mHistoryIndex;
    const uint32_t prev = mHistoryIndex ^ 1u;

    cmdList->SetComputeRootSignature(mRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress() + cbOffset);
    cmdList->SetComputeRootDescriptorTable(1, gbufferNormalSrv);
    cmdList->SetComputeRootDescriptorTable(2, tlasSrv);
    cmdList->SetComputeRootDescriptorTable(3, mRawUavGpu);
    cmdList->SetComputeRootDescriptorTable(4, mHistoryUavGpu[prev]);
    cmdList->SetComputeRootDescriptorTable(5, mHistoryUavGpu[cur]);
    cmdList->SetComputeRootDescriptorTable(6, mOutputUavGpu);
    cmdList->SetComputeRootDescriptorTable(7, mLocalUavGpu);
    const uint32_t localCur = mLocalHistoryIndex;
    const uint32_t localPrev = mLocalHistoryIndex ^ 1u;
    cmdList->SetComputeRootDescriptorTable(8, mLocalRawUavGpu);
    cmdList->SetComputeRootDescriptorTable(9, mLocalHistoryUavGpu[localCur][0]);
    cmdList->SetComputeRootDescriptorTable(10, mLocalHistoryUavGpu[localCur][1]);
    cmdList->SetComputeRootDescriptorTable(11, mLocalHistoryUavGpu[localPrev][0]);
    cmdList->SetComputeRootDescriptorTable(12, mLocalHistoryUavGpu[localPrev][1]);
    if (lightClusters.IsValid())
        LightClusterRenderer::BindCompute(cmdList, kLightClusterRootIndex, lightClusters);

    const UINT gx = (mWidth + kGroupSize - 1) / kGroupSize;
    const UINT gy = (mHeight + kGroupSize - 1) / kGroupSize;

    cmdList->SetPipelineState(mPSO_RayGen.Get());
    cmdList->Dispatch(gx, gy, 1);
    {
        auto b = CD3DX12_RESOURCE_BARRIER::UAV(mRawBuffer.Get());
        cmdList->ResourceBarrier(1, &b);
    }

    cmdList->SetPipelineState(mPSO_Temporal.Get());
    cmdList->Dispatch(gx, gy, 1);
    {
        auto b = CD3DX12_RESOURCE_BARRIER::UAV(mHistory[cur].Get());
        cmdList->ResourceBarrier(1, &b);
    }

    cmdList->SetPipelineState(mPSO_Spatial.Get());
    cmdList->Dispatch(gx, gy, 1);
    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::UAV(mOutputBuffer.Get());
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            mOutputBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(2, b);
        mOutputInSrvState = true;
    }

    mLocalWritten = false;
    if (traceLocal)
    {
        cmdList->SetPipelineState(mPSO_Local.Get());
        cmdList->Dispatch(gx, gy, 1);
        {
            auto b = CD3DX12_RESOURCE_BARRIER::UAV(mLocalRawBuffer.Get());
            cmdList->ResourceBarrier(1, &b);
        }

        cmdList->SetPipelineState(mPSO_LocalTemporal.Get());
        cmdList->Dispatch(gx, gy, 1);
        {
            D3D12_RESOURCE_BARRIER h[2];
            h[0] = CD3DX12_RESOURCE_BARRIER::UAV(mLocalHistory[localCur][0].Get());
            h[1] = CD3DX12_RESOURCE_BARRIER::UAV(mLocalHistory[localCur][1].Get());
            cmdList->ResourceBarrier(2, h);
        }

        cmdList->SetPipelineState(mPSO_LocalSpatial.Get());
        cmdList->Dispatch(gx, gy, 1);

        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::UAV(mLocalBuffer.Get());
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            mLocalBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(2, b);
        mLocalInSrvState = true;
        mLocalWritten = true;
        mLocalHistoryIndex = localPrev;
        mLocalHistoryValid = true;
    }

    mHistoryIndex = prev;
    mHistoryValid = true;
    ++mFrameIndex;
}

bool RtShadows::CreateRootSignature()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev)
    {
        mLastError = "RtShadows: no D3D12 device.";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[12]{};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }; // t0 G-Buffer normal + depth
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 }; // t1 TLAS
    ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 }; // u0 raw
    ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0 }; // u1 previous history
    ranges[4] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 0, 0 }; // u2 history
    ranges[5] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 0, 0 }; // u3 output
    ranges[6] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 0, 0 }; // u4 local light pairs
    ranges[7] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 5, 0, 0 }; // u5 raw local light pairs
    ranges[8] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 6, 0, 0 }; // u6 local history, entries 0-3
    ranges[9] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 7, 0, 0 }; // u7 local history, entries 4-7
    ranges[10] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 8, 0, 0 }; // u8 previous local history, 0-3
    ranges[11] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 9, 0, 0 }; // u9 previous local history, 4-7

    // 0: b0, 1..12: the tables above, 13..17: the light clusters (b0, t0..t3 in space 7).
    D3D12_ROOT_PARAMETER params[kLightClusterRootIndex + LightClusterRenderer::kRootParameterCount]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 12; ++i)
    {
        params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable = { 1, &ranges[i] };
        params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    LightClusterRenderer::FillRootParameters(&params[kLightClusterRootIndex], D3D12_SHADER_VISIBILITY_ALL);

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(std::size(params));
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialised, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &serialised, &errors)))
    {
        mLastError = "RtShadows: D3D12SerializeRootSignature failed.";
        return false;
    }

    if (FAILED(dev->CreateRootSignature(0, serialised->GetBufferPointer(), serialised->GetBufferSize(), IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "RtShadows: CreateRootSignature failed.";
        return false;
    }

    mRootSignature->SetName(L"RtShadow_RootSignature");
    return true;
}

bool RtShadows::CreatePipelines()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev) return false;

    if (!CompilePSO(dev, mRootSignature.Get(), kShaderRayGen, mPSO_RayGen, mLastError)) return false;
    if (!CompilePSO(dev, mRootSignature.Get(), kShaderTemporal, mPSO_Temporal, mLastError)) return false;
    if (!CompilePSO(dev, mRootSignature.Get(), kShaderSpatial, mPSO_Spatial, mLastError)) return false;
    if (!CompilePSO(dev, mRootSignature.Get(), kShaderLocal, mPSO_Local, mLastError)) return false;
    if (!CompilePSO(dev, mRootSignature.Get(), kShaderLocalTemporal, mPSO_LocalTemporal, mLastError)) return false;
    if (!CompilePSO(dev, mRootSignature.Get(), kShaderLocalSpatial, mPSO_LocalSpatial, mLastError)) return false;
    return true;
}

bool RtShadows::CreateResolutionResources()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev) return false;

    mRawBuffer.Reset();
    mHistory[0].Reset();
    mHistory[1].Reset();
    mOutputBuffer.Reset();
    mLocalBuffer.Reset();

    mLocalRawBuffer.Reset();
    for (auto& pingPong : mLocalHistory)
        for (auto& half : pingPong)
            half.Reset();
    mLocalBuffer  = MakeTexture2D(dev, mWidth, mHeight, kLocalFormat, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"RtShadow_LocalLights");
    mLocalRawBuffer = MakeTexture2D(dev, mWidth, mHeight, kLocalFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_LocalRaw");
    mLocalHistory[0][0] = MakeTexture2D(dev, mWidth, mHeight, kLocalFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_LocalHistoryA0");
    mLocalHistory[0][1] = MakeTexture2D(dev, mWidth, mHeight, kLocalFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_LocalHistoryA1");
    mLocalHistory[1][0] = MakeTexture2D(dev, mWidth, mHeight, kLocalFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_LocalHistoryB0");
    mLocalHistory[1][1] = MakeTexture2D(dev, mWidth, mHeight, kLocalFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_LocalHistoryB1");
    mLocalHistoryIndex = 0;
    mLocalHistoryValid = false;
    mLocalInSrvState = true;
    mLocalWritten = false;
    mRawBuffer    =MakeTexture2D(dev, mWidth, mHeight, kRawFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_Raw");
    mHistory[0]   = MakeTexture2D(dev, mWidth, mHeight, kHistoryFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_HistoryA");
    mHistory[1]   = MakeTexture2D(dev, mWidth, mHeight, kHistoryFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RtShadow_HistoryB");
    mOutputBuffer = MakeTexture2D(dev, mWidth, mHeight, kOutputFormat, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, L"RtShadow_Output");
    mOutputInSrvState = true;
    mHistoryIndex = 0;

    if (!mConstantBuffer)
        mConstantBuffer = MakeUploadCB(dev, sizeof(RtShadowConstants) * kCbRingSlots, &mMappedCb);

    if (!mDescriptorsAllocated)
    {
        if (!AllocDesc(mRawUavCpu, mRawUavGpu, mLastError)) return false;
        if (!AllocDesc(mHistoryUavCpu[0], mHistoryUavGpu[0], mLastError)) return false;
        if (!AllocDesc(mHistoryUavCpu[1], mHistoryUavGpu[1], mLastError)) return false;
        if (!AllocDesc(mOutputUavCpu, mOutputUavGpu, mLastError)) return false;
        if (!AllocDesc(mOutputSrvCpu, mOutputSrvGpu, mLastError)) return false;
        if (!AllocDesc(mLocalUavCpu, mLocalUavGpu, mLastError)) return false;
        if (!AllocDesc(mLocalSrvCpu, mLocalSrvGpu, mLastError)) return false;
        if (!AllocDesc(mLocalRawUavCpu, mLocalRawUavGpu, mLastError)) return false;
        for (int p = 0; p < 2; ++p)
            for (int h = 0; h < 2; ++h)
                if (!AllocDesc(mLocalHistoryUavCpu[p][h], mLocalHistoryUavGpu[p][h], mLastError)) return false;
        mDescriptorsAllocated = true;
    }

    auto MakeUav = [&](ID3D12Resource* res, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE cpu)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = format;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(res, nullptr, &d, cpu);
    };

    MakeUav(mRawBuffer.Get(), kRawFormat, mRawUavCpu);
    MakeUav(mHistory[0].Get(), kHistoryFormat, mHistoryUavCpu[0]);
    MakeUav(mHistory[1].Get(), kHistoryFormat, mHistoryUavCpu[1]);
    MakeUav(mOutputBuffer.Get(), kOutputFormat, mOutputUavCpu);
    MakeUav(mLocalBuffer.Get(), kLocalFormat, mLocalUavCpu);
    MakeUav(mLocalRawBuffer.Get(), kLocalFormat, mLocalRawUavCpu);
    for (int p = 0; p < 2; ++p)
        for (int h = 0; h < 2; ++h)
            MakeUav(mLocalHistory[p][h].Get(), kLocalFormat, mLocalHistoryUavCpu[p][h]);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kOutputFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(mOutputBuffer.Get(), &srv, mOutputSrvCpu);
    srv.Format = kLocalFormat;
    dev->CreateShaderResourceView(mLocalBuffer.Get(), &srv, mLocalSrvCpu);

    return true;
}
