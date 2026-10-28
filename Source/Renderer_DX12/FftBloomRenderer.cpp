#include "pch.h"
#include "FftBloomRenderer.h"


#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <utility>

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
    constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr float kPi = 3.14159265358979f;

    // The mip-chain bloom adds every level of its chain back together, roughly six copies
    // of the thresholded energy; the FFT kernel is normalised to one. This keeps the same
    // Intensity looking about as strong with either method.
    constexpr float kEnergyMatch = 5.0f;

    constexpr UINT kCbStride = 256;

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

    UINT ClampFftSize(int requested)
    {
        if (requested <= 256)
            return 256;
        if (requested <= 512)
            return 512;
        return 1024;
    }

    UINT Log2(UINT n)
    {
        UINT log = 0;
        while ((1u << log) < n)
            ++log;
        return log;
    }
}

// -------------------------------------------------------------------------
// Initialize / Shutdown
// -------------------------------------------------------------------------

bool FftBloomRenderer::Initialize(UINT width, UINT height)
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
        // The frame's aspect decides where the image sits in the grid, not the kernel, so
        // the kernel survives a resize; the tables are rewritten for the new output.
        mIsInitialized = true;
        WriteTableDescriptors();
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("FftBloomRenderer::Initialize: ") + ex.what();
        return false;
    }
}

void FftBloomRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedCb)
        mConstantBuffer->Unmap(0, nullptr);
    mMappedCb = nullptr;
    mConstantBuffer.Reset();
    mHeap.Reset();
    mDataRG.Reset();
    mDataB.Reset();
    mKernelRG.Reset();
    mKernelB.Reset();
    mKernelUpload.Reset();
    mKernelValid = false;
    mKernelKey = {};
    mGridN = 0;
    mOutputTexture.Reset();
    mPrefilterPso.Reset(); mRowsForwardPso.Reset(); mRowsInversePso.Reset();
    mColumnsForwardPso.Reset(); mColumnsConvolvePso.Reset(); mCompositePso.Reset();
    mRootSignature.Reset();
    mIsInitialized = false;
}

// -------------------------------------------------------------------------
// Pipelines and resources
// -------------------------------------------------------------------------

bool FftBloomRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "FftBloomRenderer: null device.";
        return false;
    }

    struct Entry { DX12Shader* Shader; const wchar_t* Name; };
    const Entry entries[] = {
        { &mPrefilterShader, L"CSPrefilter" },
        { &mRowsForwardShader, L"CSFftRowsForward" },
        { &mRowsInverseShader, L"CSFftRowsInverse" },
        { &mColumnsForwardShader, L"CSFftColumnsForward" },
        { &mColumnsConvolveShader, L"CSFftColumnsConvolve" },
        { &mCompositeShader, L"CSComposite" },
    };
    for (const Entry& entry : entries)
    {
        const ShaderCompileRequest request{ L"Shaders\\FftBloom.hlsl", entry.Name, L"cs_5_0", ShaderStage::Compute };
        if (!entry.Shader->Compile(request))
        {
            mLastError = std::string("FftBloomRenderer: shader compile failed: ")
                + (entry.Shader->GetLastErrorMessage() ? entry.Shader->GetLastErrorMessage() : "unknown");
            return false;
        }
    }

    //   [0] CBV b0
    //   [1] SRV t0..t4 (source, unused, unused, kernel RG, kernel B)
    //   [2] UAV u0..u2 (data RG, data B, output)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = UavDataRG - SrvSource;
    srvRange.BaseShaderRegister = 0;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = DescriptorsPerTable - UavDataRG;
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

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(std::size(params));
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;

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
    makePso(mPrefilterShader, mPrefilterPso);
    makePso(mRowsForwardShader, mRowsForwardPso);
    makePso(mRowsInverseShader, mRowsInversePso);
    makePso(mColumnsForwardShader, mColumnsForwardPso);
    makePso(mColumnsConvolveShader, mColumnsConvolvePso);
    makePso(mCompositeShader, mCompositePso);

    // One constant slot per frame in flight, plus one for the kernel build.
    mConstantBuffer = MakeBuffer(device, static_cast<UINT64>(kCbStride) * (kFrameSlots + 1), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCb)));

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = DescriptorsPerTable * kTableCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mHeap)));
    mDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool FftBloomRenderer::CreateOutput(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (!mUiSlotAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputUiSrvCpu, &mOutputUiSrvGpu))
        {
            mLastError = "FftBloomRenderer: failed to allocate a Ui SRV slot.";
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

D3D12_CPU_DESCRIPTOR_HANDLE FftBloomRenderer::HeapCpu(UINT table, UINT descriptor) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(table * DescriptorsPerTable + descriptor) * mDescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE FftBloomRenderer::HeapGpu(UINT table, UINT descriptor) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = mHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(table * DescriptorsPerTable + descriptor) * mDescriptorSize;
    return handle;
}

// Only called with the GPU idle (init, or right after a rebuild waited for it).
void FftBloomRenderer::WriteTableDescriptors()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || !mHeap)
        return;

    const UINT elements = (std::max)(mGridN * mGridN, 1u);
    auto bufferSrv = [&](ID3D12Resource* resource, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE target)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.NumElements = elements;
        desc.Buffer.StructureByteStride = stride;
        device->CreateShaderResourceView(resource, &desc, target);
    };
    auto bufferUav = [&](ID3D12Resource* resource, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE target)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.NumElements = elements;
        desc.Buffer.StructureByteStride = stride;
        device->CreateUnorderedAccessView(resource, nullptr, &desc, target);
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC nullTexture{};
    nullTexture.Format = kOutputFormat;
    nullTexture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullTexture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullTexture.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav{};
    outputUav.Format = kOutputFormat;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    for (UINT table = 0; table < kTableCount; ++table)
    {
        const bool kernelTable = table == kKernelTable;
        device->CreateShaderResourceView(nullptr, &nullTexture, HeapCpu(table, SrvSource));
        bufferSrv(nullptr, 16, HeapCpu(table, SrvUnused1));
        bufferSrv(nullptr, 8, HeapCpu(table, SrvUnused2));
        bufferSrv(kernelTable ? nullptr : mKernelRG.Get(), 16, HeapCpu(table, SrvKernelRG));
        bufferSrv(kernelTable ? nullptr : mKernelB.Get(), 8, HeapCpu(table, SrvKernelB));
        bufferUav(kernelTable ? mKernelRG.Get() : mDataRG.Get(), 16, HeapCpu(table, UavDataRG));
        bufferUav(kernelTable ? mKernelB.Get() : mDataB.Get(), 8, HeapCpu(table, UavDataB));
        device->CreateUnorderedAccessView(kernelTable ? nullptr : mOutputTexture.Get(), nullptr, &outputUav,
            HeapCpu(table, UavOutput));
    }
}

void FftBloomRenderer::RegionFor(UINT n, UINT& width, UINT& height) const
{
    // Half the grid is padding: the frame's longer side spans n / 2.
    const float scale = 0.5f * static_cast<float>(n) / static_cast<float>((std::max)(mWidth, mHeight));
    width = (std::clamp)(static_cast<UINT>(std::lround(mWidth * scale)), 1u, n / 2);
    height = (std::clamp)(static_cast<UINT>(std::lround(mHeight * scale)), 1u, n / 2);
}

bool FftBloomRenderer::EnsureGrid(UINT n)
{
    if (mGridN == n && mDataRG)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    DX12Context_WaitForGPU();
    mDataRG.Reset();
    mDataB.Reset();
    mKernelRG.Reset();
    mKernelB.Reset();
    mKernelValid = false;

    const UINT64 count = static_cast<UINT64>(n) * n;
    mDataRG = MakeBuffer(device, count * 16, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    mDataB = MakeBuffer(device, count * 8, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    mGridN = n;
    WriteTableDescriptors();
    return true;
}

// The point spread function: a sharp core, a power-law halo and the aperture's
// diffraction spikes, each normalised to unit energy and mixed by the settings.
bool FftBloomRenderer::EnsureKernel(ID3D12GraphicsCommandList* commandList, const BloomSettings& settings, UINT n)
{
    KernelKey key;
    key.N = static_cast<int>(n);
    key.Size = std::clamp(settings.FftKernelSize, 0.02f, 1.0f);
    key.Falloff = std::clamp(settings.FftHaloFalloff, 0.5f, 6.0f);
    key.Halo = std::clamp(settings.FftHaloStrength, 0.0f, 1.0f);
    key.Streak = std::clamp(settings.FftStreakStrength, 0.0f, 1.0f);
    key.Blades = std::clamp(settings.FftApertureBlades, 3, 16);
    key.Rotation = settings.FftApertureRotation;
    key.Spread = std::clamp(settings.FftChromaticSpread, 0.0f, 2.0f);
    if (mKernelValid && key == mKernelKey)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const std::size_t count = static_cast<std::size_t>(n) * n;
    std::vector<float> core(count * 3, 0.0f), halo(count * 3, 0.0f), streak(count * 3, 0.0f);

    const float reach = (std::min)(key.Size * 0.5f * static_cast<float>(n), 0.5f * static_cast<float>(n) - 1.0f);
    // Past n / 2 the offsets would alias onto the other side of the wrapped grid.
    const int box = (std::min)(static_cast<int>(std::ceil(reach * (1.0f + key.Spread * 0.12f))) + 1,
        static_cast<int>(n / 2) - 1);

    // Glare spreads with wavelength: red reaches further than blue.
    const float channelLambda[3] = { 610.0f, 550.0f, 465.0f };
    float channelScale[3];
    for (int c = 0; c < 3; ++c)
        channelScale[c] = 1.0f + key.Spread * (channelLambda[c] / 550.0f - 1.0f);

    // Diffraction spikes run along the normals of the iris edges - the directions the
    // aperture's Fraunhofer pattern puts them in (LensDiffraction). Opposite edges of an
    // even-bladed iris share a line, so it gives `blades` spikes, an odd one twice that.
    // They are modelled directly as thin lines rather than sampled from that pattern: most
    // of the pattern's energy sits in its Airy rings, which a full-frame kernel smears into
    // nothing, and the spikes are what the setting is for.
    std::vector<std::pair<float, float>> spikeLines;
    if (key.Streak > 0.0f)
    {
        const float sector = 2.0f * kPi / static_cast<float>(key.Blades);
        const int lines = (key.Blades % 2 == 0) ? key.Blades / 2 : key.Blades;
        for (int k = 0; k < lines; ++k)
        {
            const float angle = key.Rotation * (kPi / 180.0f) + (k + 0.5f) * sector;
            spikeLines.emplace_back(std::cos(angle), std::sin(angle));
        }
    }

    double sums[3][3] = {};
    for (int dy = -box; dy <= box; ++dy)
    {
        for (int dx = -box; dx <= box; ++dx)
        {
            const float r = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            const std::size_t index = (static_cast<std::size_t>((dy + static_cast<int>(n)) % n) * n
                + static_cast<std::size_t>((dx + static_cast<int>(n)) % n)) * 3;

            for (int c = 0; c < 3; ++c)
            {
                const float coreValue = std::exp(-0.5f * r * r / (0.7f * 0.7f));

                const float channelReach = reach * channelScale[c];
                const float x = r / channelReach;
                const float window = x < 1.0f ? (1.0f - x * x) * (1.0f - x * x) : 0.0f;
                const float haloValue = std::pow(1.0f + r * r, -0.5f * key.Falloff) * window;

                float streakValue = 0.0f;
                for (const auto& line : spikeLines)
                {
                    // Distance along and across the spike; the length scales with
                    // wavelength like the rest of the glare.
                    const float along = std::fabs(dx * line.first + dy * line.second) / channelScale[c];
                    const float across = std::fabs(dx * line.second - dy * line.first);
                    const float t = along / reach;
                    if (t >= 1.0f)
                        continue;
                    const float width = 0.6f + along * 0.004f;
                    streakValue += std::exp(-0.5f * (across / width) * (across / width)) / width
                        * std::pow(1.0f + along / 2.0f, -1.2f) * (1.0f - t) * (1.0f - t);
                }

                core[index + c] = coreValue;
                halo[index + c] = haloValue;
                streak[index + c] = streakValue;
                sums[0][c] += coreValue;
                sums[1][c] += haloValue;
                sums[2][c] += streakValue;
            }
        }
    }

    float weightCore = (std::max)(0.0f, 1.0f - key.Halo - key.Streak);
    float weightHalo = key.Halo;
    float weightStreak = spikeLines.empty() ? 0.0f : key.Streak;
    const float weightTotal = (std::max)(weightCore + weightHalo + weightStreak, 1.0e-6f);
    weightCore /= weightTotal;
    weightHalo /= weightTotal;
    weightStreak /= weightTotal;

    // Divided by N^2 here so the inverse FFT needs no normalisation pass.
    const float inverseScale = 1.0f / (static_cast<float>(n) * static_cast<float>(n));

    DX12Context_WaitForGPU();
    mKernelRG.Reset();
    mKernelB.Reset();
    mKernelUpload.Reset();
    mKernelValid = false;

    const UINT64 rgBytes = static_cast<UINT64>(count) * 16;
    const UINT64 bBytes = static_cast<UINT64>(count) * 8;
    mKernelRG = MakeBuffer(device, rgBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    mKernelB = MakeBuffer(device, bBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    mKernelUpload = MakeBuffer(device, rgBytes + bBytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);

    std::uint8_t* mapped = nullptr;
    DX12_THROW_IF_FAILED(mKernelUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    float* rg = reinterpret_cast<float*>(mapped);
    float* b = reinterpret_cast<float*>(mapped + rgBytes);
    for (std::size_t i = 0; i < count; ++i)
    {
        float value[3];
        for (int c = 0; c < 3; ++c)
        {
            const std::size_t k = i * 3 + c;
            float v = 0.0f;
            if (sums[0][c] > 0.0) v += weightCore * static_cast<float>(core[k] / sums[0][c]);
            if (sums[1][c] > 0.0) v += weightHalo * static_cast<float>(halo[k] / sums[1][c]);
            if (sums[2][c] > 0.0) v += weightStreak * static_cast<float>(streak[k] / sums[2][c]);
            value[c] = v * inverseScale;
        }
        rg[i * 4 + 0] = value[0]; rg[i * 4 + 1] = 0.0f;
        rg[i * 4 + 2] = value[1]; rg[i * 4 + 3] = 0.0f;
        b[i * 2 + 0] = value[2];  b[i * 2 + 1] = 0.0f;
    }
    mKernelUpload->Unmap(0, nullptr);

    commandList->CopyBufferRegion(mKernelRG.Get(), 0, mKernelUpload.Get(), 0, rgBytes);
    commandList->CopyBufferRegion(mKernelB.Get(), 0, mKernelUpload.Get(), rgBytes, bBytes);
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mKernelRG.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(mKernelB.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        commandList->ResourceBarrier(2, barriers);
    }

    WriteTableDescriptors();

    // Transform the kernel with the same passes the image goes through.
    FftBloomCb cb{};
    cb.N = n;
    cb.Log2N = Log2(n);
    std::memcpy(mMappedCb + static_cast<std::size_t>(kKernelTable) * kCbStride, &cb, sizeof(cb));

    ID3D12DescriptorHeap* heaps[] = { mHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(kKernelTable) * kCbStride);
    commandList->SetComputeRootDescriptorTable(1, HeapGpu(kKernelTable, SrvSource));
    commandList->SetComputeRootDescriptorTable(2, HeapGpu(kKernelTable, UavDataRG));

    commandList->SetPipelineState(mRowsForwardPso.Get());
    commandList->Dispatch(n, 1, 1);
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::UAV(mKernelRG.Get()), CD3DX12_RESOURCE_BARRIER::UAV(mKernelB.Get()) };
        commandList->ResourceBarrier(2, barriers);
    }
    commandList->SetPipelineState(mColumnsForwardPso.Get());
    commandList->Dispatch(n, 1, 1);
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(mKernelRG.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mKernelB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(2, barriers);
    }

    mKernelKey = key;
    mKernelValid = true;
    return true;
}

// -------------------------------------------------------------------------
// Apply
// -------------------------------------------------------------------------

void FftBloomRenderer::Apply(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource*            sourceResource,
    D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuSrv,
    const BloomSettings&       settings)
{
    if (!mIsInitialized || commandList == nullptr || sourceResource == nullptr)
        return;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return;

    const UINT n = ClampFftSize(settings.FftResolution);
    try
    {
        if (!EnsureGrid(n) || !EnsureKernel(commandList, settings, n))
            return;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("FftBloomRenderer: ") + ex.what();
        mKernelValid = false;
        return;
    }

    UINT regionWidth = 0, regionHeight = 0;
    RegionFor(n, regionWidth, regionHeight);

    const UINT slot = static_cast<UINT>(mFrameCounter++ % kFrameSlots);
    FftBloomCb cb{};
    cb.N = n;
    cb.Log2N = Log2(n);
    cb.RegionWidth = regionWidth;
    cb.RegionHeight = regionHeight;
    cb.SourceWidth = mWidth;
    cb.SourceHeight = mHeight;
    cb.OutputWidth = mWidth;
    cb.OutputHeight = mHeight;
    cb.Threshold = settings.Threshold;
    cb.Knee = settings.Knee;
    cb.Intensity = (std::max)(settings.Intensity, 0.0f) * kEnergyMatch;
    std::memcpy(mMappedCb + static_cast<std::size_t>(slot) * kCbStride, &cb, sizeof(cb));

    // A fresh view of the resource rather than a copy of sourceCpuSrv: the engine's SRVs
    // live in its shader-visible heap, which is not a valid copy source.
    (void)sourceCpuSrv;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DX12ShaderReadableFormat(sourceResource->GetDesc().Format);
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(sourceResource, &view, HeapCpu(slot, SrvSource));
    }

    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(sourceResource,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        commandList->ResourceBarrier(2, barriers);
    }

    ID3D12DescriptorHeap* heaps[] = { mHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * kCbStride);
    commandList->SetComputeRootDescriptorTable(1, HeapGpu(slot, SrvSource));
    commandList->SetComputeRootDescriptorTable(2, HeapGpu(slot, UavDataRG));

    const D3D12_RESOURCE_BARRIER dataDone[2] = {
        CD3DX12_RESOURCE_BARRIER::UAV(mDataRG.Get()), CD3DX12_RESOURCE_BARRIER::UAV(mDataB.Get()) };

    commandList->SetPipelineState(mPrefilterPso.Get());
    commandList->Dispatch((n + 7) / 8, (n + 7) / 8, 1);
    commandList->ResourceBarrier(2, dataDone);

    // Rows below the image are zero, and so is their transform: skip them.
    commandList->SetPipelineState(mRowsForwardPso.Get());
    commandList->Dispatch(regionHeight, 1, 1);
    commandList->ResourceBarrier(2, dataDone);

    commandList->SetPipelineState(mColumnsConvolvePso.Get());
    commandList->Dispatch(n, 1, 1);
    commandList->ResourceBarrier(2, dataDone);

    // Only the rows holding the image are read back.
    commandList->SetPipelineState(mRowsInversePso.Get());
    commandList->Dispatch(regionHeight, 1, 1);
    commandList->ResourceBarrier(2, dataDone);

    commandList->SetPipelineState(mCompositePso.Get());
    commandList->Dispatch((mWidth + 7) / 8, (mHeight + 7) / 8, 1);

    {
        D3D12_RESOURCE_BARRIER barriers[3] = {
            CD3DX12_RESOURCE_BARRIER::UAV(mOutputTexture.Get()),
            CD3DX12_RESOURCE_BARRIER::Transition(mOutputTexture.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(sourceResource,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(3, barriers);
    }

    ID3D12DescriptorHeap* sharedHeap[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, sharedHeap);
}
