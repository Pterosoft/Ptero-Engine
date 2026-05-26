// XeGtaoRenderer.cpp
// Intel XeGTAO screen-space ambient occlusion renderer.

#include "pch.h"
#include "XeGtaoRenderer.h"

// Include the XeGTAO C++ side (constants update function, settings struct, HilbertIndex, etc.)
#include "..\..\Data\Shaders\XeGTAO.h"

#include <cstring>
#include <stdexcept>
#include <algorithm>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

static const wchar_t* kShaderPrefilter  = L"Data/Shaders/XeGTAO_PrefilterDepths.hlsl";
static const wchar_t* kShaderMainPass   = L"Data/Shaders/XeGTAO_MainPass.hlsl";
static const wchar_t* kShaderDenoise    = L"Data/Shaders/XeGTAO_Denoise.hlsl";
static const wchar_t* kShaderToFloat    = L"Data/Shaders/XeGTAO_ToFloat.hlsl";

namespace
{
    constexpr UINT kGroupSize = XE_GTAO_NUMTHREADS_X; // 8

    bool AllocDesc(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu, std::string& err)
    {
        if (!DX12Context_AllocateSrvDescriptor(&cpu, &gpu))
        {
            err = "XeGtaoRenderer: ran out of SRV descriptor heap slots.";
            return false;
        }
        return true;
    }

    ComPtr<ID3D12Resource> MakeTex2D(
        ID3D12Device* dev,
        UINT w, UINT h,
        DXGI_FORMAT fmt,
        D3D12_RESOURCE_STATES initialState,
        LPCWSTR name)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask  = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = w;
        desc.Height           = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = fmt;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> res;
        DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            initialState, nullptr, IID_PPV_ARGS(&res)));
        res->SetName(name);
        return res;
    }

    bool CompilePSO(
        ID3D12Device* dev,
        ID3D12RootSignature* rootSig,
        const wchar_t* filePath,
        const wchar_t* entry,
        const wchar_t* target,
        const std::vector<std::wstring>& defines,
        ComPtr<ID3D12PipelineState>& outPSO,
        std::string& err)
    {
        ShaderCompileRequest req{};
        req.FilePath       = filePath;
        req.EntryPoint     = entry;
        req.TargetProfile  = target;
        req.Stage          = ShaderStage::Compute;
        req.Defines        = defines;

        DX12Shader shader;
        if (!shader.Compile(req))
        {
            err = std::string("XeGtaoRenderer: shader compile failed ('")
                + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "?") + "')";
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = rootSig;
        psoDesc.CS = shader.GetBytecode();
        if (FAILED(dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPSO))))
        {
            err = "XeGtaoRenderer: CreateComputePipelineState failed.";
            return false;
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// Two separate upload constant buffers, each 256-byte aligned:
//   mConstantBuffer : GTAOConstants
//   mViewCB         : float4x4 worldToView
// ---------------------------------------------------------------------------
struct alignas(256) GtaoCBLayout
{
    XeGTAO::GTAOConstants gtao;
    uint8_t _pad[256 - sizeof(XeGTAO::GTAOConstants)];
};
static_assert(sizeof(GtaoCBLayout) == 256);

struct alignas(256) GtaoViewCBLayout
{
    float worldToView[16];
    float _pad[48];
};
static_assert(sizeof(GtaoViewCBLayout) == 256);

// ---------------------------------------------------------------------------
bool XeGtaoRenderer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0) return false;
    mLastError.clear();

    try
    {
        if (!mIsInitialized)
        {
            if (!CreateRootSignatures()) return false;
            if (!CreatePipelines())     return false;
        }

        mWidth  = width;
        mHeight = height;

        if (!CreateResolutionBuffers(width, height)) return false;
        if (!CreateDescriptors())                    return false;

        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError  = std::string("XeGtaoRenderer::Initialize: ") + ex.what();
        mInitFailed = true;
        return false;
    }
}

bool XeGtaoRenderer::EnsureSize(UINT width, UINT height)
{
    if (mIsInitialized && width == mWidth && height == mHeight)
        return true;
    return Initialize(width, height);
}

void XeGtaoRenderer::Shutdown()
{
    if (mMappedCB && mConstantBuffer)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCB = nullptr;
    }
    if (mMappedViewCB && mViewCB)
    {
        mViewCB->Unmap(0, nullptr);
        mMappedViewCB = nullptr;
    }

    mConstantBuffer.Reset();
    mViewCB.Reset();

    mViewspaceDepth.Reset();
    for (int i = 0; i < 2; ++i) mWorkingAO[i].Reset();
    mWorkingEdges.Reset();
    mOutputAO.Reset();

    mRsPrefilter.Reset();
    mRsMainPass.Reset();
    mRsDenoise.Reset();
    mRsToFloat.Reset();

    mPsoPrefilter.Reset();
    mPsoMainPassLow.Reset();
    mPsoMainPassMedium.Reset();
    mPsoMainPassHigh.Reset();
    mPsoMainPassUltra.Reset();
    mPsoDenoiseFirst.Reset();
    mPsoDenoiseSecond.Reset();
    mPsoToFloat.Reset();

    mIsInitialized  = false;
    mOutputInSrvState = false;
}

// ---------------------------------------------------------------------------
bool XeGtaoRenderer::CreateRootSignatures()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev) { mLastError = "XeGtaoRenderer: no D3D12 device."; return false; }

    auto MakeRS = [&](
        const D3D12_ROOT_PARAMETER* params, UINT numParams,
        const D3D12_STATIC_SAMPLER_DESC* samplers, UINT numSamplers,
        ComPtr<ID3D12RootSignature>& outRS) -> bool
    {
        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = numParams;
        rsDesc.pParameters       = params;
        rsDesc.NumStaticSamplers = numSamplers;
        rsDesc.pStaticSamplers   = samplers;
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> blob, errBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errBlob)))
        {
            mLastError = "XeGtaoRenderer: D3D12SerializeRootSignature failed.";
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&outRS))))
        {
            mLastError = "XeGtaoRenderer: CreateRootSignature failed.";
            return false;
        }
        return true;
    };

    D3D12_STATIC_SAMPLER_DESC pointClamp{};
    pointClamp.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    pointClamp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointClamp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointClamp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointClamp.ShaderRegister   = 0;
    pointClamp.RegisterSpace    = 0;
    pointClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // --- Prefilter root signature ---
    // b0: GTAOConstants, t0: source depth, u0-u4: MIP outputs
    {
        D3D12_DESCRIPTOR_RANGE srvR{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE uavR[5];
        for (int i = 0; i < 5; ++i)
            uavR[i] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, (UINT)i, 0, 0 };

        D3D12_ROOT_PARAMETER p[7]{};
        p[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        for (int i = 0; i < 1; ++i)
        {
            p[1+i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[1+i].DescriptorTable.NumDescriptorRanges = 1;
            p[1+i].DescriptorTable.pDescriptorRanges   = &srvR;
            p[1+i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        }
        for (int i = 0; i < 5; ++i)
        {
            p[2+i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[2+i].DescriptorTable.NumDescriptorRanges = 1;
            p[2+i].DescriptorTable.pDescriptorRanges   = &uavR[i];
            p[2+i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        }
        if (!MakeRS(p, 7, &pointClamp, 1, mRsPrefilter)) return false;
    }

    // --- MainPass root signature ---
    // b0: GTAOConstants, b1: ViewCB, t0: viewspace depth, t1: GBuffer normal, u0: AO term, u1: edges
    {
        D3D12_DESCRIPTOR_RANGE srvR0{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE srvR1{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_DESCRIPTOR_RANGE uavR0{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE uavR1{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0 };

        D3D12_ROOT_PARAMETER p[6]{};
        p[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].Descriptor.ShaderRegister = 1;
        p[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        p[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges   = &srvR0;
        p[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        p[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[3].DescriptorTable.NumDescriptorRanges = 1;
        p[3].DescriptorTable.pDescriptorRanges   = &srvR1;
        p[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        p[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[4].DescriptorTable.NumDescriptorRanges = 1;
        p[4].DescriptorTable.pDescriptorRanges   = &uavR0;
        p[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        p[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[5].DescriptorTable.NumDescriptorRanges = 1;
        p[5].DescriptorTable.pDescriptorRanges   = &uavR1;
        p[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        if (!MakeRS(p, 6, &pointClamp, 1, mRsMainPass)) return false;
    }

    // --- Denoise root signature ---
    // b0: GTAOConstants, t0: source AO, t1: edges, u0: output AO
    {
        D3D12_DESCRIPTOR_RANGE srvR0{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE srvR1{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_DESCRIPTOR_RANGE uavR0{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };

        D3D12_ROOT_PARAMETER p[4]{};
        p[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges   = &srvR0;
        p[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        p[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges   = &srvR1;
        p[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        p[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[3].DescriptorTable.NumDescriptorRanges = 1;
        p[3].DescriptorTable.pDescriptorRanges   = &uavR0;
        p[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        if (!MakeRS(p, 4, &pointClamp, 1, mRsDenoise)) return false;
    }

    // --- ToFloat root signature ---
    // t0: packed AO, u0: float output
    {
        D3D12_DESCRIPTOR_RANGE srvR0{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE uavR0{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };

        D3D12_ROOT_PARAMETER p[2]{};
        p[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[0].DescriptorTable.NumDescriptorRanges = 1;
        p[0].DescriptorTable.pDescriptorRanges   = &srvR0;
        p[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges   = &uavR0;
        p[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        if (!MakeRS(p, 2, nullptr, 0, mRsToFloat)) return false;
    }

    return true;
}

bool XeGtaoRenderer::CreatePipelines()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev) return false;

    const wchar_t* cs60 = L"cs_6_0";

    if (!CompilePSO(dev, mRsPrefilter.Get(), kShaderPrefilter, L"main", cs60, {}, mPsoPrefilter, mLastError)) return false;

    auto CompileMainPass = [&](int quality, ComPtr<ID3D12PipelineState>& pso) -> bool
    {
        wchar_t qval[4];
        swprintf(qval, 4, L"%d", quality);
        std::wstring def = std::wstring(L"XE_GTAO_QUALITY_LEVEL=") + qval;
        return CompilePSO(dev, mRsMainPass.Get(), kShaderMainPass, L"main", cs60, { def }, pso, mLastError);
    };

    if (!CompileMainPass(0, mPsoMainPassLow))    return false;
    if (!CompileMainPass(1, mPsoMainPassMedium)) return false;
    if (!CompileMainPass(2, mPsoMainPassHigh))   return false;
    if (!CompileMainPass(3, mPsoMainPassUltra))  return false;

    if (!CompilePSO(dev, mRsDenoise.Get(), kShaderDenoise, L"main", cs60, {},                                        mPsoDenoiseFirst,  mLastError)) return false;
    if (!CompilePSO(dev, mRsDenoise.Get(), kShaderDenoise, L"main", cs60, { L"XE_GTAO_DENOISE_LAST=1" },             mPsoDenoiseSecond, mLastError)) return false;
    if (!CompilePSO(dev, mRsToFloat.Get(), kShaderToFloat, L"main", cs60, {},                                        mPsoToFloat,       mLastError)) return false;

    // GTAO constants buffer
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask  = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = sizeof(GtaoCBLayout);
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mConstantBuffer)));
        DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mMappedCB));
        mConstantBuffer->SetName(L"XeGTAO_ConstantBuffer");
    }

    // View constant buffer (worldToView matrix)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask  = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = sizeof(GtaoViewCBLayout);
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mViewCB)));
        DX12_THROW_IF_FAILED(mViewCB->Map(0, nullptr, &mMappedViewCB));
        mViewCB->SetName(L"XeGTAO_ViewCB");
    }

    return true;
}

bool XeGtaoRenderer::CreateResolutionBuffers(UINT width, UINT height)
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev) return false;

    // New resources are always created in their initial states (UAV/etc.),
    // so reset the tracking flag regardless of what it was before.
    mOutputInSrvState = false;

    // Single mipmapped viewspace depth texture (5 MIP levels, R32_FLOAT).
    // XeGTAO_MainPass samples it with SampleLevel(..., mipLevel) so it MUST be one texture with HW mips.
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = width;
        rd.Height           = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 5;
        rd.Format           = DXGI_FORMAT_R32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        DX12_THROW_IF_FAILED(dev->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&mViewspaceDepth)));
        mViewspaceDepth->SetName(L"XeGTAO_ViewspaceDepth");
    }

    // Working AO term (R8_UINT packed) – two buffers for ping-pong denoise
    for (int i = 0; i < 2; ++i)
    {
        wchar_t name[64]; swprintf(name, 64, L"XeGTAO_WorkingAO_%d", i);
        mWorkingAO[i] = MakeTex2D(dev, width, height, DXGI_FORMAT_R8_UINT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    }

    // Working edges (R8_UNORM)
    mWorkingEdges = MakeTex2D(dev, width, height, DXGI_FORMAT_R8_UNORM,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"XeGTAO_WorkingEdges");

    // Final output (R8_UNORM)
    mOutputAO = MakeTex2D(dev, width, height, DXGI_FORMAT_R8_UNORM,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"XeGTAO_OutputAO");

    return true;
}

bool XeGtaoRenderer::CreateDescriptors()
{
    ID3D12Device* dev = DX12Context_GetDevice();
    if (!dev) return false;

    const UINT srvIncrSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Per-mip UAVs (one per mip level) + one all-mips SRV for MainPass
    for (int i = 0; i < 5; ++i)
    {
        if (!AllocDesc(mDepthMipUavCpu[i], mDepthMipUavGpu[i], mLastError)) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format                      = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension               = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice          = static_cast<UINT>(i);
        dev->CreateUnorderedAccessView(mViewspaceDepth.Get(), nullptr, &uavDesc, mDepthMipUavCpu[i]);
    }
    {
        if (!AllocDesc(mDepthAllMipsSrvCpu, mDepthAllMipsSrvGpu, mLastError)) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels       = 5; // expose all mips so MainPass can SampleLevel(..., N)
        dev->CreateShaderResourceView(mViewspaceDepth.Get(), &srvDesc, mDepthAllMipsSrvCpu);
    }

    // Working AO UAVs and SRVs
    for (int i = 0; i < 2; ++i)
    {
        if (!AllocDesc(mWorkingAOUavCpu[i], mWorkingAOUavGpu[i], mLastError)) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format        = DXGI_FORMAT_R8_UINT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(mWorkingAO[i].Get(), nullptr, &uavDesc, mWorkingAOUavCpu[i]);

        if (!AllocDesc(mWorkingAOSrvCpu[i], mWorkingAOSrvGpu[i], mLastError)) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                    = DXGI_FORMAT_R8_UINT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        dev->CreateShaderResourceView(mWorkingAO[i].Get(), &srvDesc, mWorkingAOSrvCpu[i]);
    }

    // Edges UAV + SRV
    {
        if (!AllocDesc(mEdgesUavCpu, mEdgesUavGpu, mLastError)) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format        = DXGI_FORMAT_R8_UNORM;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(mWorkingEdges.Get(), nullptr, &uavDesc, mEdgesUavCpu);

        if (!AllocDesc(mEdgesSrvCpu, mEdgesSrvGpu, mLastError)) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                    = DXGI_FORMAT_R8_UNORM;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        dev->CreateShaderResourceView(mWorkingEdges.Get(), &srvDesc, mEdgesSrvCpu);
    }

    // Output AO UAV + SRV
    {
        if (!AllocDesc(mOutputUavCpu, mOutputUavGpu, mLastError)) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format        = DXGI_FORMAT_R8_UNORM;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev->CreateUnorderedAccessView(mOutputAO.Get(), nullptr, &uavDesc, mOutputUavCpu);

        if (!AllocDesc(mOutputSrvCpu, mOutputSrvGpu, mLastError)) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format                    = DXGI_FORMAT_R8_UNORM;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        dev->CreateShaderResourceView(mOutputAO.Get(), &srvDesc, mOutputSrvCpu);
    }

    return true;
}

void XeGtaoRenderer::Dispatch(
    ID3D12GraphicsCommandList*  commandList,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
    const GtaoSettings&         settings,
    const float                 projMatrix[16],
    const float                 worldToView[16],
    UINT                        frameIndex)
{
    if (!mIsInitialized || !commandList) return;

    // Update constant buffers
    {
        XeGTAO::GTAOSettings xeSettings{};
        xeSettings.QualityLevel              = std::clamp(settings.QualityLevel, 0, 3);
        xeSettings.DenoisePasses             = std::clamp(settings.DenoisePasses, 0, 2);
        xeSettings.Radius                    = settings.Radius;
        xeSettings.RadiusMultiplier          = settings.RadiusMultiplier;
        xeSettings.FalloffRange              = settings.FalloffRange;
        xeSettings.SampleDistributionPower   = settings.SampleDistributionPower;
        xeSettings.ThinOccluderCompensation  = settings.ThinOccluderCompensation;
        xeSettings.FinalValuePower           = settings.FinalValuePower;
        xeSettings.DepthMIPSamplingOffset    = settings.DepthMIPSamplingOffset;

        GtaoCBLayout* cb = static_cast<GtaoCBLayout*>(mMappedCB);
        XeGTAO::GTAOUpdateConstants(
            cb->gtao,
            static_cast<int>(mWidth),
            static_cast<int>(mHeight),
            xeSettings,
            projMatrix,
            true,  // row-major
            frameIndex);

        GtaoViewCBLayout* vcb = static_cast<GtaoViewCBLayout*>(mMappedViewCB);
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                vcb->worldToView[r * 4 + c] = worldToView[c * 4 + r];
            }
        }
    }

    // Restore output AO from ALL_SHADER_RESOURCE → UAV if it was read last frame
    if (mOutputInSrvState)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(mOutputAO.Get(),
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &b);
        mOutputInSrvState = false;
    }

    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    if (heap) commandList->SetDescriptorHeaps(1, &heap);

    const D3D12_GPU_VIRTUAL_ADDRESS cbAddr     = mConstantBuffer->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS viewCBAddr = mViewCB->GetGPUVirtualAddress();

    auto UAVBarrier = [&](ID3D12Resource* r)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::UAV(r);
        commandList->ResourceBarrier(1, &b);
    };
    auto ToSrv = [&](ID3D12Resource* r)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(r,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &b);
    };
    auto ToUAV = [&](ID3D12Resource* r)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(r,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &b);
    };

    // ---- Pass 0: Prefilter depths ----
    {
        commandList->SetComputeRootSignature(mRsPrefilter.Get());
        commandList->SetPipelineState(mPsoPrefilter.Get());
        commandList->SetComputeRootConstantBufferView(0, cbAddr);
        commandList->SetComputeRootDescriptorTable(1, sceneDepthSrv);
        for (int i = 0; i < 5; ++i)
            commandList->SetComputeRootDescriptorTable(2 + i, mDepthMipUavGpu[i]);

        // Dispatch: one 8x8 group per 16x16 screen tile
        const UINT gx = (mWidth  + 15) / 16;
        const UINT gy = (mHeight + 15) / 16;
        commandList->Dispatch(gx, gy, 1);

        // Transition all 5 mip subresources UAV → SRV for MainPass
        {
            auto b = CD3DX12_RESOURCE_BARRIER::Transition(mViewspaceDepth.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                0xFFFFFFFFu);
            commandList->ResourceBarrier(1, &b);
        }
    }

    // ---- Pass 1: Main GTAO pass ----
    {
        commandList->SetComputeRootSignature(mRsMainPass.Get());

        ID3D12PipelineState* mainPso = mPsoMainPassHigh.Get();
        switch (std::clamp(settings.QualityLevel, 0, 3))
        {
            case 0: mainPso = mPsoMainPassLow.Get();    break;
            case 1: mainPso = mPsoMainPassMedium.Get(); break;
            case 2: mainPso = mPsoMainPassHigh.Get();   break;
            case 3: mainPso = mPsoMainPassUltra.Get();  break;
        }
        commandList->SetPipelineState(mainPso);
        commandList->SetComputeRootConstantBufferView(0, cbAddr);
        commandList->SetComputeRootConstantBufferView(1, viewCBAddr);
        commandList->SetComputeRootDescriptorTable(2, mDepthAllMipsSrvGpu); // t0: viewspace depth (all 5 mips)
        commandList->SetComputeRootDescriptorTable(3, gbufferNormalSrv);   // t1: GBuffer normal
        commandList->SetComputeRootDescriptorTable(4, mWorkingAOUavGpu[0]); // u0: working AO (ping)
        commandList->SetComputeRootDescriptorTable(5, mEdgesUavGpu);       // u1: edges

        const UINT gx = (mWidth  + kGroupSize - 1) / kGroupSize;
        const UINT gy = (mHeight + kGroupSize - 1) / kGroupSize;
        commandList->Dispatch(gx, gy, 1);

        UAVBarrier(mWorkingAO[0].Get());
        UAVBarrier(mWorkingEdges.Get());

        // Restore all 5 mips back to UAV for next frame's prefilter
        {
            auto b = CD3DX12_RESOURCE_BARRIER::Transition(mViewspaceDepth.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                0xFFFFFFFFu);
            commandList->ResourceBarrier(1, &b);
        }
    }

    // ---- Pass 2+: Spatial denoise ----
    const int denoisePasses = std::clamp(settings.DenoisePasses, 0, 2);
    if (denoisePasses == 0)
    {
        // No denoise: convert working AO[0] (UAV) → float output directly
        ToSrv(mWorkingAO[0].Get());

        commandList->SetComputeRootSignature(mRsToFloat.Get());
        commandList->SetPipelineState(mPsoToFloat.Get());
        commandList->SetComputeRootDescriptorTable(0, mWorkingAOSrvGpu[0]);
        commandList->SetComputeRootDescriptorTable(1, mOutputUavGpu);

        const UINT gx = (mWidth  + kGroupSize - 1) / kGroupSize;
        const UINT gy = (mHeight + kGroupSize - 1) / kGroupSize;
        commandList->Dispatch(gx, gy, 1);

        UAVBarrier(mOutputAO.Get());
        ToUAV(mWorkingAO[0].Get());
    }
    else
    {
        // Convert edges to SRV (needed by denoise)
        ToSrv(mWorkingEdges.Get());
        ToSrv(mWorkingAO[0].Get());

        commandList->SetComputeRootSignature(mRsDenoise.Get());

        const UINT gxD = (mWidth / 2 + kGroupSize - 1) / kGroupSize;
        const UINT gyD = (mHeight    + kGroupSize - 1) / kGroupSize;

        auto DoDenoisePass = [&](int ping, int pong, bool isLast)
        {
            commandList->SetPipelineState(isLast ? mPsoDenoiseSecond.Get() : mPsoDenoiseFirst.Get());
            commandList->SetComputeRootConstantBufferView(0, cbAddr);
            commandList->SetComputeRootDescriptorTable(1, mWorkingAOSrvGpu[ping]);
            commandList->SetComputeRootDescriptorTable(2, mEdgesSrvGpu);
            commandList->SetComputeRootDescriptorTable(3, mWorkingAOUavGpu[pong]);
            commandList->Dispatch(gxD, gyD, 1);
            UAVBarrier(mWorkingAO[pong].Get());
            ToSrv(mWorkingAO[pong].Get());
            ToUAV(mWorkingAO[ping].Get());
        };

        if (denoisePasses == 1)
        {
            DoDenoisePass(0, 1, true);
            // Convert final AO to float
            commandList->SetComputeRootSignature(mRsToFloat.Get());
            commandList->SetPipelineState(mPsoToFloat.Get());
            commandList->SetComputeRootDescriptorTable(0, mWorkingAOSrvGpu[1]);
            commandList->SetComputeRootDescriptorTable(1, mOutputUavGpu);
            const UINT gx = (mWidth  + kGroupSize - 1) / kGroupSize;
            const UINT gy = (mHeight + kGroupSize - 1) / kGroupSize;
            commandList->Dispatch(gx, gy, 1);
            UAVBarrier(mOutputAO.Get());
            ToUAV(mWorkingAO[1].Get());
        }
        else // 2 passes
        {
            DoDenoisePass(0, 1, false);
            DoDenoisePass(1, 0, true);
            commandList->SetComputeRootSignature(mRsToFloat.Get());
            commandList->SetPipelineState(mPsoToFloat.Get());
            commandList->SetComputeRootDescriptorTable(0, mWorkingAOSrvGpu[0]);
            commandList->SetComputeRootDescriptorTable(1, mOutputUavGpu);
            const UINT gx = (mWidth  + kGroupSize - 1) / kGroupSize;
            const UINT gy = (mHeight + kGroupSize - 1) / kGroupSize;
            commandList->Dispatch(gx, gy, 1);
            UAVBarrier(mOutputAO.Get());
            ToUAV(mWorkingAO[0].Get());
        }

        ToUAV(mWorkingEdges.Get());
    }

    // Transition output to ALL_SHADER_RESOURCE for the deferred lighting pass
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(mOutputAO.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &b);
        mOutputInSrvState = true;
    }
}
