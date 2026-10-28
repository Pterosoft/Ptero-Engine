#include "pch.h"
#include "SkyGlareRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

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
    constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr float kPi = 3.14159265358979f;
    constexpr float kDegToRad = kPi / 180.0f;
    constexpr UINT kCbStride = 512;

    // How far past the screen edge (in NDC) a body may be and still glare, fading over
    // the last stretch - as the lens flare does, for the same reason: past the edge the
    // depth buffer can no longer say whether it is hidden.
    constexpr float kOffscreenLimit = 1.25f;
    constexpr float kOffscreenFade = 0.25f;

    // Across a spike at its root, in pixels: a diffraction spike is about as thin as
    // the image can show.
    constexpr float kSpikeWidth = 0.7f;

    ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = heapType;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
        ComPtr<ID3D12Resource> resource;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }

    // The same profiles SkyGlare.hlsl evaluates, for their normalisation.
    float HaloProfile(float r, float core, float radius, float falloff)
    {
        const float x = r / radius;
        if (x >= 1.0f)
            return 0.0f;
        const float window = (1.0f - x * x) * (1.0f - x * x);
        const float q = r / core;
        return std::pow(1.0f + q * q, -0.5f * falloff) * window;
    }

    float SpikeProfile(float along, float core, float length)
    {
        const float t = along / length;
        if (t >= 1.0f)
            return 0.0f;
        return std::pow(1.0f + along / core, -1.5f) * (1.0f - t) * (1.0f - t);
    }
}

// -------------------------------------------------------------------------
// Initialize / Shutdown
// -------------------------------------------------------------------------

bool SkyGlareRenderer::Initialize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;
    if (mIsInitialized && width == mWidth && height == mHeight)
        return true;

    mLastError.clear();
    try
    {
        if (!mIsInitialized && !CreatePipelines())
            return false;
        if (mIsInitialized)
            DX12Context_WaitForGPU();
        if (!CreateOutput(width, height))
            return false;

        mWidth = width;
        mHeight = height;
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("SkyGlareRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void SkyGlareRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
        mConstantBuffer->Unmap(0, nullptr);
    mMappedCb = nullptr;
    mConstantBuffer.Reset();
    mHeap.Reset();
    mVisibilityBuffer.Reset();
    for (SourceState& state : mSources)
        state = {};
    mOutputTexture.Reset();
    mVisibilityPso.Reset();
    mApplyPso.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
}

// -------------------------------------------------------------------------
// Pipelines and resources
// -------------------------------------------------------------------------

bool SkyGlareRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "SkyGlareRenderer: null device.";
        return false;
    }

    struct Entry { DX12Shader* Shader; const wchar_t* Name; };
    const Entry entries[] = {
        { &mVisibilityShader, L"CSVisibility" },
        { &mApplyShader, L"CSApply" },
    };
    for (const Entry& entry : entries)
    {
        const ShaderCompileRequest request{ L"Shaders\\SkyGlare.hlsl", entry.Name, L"cs_5_0", ShaderStage::Compute };
        if (!entry.Shader->Compile(request))
        {
            mLastError = std::string("SkyGlareRenderer: shader compile failed: ")
                + (entry.Shader->GetLastErrorMessage() ? entry.Shader->GetLastErrorMessage() : "unknown");
            return false;
        }
    }

    //   [0] CBV b0
    //   [1] SRV t0..t2 (input, scene colour, depth)
    //   [2] UAV u0..u1 (output, visibility)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = UavOutput - SrvInput;
    srvRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = DescriptorsPerTable - UavOutput;
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(std::size(params));
    rsDesc.pParameters = params;

    ComPtr<ID3DBlob> serialized, errors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));

    auto makePso = [&](DX12Shader& shader, ComPtr<ID3D12PipelineState>& pso)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mRootSignature.Get();
        desc.CS = shader.GetBytecode();
        DX12_THROW_IF_FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    };
    makePso(mVisibilityShader, mVisibilityPso);
    makePso(mApplyShader, mApplyPso);

    // One constant slot per frame in flight.
    static_assert(sizeof(SkyGlareCb) <= kCbStride, "SkyGlareCb outgrew its constant slot");
    mConstantBuffer = MakeBuffer(device, static_cast<UINT64>(kCbStride) * kFrameSlots, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCb)));

    mVisibilityBuffer = MakeBuffer(device, 256, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    for (SourceState& state : mSources)
        state.VisibilityValid = false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = DescriptorsPerTable * kFrameSlots;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mHeap)));
    mDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool SkyGlareRenderer::CreateOutput(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (!mUiSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUiSrvCpu, &mOutputUiSrvGpu))
        {
            mLastError = "SkyGlareRenderer: failed to allocate a Ui SRV slot.";
            return false;
        }
        mUiSlotAllocated = true;
        mOutputTextureId = static_cast<UiTextureID>(mOutputUiSrvGpu.ptr);
    }

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
    desc.Format = kOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    mOutputTexture.Reset();
    DX12_THROW_IF_FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&mOutputTexture)));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kOutputFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srv, mOutputUiSrvCpu);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE SkyGlareRenderer::HeapCpu(UINT table, UINT descriptor) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(table * DescriptorsPerTable + descriptor) * mDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE SkyGlareRenderer::HeapGpu(UINT table, UINT descriptor) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = mHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(table * DescriptorsPerTable + descriptor) * mDescriptorSize;
    return handle;
}

// The halo's integral over the image plane and the spikes' along their length, so each
// part of the PSF carries exactly the energy its share says. Numerical: the profiles
// are cheap, and this only runs when the shape or the field of view changes.
void SkyGlareRenderer::UpdateNormalisation(const ShapeKey& key, SourceState& state)
{
    if (key == state.Shape)
        return;
    state.Shape = key;

    constexpr int kSteps = 4096;
    double halo = 0.0;
    const double haloStep = key.HaloRadius / kSteps;
    for (int i = 0; i < kSteps; ++i)
    {
        const float r = static_cast<float>((i + 0.5) * haloStep);
        halo += HaloProfile(r, key.HaloCore, key.HaloRadius, key.HaloFalloff) * 2.0 * kPi * r * haloStep;
    }
    state.HaloIntegral = static_cast<float>((std::max)(halo, 1.0e-12));

    double spike = 0.0;
    const double spikeStep = key.SpikeLength / kSteps;
    for (int i = 0; i < kSteps; ++i)
        spike += SpikeProfile(static_cast<float>((i + 0.5) * spikeStep), key.SpikeCore, key.SpikeLength) * spikeStep;
    // Each line is a spike in both directions.
    state.SpikeIntegral = static_cast<float>((std::max)(spike * 2.0 * (std::max)(key.SpikeCount, 1), 1.0e-12));
}

bool SkyGlareRenderer::PrepareSource(const Source& source, const XMMATRIX& viewProjection,
                                     float pixelsPerRadian, SourceState& state, SourceCb& cb) const
{
    cb = {};
    const SkyGlareSettings* settings = source.Settings;
    const float intensity = (settings && settings->Enabled) ? (std::max)(settings->Intensity, 0.0f) : 0.0f;
    const float luminance = 0.2126f * source.Illuminance.x + 0.7152f * source.Illuminance.y
        + 0.0722f * source.Illuminance.z;

    // ---- Where it is, and how much light it brings -----------------------------------
    XMFLOAT4 clip{};
    XMStoreFloat4(&clip, XMVector4Transform(
        XMVectorSet(source.Direction.x, source.Direction.y, source.Direction.z, 0.0f), viewProjection));

    float offscreenFade = 0.0f;
    if (clip.w > 1.0e-4f)
    {
        const float ndcX = clip.x / clip.w;
        const float ndcY = clip.y / clip.w;
        const float edge = (std::max)(std::fabs(ndcX), std::fabs(ndcY));
        offscreenFade = std::clamp((kOffscreenLimit - edge) / kOffscreenFade, 0.0f, 1.0f);
        cb.Pixel = XMFLOAT2((ndcX * 0.5f + 0.5f) * mWidth, (0.5f - ndcY * 0.5f) * mHeight);
    }
    if (intensity <= 0.0f || luminance <= 0.0f || offscreenFade <= 0.0f)
        return false;

    // Its illuminance over one pixel's solid angle is the radiance one pixel would carry
    // all of its light with: the glare's total, before the PSF spreads it.
    const float energyScale = intensity * pixelsPerRadian * pixelsPerRadian;
    cb.Energy = XMFLOAT3(source.Illuminance.x * energyScale, source.Illuminance.y * energyScale,
                         source.Illuminance.z * energyScale);
    cb.OffscreenFade = offscreenFade;

    // ---- Shape -------------------------------------------------------------------------
    const int blades = std::clamp(settings->ApertureBlades, 3, 16);
    const float streak = std::clamp(settings->StreakStrength, 0.0f, 1.0f);
    const int lines = streak > 0.0f ? ((blades % 2 == 0) ? blades / 2 : blades) : 0;
    const float discPixels = source.DiscRadius * pixelsPerRadian;

    ShapeKey key;
    key.HaloRadius = (std::max)(settings->HaloRadius * kDegToRad * pixelsPerRadian, 2.0f);
    // The halo is a point spread function; never let it be tighter than the disc it
    // spreads, or an extended body like the moon gets a hot spot in its middle.
    key.HaloCore = std::clamp((std::max)(settings->HaloCore * kDegToRad * pixelsPerRadian, discPixels),
                              0.5f, key.HaloRadius);
    key.HaloFalloff = std::clamp(settings->HaloFalloff, 0.5f, 8.0f);
    key.SpikeLength = (std::max)(settings->StreakLength * kDegToRad * pixelsPerRadian, 2.0f);
    key.SpikeCore = (std::max)(0.5f * key.HaloCore, 1.0f);
    key.SpikeCount = lines;
    UpdateNormalisation(key, state);

    cb.HaloCore = key.HaloCore;
    cb.HaloRadius = key.HaloRadius;
    cb.HaloFalloff = key.HaloFalloff;
    cb.HaloWeight = (1.0f - streak) / state.HaloIntegral;
    cb.SpikeLength = key.SpikeLength;
    cb.SpikeCore = key.SpikeCore;
    cb.SpikeWeight = lines > 0 ? streak / state.SpikeIntegral : 0.0f;
    cb.SpikeCount = static_cast<float>(lines);

    // Spikes run along the normals of the iris edges, as in the FFT bloom kernel.
    const float sector = 2.0f * kPi / static_cast<float>(blades);
    for (int k = 0; k < lines; ++k)
    {
        const float angle = settings->ApertureRotation * kDegToRad + (k + 0.5f) * sector;
        float* pair = &cb.SpikeDirections[k / 2].x;
        pair[(k & 1) * 2 + 0] = std::cos(angle);
        pair[(k & 1) * 2 + 1] = std::sin(angle);
    }

    // Glare spreads with wavelength: red reaches further than blue.
    const float spread = std::clamp(settings->ChromaticSpread, 0.0f, 1.0f);
    cb.ChannelScale = XMFLOAT3(1.0f + spread * (610.0f / 550.0f - 1.0f), 1.0f,
                               1.0f + spread * (465.0f / 550.0f - 1.0f));

    // ---- Visibility ------------------------------------------------------------------
    // The sun is sampled well inside its disc, so the antialiased edge does not read as
    // partial cover; the moon, judged on its mean, over most of its face.
    cb.SampleRadius = (std::max)((source.AverageDiscTest ? 0.8f : 0.6f) * discPixels, 1.0f);
    cb.ExpectedDisc = (std::max)(source.DiscLuminance, 0.0f);
    cb.AverageDiscTest = source.AverageDiscTest ? 1.0f : 0.0f;
    return true;
}

// -------------------------------------------------------------------------
// Apply
// -------------------------------------------------------------------------

void SkyGlareRenderer::Apply(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs)
{
    if (!mIsInitialized || commandList == nullptr || inputs.Input == nullptr || inputs.Depth == nullptr)
        return;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return;

    ID3D12Resource* sceneColor = inputs.SceneColor ? inputs.SceneColor : inputs.Input;

    const UINT slot = static_cast<UINT>(mFrameCounter++ % kFrameSlots);
    SkyGlareCb cb{};
    cb.OutputSize = XMFLOAT2(static_cast<float>(mWidth), static_cast<float>(mHeight));
    // Settles within about a tenth of a second, like the lens flare's.
    cb.VisibilityBlend = 1.0f - std::exp(-(std::max)(inputs.DeltaSeconds, 0.0f) / 0.05f);
    cb.SpikeWidth = kSpikeWidth;

    // Pixels per radian at the centre of the image; the PSFs are laid out in pixels.
    const float pixelsPerRadian = 0.5f * static_cast<float>(mHeight) * std::fabs(inputs.Projection._22);
    const XMMATRIX viewProjection = XMLoadFloat4x4(&inputs.ViewProjection);

    bool anyActive = false;
    for (UINT s = 0; s < kSourceCount; ++s)
    {
        SourceState& state = mSources[s];
        const bool active = PrepareSource(inputs.Sources[s], viewProjection, pixelsPerRadian, state, cb.Sources[s]);
        // A source that was gone takes its first visibility as is rather than fading in
        // from whatever it last had.
        cb.Sources[s].Reset = state.VisibilityValid ? 0.0f : 1.0f;
        state.VisibilityValid = active;
        anyActive |= active;
    }

    std::memcpy(mMappedCb + static_cast<std::size_t>(slot) * kCbStride, &cb, sizeof(cb));

    // ---- Descriptors for this frame's slot ----------------------------------------------
    // Fresh views of the resources rather than copies of the engine's SRVs: those live in
    // its shader-visible heap, which is not a valid copy source.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        view.Format = DX12ShaderReadableFormat(inputs.Input->GetDesc().Format);
        device->CreateShaderResourceView(inputs.Input, &view, HeapCpu(slot, SrvInput));
        view.Format = DX12ShaderReadableFormat(sceneColor->GetDesc().Format);
        device->CreateShaderResourceView(sceneColor, &view, HeapCpu(slot, SrvSceneColor));
        view.Format = inputs.DepthSrvFormat;
        device->CreateShaderResourceView(inputs.Depth, &view, HeapCpu(slot, SrvDepth));

        D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav{};
        outputUav.Format = kOutputFormat;
        outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mOutputTexture.Get(), nullptr, &outputUav, HeapCpu(slot, UavOutput));

        D3D12_UNORDERED_ACCESS_VIEW_DESC visibilityUav{};
        visibilityUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        visibilityUav.Buffer.NumElements = kSourceCount;
        visibilityUav.Buffer.StructureByteStride = sizeof(float);
        device->CreateUnorderedAccessView(mVisibilityBuffer.Get(), nullptr, &visibilityUav, HeapCpu(slot, UavVisibility));
    }

    // ---- Dispatch ----------------------------------------------------------------------
    const bool separateSceneColor = sceneColor != inputs.Input;
    {
        D3D12_RESOURCE_BARRIER barriers[3] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.Input,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(sceneColor,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(separateSceneColor ? 3 : 2, barriers);
    }

    ID3D12DescriptorHeap* heaps[] = { mHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * kCbStride);
    commandList->SetComputeRootDescriptorTable(1, HeapGpu(slot, SrvInput));
    commandList->SetComputeRootDescriptorTable(2, HeapGpu(slot, UavOutput));

    if (anyActive)
    {
        // One group per source; an inactive one writes a visibility nothing reads.
        commandList->SetPipelineState(mVisibilityPso.Get());
        commandList->Dispatch(kSourceCount, 1, 1);
        const auto visibilityDone = CD3DX12_RESOURCE_BARRIER::UAV(mVisibilityBuffer.Get());
        commandList->ResourceBarrier(1, &visibilityDone);
    }

    // Always runs, so the output is the input even when there is nothing to add.
    commandList->SetPipelineState(mApplyPso.Get());
    commandList->Dispatch((mWidth + 7) / 8, (mHeight + 7) / 8, 1);

    {
        D3D12_RESOURCE_BARRIER barriers[3] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(inputs.Input,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(sceneColor,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(separateSceneColor ? 3 : 2, barriers);
    }

    ID3D12DescriptorHeap* sharedHeap[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, sharedHeap);
}
