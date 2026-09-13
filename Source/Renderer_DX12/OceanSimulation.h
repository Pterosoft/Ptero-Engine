#pragma once

// OceanSimulation
// ---------------
// Cascaded Tessendorf FFT ocean.  Each cascade is an independent 256x256
// inverse-FFT patch covering a different band of the wave spectrum:
//
//   cascade 0 - large swell   (long patch, low wavenumbers, shapes silhouettes)
//   cascade 1 - wind waves    (medium patch, the main surface structure)
//   cascade 2 - short chop    (small patch, high-frequency glint detail)
//
// Wavenumber cutoffs keep the bands disjoint so energy is never double counted
// where the patches overlap.  Per frame the work is:
//
//   time spectrum  ->  FFT rows  ->  FFT columns  ->  assemble
//
// producing, for each cascade, a displacement map (XY choppy + Z height, W
// folding) and a normal map.  The initial spectrum is only rebuilt when the
// wind/fetch parameters actually change.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <string>

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

// Transform size is fixed: the FFT compute shader's thread group must match it
// exactly (group barriers require every thread to participate).
inline constexpr std::uint32_t kOceanFftSize = 256;
inline constexpr int kOceanCascadeCount = 3;

// Artist-facing ocean simulation parameters.
struct OceanSpectrumSettings
{
    float WindSpeed = 9.0f;      // m/s
    float WindAngle = 0.3f;      // radians
    float Fetch     = 100.0f;    // km
    float Depth     = 200.0f;    // m
    float Swell     = 0.35f;     // 0..1, energy held in the long swell
    float SpreadBlend = 0.6f;    // 0..1, directional tightness
    float Amplitude = 1.0f;      // overall scale
    float Choppiness = 1.1f;     // horizontal displacement scale
    float FoamBias  = 1.0f;      // Jacobian level where folding starts

    // World-space size of each cascade patch, metres.
    std::array<float, kOceanCascadeCount> PatchSizes{ 512.0f, 96.0f, 18.0f };

    bool operator==(const OceanSpectrumSettings& other) const;
    bool operator!=(const OceanSpectrumSettings& other) const { return !(*this == other); }
};

class OceanSimulation
{
public:
    bool Initialize();
    void Shutdown();

    // Runs the per-frame simulation for every cascade.  Rebuilds the static
    // spectrum first if `settings` changed since the last call.
    void Update(
        ID3D12GraphicsCommandList* commandList,
        const OceanSpectrumSettings& settings,
        float timeSeconds);

    bool IsReady() const { return mIsInitialized; }

    // Bound one cascade per descriptor table.  The shared SRV heap is a bump
    // allocator, so consecutive allocations happen to be contiguous, but not
    // depending on that keeps this robust if allocation order ever changes
    // (same reasoning as TerrainRenderer's per-layer tables).
    D3D12_GPU_DESCRIPTOR_HANDLE GetDisplacementSrv(int cascade) const
    {
        return mCascades[cascade].DisplacementSrv;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GetNormalSrv(int cascade) const
    {
        return mCascades[cascade].NormalSrv;
    }

    float GetPatchSize(int cascade) const { return mSettings.PatchSizes[cascade]; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    struct Cascade
    {
        // Static spectrum h0(k); rebuilt only when the settings change.
        Microsoft::WRL::ComPtr<ID3D12Resource> Spectrum;
        // Ping-pong targets carrying the complex fields through the FFT.
        Microsoft::WRL::ComPtr<ID3D12Resource> DisplacementFft;
        Microsoft::WRL::ComPtr<ID3D12Resource> HeightSlopeFft;
        // Final sampled outputs.
        Microsoft::WRL::ComPtr<ID3D12Resource> Displacement;
        Microsoft::WRL::ComPtr<ID3D12Resource> Normal;

        D3D12_GPU_DESCRIPTOR_HANDLE SpectrumUav{};
        D3D12_GPU_DESCRIPTOR_HANDLE SpectrumSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE DisplacementFftUav{};
        D3D12_GPU_DESCRIPTOR_HANDLE DisplacementFftSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE HeightSlopeFftUav{};
        D3D12_GPU_DESCRIPTOR_HANDLE HeightSlopeFftSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE DisplacementUav{};
        D3D12_GPU_DESCRIPTOR_HANDLE NormalUav{};
        D3D12_GPU_DESCRIPTOR_HANDLE DisplacementSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE NormalSrv{};

        float CutoffLow  = 0.0f;
        float CutoffHigh = 0.0f;
    };

    struct alignas(256) SpectrumConstants
    {
        std::uint32_t Size = kOceanFftSize;
        float PatchSize = 0.0f;
        float WindSpeed = 0.0f;
        float WindAngle = 0.0f;
        float Fetch = 0.0f;
        float Depth = 0.0f;
        float Swell = 0.0f;
        float SpreadBlend = 0.0f;
        float CutoffLow = 0.0f;
        float CutoffHigh = 0.0f;
        float Amplitude = 1.0f;
        std::uint32_t Seed = 0;
        std::byte Padding[208]{};
    };
    static_assert(sizeof(SpectrumConstants) == 256);

    struct alignas(256) EvolveConstants
    {
        std::uint32_t Size = kOceanFftSize;
        float PatchSize = 0.0f;
        float Time = 0.0f;
        float Depth = 0.0f;
        std::byte Padding[240]{};
    };
    static_assert(sizeof(EvolveConstants) == 256);

    struct alignas(256) FftConstants
    {
        std::uint32_t Direction = 0;
        std::uint32_t Normalize = 0;
        std::uint32_t Pad0 = 0;
        std::uint32_t Pad1 = 0;
        std::byte Padding[240]{};
    };
    static_assert(sizeof(FftConstants) == 256);

    struct alignas(256) AssembleConstants
    {
        std::uint32_t Size = kOceanFftSize;
        float PatchSize = 0.0f;
        float Choppiness = 1.0f;
        float FoamBias = 1.0f;
        std::byte Padding[240]{};
    };
    static_assert(sizeof(AssembleConstants) == 256);

    bool CreatePipelines();
    bool CreateResources();
    bool CreateTexture(
        DXGI_FORMAT format,
        bool allowUav,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outResource);
    bool CreateUav(ID3D12Resource* resource, DXGI_FORMAT format, D3D12_GPU_DESCRIPTOR_HANDLE& outHandle);
    bool CreateSrv(
        ID3D12Resource* resource,
        DXGI_FORMAT format,
        D3D12_GPU_DESCRIPTOR_HANDLE& outHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE* outCpu = nullptr);
    void BuildInitialSpectrum(ID3D12GraphicsCommandList* commandList);
    void UavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource);

    std::array<Cascade, kOceanCascadeCount> mCascades;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mSpectrumRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mSpectrumPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mEvolveRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mEvolvePso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mFftRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mFftPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mAssembleRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mAssemblePso;

    DX12Shader mSpectrumShader;
    DX12Shader mEvolveShader;
    DX12Shader mFftShader;
    DX12Shader mAssembleShader;

    // One constant-buffer slot per cascade per pass, cycled per frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    std::uint8_t* mMappedConstants = nullptr;
    std::size_t   mConstantCursor = 0;

    OceanSpectrumSettings mSettings;
    bool mSpectrumDirty = true;
    bool mIsInitialized = false;
    std::string mLastError;
};
