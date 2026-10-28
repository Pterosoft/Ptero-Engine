#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "BloomSettings.h"

#include "../QtUi/UiTypes.h"

#include <string>
#include <vector>

// FftBloomRenderer - convolution bloom through the FFT (BloomMethod::FftConvolution).
//
// A spatial convolution with a kernel the size of the frame costs O(pixels x kernel); in
// the frequency domain it is two transforms and a multiply, whatever the kernel's size.
// That is what makes a physically shaped glare kernel affordable: a sharp core, a
// power-law halo reaching across the whole frame, and the aperture's diffraction spikes
// (along the iris-edge normals, as LensDiffraction's pattern has them) - the look a real
// lens and eye give a bright light, which a mip-chain blur cannot produce.
//
// Per frame (FftBloom.hlsl): threshold + downscale into an N x N grid, forward FFT of the
// rows, forward FFT x kernel spectrum x inverse FFT of the columns, inverse FFT of the
// rows, composite. The kernel is built on the CPU and transformed on the GPU only when a
// kernel setting changes.
//
// Same contract as BloomRenderer: reads an HDR image in PIXEL_SHADER_RESOURCE, writes its
// own output texture of the same size, both in PIXEL_SHADER_RESOURCE on return.
class FftBloomRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    void Shutdown();

    void Apply(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            sourceResource,
        D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuSrv,
        const BloomSettings&       settings);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()    const { return mOutputUiSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()    const { return mOutputUiSrvCpu; }
    ID3D12Resource*             GetOutputResource()  const { return mOutputTexture.Get(); }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId; }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    static constexpr UINT kFrameSlots = 3;

    // Descriptor layout of one table in mHeap; registers as in FftBloom.hlsl.
    enum Descriptor : UINT
    {
        SrvSource = 0, SrvUnused1, SrvUnused2, SrvKernelRG, SrvKernelB,
        UavDataRG, UavDataB, UavOutput,
        DescriptorsPerTable
    };
    // Tables 0..2: one per frame in flight (the source view changes). Table 3: the kernel
    // build, whose UAVs are the kernel buffers.
    static constexpr UINT kKernelTable = kFrameSlots;
    static constexpr UINT kTableCount = kFrameSlots + 1;

    // Mirrors FftBloomConstants in FftBloom.hlsl.
    struct FftBloomCb
    {
        UINT  N;
        UINT  Log2N;
        UINT  RegionWidth;
        UINT  RegionHeight;
        UINT  SourceWidth;
        UINT  SourceHeight;
        UINT  OutputWidth;
        UINT  OutputHeight;
        float Threshold;
        float Knee;
        float Intensity;
        float Pad0;
    };

    struct KernelKey
    {
        int   N = 0;
        float Size = 0, Falloff = 0, Halo = 0, Streak = 0, Rotation = 0, Spread = 0;
        int   Blades = 0;
        bool operator==(const KernelKey& o) const
        {
            return N == o.N && Size == o.Size && Falloff == o.Falloff && Halo == o.Halo
                && Streak == o.Streak && Rotation == o.Rotation && Spread == o.Spread && Blades == o.Blades;
        }
    };

    bool CreatePipelines();
    bool CreateOutput(UINT width, UINT height);
    bool EnsureGrid(UINT n);
    bool EnsureKernel(ID3D12GraphicsCommandList* commandList, const BloomSettings& settings, UINT n);
    void WriteTableDescriptors();
    D3D12_CPU_DESCRIPTOR_HANDLE HeapCpu(UINT table, UINT descriptor) const;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapGpu(UINT table, UINT descriptor) const;
    void RegionFor(UINT n, UINT& width, UINT& height) const;

    DX12Shader mPrefilterShader, mRowsForwardShader, mRowsInverseShader;
    DX12Shader mColumnsForwardShader, mColumnsConvolveShader, mCompositeShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPrefilterPso, mRowsForwardPso, mRowsInversePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mColumnsForwardPso, mColumnsConvolvePso, mCompositePso;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;   // ringed, kFrameSlots + 1 (kernel build)
    std::uint8_t* mMappedCb = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mHeap;
    UINT mDescriptorSize = 0;
    UINT64 mFrameCounter = 0;

    // Frequency-domain working set, N x N: R,G complex in a float4, B complex in a float2.
    UINT mGridN = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDataRG, mDataB;     // always UNORDERED_ACCESS
    Microsoft::WRL::ComPtr<ID3D12Resource> mKernelRG, mKernelB; // NON_PIXEL_SHADER_RESOURCE once built
    Microsoft::WRL::ComPtr<ID3D12Resource> mKernelUpload;       // kept until the next rebuild (which waits for the GPU)
    KernelKey mKernelKey{};
    bool mKernelValid = false;

    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu{};
    UiTextureID mOutputTextureId = UiTextureID_Invalid;
    bool mUiSlotAllocated = false;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
