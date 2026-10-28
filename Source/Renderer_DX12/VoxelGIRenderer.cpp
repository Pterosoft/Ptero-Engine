#include "pch.h"
#include "VoxelGIRenderer.h"

#include "EntityMeshRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static_assert(offsetof(VoxelGIConstants, PointLights) % 16 == 0,
              "VoxelGIConstants::PointLights must start on a cbuffer register like gPointLights");

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

namespace
{
    constexpr DXGI_FORMAT kAlbedoFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kNormalFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr DXGI_FORMAT kRadianceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kScreenFormat   = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kKeyFormat      = DXGI_FORMAT_R32_UINT;   // InterlockedMax needs a 32-bit integer

    constexpr UINT kVolumeGroupSize = 4;   // [numthreads(4, 4, 4)]
    constexpr UINT kScreenGroupSize = 8;   // [numthreads(8, 8, 1)]

    // Root parameter indices of the compute root signature.
    constexpr UINT kComputeCbv = 0;
    constexpr UINT kComputeSrv0 = 1;       // t0..t6 at 1..7
    constexpr UINT kComputeUav0 = 8;       // u0..u3 at 8..11
    constexpr UINT kComputeUavCount = 4;
    constexpr UINT kComputeVsmPool = 12;      // t40, the virtual shadow map pool
    constexpr UINT kComputeVsmPageTable = 13; // t41, its page table (root SRV)
    constexpr UINT kComputeRootCount = 14;

    // Root parameter indices of the voxelization root signature. 1 and the
    // texture tables from 3 on are the G-Buffer pass's, which EntityMeshRenderer
    // binds materials into - one table per material texture slot, so the UAVs
    // must come after however many slots it binds. Hard-coding them at 11 and 12
    // let the two slots added for the principled material overwrite the voxel
    // UAVs with texture SRVs, and the clipmap was never written.
    constexpr UINT kVoxelizeModel = 0;
    constexpr UINT kVoxelizeConstants = 2;
    constexpr UINT kVoxelizeTexture0 = 3;
    constexpr UINT kVoxelizeTextureCount = EntityMeshRenderer::kMaterialTextureSlotCount;
    constexpr UINT kVoxelizeAlbedoUav = kVoxelizeTexture0 + kVoxelizeTextureCount;
    constexpr UINT kVoxelizeNormalUav = kVoxelizeAlbedoUav + 1;
    constexpr UINT kVoxelizeRootCount = kVoxelizeNormalUav + 1;

    constexpr uint32_t kResolutionChoices[] = { 32u, 64u, 96u, 128u };

    constexpr D3D12_RESOURCE_STATES kAllShaderResource =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    UINT DivideRoundUp(UINT value, UINT divisor)
    {
        return (value + divisor - 1) / divisor;
    }

    uint32_t SnapResolution(int requested)
    {
        uint32_t best = kResolutionChoices[0];
        for (uint32_t choice : kResolutionChoices)
        {
            if (static_cast<int>(choice) <= (std::max)(requested, 32))
                best = choice;
        }
        return best;
    }

    bool CompileShader(DX12Shader& shader, const wchar_t* file, const wchar_t* entry,
                       const wchar_t* profile, ShaderStage stage, std::string& error)
    {
        ShaderCompileRequest request{};
        request.FilePath = file;
        request.EntryPoint = entry;
        request.TargetProfile = profile;
        request.Stage = stage;
        if (shader.Compile(request))
            return true;

        error = "VoxelGIRenderer: shader compile failed: ";
        error += shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown error";
        return false;
    }

    bool SerializeAndCreate(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
                            ComPtr<ID3D12RootSignature>& out, std::string& error)
    {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
        {
            error = "VoxelGIRenderer: D3D12SerializeRootSignature failed";
            if (errors)
                error += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&out))))
        {
            error = "VoxelGIRenderer: CreateRootSignature failed.";
            return false;
        }
        return true;
    }
}

bool VoxelGIRenderer::Initialize()
{
    if (mIsInitialized)
        return true;

    mLastError.clear();
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "VoxelGIRenderer: no D3D12 device.";
        mInitFailed = true;
        return false;
    }

    try
    {
        if (!CreateRootSignatures() || !CreatePipelines() || !AllocateDescriptors())
        {
            mInitFailed = true;
            return false;
        }

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = kConstantStride * kBlocksPerFrame * kFramesInFlight;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        DX12_THROW_IF_FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mConstantBuffer)));
        mConstantBuffer->SetName(L"VoxelGI_Constants");
        DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedConstants)));
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("VoxelGIRenderer::Initialize: ") + ex.what();
        mInitFailed = true;
        return false;
    }

    mIsInitialized = true;
    mInitFailed = false;
    return true;
}

void VoxelGIRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedConstants)
        mConstantBuffer->Unmap(0, nullptr);
    mMappedConstants = nullptr;
    mConstantBuffer.Reset();

    for (Texture* texture : { &mAlbedo, &mNormal, &mRadiance, &mIndirect, &mRadianceScratch, &mAlbedoKeys, &mNormalKeys,
                              &mTraceDiffuse, &mTraceReflection, &mDiffuseOut, &mSpecularOut, &mReflectionOut })
    {
        texture->Resource.Reset();
        texture->State = D3D12_RESOURCE_STATE_COMMON;
    }
    for (auto& pending : mPendingReleases)
        pending.clear();

    mVoxelizePso.Reset();
    mResolveKeysPso.Reset();
    mClearKeysPso.Reset();
    mClearVolumePso.Reset();
    mInjectPso.Reset();
    mBouncePso.Reset();
    mTracePso.Reset();
    mResolvePso.Reset();
    mVoxelizeRootSignature.Reset();
    mComputeRootSignature.Reset();

    // Descriptors come from the shared heap, which never frees; the slots stay
    // allocated and are rewritten if this renderer is initialised again.
    mIsInitialized = false;
    mInitFailed = false;
    mResolution = 0;
    mLevelCount = 0;
    mClipmapBytes = 0;
    mScreenWidth = 0;
    mScreenHeight = 0;
    mLevelOrigin = {};
    mIndirectOrigin = {};
    mLevelUpdateFrame = {};
    mVoxelSize = 0.0f;
}

bool VoxelGIRenderer::CreateRootSignatures()
{
    ID3D12Device* device = DX12Context_GetDevice();

    // ---- Voxelization (graphics) ------------------------------------------
    {
        D3D12_DESCRIPTOR_RANGE srvRanges[kVoxelizeTextureCount]{};
        D3D12_DESCRIPTOR_RANGE uavRanges[2]{};
        D3D12_ROOT_PARAMETER params[kVoxelizeRootCount]{};

        params[kVoxelizeModel].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[kVoxelizeModel].Constants.ShaderRegister = 0;
        params[kVoxelizeModel].Constants.Num32BitValues = 16;
        params[kVoxelizeModel].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b1 material
        params[1].Descriptor.ShaderRegister = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[kVoxelizeConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   // b2
        params[kVoxelizeConstants].Descriptor.ShaderRegister = 2;
        params[kVoxelizeConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        for (UINT i = 0; i < kVoxelizeTextureCount; ++i)
        {
            srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[i].NumDescriptors = 1;
            srvRanges[i].BaseShaderRegister = i;
            params[kVoxelizeTexture0 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kVoxelizeTexture0 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[kVoxelizeTexture0 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
            params[kVoxelizeTexture0 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        for (UINT i = 0; i < 2; ++i)
        {
            uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRanges[i].NumDescriptors = 1;
            uavRanges[i].BaseShaderRegister = i;
            params[kVoxelizeAlbedoUav + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kVoxelizeAlbedoUav + i].DescriptorTable.NumDescriptorRanges = 1;
            params[kVoxelizeAlbedoUav + i].DescriptorTable.pDescriptorRanges = &uavRanges[i];
            params[kVoxelizeAlbedoUav + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = static_cast<UINT>(std::size(params));
        desc.pParameters = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &sampler;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        if (!SerializeAndCreate(device, desc, mVoxelizeRootSignature, mLastError))
            return false;
    }

    // ---- Compute: b0, one table per t0-t6 and u0-u3 ------------------------
    {
        D3D12_DESCRIPTOR_RANGE srvRanges[7]{};
        D3D12_DESCRIPTOR_RANGE uavRanges[kComputeUavCount]{};
        D3D12_ROOT_PARAMETER params[kComputeRootCount]{};

        params[kComputeCbv].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[kComputeCbv].Descriptor.ShaderRegister = 0;
        params[kComputeCbv].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        for (UINT i = 0; i < 7; ++i)
        {
            srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[i].NumDescriptors = 1;
            srvRanges[i].BaseShaderRegister = i;
            params[kComputeSrv0 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kComputeSrv0 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[kComputeSrv0 + i].DescriptorTable.pDescriptorRanges = &srvRanges[i];
            params[kComputeSrv0 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        for (UINT i = 0; i < kComputeUavCount; ++i)
        {
            uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRanges[i].NumDescriptors = 1;
            uavRanges[i].BaseShaderRegister = i;
            params[kComputeUav0 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[kComputeUav0 + i].DescriptorTable.NumDescriptorRanges = 1;
            params[kComputeUav0 + i].DescriptorTable.pDescriptorRanges = &uavRanges[i];
            params[kComputeUav0 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        // The virtual shadow map, for InjectCS's light visibility (RtGI_Vsm.hlsli).
        D3D12_DESCRIPTOR_RANGE vsmPoolRange{};
        vsmPoolRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        vsmPoolRange.NumDescriptors = 1;
        vsmPoolRange.BaseShaderRegister = 40;
        params[kComputeVsmPool].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kComputeVsmPool].DescriptorTable.NumDescriptorRanges = 1;
        params[kComputeVsmPool].DescriptorTable.pDescriptorRanges = &vsmPoolRange;
        params[kComputeVsmPool].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[kComputeVsmPageTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[kComputeVsmPageTable].Descriptor.ShaderRegister = 41;
        params[kComputeVsmPageTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = static_cast<UINT>(std::size(params));
        desc.pParameters = params;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers = &sampler;
        if (!SerializeAndCreate(device, desc, mComputeRootSignature, mLastError))
            return false;
    }

    return true;
}

bool VoxelGIRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    constexpr const wchar_t* kVoxelizeFile = L"Data/Shaders/VoxelGI_Voxelize.hlsl";
    constexpr const wchar_t* kUpdateFile = L"Data/Shaders/VoxelGI_Update.hlsl";
    constexpr const wchar_t* kTraceFile = L"Data/Shaders/VoxelGI_Trace.hlsl";
    constexpr const wchar_t* kResolveFile = L"Data/Shaders/VoxelGI_Resolve.hlsl";

    // ---- Voxelization ------------------------------------------------------
    {
        DX12Shader vs, gs, ps;
        if (!CompileShader(vs, kVoxelizeFile, L"VSMain", L"vs_5_0", ShaderStage::Vertex, mLastError)
            || !CompileShader(gs, kVoxelizeFile, L"GSMain", L"gs_5_0", ShaderStage::Geometry, mLastError)
            || !CompileShader(ps, kVoxelizeFile, L"PSMain", L"ps_5_0", ShaderStage::Pixel, mLastError))
        {
            return false;
        }

        // Matches System/Mesh.h :: Vertex, as the G-Buffer pipeline does.
        const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        const bool conservative = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
            && options.ConservativeRasterizationTier != D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mVoxelizeRootSignature.Get();
        desc.VS = vs.GetBytecode();
        desc.GS = gs.GetBytecode();
        desc.PS = ps.GetBytecode();
        desc.SampleMask = UINT_MAX;
        desc.NumRenderTargets = 0;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        // Both faces: a voxel is solid whichever side its surface faces.
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        // Conservative rasterization keeps triangles thinner than a voxel - poles,
        // railings, distant walls in the coarse levels - from falling between
        // pixel centres and vanishing from the clipmap.
        desc.RasterizerState.ConservativeRaster = conservative
            ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
            : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;

        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&mVoxelizePso))))
        {
            mLastError = "VoxelGIRenderer: CreateGraphicsPipelineState failed for the voxelization pipeline.";
            return false;
        }
    }

    // ---- Compute -----------------------------------------------------------
    const auto makeCompute = [&](const wchar_t* file, const wchar_t* entry, ComPtr<ID3D12PipelineState>& out) -> bool
    {
        DX12Shader shader;
        if (!CompileShader(shader, file, entry, L"cs_5_0", ShaderStage::Compute, mLastError))
            return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mComputeRootSignature.Get();
        desc.CS = shader.GetBytecode();
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out))))
        {
            mLastError = "VoxelGIRenderer: CreateComputePipelineState failed for ";
            for (const wchar_t* c = entry; *c; ++c)
                mLastError += static_cast<char>(*c);
            return false;
        }
        return true;
    };

    return makeCompute(kResolveFile, L"ResolveKeysCS", mResolveKeysPso)
        && makeCompute(kResolveFile, L"ClearKeysCS", mClearKeysPso)
        && makeCompute(kUpdateFile, L"ClearVolumeCS", mClearVolumePso)
        && makeCompute(kUpdateFile, L"InjectCS", mInjectPso)
        && makeCompute(kUpdateFile, L"BounceCS", mBouncePso)
        && makeCompute(kTraceFile, L"TraceCS", mTracePso)
        && makeCompute(kTraceFile, L"ResolveCS", mResolvePso);
}

bool VoxelGIRenderer::AllocateDescriptors()
{
    if (mDescriptorsAllocated)
        return true;

    // Every texture gets its two slots once, for the life of the process: the
    // shared heap never frees, so a slot allocated per resize would drain it.
    for (Texture* texture : { &mAlbedo, &mNormal, &mRadiance, &mIndirect, &mRadianceScratch, &mAlbedoKeys, &mNormalKeys,
                              &mTraceDiffuse, &mTraceReflection, &mDiffuseOut, &mSpecularOut, &mReflectionOut })
    {
        if (!DX12Context_AllocateSrvDescriptor(&texture->SrvCpu, &texture->Srv)
            || !DX12Context_AllocateSrvDescriptor(&texture->UavCpu, &texture->Uav))
        {
            mLastError = "VoxelGIRenderer: ran out of SRV descriptor heap slots.";
            return false;
        }
    }

    mDescriptorsAllocated = true;
    return true;
}

void VoxelGIRenderer::CreateTexture3D(Texture& texture, UINT width, UINT height, UINT depth,
                                      DXGI_FORMAT format, const wchar_t* name)
{
    ID3D12Device* device = DX12Context_GetDevice();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

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

    // Earlier frames may still be reading the texture this replaces.
    if (texture.Resource)
        mPendingReleases[mFrameSlot].push_back(std::move(texture.Resource));

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture.Resource)));
    texture.Resource->SetName(name);
    texture.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    WriteViews(texture, true);
}

void VoxelGIRenderer::CreateTexture2D(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name)
{
    ID3D12Device* device = DX12Context_GetDevice();

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
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (texture.Resource)
        mPendingReleases[mFrameSlot].push_back(std::move(texture.Resource));

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture.Resource)));
    texture.Resource->SetName(name);
    texture.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    WriteViews(texture, false);
}

void VoxelGIRenderer::WriteViews(Texture& texture, bool volume)
{
    ID3D12Device* device = DX12Context_GetDevice();
    const D3D12_RESOURCE_DESC resourceDesc = texture.Resource->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = resourceDesc.Format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = resourceDesc.Format;

    if (volume)
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = 1;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.WSize = resourceDesc.DepthOrArraySize;
    }
    else
    {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }

    device->CreateShaderResourceView(texture.Resource.Get(), &srv, texture.SrvCpu);
    device->CreateUnorderedAccessView(texture.Resource.Get(), nullptr, &uav, texture.UavCpu);
}

bool VoxelGIRenderer::EnsureClipmap(ID3D12GraphicsCommandList* cmdList, uint32_t resolution, uint32_t levelCount)
{
    if (mRadiance.Resource && resolution == mResolution && levelCount == mLevelCount)
        return true;

    try
    {
        const UINT depth = resolution * levelCount;
        CreateTexture3D(mAlbedo, resolution, resolution, depth, kAlbedoFormat, L"VoxelGI_Albedo");
        CreateTexture3D(mNormal, resolution, resolution, depth, kNormalFormat, L"VoxelGI_Normal");
        CreateTexture3D(mRadiance, resolution, resolution, depth, kRadianceFormat, L"VoxelGI_Radiance");
        CreateTexture3D(mIndirect, resolution, resolution, depth, kRadianceFormat, L"VoxelGI_Indirect");
        CreateTexture3D(mRadianceScratch, resolution, resolution, resolution, kRadianceFormat, L"VoxelGI_RadianceScratch");
        CreateTexture3D(mAlbedoKeys, resolution, resolution, resolution, kKeyFormat, L"VoxelGI_AlbedoKeys");
        CreateTexture3D(mNormalKeys, resolution, resolution, resolution, kKeyFormat, L"VoxelGI_NormalKeys");
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("VoxelGIRenderer: clipmap allocation failed: ") + ex.what();
        mRadiance.Resource.Reset();
        return false;
    }

    mResolution = resolution;
    mLevelCount = levelCount;
    const uint64_t voxels = static_cast<uint64_t>(resolution) * resolution * resolution * levelCount;
    mClipmapBytes = voxels * (4 + 4 + 8 + 8) + static_cast<uint64_t>(resolution) * resolution * resolution * (8 + 4 + 4);

    // Nothing in the new volumes is valid yet: every level is refilled this frame.
    mLevelOrigin = {};
    mIndirectOrigin = {};
    mLevelUpdateFrame = {};

    // Committed resources are not guaranteed to start zeroed, and the radiance
    // and indirect volumes are read before every level has written them.
    cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(kComputeCbv, mConstantBuffer->GetGPUVirtualAddress());
    cmdList->SetPipelineState(mClearVolumePso.Get());
    for (Texture* texture : { &mAlbedo, &mNormal, &mRadiance, &mIndirect })
    {
        cmdList->SetComputeRootDescriptorTable(kComputeUav0, texture->Uav);
        cmdList->Dispatch(DivideRoundUp(resolution, kVolumeGroupSize), DivideRoundUp(resolution, kVolumeGroupSize),
                          DivideRoundUp(resolution * levelCount, kVolumeGroupSize));
        UavBarrier(cmdList, *texture);
    }

    // The keys start at zero and ResolveKeysCS leaves them so after every level.
    cmdList->SetPipelineState(mClearKeysPso.Get());
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 2, mAlbedoKeys.Uav);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 3, mNormalKeys.Uav);
    cmdList->Dispatch(DivideRoundUp(resolution, kVolumeGroupSize), DivideRoundUp(resolution, kVolumeGroupSize),
                      DivideRoundUp(resolution, kVolumeGroupSize));
    UavBarrier(cmdList, mAlbedoKeys);
    UavBarrier(cmdList, mNormalKeys);
    return true;
}

bool VoxelGIRenderer::EnsureScreenTextures(UINT width, UINT height, bool halfResolution)
{
    if (mDiffuseOut.Resource && width == mScreenWidth && height == mScreenHeight && halfResolution == mHalfResolution)
        return true;

    const UINT traceWidth = halfResolution ? (width + 1) / 2 : width;
    const UINT traceHeight = halfResolution ? (height + 1) / 2 : height;

    try
    {
        CreateTexture2D(mTraceDiffuse, traceWidth, traceHeight, kScreenFormat, L"VoxelGI_TraceDiffuse");
        CreateTexture2D(mTraceReflection, traceWidth, traceHeight, kScreenFormat, L"VoxelGI_TraceReflection");
        CreateTexture2D(mDiffuseOut, width, height, kScreenFormat, L"VoxelGI_Diffuse");
        CreateTexture2D(mSpecularOut, width, height, kScreenFormat, L"VoxelGI_Specular");
        CreateTexture2D(mReflectionOut, width, height, kScreenFormat, L"VoxelGI_Reflection");
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("VoxelGIRenderer: screen texture allocation failed: ") + ex.what();
        mDiffuseOut.Resource.Reset();
        return false;
    }

    mScreenWidth = width;
    mScreenHeight = height;
    mHalfResolution = halfResolution;
    return true;
}

void VoxelGIRenderer::Transition(ID3D12GraphicsCommandList* cmdList, Texture& texture, D3D12_RESOURCE_STATES state)
{
    if (!texture.Resource || texture.State == state)
        return;
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Resource.Get(), texture.State, state);
    cmdList->ResourceBarrier(1, &barrier);
    texture.State = state;
}

void VoxelGIRenderer::UavBarrier(ID3D12GraphicsCommandList* cmdList, Texture& texture)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(texture.Resource.Get());
    cmdList->ResourceBarrier(1, &barrier);
}

D3D12_GPU_VIRTUAL_ADDRESS VoxelGIRenderer::WriteConstants(uint32_t block, const VoxelGIConstants& constants)
{
    const UINT64 offset = (static_cast<UINT64>(mFrameSlot) * kBlocksPerFrame + block) * kConstantStride;
    std::memcpy(mMappedConstants + offset, &constants, sizeof(VoxelGIConstants));
    return mConstantBuffer->GetGPUVirtualAddress() + offset;
}

void VoxelGIRenderer::UpdateLevel(
    ID3D12GraphicsCommandList* cmdList,
    EntityMeshRenderer&        meshes,
    const VoxelGISettings&     settings,
    VoxelGIConstants&          constants,
    uint32_t                   level,
    uint32_t                   block,
    bool                       keepHistory)
{
    const float voxelSize = mVoxelSize * static_cast<float>(1u << level);
    const float half = 0.5f * static_cast<float>(mResolution);
    const XMFLOAT3 camera{ constants.CameraPos[0], constants.CameraPos[1], constants.CameraPos[2] };

    // Snapped to this level's voxel grid, which is anchored at the world
    // origin, so a coarse voxel always spans exactly 2x2x2 finer ones.
    const XMFLOAT3 origin{
        (std::floor(camera.x / voxelSize) - half) * voxelSize,
        (std::floor(camera.y / voxelSize) - half) * voxelSize,
        (std::floor(camera.z / voxelSize) - half) * voxelSize };

    constants.UpdateLevel = level;
    constants.LevelOrigin[level][0] = origin.x;
    constants.LevelOrigin[level][1] = origin.y;
    constants.LevelOrigin[level][2] = origin.z;
    constants.LevelOrigin[level][3] = voxelSize;

    // Where the level's current content sits, for InjectCS to blend with. Not
    // after a reset, nor across a voxel size change (w is the old voxel size).
    const XMFLOAT4& previous = mLevelOrigin[level];
    const bool history = keepHistory && previous.w == voxelSize;
    constants.PrevLevelOrigin[0] = previous.x;
    constants.PrevLevelOrigin[1] = previous.y;
    constants.PrevLevelOrigin[2] = previous.z;
    constants.PrevLevelOrigin[3] = history ? 1.0f : 0.0f;

    // The blend is a per-frame rate: a level refreshed every n frames takes
    // what n frames at that rate would have taken, so every level follows a
    // change in the light equally fast however rarely its turn comes.
    const float perFrame = std::clamp(settings.TemporalBlend, 0.01f, 1.0f);
    const uint32_t framesSince = history ? (std::max)(mFrameCounter - mLevelUpdateFrame[level], 1u) : 1u;
    constants.TemporalBlend = history
        ? 1.0f - std::pow(1.0f - perFrame, static_cast<float>(framesSince))
        : 1.0f;
    mLevelUpdateFrame[level] = mFrameCounter;

    const D3D12_GPU_VIRTUAL_ADDRESS cb = WriteConstants(block, constants);

    const UINT groups = DivideRoundUp(mResolution, kVolumeGroupSize);

    // 1. Voxelize the meshes that reach into it, into the keys - which the
    // resolve below left at zero.
    Transition(cmdList, mAlbedoKeys, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mNormalKeys, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->SetGraphicsRootSignature(mVoxelizeRootSignature.Get());
    cmdList->SetPipelineState(mVoxelizePso.Get());
    cmdList->SetGraphicsRootConstantBufferView(kVoxelizeConstants, cb);
    cmdList->SetGraphicsRootDescriptorTable(kVoxelizeAlbedoUav, mAlbedoKeys.Uav);
    cmdList->SetGraphicsRootDescriptorTable(kVoxelizeNormalUav, mNormalKeys.Uav);
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mResolution), static_cast<float>(mResolution), 0.0f, 1.0f };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mResolution), static_cast<LONG>(mResolution) };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);

    const float extent = voxelSize * static_cast<float>(mResolution);
    const XMFLOAT3 regionMax{ origin.x + extent, origin.y + extent, origin.z + extent };
    const std::size_t lod = static_cast<std::size_t>((std::max)(settings.LodBias, 0)) + level;
    meshes.RenderVoxelization(cmdList, origin, regionMax, lod, camera);

    // The UAV barriers order the pixel shader's atomics before the resolve reads them.
    UavBarrier(cmdList, mAlbedoKeys);
    UavBarrier(cmdList, mNormalKeys);

    // 2. Resolve the keys into the level's albedo and normal slices, every
    // voxel of it, and zero them for the next level.
    Transition(cmdList, mAlbedo, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mNormal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(kComputeCbv, cb);
    cmdList->SetPipelineState(mResolveKeysPso.Get());
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 0, mAlbedo.Uav);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 1, mNormal.Uav);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 2, mAlbedoKeys.Uav);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 3, mNormalKeys.Uav);
    cmdList->Dispatch(groups, groups, groups);
    UavBarrier(cmdList, mAlbedo);
    UavBarrier(cmdList, mNormal);
    UavBarrier(cmdList, mAlbedoKeys);
    UavBarrier(cmdList, mNormalKeys);

    // 3. Light it into the scratch level.
    Transition(cmdList, mAlbedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mNormal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mRadiance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mIndirect, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mRadianceScratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(kComputeCbv, cb);
    cmdList->SetPipelineState(mInjectPso.Get());
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 0, mAlbedo.Srv);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 1, mNormal.Srv);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 2, mRadiance.Srv);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 3, mIndirect.Srv);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0, mRadianceScratch.Uav);
    // t40 / t41: the virtual shadow map, read only when the constants say it is
    // bound; the albedo volume and the constants stand in otherwise.
    cmdList->SetComputeRootDescriptorTable(kComputeVsmPool, mVsmPoolSrv.ptr != 0 ? mVsmPoolSrv : mAlbedo.Srv);
    cmdList->SetComputeRootShaderResourceView(kComputeVsmPageTable, mVsmPageTable != 0 ? mVsmPageTable : cb);
    cmdList->Dispatch(groups, groups, groups);

    // 4. Copy it into the level's slices. The inject pass reads the finer level
    // from the same volume, so it cannot write there directly.
    Transition(cmdList, mRadianceScratch, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmdList, mRadiance, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = mRadiance.Resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = mRadianceScratch.Resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cmdList->CopyTextureRegion(&dst, 0, 0, level * mResolution, &src, nullptr);
    }
    Transition(cmdList, mRadiance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 5. Gather the light each voxel receives, for the next update's bounce.
    if (settings.MultiBounce > 0.0f)
    {
        Transition(cmdList, mIndirect, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->SetPipelineState(mBouncePso.Get());
        cmdList->SetComputeRootDescriptorTable(kComputeUav0, mIndirect.Uav);
        cmdList->Dispatch(groups, groups, groups);
        Transition(cmdList, mIndirect, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        mIndirectOrigin[level] = XMFLOAT4(origin.x, origin.y, origin.z, 1.0f);
    }
    else
    {
        mIndirectOrigin[level].w = 0.0f;
    }

    mLevelOrigin[level] = XMFLOAT4(origin.x, origin.y, origin.z, voxelSize);
    std::memcpy(constants.IndirectOrigin[level], &mIndirectOrigin[level], sizeof(float) * 4);
}

bool VoxelGIRenderer::Render(
    ID3D12GraphicsCommandList* cmdList,
    EntityMeshRenderer&        meshes,
    const VoxelGISettings&     settings,
    const FrameInputs&         inputs)
{
    if (!mIsInitialized || !cmdList || inputs.Width == 0 || inputs.Height == 0)
        return false;

    ID3D12DescriptorHeap* srvHeap = DX12Context_GetSrvDescriptorHeap();
    if (!srvHeap)
        return false;
    mLastError.clear();

    // Advance the frame slot and free what was retired the last time round.
    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;
    mPendingReleases[mFrameSlot].clear();
    ++mFrameCounter;

    cmdList->SetDescriptorHeaps(1, &srvHeap);

    const uint32_t resolution = SnapResolution(settings.Resolution);
    const uint32_t levelCount = static_cast<uint32_t>(std::clamp(settings.LevelCount, 2, VoxelGIConstants::kMaxLevels));
    const float voxelSize = std::clamp(settings.VoxelSize, 0.05f, 2.0f);
    const bool halfResolution = settings.HalfResolution;

    if (!EnsureClipmap(cmdList, resolution, levelCount) || !EnsureScreenTextures(inputs.Width, inputs.Height, halfResolution))
        return false;

    bool resetAll = inputs.ResetClipmap;
    if (voxelSize != mVoxelSize)
    {
        mVoxelSize = voxelSize;
        resetAll = true;
    }

    // ---- Constants shared by every pass this frame --------------------------
    VoxelGIConstants constants{};
    for (int level = 0; level < VoxelGIConstants::kMaxLevels; ++level)
    {
        std::memcpy(constants.LevelOrigin[level], &mLevelOrigin[level], sizeof(float) * 4);
        std::memcpy(constants.IndirectOrigin[level], &mIndirectOrigin[level], sizeof(float) * 4);
    }
    if (inputs.ViewProjInv)
        std::memcpy(constants.ViewProjInv, inputs.ViewProjInv, sizeof(constants.ViewProjInv));
    constants.CameraPos[0] = inputs.CameraPos.x;
    constants.CameraPos[1] = inputs.CameraPos.y;
    constants.CameraPos[2] = inputs.CameraPos.z;
    constants.MaxTraceDistance = (std::max)(settings.MaxTraceDistance, 1.0f);
    std::memcpy(constants.SunDir, inputs.SunDir, sizeof(constants.SunDir));
    constants.SurfaceOffset = std::clamp(settings.SurfaceOffset, 0.0f, 4.0f);
    std::memcpy(constants.SunColor, inputs.SunColor, sizeof(constants.SunColor));
    constants.MultiBounce = std::clamp(settings.MultiBounce, 0.0f, 1.0f);
    std::memcpy(constants.SkyColor, inputs.SkyColor, sizeof(constants.SkyColor));
    constants.Sky = inputs.Sky;
    constants.SpecularRoughnessThreshold = std::clamp(settings.SpecularRoughnessThreshold, 0.01f, 1.0f);
    constants.Resolution = resolution;
    constants.LevelCount = levelCount;
    constants.DiffuseCones = settings.DiffuseCones <= 1 ? 1u : (settings.DiffuseCones >= 9 ? 9u : 5u);
    constants.FullWidth = inputs.Width;
    constants.FullHeight = inputs.Height;
    constants.TraceWidth = halfResolution ? (inputs.Width + 1) / 2 : inputs.Width;
    constants.TraceHeight = halfResolution ? (inputs.Height + 1) / 2 : inputs.Height;
    constants.InvResolution = 1.0f / static_cast<float>(resolution);
    constants.SunShadows = settings.SunShadows ? 1u : 0u;
    constants.LocalLightShadows = settings.LocalLightShadows ? 1u : 0u;
    constants.DebugView = settings.DebugView;
    constants.NumPointLights = static_cast<int32_t>((std::min)(inputs.NumPointLights, static_cast<uint32_t>(VoxelGIConstants::kMaxPointLights)));
    constants.HalfResolution = halfResolution ? 1u : 0u;
    constants.SpecularEnabled = settings.SpecularEnabled ? 1u : 0u;
    constants.SpecularIntensity = settings.SpecularIntensity;
    const bool vsmBound = inputs.Vsm != nullptr && inputs.VsmPageTable != 0 && inputs.VsmPoolSrv.ptr != 0;
    constants.VsmMaxTexelSize = std::clamp(settings.VsmMaxTexelSize, 0.005f, 0.5f);
    constants.VsmVisibility = (settings.VsmVisibility && vsmBound) ? 1 : 0;
    constants.LightSamples = settings.StochasticLights ? std::clamp(settings.LightSamples, 1, 4) : 0;
    constants.LightSampleSeed = mFrameCounter * 2654435761u;
    if (vsmBound)
        constants.Vsm = *inputs.Vsm;
    mVsmPoolSrv = vsmBound ? inputs.VsmPoolSrv : D3D12_GPU_DESCRIPTOR_HANDLE{};
    mVsmPageTable = vsmBound ? inputs.VsmPageTable : 0;
    if (inputs.PointLights && constants.NumPointLights > 0)
        std::memcpy(constants.PointLights, inputs.PointLights, sizeof(DeferredLightingPass::PointLightGpu) * constants.NumPointLights);

    // ---- Clipmap levels due this frame ---------------------------------------
    // The finest level every other frame, the others in turn on the frames in
    // between. A level the camera has moved far through since it was last
    // filled - a teleport, a level load - is refilled at once, whatever its turn.
    const uint32_t scheduled = (mFrameCounter & 1u) == 0u || levelCount < 2
        ? 0u
        : 1u + (mCoarseCursor++ % (levelCount - 1));

    uint32_t block = 0;
    for (uint32_t level = 0; level < levelCount; ++level)
    {
        const float levelVoxel = voxelSize * static_cast<float>(1u << level);
        const XMFLOAT4& current = mLevelOrigin[level];
        bool due = resetAll || level == scheduled || current.w != levelVoxel;
        if (!due)
        {
            const float center = 0.5f * static_cast<float>(resolution) * levelVoxel;
            const float limit = 0.25f * static_cast<float>(resolution) * levelVoxel;
            due = std::fabs(current.x + center - inputs.CameraPos.x) > limit
               || std::fabs(current.y + center - inputs.CameraPos.y) > limit
               || std::fabs(current.z + center - inputs.CameraPos.z) > limit;
        }
        if (due)
            UpdateLevel(cmdList, meshes, settings, constants, level, block++, !resetAll);
    }

    // ---- Screen --------------------------------------------------------------
    constants.UpdateLevel = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = WriteConstants(VoxelGIConstants::kMaxLevels, constants);

    Transition(cmdList, mAlbedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mRadiance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mTraceDiffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mTraceReflection, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(kComputeCbv, cb);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 0, inputs.GBufferAlbedo);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 1, inputs.GBufferNormal);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 2, inputs.GBufferMaterial);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 3, mRadiance.Srv);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 4, mAlbedo.Srv);

    cmdList->SetPipelineState(mTracePso.Get());
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 0, mTraceDiffuse.Uav);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 1, mTraceReflection.Uav);
    cmdList->Dispatch(DivideRoundUp(constants.TraceWidth, kScreenGroupSize), DivideRoundUp(constants.TraceHeight, kScreenGroupSize), 1);

    Transition(cmdList, mTraceDiffuse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mTraceReflection, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mDiffuseOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mSpecularOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mReflectionOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetPipelineState(mResolvePso.Get());
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 5, mTraceDiffuse.Srv);
    cmdList->SetComputeRootDescriptorTable(kComputeSrv0 + 6, mTraceReflection.Srv);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 0, mDiffuseOut.Uav);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 1, mSpecularOut.Uav);
    cmdList->SetComputeRootDescriptorTable(kComputeUav0 + 2, mReflectionOut.Uav);
    cmdList->Dispatch(DivideRoundUp(inputs.Width, kScreenGroupSize), DivideRoundUp(inputs.Height, kScreenGroupSize), 1);

    // The deferred resolve reads these from its pixel shader, SSR from compute.
    Transition(cmdList, mDiffuseOut, kAllShaderResource);
    Transition(cmdList, mSpecularOut, kAllShaderResource);
    Transition(cmdList, mReflectionOut, kAllShaderResource);
    return true;
}
