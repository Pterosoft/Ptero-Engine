// NrdDenoiser.cpp
// Pure D3D12 integration for NVIDIA NRD RELAX_DIFFUSE.
// Manages pipelines built from DXIL bytecode embedded in NRD.lib,
// pool textures, descriptor heaps, and per-frame dispatch execution.
//
// Integration strategy:
//  - Build one D3D12 PSO and root signature per NRD pipeline.
//  - Maintain a CPU-only descriptor heap for all resources (pool + external).
//  - Copy relevant CPU descriptors into a shader-visible heap each Denoise() call.
//  - Constant data is uploaded via a persistently-mapped UPLOAD ring buffer.

#include "pch.h"

// Prevent Windows min/max macros from colliding with std::min / std::max / std::clamp.
#undef min
#undef max

#include "NrdDenoiser.h"
#include "DX12Helper.h"

#include <cassert>
#include <cstring>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// Forward declaration of the DX12 device accessor (same as in RtGlobalIllumination.cpp).
extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

static ComPtr<ID3D12Resource> MakeNrdTexture(
    ID3D12Device* device,
    UINT width, UINT height,
    DXGI_FORMAT fmt,
    LPCWSTR name)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC d{};
    d.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width            = width;
    d.Height           = height;
    d.DepthOrArraySize = 1;
    d.MipLevels        = 1;
    d.Format           = fmt;
    d.SampleDesc.Count = 1;
    d.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> res;
    if (FAILED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&res))))
        return nullptr;
    if (name) res->SetName(name);
    return res;
}

// ─── Format conversion helpers ────────────────────────────────────────────────

DXGI_FORMAT NrdDenoiser::NrdToDxgi(nrd::Format f)
{
    switch (f)
    {
    case nrd::Format::R8_UNORM:           return DXGI_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM:           return DXGI_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT:            return DXGI_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT:            return DXGI_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM:          return DXGI_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM:          return DXGI_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT:           return DXGI_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT:           return DXGI_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM:        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM:        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT:         return DXGI_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT:         return DXGI_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB:         return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case nrd::Format::R16_UNORM:          return DXGI_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM:          return DXGI_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT:           return DXGI_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT:           return DXGI_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT:         return DXGI_FORMAT_R16_FLOAT;
    case nrd::Format::RG16_UNORM:         return DXGI_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM:         return DXGI_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT:          return DXGI_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT:          return DXGI_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT:        return DXGI_FORMAT_R16G16_FLOAT;
    case nrd::Format::RGBA16_UNORM:       return DXGI_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM:       return DXGI_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT:        return DXGI_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT:        return DXGI_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT:      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case nrd::Format::R32_UINT:           return DXGI_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT:           return DXGI_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT:         return DXGI_FORMAT_R32_FLOAT;
    case nrd::Format::RG32_UINT:          return DXGI_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT:          return DXGI_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT:        return DXGI_FORMAT_R32G32_FLOAT;
    case nrd::Format::RGBA32_UINT:        return DXGI_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT:        return DXGI_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT:  return DXGI_FORMAT_R10G10B10A2_UINT;
    case nrd::Format::R11_G11_B10_UFLOAT:   return DXGI_FORMAT_R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:   return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default:                              return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT NrdDenoiser::NormalEncodingToDxgi(nrd::NormalEncoding enc)
{
    switch (enc)
    {
    case nrd::NormalEncoding::RGBA8_UNORM:        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case nrd::NormalEncoding::RGBA8_SNORM:        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case nrd::NormalEncoding::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case nrd::NormalEncoding::RGBA16_UNORM:       return DXGI_FORMAT_R16G16B16A16_UNORM;
    case nrd::NormalEncoding::RGBA16_SNORM:       return DXGI_FORMAT_R16G16B16A16_SNORM;
    default:                                      return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

// ─── Initialize ───────────────────────────────────────────────────────────────

bool NrdDenoiser::Initialize(UINT width, UINT height, const NrdInitTextures& textures)
{
    Shutdown();
    mLastError.clear();

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "NrdDenoiser: null DX12 device.";
        return false;
    }

    mWidth  = width;
    mHeight = height;
    mInitTextures = textures;

    // ── Determine normal/roughness format from the NRD library descriptor ───
    const nrd::LibraryDesc* libDesc = nrd::GetLibraryDesc();
    if (!libDesc)
    {
        mLastError = "NrdDenoiser: nrd::GetLibraryDesc() returned null.";
        return false;
    }
    mNormalRoughnessFormat = NormalEncodingToDxgi(libDesc->normalEncoding);

    // ── Create the NRD instance with RELAX_DIFFUSE ───────────────────────────
    nrd::DenoiserDesc denoiserDesc{};
    denoiserDesc.identifier = kDenoiserId;
    denoiserDesc.denoiser   = nrd::Denoiser::RELAX_DIFFUSE;

    nrd::InstanceCreationDesc instDesc{};
    instDesc.denoisers    = &denoiserDesc;
    instDesc.denoisersNum = 1;

    if (nrd::CreateInstance(instDesc, mNrdInstance) != nrd::Result::SUCCESS)
    {
        mLastError = "NrdDenoiser: nrd::CreateInstance failed.";
        return false;
    }

    const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*mNrdInstance);

    mPermCount  = desc.permanentPoolSize;
    mTransCount = desc.transientPoolSize;
    mDescriptorStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ── Allocate pool textures ───────────────────────────────────────────────
    if (!CreatePoolTextures()) return false;

    // ── Build pipelines ──────────────────────────────────────────────────────
    if (!CreatePipelinesAndRootSignatures()) return false;

    // ── Build descriptor heaps ───────────────────────────────────────────────
    if (!CreateCpuDescriptorHeap()) return false;
    if (!CreateShaderVisibleDescriptorHeap()) return false;

    // ── Fill CPU-heap SRV/UAV for all pool textures ─────────────────────────
    // Layout: [perm_SRVs | perm_UAVs | trans_SRVs | trans_UAVs | ext_SRVs | ext_UAVs]
    auto FillTex = [&](ID3D12Resource* res, DXGI_FORMAT fmt,
                       UINT srvSlot, UINT uavSlot)
    {
        CreateTexDescriptors(device, res, fmt, srvSlot, uavSlot);
    };

    for (UINT i = 0; i < mPermCount; ++i)
    {
        DXGI_FORMAT fmt = NrdToDxgi(desc.permanentPool[i].format);
        FillTex(mPermanentTextures[i].Get(), fmt,
                i,              // SRV slot
                mPermCount + i); // UAV slot
    }
    for (UINT i = 0; i < mTransCount; ++i)
    {
        DXGI_FORMAT fmt = NrdToDxgi(desc.transientPool[i].format);
        UINT base = 2u * mPermCount;
        FillTex(mTransientTextures[i].Get(), fmt,
                base + i,
                base + mTransCount + i);
    }

    // External textures (inputs and output).
    UINT extBase = 2u * mPermCount + 2u * mTransCount;
    FillTex(textures.diffRadianceHitDist, textures.diffRadianceHitDistFmt,
            extBase + kExtDiffRadiance, extBase + kExtCount + kExtDiffRadiance);
    FillTex(textures.motionVectors, textures.motionVectorsFmt,
            extBase + kExtMotionVec,  extBase + kExtCount + kExtMotionVec);
    // NRD reads IN_NORMAL_ROUGHNESS as R10G10B10A2_UNORM (SRV), but R10G10B10A2_UNORM
    // does not support typed UAV stores. The texture is created as R10G10B10A2_TYPELESS so
    // we register the SRV as R10G10B10A2_UNORM and the UAV as R10G10B10A2_UINT.
    CreateTexDescriptors(device,
        textures.normalRoughness,
        /*srvFmt=*/DXGI_FORMAT_R10G10B10A2_UNORM,
        /*uavFmt=*/DXGI_FORMAT_R10G10B10A2_UINT,
        extBase + kExtNormalRoughness,
        extBase + kExtCount + kExtNormalRoughness);
    FillTex(textures.viewZ, textures.viewZFmt,
            extBase + kExtViewZ,  extBase + kExtCount + kExtViewZ);
    FillTex(textures.outDiffRadiance, textures.outDiffRadianceFmt,
            extBase + kExtOutDiff,extBase + kExtCount + kExtOutDiff);

    // ── Allocate constant buffer ring buffer ─────────────────────────────────
    // Each NRD dispatch can have its own constant data up to desc.constantBufferMaxDataSize.
    // D3D12 CBVs require 256-byte alignment, so round the slot size up accordingly.
    mCbSlotSize = std::max(256u, (desc.constantBufferMaxDataSize + 255u) & ~255u);

    // We need one slot per dispatch, all dispatches from one GetComputeDispatches() call.
    // Upper bound: max pipelinesNum dispatches (usually < 30 for RELAX).
    mCbSlotCount = std::max(desc.pipelinesNum * 2u, 64u);  // generous upper bound
    mCbRingSize  = mCbSlotCount * mCbSlotSize;

    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hp.CreationNodeMask = 1; hp.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width  = mCbRingSize;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&mCbRingBuffer))))
        {
            mLastError = "NrdDenoiser: failed to create constant buffer ring.";
            return false;
        }
        mCbRingBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mCbMapped));
        mCbRingBuffer->SetName(L"NRD_ConstantRing");
    }

    mIsInitialized = true;
    return true;
}

// ─── Pool texture creation ────────────────────────────────────────────────────

bool NrdDenoiser::CreatePoolTextures()
{
    ID3D12Device* device = DX12Context_GetDevice();
    const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*mNrdInstance);

    // Permanent textures live for the lifetime of the NrdDenoiser.
    mPermanentTextures.resize(desc.permanentPoolSize);
    mPermStates.resize(desc.permanentPoolSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (UINT i = 0; i < desc.permanentPoolSize; ++i)
    {
        DXGI_FORMAT fmt = NrdToDxgi(desc.permanentPool[i].format);
        UINT w = (mWidth  + desc.permanentPool[i].downsampleFactor - 1)
                 / desc.permanentPool[i].downsampleFactor;
        UINT h = (mHeight + desc.permanentPool[i].downsampleFactor - 1)
                 / desc.permanentPool[i].downsampleFactor;
        mPermanentTextures[i] = MakeNrdTexture(device, w, h, fmt, L"NRD_Perm");
        if (!mPermanentTextures[i])
        {
            mLastError = "NrdDenoiser: failed to create permanent pool texture.";
            return false;
        }
    }

    // Transient textures can theoretically be aliased per-frame but we allocate
    // them persistently for simplicity.
    mTransientTextures.resize(desc.transientPoolSize);
    mTransStates.resize(desc.transientPoolSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (UINT i = 0; i < desc.transientPoolSize; ++i)
    {
        DXGI_FORMAT fmt = NrdToDxgi(desc.transientPool[i].format);
        UINT w = (mWidth  + desc.transientPool[i].downsampleFactor - 1)
                 / desc.transientPool[i].downsampleFactor;
        UINT h = (mHeight + desc.transientPool[i].downsampleFactor - 1)
                 / desc.transientPool[i].downsampleFactor;
        mTransientTextures[i] = MakeNrdTexture(device, w, h, fmt, L"NRD_Trans");
        if (!mTransientTextures[i])
        {
            mLastError = "NrdDenoiser: failed to create transient pool texture.";
            return false;
        }
    }
    return true;
}

// ─── Pipeline creation ────────────────────────────────────────────────────────

bool NrdDenoiser::CreatePipelinesAndRootSignatures()
{
    ID3D12Device* device = DX12Context_GetDevice();
    const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*mNrdInstance);

    mPipelines.resize(desc.pipelinesNum);

    for (UINT pi = 0; pi < desc.pipelinesNum; ++pi)
    {
        const nrd::PipelineDesc& pd = desc.pipelines[pi];
        NrdPipeline& npl = mPipelines[pi];

        // Count SRV and UAV ranges from the resource range descriptors.
        npl.srvCount = 0;
        npl.uavCount = 0;
        for (UINT ri = 0; ri < pd.resourceRangesNum; ++ri)
        {
            if (pd.resourceRanges[ri].descriptorType == nrd::DescriptorType::TEXTURE)
                npl.srvCount += pd.resourceRanges[ri].descriptorsNum;
            else
                npl.uavCount += pd.resourceRanges[ri].descriptorsNum;
        }
        npl.hasConstantData = pd.hasConstantData;

        // ── Root signature ────────────────────────────────────────────────
        // Layout:
        //  [0] CBV inline  b0  space1  (constant data, optional)
        //  [1] SRV table   t0  space0  (all SRVs for this pass)
        //  [2] UAV table   u0  space0  (all UAVs for this pass)
        // Static samplers for NEAREST_CLAMP and LINEAR_CLAMP.

        std::vector<D3D12_ROOT_PARAMETER>       params;
        std::vector<D3D12_DESCRIPTOR_RANGE>     ranges;
        ranges.reserve(2);

        // CBV
        D3D12_ROOT_PARAMETER cbvParam{};
        cbvParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cbvParam.Descriptor.ShaderRegister = desc.constantBufferRegisterIndex;
        cbvParam.Descriptor.RegisterSpace  = desc.constantBufferAndSamplersSpaceIndex;
        cbvParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(cbvParam);

        // SRV table
        if (npl.srvCount > 0)
        {
            D3D12_DESCRIPTOR_RANGE srvRange{};
            srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRange.NumDescriptors     = npl.srvCount;
            srvRange.BaseShaderRegister = desc.resourcesBaseRegisterIndex;
            srvRange.RegisterSpace      = desc.resourcesSpaceIndex;
            ranges.push_back(srvRange);

            D3D12_ROOT_PARAMETER srvParam{};
            srvParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            srvParam.DescriptorTable = { 1, &ranges.back() };
            srvParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(srvParam);
        }

        // UAV table
        if (npl.uavCount > 0)
        {
            D3D12_DESCRIPTOR_RANGE uavRange{};
            uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRange.NumDescriptors     = npl.uavCount;
            uavRange.BaseShaderRegister = desc.resourcesBaseRegisterIndex;
            uavRange.RegisterSpace      = desc.resourcesSpaceIndex;
            ranges.push_back(uavRange);

            D3D12_ROOT_PARAMETER uavParam{};
            uavParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            uavParam.DescriptorTable = { 1, &ranges.back() };
            uavParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(uavParam);
        }

        // Static samplers: NEAREST_CLAMP (s0) and LINEAR_CLAMP (s1) in space1.
        D3D12_STATIC_SAMPLER_DESC staticSamplers[2]{};
        // Nearest-clamp
        staticSamplers[0].Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
        staticSamplers[0].AddressU = staticSamplers[0].AddressV = staticSamplers[0].AddressW
                                   = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplers[0].ShaderRegister = desc.samplersBaseRegisterIndex + 0;
        staticSamplers[0].RegisterSpace  = desc.constantBufferAndSamplersSpaceIndex;
        staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // Linear-clamp
        staticSamplers[1].Filter   = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        staticSamplers[1].AddressU = staticSamplers[1].AddressV = staticSamplers[1].AddressW
                                   = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplers[1].ShaderRegister = desc.samplersBaseRegisterIndex + 1;
        staticSamplers[1].RegisterSpace  = desc.constantBufferAndSamplersSpaceIndex;
        staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = static_cast<UINT>(params.size());
        rsDesc.pParameters       = params.data();
        rsDesc.NumStaticSamplers = 2;
        rsDesc.pStaticSamplers   = staticSamplers;

        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               &blob, &err)))
        {
            mLastError = "NrdDenoiser: D3D12SerializeRootSignature failed for pipeline "
                       + std::to_string(pi);
            return false;
        }
        if (FAILED(device->CreateRootSignature(
                0, blob->GetBufferPointer(), blob->GetBufferSize(),
                IID_PPV_ARGS(&npl.rootSig))))
        {
            mLastError = "NrdDenoiser: CreateRootSignature failed for pipeline "
                       + std::to_string(pi);
            return false;
        }

        // ── PSO ─────────────────────────────────────────────────────────────
        // Prefer DXIL bytecode (DX12 / modern); fall back to DXBC if unavailable.
        D3D12_SHADER_BYTECODE cs{};
        if (pd.computeShaderDXIL.bytecode && pd.computeShaderDXIL.size > 0)
        {
            cs.pShaderBytecode = pd.computeShaderDXIL.bytecode;
            cs.BytecodeLength  = static_cast<SIZE_T>(pd.computeShaderDXIL.size);
        }
        else if (pd.computeShaderDXBC.bytecode && pd.computeShaderDXBC.size > 0)
        {
            cs.pShaderBytecode = pd.computeShaderDXBC.bytecode;
            cs.BytecodeLength  = static_cast<SIZE_T>(pd.computeShaderDXBC.size);
        }
        else
        {
            mLastError = "NrdDenoiser: no usable bytecode for NRD pipeline "
                       + std::to_string(pi);
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = npl.rootSig.Get();
        psoDesc.CS             = cs;
        if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&npl.pso))))
        {
            mLastError = "NrdDenoiser: CreateComputePipelineState failed for pipeline "
                       + std::to_string(pi)
                       + " (" + pd.shaderIdentifier + ")";
            return false;
        }
    }
    return true;
}

// ─── Descriptor heap creation ─────────────────────────────────────────────────

bool NrdDenoiser::CreateCpuDescriptorHeap()
{
    ID3D12Device* device = DX12Context_GetDevice();

    // CPU heap layout (all CBV_SRV_UAV, non-shader-visible):
    //   [0              .. permCount-1            ]  permanent SRVs
    //   [permCount       .. 2*permCount-1           ]  permanent UAVs
    //   [2*permCount     .. 2*permCount+transCount-1]  transient SRVs
    //   [2*permCount+transCount .. 2*permCount+2*transCount-1] transient UAVs
    //   [2*permCount+2*transCount .. +kExtCount-1  ]  external SRVs
    //   [2*permCount+2*transCount+kExtCount .. +kExtCount-1] external UAVs
    mCpuHeapSize = 2u * mPermCount + 2u * mTransCount + 2u * kExtCount;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = mCpuHeapSize;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&mCpuHeap))))
    {
        mLastError = "NrdDenoiser: failed to create CPU descriptor heap.";
        return false;
    }
    return true;
}

bool NrdDenoiser::CreateShaderVisibleDescriptorHeap()
{
    ID3D12Device* device = DX12Context_GetDevice();
    const nrd::InstanceDesc& desc = *nrd::GetInstanceDesc(*mNrdInstance);

    // Shader-visible heap: accommodate totalTexturesNum + totalStorageTexturesNum
    // for all dispatches simultaneously (worst case per-frame copy).
    mSrvHeapCapacity = desc.descriptorPoolDesc.totalTexturesNum
                     + desc.descriptorPoolDesc.totalStorageTexturesNum;
    if (mSrvHeapCapacity == 0) mSrvHeapCapacity = 256; // safety

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = mSrvHeapCapacity;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&mSrvHeap))))
    {
        mLastError = "NrdDenoiser: failed to create shader-visible descriptor heap.";
        return false;
    }
    return true;
}

// ─── Descriptor helpers ───────────────────────────────────────────────────────

void NrdDenoiser::CreateTexDescriptors(
    ID3D12Device* device,
    ID3D12Resource* res,
    DXGI_FORMAT srvFmt,
    DXGI_FORMAT uavFmt,
    UINT srvSlot,
    UINT uavSlot)
{
    if (!res) return;

    auto cpuBase = mCpuHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                    = srvFmt;
    srvDesc.Texture2D.MipLevels       = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = cpuBase;
    srvHandle.ptr += static_cast<SIZE_T>(srvSlot) * mDescriptorStride;
    device->CreateShaderResourceView(res, &srvDesc, srvHandle);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format        = uavFmt;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpuBase;
    uavHandle.ptr += static_cast<SIZE_T>(uavSlot) * mDescriptorStride;
    device->CreateUnorderedAccessView(res, nullptr, &uavDesc, uavHandle);
}

void NrdDenoiser::CreateTexDescriptors(
    ID3D12Device* device,
    ID3D12Resource* res,
    DXGI_FORMAT fmt,
    UINT srvSlot,
    UINT uavSlot)
{
    CreateTexDescriptors(device, res, fmt, fmt, srvSlot, uavSlot);
}

// ─── GetCpuSlot ──────────────────────────────────────────────────────────────

UINT NrdDenoiser::GetCpuSlot(const nrd::ResourceDesc& r, bool wantSrv) const
{
    const bool  isPerm  = (r.type == nrd::ResourceType::PERMANENT_POOL);
    const bool  isTrans = (r.type == nrd::ResourceType::TRANSIENT_POOL);
    const UINT  idx     = r.indexInPool;

    if (isPerm)
    {
        return wantSrv ? idx : (mPermCount + idx);
    }
    if (isTrans)
    {
        const UINT base = 2u * mPermCount;
        return wantSrv ? (base + idx) : (base + mTransCount + idx);
    }

    // External resources — map by ResourceType.
    const UINT extBase = 2u * mPermCount + 2u * mTransCount;
    UINT extIdx = 0;
    switch (r.type)
    {
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: extIdx = kExtDiffRadiance;    break;
    case nrd::ResourceType::IN_MV:                    extIdx = kExtMotionVec;       break;
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:      extIdx = kExtNormalRoughness; break;
    case nrd::ResourceType::IN_VIEWZ:                 extIdx = kExtViewZ;           break;
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:extIdx = kExtOutDiff;         break;
    default:
        // Unrecognised external resource; use slot 0 (will likely produce wrong results
        // but avoids a crash).
        extIdx = 0;
        break;
    }
    return wantSrv ? (extBase + extIdx) : (extBase + kExtCount + extIdx);
}

// ─── Denoise ──────────────────────────────────────────────────────────────────

void NrdDenoiser::Denoise(
    ID3D12GraphicsCommandList* cmdList,
    const float viewToClipMatrix[16],
    const float viewToClipMatrixPrev[16],
    const float worldToViewMatrix[16],
    const float worldToViewMatrixPrev[16],
    const float cameraJitter[2],
    const float cameraJitterPrev[2],
    UINT        frameIndex,
    bool        resetHistory,
    float       timeDeltaMs)
{
    if (!mIsInitialized || !mNrdInstance) return;

    // External textures arrive from NrdPrepare in UAV state.
    // NRD will transition them as needed; we seed our state tracker accordingly.
    mExtStates[kExtDiffRadiance]    = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mExtStates[kExtMotionVec]       = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mExtStates[kExtNormalRoughness] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mExtStates[kExtViewZ]           = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mExtStates[kExtOutDiff]         = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // ── Apply RELAX denoiser settings ────────────────────────────────────────
    {
        nrd::RelaxSettings relaxSettings{};
        float maxFrames = std::max(1.0f, mMaxAccumTime * mFps);
        relaxSettings.diffuseMaxAccumulatedFrameNum     = static_cast<uint32_t>(maxFrames);
        relaxSettings.specularMaxAccumulatedFrameNum    = static_cast<uint32_t>(maxFrames);
        // Keep fast history very short for this noisy 1spp diffuse GI so motion responds faster.
        relaxSettings.diffuseMaxFastAccumulatedFrameNum = std::max(1u, static_cast<uint32_t>(maxFrames / 16.0f + 0.5f));
        relaxSettings.historyFixFrameNum                = 0;
        relaxSettings.atrousIterationNum                = static_cast<uint32_t>(
            std::clamp(mAtrousIterations, 2, 8));
        // Disable the large default prepass blur; it is too soft for this GI signal.
        relaxSettings.diffusePrepassBlurRadius          = 0.0f;
        // Tighten spatial edge stopping so indirect light does not wash across edges as much.
        relaxSettings.diffusePhiLuminance               = 0.65f;
        relaxSettings.lobeAngleFraction                 = 0.08f;
        relaxSettings.roughnessFraction                 = 0.10f;
        relaxSettings.diffuseMinLuminanceWeight         = 0.05f;
        relaxSettings.depthThreshold                    = mDisocclusionThreshold;
        relaxSettings.fastHistoryClampingSigmaScale     = 1.2f;
        relaxSettings.spatialVarianceEstimationHistoryThreshold = 1;
        relaxSettings.minHitDistanceWeight              = 0.2f;
        relaxSettings.luminanceEdgeStoppingRelaxation   = 0.15f;
        relaxSettings.normalEdgeStoppingRelaxation      = 0.1f;
        relaxSettings.roughnessEdgeStoppingRelaxation   = 0.5f;
        relaxSettings.enableAntiFirefly                 = true;
        relaxSettings.antilagSettings.accelerationAmount = 0.9f;
        relaxSettings.antilagSettings.spatialSigmaScale  = 2.0f;
        relaxSettings.antilagSettings.temporalSigmaScale = 0.15f;
        relaxSettings.antilagSettings.resetAmount        = 0.8f;
        // Use material roughness from the G-buffer to preserve edges instead of blurring everything.
        relaxSettings.enableRoughnessEdgeStopping       = true;

        nrd::SetDenoiserSettings(*mNrdInstance, kDenoiserId, &relaxSettings);
    }

    // ── Apply common settings ────────────────────────────────────────────────
    {
        nrd::CommonSettings cs{};

        // NRD expects COLUMN-MAJOR matrices in row-major float[16] storage
        // (i.e., the layout returned by XMStoreFloat4x4 with the XMMatrix directly).
        // The caller provides the matrices the same way as they are stored in XMFLOAT4X4.
        std::memcpy(cs.viewToClipMatrix,     viewToClipMatrix,     sizeof(float) * 16);
        std::memcpy(cs.viewToClipMatrixPrev, viewToClipMatrixPrev, sizeof(float) * 16);
        std::memcpy(cs.worldToViewMatrix,    worldToViewMatrix,    sizeof(float) * 16);
        std::memcpy(cs.worldToViewMatrixPrev,worldToViewMatrixPrev,sizeof(float) * 16);

        cs.frameIndex        = frameIndex;
        // RtGI_NrdPrepare outputs UV-space motion = prevUv - currUv, matching NRD's screen-space path.
        cs.isMotionVectorInWorldSpace = false; // we produce screen-space MVs
        cs.motionVectorScale[0] = 1.0f;
        cs.motionVectorScale[1] = 1.0f;
        cs.motionVectorScale[2] = 0.0f;
        if (cameraJitter && cameraJitterPrev)
        {
            cs.cameraJitter[0] = cameraJitter[0];
            cs.cameraJitter[1] = cameraJitter[1];
            cs.cameraJitterPrev[0] = cameraJitterPrev[0];
            cs.cameraJitterPrev[1] = cameraJitterPrev[1];
        }
        cs.isHistoryConfidenceAvailable = false;
        cs.disocclusionThreshold = mDisocclusionThreshold;
        cs.disocclusionThresholdAlternate = std::max(mDisocclusionThreshold, mDisocclusionThreshold * 2.0f);
        cs.accumulationMode  = resetHistory
            ? nrd::AccumulationMode::CLEAR_AND_RESTART
            : nrd::AccumulationMode::CONTINUE;
        cs.timeDeltaBetweenFrames = (timeDeltaMs > 0.0f) ? timeDeltaMs : (1000.0f / 60.0f);

        // Render resolution (no upscaling). Prev == current (no dynamic resolution).
        cs.resourceSize[0]     = mWidth;
        cs.resourceSize[1]     = mHeight;
        cs.resourceSizePrev[0] = mWidth;
        cs.resourceSizePrev[1] = mHeight;
        cs.rectSize[0]         = mWidth;
        cs.rectSize[1]         = mHeight;
        cs.rectSizePrev[0]     = mWidth;
        cs.rectSizePrev[1]     = mHeight;
        cs.rectOrigin[0]       = 0;
        cs.rectOrigin[1]       = 0;

        nrd::SetCommonSettings(*mNrdInstance, cs);
    }

    // ── Retrieve dispatches ───────────────────────────────────────────────────
    const nrd::DispatchDesc* dispatches  = nullptr;
    uint32_t                 dispatchNum = 0;
    {
        nrd::Identifier id = kDenoiserId;
        if (nrd::GetComputeDispatches(*mNrdInstance, &id, 1,
                                      dispatches, dispatchNum) != nrd::Result::SUCCESS)
            return;
    }

    // ── Execute all dispatches ────────────────────────────────────────────────
    // Reset the shader-visible heap offset so each frame starts from the beginning.
    mFrameHeapOffset = 0;
    mCbDispatchIndex = 0;
    for (uint32_t di = 0; di < dispatchNum; ++di)
        ExecuteDispatch(cmdList, dispatches[di]);
}

// ─── Helper: get the D3D12 resource and current tracked state for an NRD resource ───

static constexpr D3D12_RESOURCE_STATES kSrvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static constexpr D3D12_RESOURCE_STATES kUavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// ─── ExecuteDispatch ─────────────────────────────────────────────────────────

void NrdDenoiser::ExecuteDispatch(
    ID3D12GraphicsCommandList* cmdList,
    const nrd::DispatchDesc&   dispatch)
{
    ID3D12Device* device = DX12Context_GetDevice();
    const UINT pi = dispatch.pipelineIndex;
    const NrdPipeline& npl = mPipelines[pi];
    const nrd::InstanceDesc& instDesc = *nrd::GetInstanceDesc(*mNrdInstance);
    const nrd::PipelineDesc& pd = instDesc.pipelines[pi];

    // ── Issue resource state transitions ─────────────────────────────────────
    // NRD tells us which resources to bind as SRV (TEXTURE) or UAV (STORAGE_TEXTURE).
    // We track the current state of each resource and emit Transition barriers
    // for any mismatch before the dispatch.
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(dispatch.resourcesNum);

        auto TryTransition = [&](ID3D12Resource* res,
                                  D3D12_RESOURCE_STATES& currentState,
                                  D3D12_RESOURCE_STATES  wantedState)
        {
            if (currentState != wantedState)
            {
                barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                    res, currentState, wantedState));
                currentState = wantedState;
            }
        };

        for (UINT ri = 0; ri < dispatch.resourcesNum; ++ri)
        {
            const nrd::ResourceDesc& r = dispatch.resources[ri];
            const bool wantSrv = (r.descriptorType == nrd::DescriptorType::TEXTURE);
            D3D12_RESOURCE_STATES wantedState = wantSrv ? kSrvState : kUavState;

            if (r.type == nrd::ResourceType::PERMANENT_POOL)
            {
                TryTransition(mPermanentTextures[r.indexInPool].Get(),
                              mPermStates[r.indexInPool], wantedState);
            }
            else if (r.type == nrd::ResourceType::TRANSIENT_POOL)
            {
                TryTransition(mTransientTextures[r.indexInPool].Get(),
                              mTransStates[r.indexInPool], wantedState);
            }
            else
            {
                // External textures: map ResourceType to our kExt* index.
                UINT extIdx = 0;
                switch (r.type)
                {
                case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: extIdx = kExtDiffRadiance;    break;
                case nrd::ResourceType::IN_MV:                    extIdx = kExtMotionVec;       break;
                case nrd::ResourceType::IN_NORMAL_ROUGHNESS:      extIdx = kExtNormalRoughness; break;
                case nrd::ResourceType::IN_VIEWZ:                 extIdx = kExtViewZ;           break;
                case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:extIdx = kExtOutDiff;         break;
                default: continue;
                }
                // Resolve the actual resource pointer from the init textures.
                ID3D12Resource* extRes = nullptr;
                switch (extIdx)
                {
                case kExtDiffRadiance:    extRes = mInitTextures.diffRadianceHitDist; break;
                case kExtMotionVec:       extRes = mInitTextures.motionVectors;       break;
                case kExtNormalRoughness: extRes = mInitTextures.normalRoughness;     break;
                case kExtViewZ:           extRes = mInitTextures.viewZ;               break;
                case kExtOutDiff:         extRes = mInitTextures.outDiffRadiance; break;
                }
                if (extRes)
                    TryTransition(extRes, mExtStates[extIdx], wantedState);
            }
        }

        if (!barriers.empty())
            cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    // ── Compute SRV/UAV counts for this specific dispatch ────────────────────
    UINT srvCount = 0, uavCount = 0;
    for (UINT ri = 0; ri < pd.resourceRangesNum; ++ri)
    {
        if (pd.resourceRanges[ri].descriptorType == nrd::DescriptorType::TEXTURE)
            srvCount += pd.resourceRanges[ri].descriptorsNum;
        else
            uavCount += pd.resourceRanges[ri].descriptorsNum;
    }

    // ── Copy descriptors into the shader-visible heap ─────────────────────────
    if (mFrameHeapOffset + srvCount + uavCount > mSrvHeapCapacity)
        mFrameHeapOffset = 0; // wrap-around safety

    const UINT srvBaseInSrvHeap = mFrameHeapOffset;
    const UINT uavBaseInSrvHeap = mFrameHeapOffset + srvCount;
    mFrameHeapOffset += srvCount + uavCount;

    auto cpuSrvHeapBase = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    auto cpuCpuHeapBase = mCpuHeap->GetCPUDescriptorHandleForHeapStart();

    UINT srvFilled = 0, uavFilled = 0;
    for (UINT ri = 0; ri < dispatch.resourcesNum; ++ri)
    {
        const nrd::ResourceDesc& res = dispatch.resources[ri];
        bool isSrv = (res.descriptorType == nrd::DescriptorType::TEXTURE);

        UINT cpuSlot = GetCpuSlot(res, isSrv);
        D3D12_CPU_DESCRIPTOR_HANDLE srcHandle = cpuCpuHeapBase;
        srcHandle.ptr += static_cast<SIZE_T>(cpuSlot) * mDescriptorStride;

        if (isSrv)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dstHandle = cpuSrvHeapBase;
            dstHandle.ptr += static_cast<SIZE_T>(srvBaseInSrvHeap + srvFilled) * mDescriptorStride;
            device->CopyDescriptorsSimple(1, dstHandle, srcHandle,
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            ++srvFilled;
        }
        else
        {
            D3D12_CPU_DESCRIPTOR_HANDLE dstHandle = cpuSrvHeapBase;
            dstHandle.ptr += static_cast<SIZE_T>(uavBaseInSrvHeap + uavFilled) * mDescriptorStride;
            device->CopyDescriptorsSimple(1, dstHandle, srcHandle,
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            ++uavFilled;
        }
    }

    // ── Upload constant data ──────────────────────────────────────────────────
    D3D12_GPU_VIRTUAL_ADDRESS cbGpuVA = 0;
    if (npl.hasConstantData && dispatch.constantBufferDataSize > 0 && dispatch.constantBufferData)
    {
        UINT slot   = mCbDispatchIndex % mCbSlotCount;
        UINT offset = slot * mCbSlotSize;
        std::memcpy(mCbMapped + offset, dispatch.constantBufferData,
                    std::min(static_cast<UINT>(dispatch.constantBufferDataSize), mCbSlotSize));
        cbGpuVA = mCbRingBuffer->GetGPUVirtualAddress() + offset;
    }
    ++mCbDispatchIndex;

    // ── Set pipeline state ────────────────────────────────────────────────────
    cmdList->SetPipelineState(npl.pso.Get());
    cmdList->SetComputeRootSignature(npl.rootSig.Get());

    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Root param 0: CBV.
    if (npl.hasConstantData && cbGpuVA != 0)
        cmdList->SetComputeRootConstantBufferView(0, cbGpuVA);

    // Root param 1: SRV table.
    if (srvCount > 0)
    {
        auto gpuBase = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuBase.ptr += static_cast<SIZE_T>(srvBaseInSrvHeap) * mDescriptorStride;
        cmdList->SetComputeRootDescriptorTable(1, gpuBase);
    }

    // Root param 2: UAV table.
    if (uavCount > 0)
    {
        auto gpuBase = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuBase.ptr += static_cast<SIZE_T>(uavBaseInSrvHeap) * mDescriptorStride;
        UINT uavParamIndex = (srvCount > 0) ? 2 : 1;
        cmdList->SetComputeRootDescriptorTable(uavParamIndex, gpuBase);
    }

    // ── Dispatch ─────────────────────────────────────────────────────────────
    cmdList->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
}

// ─── Shutdown ─────────────────────────────────────────────────────────────────

void NrdDenoiser::Shutdown()
{
    if (mCbMapped && mCbRingBuffer)
    {
        mCbRingBuffer->Unmap(0, nullptr);
        mCbMapped = nullptr;
    }
    mCbRingBuffer.Reset();

    mPipelines.clear();
    mPermanentTextures.clear();
    mTransientTextures.clear();
    mPermStates.clear();
    mTransStates.clear();
    mCpuHeap.Reset();
    mSrvHeap.Reset();

    if (mNrdInstance)
    {
        nrd::DestroyInstance(*mNrdInstance);
        mNrdInstance = nullptr;
    }

    mIsInitialized = false;
    mPermCount     = 0;
    mTransCount    = 0;
    mWidth         = 0;
    mHeight        = 0;
}
