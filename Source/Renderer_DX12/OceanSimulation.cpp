#include "pch.h"

#include "OceanSimulation.h"

#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
    // Complex fields need signed float storage; RGBA16F would lose precision on
    // the long-wavelength cascade where displacement is metres-scale.
    constexpr DXGI_FORMAT kFftFormat    = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr DXGI_FORMAT kOutputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Enough 256-byte slots that a frame's constants are never overwritten
    // while the GPU is still reading the previous frames'.
    constexpr std::size_t kConstantSlotCount = 256;

    constexpr float kTwoPi = 6.28318531f;
}

bool OceanSpectrumSettings::operator==(const OceanSpectrumSettings& other) const
{
    auto same = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
    return same(WindSpeed, other.WindSpeed)
        && same(WindAngle, other.WindAngle)
        && same(Fetch, other.Fetch)
        && same(Depth, other.Depth)
        && same(Swell, other.Swell)
        && same(SpreadBlend, other.SpreadBlend)
        && same(Amplitude, other.Amplitude)
        && same(PatchSizes[0], other.PatchSizes[0])
        && same(PatchSizes[1], other.PatchSizes[1])
        && same(PatchSizes[2], other.PatchSizes[2]);
    // Choppiness and FoamBias feed the assemble pass every frame, so they do
    // not require a spectrum rebuild.
}

bool OceanSimulation::Initialize()
{
    if (!CreatePipelines())
        return false;
    if (!CreateResources())
        return false;

    mSpectrumDirty = true;
    mIsInitialized = true;
    mLastError.clear();
    return true;
}

void OceanSimulation::Shutdown()
{
    if (mConstantBuffer)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mConstantBuffer.Reset();
    }
    mMappedConstants = nullptr;

    for (Cascade& cascade : mCascades)
    {
        cascade.Spectrum.Reset();
        cascade.DisplacementFft.Reset();
        cascade.HeightSlopeFft.Reset();
        cascade.Displacement.Reset();
        cascade.Normal.Reset();
    }

    mSpectrumRootSignature.Reset();  mSpectrumPso.Reset();
    mEvolveRootSignature.Reset();    mEvolvePso.Reset();
    mFftRootSignature.Reset();       mFftPso.Reset();
    mAssembleRootSignature.Reset();  mAssemblePso.Reset();

    mIsInitialized = false;
    mLastError.clear();
}

namespace
{
    // Builds a compute root signature from a descriptor of ranges:
    //   slot 0        - root CBV b0
    //   slots 1..n    - one single-descriptor table each (SRV or UAV)
    bool BuildComputeRootSignature(
        ID3D12Device* device,
        const D3D12_DESCRIPTOR_RANGE_TYPE* rangeTypes,
        const UINT* registers,
        int rangeCount,
        ComPtr<ID3D12RootSignature>& outRootSignature)
    {
        constexpr int kMaxRanges = 8;
        if (rangeCount > kMaxRanges)
            return false;

        D3D12_DESCRIPTOR_RANGE ranges[kMaxRanges]{};
        D3D12_ROOT_PARAMETER params[kMaxRanges + 1]{};

        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        for (int i = 0; i < rangeCount; ++i)
        {
            ranges[i].RangeType          = rangeTypes[i];
            ranges[i].NumDescriptors     = 1;
            ranges[i].BaseShaderRegister = registers[i];
            ranges[i].RegisterSpace      = 0;
            ranges[i].OffsetInDescriptorsFromTableStart = 0;

            params[i + 1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
            params[i + 1].DescriptorTable.pDescriptorRanges   = &ranges[i];
            params[i + 1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = static_cast<UINT>(rangeCount + 1);
        desc.pParameters   = params;
        desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
            return false;

        return SUCCEEDED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&outRootSignature)));
    }

    bool BuildComputePso(
        ID3D12Device* device,
        ID3D12RootSignature* rootSignature,
        const DX12Shader& shader,
        ComPtr<ID3D12PipelineState>& outPso)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignature;
        desc.CS = shader.GetBytecode();
        return SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&outPso)));
    }
}

bool OceanSimulation::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "OceanSimulation: device is null.";
        return false;
    }

    struct ShaderSetup
    {
        const wchar_t* Path;
        DX12Shader* Shader;
        const char* Name;
    };
    const ShaderSetup shaders[] = {
        { L"Shaders\\Ocean_InitialSpectrum.hlsl", &mSpectrumShader, "Ocean_InitialSpectrum" },
        { L"Shaders\\Ocean_TimeSpectrum.hlsl",    &mEvolveShader,   "Ocean_TimeSpectrum"    },
        { L"Shaders\\Ocean_FFT.hlsl",             &mFftShader,      "Ocean_FFT"             },
        { L"Shaders\\Ocean_Assemble.hlsl",        &mAssembleShader, "Ocean_Assemble"        },
    };

    for (const ShaderSetup& setup : shaders)
    {
        ShaderCompileRequest request{ setup.Path, L"CSMain", L"cs_5_0", ShaderStage::Compute };
        if (!setup.Shader->Compile(request))
        {
            mLastError = std::string(setup.Name) + " compile failed: "
                + (setup.Shader->GetLastErrorMessage() ? setup.Shader->GetLastErrorMessage() : "unknown");
            return false;
        }
    }

    // Initial spectrum: b0 + u0.
    {
        const D3D12_DESCRIPTOR_RANGE_TYPE types[] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV };
        const UINT regs[] = { 0 };
        if (!BuildComputeRootSignature(device, types, regs, 1, mSpectrumRootSignature)
            || !BuildComputePso(device, mSpectrumRootSignature.Get(), mSpectrumShader, mSpectrumPso))
        {
            mLastError = "OceanSimulation: failed to create the initial-spectrum pipeline.";
            return false;
        }
    }

    // Time spectrum: b0 + t0 + u0 + u1.
    {
        const D3D12_DESCRIPTOR_RANGE_TYPE types[] = {
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV };
        const UINT regs[] = { 0, 0, 1 };
        if (!BuildComputeRootSignature(device, types, regs, 3, mEvolveRootSignature)
            || !BuildComputePso(device, mEvolveRootSignature.Get(), mEvolveShader, mEvolvePso))
        {
            mLastError = "OceanSimulation: failed to create the time-spectrum pipeline.";
            return false;
        }
    }

    // FFT: b0 + u0.
    {
        const D3D12_DESCRIPTOR_RANGE_TYPE types[] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV };
        const UINT regs[] = { 0 };
        if (!BuildComputeRootSignature(device, types, regs, 1, mFftRootSignature)
            || !BuildComputePso(device, mFftRootSignature.Get(), mFftShader, mFftPso))
        {
            mLastError = "OceanSimulation: failed to create the FFT pipeline.";
            return false;
        }
    }

    // Assemble: b0 + t0 + t1 + u0 + u1.
    {
        const D3D12_DESCRIPTOR_RANGE_TYPE types[] = {
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV };
        const UINT regs[] = { 0, 1, 0, 1 };
        if (!BuildComputeRootSignature(device, types, regs, 4, mAssembleRootSignature)
            || !BuildComputePso(device, mAssembleRootSignature.Get(), mAssembleShader, mAssemblePso))
        {
            mLastError = "OceanSimulation: failed to create the assemble pipeline.";
            return false;
        }
    }

    return true;
}

bool OceanSimulation::CreateTexture(
    DXGI_FORMAT format,
    bool allowUav,
    ComPtr<ID3D12Resource>& outResource)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask  = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = kOceanFftSize;
    desc.Height           = kOceanFftSize;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = format;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags            = allowUav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                     : D3D12_RESOURCE_FLAG_NONE;

    // Everything here lives in UNORDERED_ACCESS and is synchronised with UAV
    // barriers; the water shader reads them through SRVs in the same state,
    // which D3D12 allows because the resource is never in a read-only state.
    return SUCCEEDED(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outResource)));
}

bool OceanSimulation::CreateUav(
    ID3D12Resource* resource,
    DXGI_FORMAT format,
    D3D12_GPU_DESCRIPTOR_HANDLE& outHandle)
{
    ID3D12Device* device = DX12Context_GetDevice();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    if (device == nullptr || !DX12Context_AllocateSrvDescriptor(&cpu, &outHandle))
        return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format        = format;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(resource, nullptr, &desc, cpu);
    return true;
}

bool OceanSimulation::CreateSrv(
    ID3D12Resource* resource,
    DXGI_FORMAT format,
    D3D12_GPU_DESCRIPTOR_HANDLE& outHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE* outCpu)
{
    ID3D12Device* device = DX12Context_GetDevice();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    if (device == nullptr || !DX12Context_AllocateSrvDescriptor(&cpu, &outHandle))
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format                  = format;
    desc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(resource, &desc, cpu);

    if (outCpu)
        *outCpu = cpu;
    return true;
}

bool OceanSimulation::CreateResources()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    for (int i = 0; i < kOceanCascadeCount; ++i)
    {
        Cascade& cascade = mCascades[i];

        if (!CreateTexture(kFftFormat, true, cascade.Spectrum)
            || !CreateTexture(kFftFormat, true, cascade.DisplacementFft)
            || !CreateTexture(kFftFormat, true, cascade.HeightSlopeFft)
            || !CreateTexture(kOutputFormat, true, cascade.Displacement)
            || !CreateTexture(kOutputFormat, true, cascade.Normal))
        {
            mLastError = "OceanSimulation: failed to create cascade textures.";
            return false;
        }

        if (!CreateUav(cascade.Spectrum.Get(), kFftFormat, cascade.SpectrumUav)
            || !CreateSrv(cascade.Spectrum.Get(), kFftFormat, cascade.SpectrumSrv)
            || !CreateUav(cascade.DisplacementFft.Get(), kFftFormat, cascade.DisplacementFftUav)
            || !CreateSrv(cascade.DisplacementFft.Get(), kFftFormat, cascade.DisplacementFftSrv)
            || !CreateUav(cascade.HeightSlopeFft.Get(), kFftFormat, cascade.HeightSlopeFftUav)
            || !CreateSrv(cascade.HeightSlopeFft.Get(), kFftFormat, cascade.HeightSlopeFftSrv)
            || !CreateUav(cascade.Displacement.Get(), kOutputFormat, cascade.DisplacementUav)
            || !CreateSrv(cascade.Displacement.Get(), kOutputFormat, cascade.DisplacementSrv)
            || !CreateUav(cascade.Normal.Get(), kOutputFormat, cascade.NormalUav)
            || !CreateSrv(cascade.Normal.Get(), kOutputFormat, cascade.NormalSrv))
        {
            mLastError = "OceanSimulation: failed to allocate cascade descriptors.";
            return false;
        }
    }

    // Shared upload buffer for all pass constants.
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    const CD3DX12_RESOURCE_DESC bufferDesc =
        CD3DX12_RESOURCE_DESC::Buffer(256ull * kConstantSlotCount);
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mConstantBuffer))))
    {
        mLastError = "OceanSimulation: failed to allocate the constant buffer.";
        return false;
    }
    if (FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedConstants))))
    {
        mLastError = "OceanSimulation: failed to map the constant buffer.";
        return false;
    }

    return true;
}

void OceanSimulation::UavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    commandList->ResourceBarrier(1, &barrier);
}

void OceanSimulation::BuildInitialSpectrum(ID3D12GraphicsCommandList* commandList)
{
    commandList->SetComputeRootSignature(mSpectrumRootSignature.Get());
    commandList->SetPipelineState(mSpectrumPso.Get());

    for (int i = 0; i < kOceanCascadeCount; ++i)
    {
        Cascade& cascade = mCascades[i];

        SpectrumConstants constants;
        constants.Size        = kOceanFftSize;
        constants.PatchSize   = mSettings.PatchSizes[i];
        constants.WindSpeed   = mSettings.WindSpeed;
        constants.WindAngle   = mSettings.WindAngle;
        constants.Fetch       = mSettings.Fetch;
        constants.Depth       = mSettings.Depth;
        constants.Swell       = mSettings.Swell;
        constants.SpreadBlend = mSettings.SpreadBlend;
        constants.CutoffLow   = cascade.CutoffLow;
        constants.CutoffHigh  = cascade.CutoffHigh;
        constants.Amplitude   = mSettings.Amplitude;
        constants.Seed        = static_cast<std::uint32_t>(i * 7919 + 1);

        const std::size_t slot = mConstantCursor % kConstantSlotCount;
        ++mConstantCursor;
        std::memcpy(mMappedConstants + slot * 256, &constants, sizeof(constants));

        commandList->SetComputeRootConstantBufferView(
            0, mConstantBuffer->GetGPUVirtualAddress() + slot * 256);
        commandList->SetComputeRootDescriptorTable(1, cascade.SpectrumUav);
        commandList->Dispatch(kOceanFftSize / 8, kOceanFftSize / 8, 1);

        UavBarrier(commandList, cascade.Spectrum.Get());
    }
}

void OceanSimulation::Update(
    ID3D12GraphicsCommandList* commandList,
    const OceanSpectrumSettings& settings,
    float timeSeconds)
{
    if (!mIsInitialized || commandList == nullptr)
        return;

    if (settings != mSettings)
        mSpectrumDirty = true;
    mSettings = settings;

    // Split the spectrum into disjoint wavenumber bands so overlapping patches
    // never inject the same wave twice.  Patches are ordered large -> small, so
    // each cascade covers from the previous cascade's Nyquist limit up to its
    // own; cascade 0 starts at zero and the last one carries the short chop.
    for (int i = 0; i < kOceanCascadeCount; ++i)
    {
        auto nyquistOf = [this](int index) {
            const float patch = (std::max)(mSettings.PatchSizes[index], 0.01f);
            return kTwoPi / patch * (kOceanFftSize * 0.5f);
        };
        mCascades[i].CutoffLow  = (i == 0) ? 0.0f : nyquistOf(i - 1);
        mCascades[i].CutoffHigh = nyquistOf(i);
    }

    ID3D12DescriptorHeap* heap = DX12Context_GetSrvDescriptorHeap();
    if (heap == nullptr)
        return;
    commandList->SetDescriptorHeaps(1, &heap);

    if (mSpectrumDirty)
    {
        BuildInitialSpectrum(commandList);
        mSpectrumDirty = false;
    }

    // ---- Per-cascade: evolve -> FFT rows -> FFT columns -> assemble ----
    for (int i = 0; i < kOceanCascadeCount; ++i)
    {
        Cascade& cascade = mCascades[i];

        // 1. Time evolution.
        {
            EvolveConstants constants;
            constants.Size      = kOceanFftSize;
            constants.PatchSize = mSettings.PatchSizes[i];
            constants.Time      = timeSeconds;
            constants.Depth     = mSettings.Depth;

            const std::size_t slot = mConstantCursor % kConstantSlotCount;
            ++mConstantCursor;
            std::memcpy(mMappedConstants + slot * 256, &constants, sizeof(constants));

            commandList->SetComputeRootSignature(mEvolveRootSignature.Get());
            commandList->SetPipelineState(mEvolvePso.Get());
            commandList->SetComputeRootConstantBufferView(
                0, mConstantBuffer->GetGPUVirtualAddress() + slot * 256);
            commandList->SetComputeRootDescriptorTable(1, cascade.SpectrumSrv);
            commandList->SetComputeRootDescriptorTable(2, cascade.DisplacementFftUav);
            commandList->SetComputeRootDescriptorTable(3, cascade.HeightSlopeFftUav);
            commandList->Dispatch(kOceanFftSize / 8, kOceanFftSize / 8, 1);

            UavBarrier(commandList, cascade.DisplacementFft.Get());
            UavBarrier(commandList, cascade.HeightSlopeFft.Get());
        }

        // 2. Inverse FFT: rows then columns, on both packed textures.
        commandList->SetComputeRootSignature(mFftRootSignature.Get());
        commandList->SetPipelineState(mFftPso.Get());

        struct FftTarget { ID3D12Resource* Resource; D3D12_GPU_DESCRIPTOR_HANDLE Uav; };
        const FftTarget targets[] = {
            { cascade.DisplacementFft.Get(), cascade.DisplacementFftUav },
            { cascade.HeightSlopeFft.Get(),  cascade.HeightSlopeFftUav  },
        };

        for (const FftTarget& target : targets)
        {
            for (std::uint32_t direction = 0; direction < 2; ++direction)
            {
                FftConstants constants;
                constants.Direction = direction;
                // Normalising on both passes gives the 1/N^2 an inverse 2-D
                // transform requires.
                constants.Normalize = 1;

                const std::size_t slot = mConstantCursor % kConstantSlotCount;
                ++mConstantCursor;
                std::memcpy(mMappedConstants + slot * 256, &constants, sizeof(constants));

                commandList->SetComputeRootConstantBufferView(
                    0, mConstantBuffer->GetGPUVirtualAddress() + slot * 256);
                commandList->SetComputeRootDescriptorTable(1, target.Uav);
                // One thread group per row/column.
                commandList->Dispatch(kOceanFftSize, 1, 1);

                UavBarrier(commandList, target.Resource);
            }
        }

        // 3. Assemble into the sampled displacement / normal maps.
        {
            AssembleConstants constants;
            constants.Size       = kOceanFftSize;
            constants.PatchSize  = mSettings.PatchSizes[i];
            constants.Choppiness = mSettings.Choppiness;
            constants.FoamBias   = mSettings.FoamBias;

            const std::size_t slot = mConstantCursor % kConstantSlotCount;
            ++mConstantCursor;
            std::memcpy(mMappedConstants + slot * 256, &constants, sizeof(constants));

            commandList->SetComputeRootSignature(mAssembleRootSignature.Get());
            commandList->SetPipelineState(mAssemblePso.Get());
            commandList->SetComputeRootConstantBufferView(
                0, mConstantBuffer->GetGPUVirtualAddress() + slot * 256);
            commandList->SetComputeRootDescriptorTable(1, cascade.DisplacementFftSrv);
            commandList->SetComputeRootDescriptorTable(2, cascade.HeightSlopeFftSrv);
            commandList->SetComputeRootDescriptorTable(3, cascade.DisplacementUav);
            commandList->SetComputeRootDescriptorTable(4, cascade.NormalUav);
            commandList->Dispatch(kOceanFftSize / 8, kOceanFftSize / 8, 1);

            UavBarrier(commandList, cascade.Displacement.Get());
            UavBarrier(commandList, cascade.Normal.Get());
        }
    }
}
