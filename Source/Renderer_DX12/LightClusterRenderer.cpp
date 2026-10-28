#include "pch.h"
#include "LightClusterRenderer.h"

#include "d3dx12.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
}

namespace
{
    // Upload slot layout: constants first (256-aligned), then the lights, then the
    // extras. Structured buffers bound as root SRVs only need 4-byte alignment, but
    // keeping each region 256-aligned costs nothing.
    constexpr UINT64 Align256(UINT64 value) { return (value + 255ull) & ~255ull; }

    // Build pipeline root parameters.
    enum BuildRoot : UINT { BuildRootConstants = 0, BuildRootLights, BuildRootViewMasks, BuildRootGridMasks, BuildRootCount };

    ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, UINT64 bytes,
                                        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        const CD3DX12_HEAP_PROPERTIES heap(heapType);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
        ComPtr<ID3D12Resource> buffer;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
            return nullptr;
        return buffer;
    }
}

bool LightClusterRenderer::Initialize()
{
    Shutdown();
    mLastError.clear();
    if (!CreateBuffers() || !CreatePipelines())
    {
        Shutdown();
        return false;
    }
    mInitialized = true;
    return true;
}

void LightClusterRenderer::Shutdown()
{
    if (mUpload && mUploadMapped)
        mUpload->Unmap(0, nullptr);
    mUploadMapped = nullptr;
    mUpload.Reset();
    mViewMasks.Reset();
    mGridMasks.Reset();
    mMaskState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mRootSignature.Reset();
    mBuildViewPipeline.Reset();
    mBuildGridPipeline.Reset();
    mBindings = LightClusterBindings{};
    mInitialized = false;
}

bool LightClusterRenderer::CreateBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
    {
        mLastError = "LightClusterRenderer: DX12 device is null.";
        return false;
    }

    const UINT64 slotBytes = Align256(kConstantStride) + Align256(kLightBytes) + Align256(kExtrasBytes);
    mUpload = CreateBuffer(device, D3D12_HEAP_TYPE_UPLOAD, slotBytes * kUploadSlots,
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    mViewMasks = CreateBuffer(device, D3D12_HEAP_TYPE_DEFAULT, kViewMaskBytes,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    mGridMasks = CreateBuffer(device, D3D12_HEAP_TYPE_DEFAULT, kGridMaskBytes,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!mUpload || !mViewMasks || !mGridMasks)
    {
        mLastError = "LightClusterRenderer: failed to create its buffers.";
        return false;
    }
    mUpload->SetName(L"LightClusters Upload");
    mViewMasks->SetName(L"LightClusters ViewMasks");
    mGridMasks->SetName(L"LightClusters GridMasks");
    mMaskState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    const D3D12_RANGE noRead{ 0, 0 };
    void* mapped = nullptr;
    if (FAILED(mUpload->Map(0, &noRead, &mapped)))
    {
        mLastError = "LightClusterRenderer: failed to map its upload buffer.";
        return false;
    }
    mUploadMapped = static_cast<std::byte*>(mapped);
    std::memset(mUploadMapped, 0, static_cast<size_t>(slotBytes * kUploadSlots));
    return true;
}

bool LightClusterRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device)
        return false;

    D3D12_ROOT_PARAMETER params[BuildRootCount]{};
    params[BuildRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[BuildRootConstants].Descriptor = { 0, kRegisterSpace };
    params[BuildRootLights].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[BuildRootLights].Descriptor = { 0, kRegisterSpace };
    params[BuildRootViewMasks].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[BuildRootViewMasks].Descriptor = { 0, kRegisterSpace };
    params[BuildRootGridMasks].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[BuildRootGridMasks].Descriptor = { 1, kRegisterSpace };
    for (D3D12_ROOT_PARAMETER& param : params)
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = BuildRootCount;
    rsDesc.pParameters = params;
    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors))
        || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "LightClusterRenderer: failed to create the build root signature.";
        return false;
    }

    const auto compile = [&](DX12Shader& shader, const wchar_t* entryPoint, ComPtr<ID3D12PipelineState>& pipeline)
    {
        ShaderCompileRequest request{};
        request.FilePath = L"Data/Shaders/LightClusterBuild.hlsl";
        request.EntryPoint = entryPoint;
        request.TargetProfile = L"cs_6_0";
        request.Stage = ShaderStage::Compute;
        if (!shader.Compile(request))
        {
            mLastError = std::string("LightClusterRenderer: shader compile failed: ")
                + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mRootSignature.Get();
        desc.CS = shader.GetBytecode();
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline))))
        {
            mLastError = "LightClusterRenderer: CreateComputePipelineState failed.";
            return false;
        }
        return true;
    };
    return compile(mBuildViewShader, L"CSBuildView", mBuildViewPipeline)
        && compile(mBuildGridShader, L"CSBuildGrid", mBuildGridPipeline);
}

void LightClusterRenderer::Build(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs)
{
    if (!mInitialized || !commandList)
        return;

    const int count = std::clamp(inputs.Count, 0, kMaxSceneLights);

    // --- Constants ---------------------------------------------------------------
    LightClusterGpuConstants& c = mConstants;
    c = LightClusterGpuConstants{};
    c.View = inputs.View;
    const XMFLOAT4X4& p = inputs.Projection;
    c.ProjX = p._11;
    c.ProjY = p._22;
    c.ProjOffsetX = p._31;
    c.ProjOffsetY = p._32;
    c.ProjW = p._34 >= 0.0f ? 1.0f : -1.0f;
    c.NearDepth = (std::max)(inputs.NearDepth, 1e-3f);
    c.FarDepth = (std::max)(inputs.FarDepth, c.NearDepth * 2.0f);
    c.SliceScale = float(kSlices) / std::log(c.FarDepth / c.NearDepth);
    // Bigger tiles past 8K, so the mask buffer always holds the whole screen.
    const UINT tileSize = (std::max)({ kTileSize, (inputs.Width + kMaxTilesX - 1) / kMaxTilesX,
                                       (inputs.Height + kMaxTilesY - 1) / kMaxTilesY });
    c.TilesX = (std::max)((inputs.Width + tileSize - 1) / tileSize, 1u);
    c.TilesY = (std::max)((inputs.Height + tileSize - 1) / tileSize, 1u);
    c.Slices = kSlices;
    c.LightCount = static_cast<std::uint32_t>(count);
    c.WordCount = static_cast<std::uint32_t>((count + 31) / 32);
    c.Enabled = inputs.Enabled ? 1u : 0u;
    c.GridX = kGridX;
    c.GridY = kGridY;
    c.GridZ = kGridZ;
    c.GridCellSize = (std::max)(inputs.GridCellSize, 0.25f);
    // Snapped to whole cells, so the grid only changes when the camera crosses one.
    const auto snap = [&](float camera, UINT cells)
    {
        return (std::floor(camera / c.GridCellSize) - float(cells / 2)) * c.GridCellSize;
    };
    c.GridOrigin = XMFLOAT3(snap(inputs.CameraPosition.x, kGridX),
                            snap(inputs.CameraPosition.y, kGridY),
                            snap(inputs.CameraPosition.z, kGridZ));

    // --- Upload ------------------------------------------------------------------
    mSlot = (mSlot + 1) % kUploadSlots;
    const UINT64 slotBytes = Align256(kConstantStride) + Align256(kLightBytes) + Align256(kExtrasBytes);
    const UINT64 slotOffset = slotBytes * mSlot;
    const UINT64 lightsOffset = slotOffset + Align256(kConstantStride);
    const UINT64 extrasOffset = lightsOffset + Align256(kLightBytes);
    std::memcpy(mUploadMapped + slotOffset, &c, sizeof(c));
    if (count > 0 && inputs.Lights)
        std::memcpy(mUploadMapped + lightsOffset, inputs.Lights, size_t(count) * sizeof(DeferredLightingPass::PointLightGpu));
    if (count > 0)
    {
        if (inputs.Extras)
            std::memcpy(mUploadMapped + extrasOffset, inputs.Extras, size_t(count) * sizeof(XMFLOAT4));
        else
        {
            const XMFLOAT4 unit(1.0f, 1.0f, 0.0f, 0.0f);
            for (int i = 0; i < count; ++i)
                std::memcpy(mUploadMapped + extrasOffset + UINT64(i) * sizeof(XMFLOAT4), &unit, sizeof(unit));
        }
    }

    const D3D12_GPU_VIRTUAL_ADDRESS uploadBase = mUpload->GetGPUVirtualAddress();
    mBindings.Constants = uploadBase + slotOffset;
    mBindings.Lights = uploadBase + lightsOffset;
    mBindings.Extras = uploadBase + extrasOffset;
    mBindings.ViewMasks = mViewMasks->GetGPUVirtualAddress();
    mBindings.GridMasks = mGridMasks->GetGPUVirtualAddress();

    // --- Build -------------------------------------------------------------------
    const D3D12_RESOURCE_STATES readState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Off, the lookups never read the masks, so there is nothing to build; they are
    // still bound, so leave them in the state a reader expects.
    if (!inputs.Enabled)
    {
        if (mMaskState != readState)
        {
            const D3D12_RESOURCE_BARRIER toSrv[2] =
            {
                CD3DX12_RESOURCE_BARRIER::Transition(mViewMasks.Get(), mMaskState, readState),
                CD3DX12_RESOURCE_BARRIER::Transition(mGridMasks.Get(), mMaskState, readState),
            };
            commandList->ResourceBarrier(2, toSrv);
            mMaskState = readState;
        }
        return;
    }

    if (mMaskState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const D3D12_RESOURCE_BARRIER toUav[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(mViewMasks.Get(), mMaskState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(mGridMasks.Get(), mMaskState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        commandList->ResourceBarrier(2, toUav);
        mMaskState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(BuildRootConstants, mBindings.Constants);
    commandList->SetComputeRootShaderResourceView(BuildRootLights, mBindings.Lights);
    commandList->SetComputeRootUnorderedAccessView(BuildRootViewMasks, mBindings.ViewMasks);
    commandList->SetComputeRootUnorderedAccessView(BuildRootGridMasks, mBindings.GridMasks);

    const UINT clusterCount = c.TilesX * c.TilesY * c.Slices;
    commandList->SetPipelineState(mBuildViewPipeline.Get());
    commandList->Dispatch((clusterCount + 63) / 64, 1, 1);
    const UINT cellCount = kGridX * kGridY * kGridZ;
    commandList->SetPipelineState(mBuildGridPipeline.Get());
    commandList->Dispatch((cellCount + 63) / 64, 1, 1);

    const D3D12_RESOURCE_BARRIER toSrv[2] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(mViewMasks.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readState),
        CD3DX12_RESOURCE_BARRIER::Transition(mGridMasks.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readState),
    };
    commandList->ResourceBarrier(2, toSrv);
    mMaskState = readState;
}

void LightClusterRenderer::FillRootParameters(D3D12_ROOT_PARAMETER* params, D3D12_SHADER_VISIBILITY visibility)
{
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = { 0, kRegisterSpace };
    params[0].ShaderVisibility = visibility;
    for (UINT i = 1; i < kRootParameterCount; ++i)
    {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor = { i - 1, kRegisterSpace };
        params[i].ShaderVisibility = visibility;
    }
}

void LightClusterRenderer::BindGraphics(ID3D12GraphicsCommandList* commandList, UINT firstRootIndex,
                                        const LightClusterBindings& bindings)
{
    commandList->SetGraphicsRootConstantBufferView(firstRootIndex + 0, bindings.Constants);
    commandList->SetGraphicsRootShaderResourceView(firstRootIndex + 1, bindings.Lights);
    commandList->SetGraphicsRootShaderResourceView(firstRootIndex + 2, bindings.Extras);
    commandList->SetGraphicsRootShaderResourceView(firstRootIndex + 3, bindings.ViewMasks);
    commandList->SetGraphicsRootShaderResourceView(firstRootIndex + 4, bindings.GridMasks);
}

void LightClusterRenderer::BindCompute(ID3D12GraphicsCommandList* commandList, UINT firstRootIndex,
                                       const LightClusterBindings& bindings)
{
    commandList->SetComputeRootConstantBufferView(firstRootIndex + 0, bindings.Constants);
    commandList->SetComputeRootShaderResourceView(firstRootIndex + 1, bindings.Lights);
    commandList->SetComputeRootShaderResourceView(firstRootIndex + 2, bindings.Extras);
    commandList->SetComputeRootShaderResourceView(firstRootIndex + 3, bindings.ViewMasks);
    commandList->SetComputeRootShaderResourceView(firstRootIndex + 4, bindings.GridMasks);
}
