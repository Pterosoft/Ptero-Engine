#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "BloomSettings.h"

#include "../QtUi/UiTypes.h"

#include <array>
#include <string>

// BloomRenderer implements physical mip-chain bloom (Dual-Kawase style):
//
//   1. Threshold / prefilter pass  – extract bright regions from the HDR scene.
//   2. Downsample chain            – progressively halve resolution (up to 8 levels).
//   3. Upsample chain              – progressively double, adding contributions back.
//   4. Composite pass              – additive blend of accumulated bloom onto source.
//
// The renderer writes the composited result into a dedicated output texture that
// can be used as input to the AgX tonemapper (or displayed directly if tonemap is off).
class BloomRenderer
{
public:
    // Create or resize resources for the given render resolution.
    bool Initialize(UINT width, UINT height);

    // Release all GPU resources.
    void Shutdown();

    // Run the full bloom pipeline.
    // sourceResource must be in PIXEL_SHADER_RESOURCE state on entry.
    // sourceCpuSrv   is the CPU SRV descriptor for sourceResource.
    // On return the output is in PIXEL_SHADER_RESOURCE state.
    void Apply(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource*            sourceResource,
        D3D12_CPU_DESCRIPTOR_HANDLE sourceCpuSrv,
        const BloomSettings&       settings);

    // Ui-displayable GPU SRV of the composited output.
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv()   const { return mOutputUiSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv()   const { return mOutputUiSrvCpu; }
    ID3D12Resource*             GetOutputResource() const { return mOutputTexture.Get(); }
    UiTextureID                 GetOutputTextureId() const { return mOutputTextureId; }

    bool IsInitialized() const { return mIsInitialized; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    static constexpr int kMaxMips = 8;

    bool CreatePipelines();
    bool CreateTextures(UINT width, UINT height);

    // ---- constant buffer layout (must match bloom shaders) -----------
    struct alignas(256) BloomCbData
    {
        float Threshold;
        float Knee;
        float Intensity;
        float Radius;
        UINT  SrcWidth;
        UINT  SrcHeight;
        UINT  DstWidth;
        UINT  DstHeight;
        int   MipLevel;
        int   MaxMips;
        float Pad0;
        float Pad1;
    };

    // ---- pipelines --------------------------------------------------
    DX12Shader mDownsampleShader;
    DX12Shader mUpsampleShader;
    DX12Shader mCompositeShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDownsamplePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mUpsamplePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mCompositePso;
    Microsoft::WRL::ComPtr<ID3D12Resource>      mConstantBuffer;
    void* mMappedCb = nullptr;

    // Each pass has its own CB offset so multiple dispatches per frame can coexist.
    // Layout: [0] = prefilter/downsample mip0, [1..kMaxMips-1] = remaining downs, [kMaxMips..2*kMaxMips-1] = ups, [2*kMaxMips] = composite
    static constexpr int kMaxCbSlots = 2 * kMaxMips + 1;

    // ---- per-mip textures (UAV + SRV) -------------------------------
    // mMipTextures[i] is the result of downsampling to level i.
    // mMipTextures[0] is the prefiltered half-res source.
    struct MipLevel
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Texture;
        UINT Width  = 0;
        UINT Height = 0;

        // Private compute heap slots for this mip.
        D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE UavCpu{}; // non-shader-visible for potential clears
        D3D12_CPU_DESCRIPTOR_HANDLE UavShaderVisibleCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE UavShaderVisibleGpu{};
    };
    std::array<MipLevel, kMaxMips> mMips{};
    int mNumMips = 0;

    // Final composited output (same size as the source input).
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUavShaderVisibleCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUavShaderVisibleGpu{};

    // Non-shader-visible heap for UAV clears.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mNsUavHeap;

    // Private compute heap: 2 SRVs + 1 UAV per dispatch, re-populated each frame.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mComputeHeap;
    UINT mComputeHeapStride  = 0;
    UINT mComputeHeapSlots   = 0; // total slots allocated

    // Shared-context-heap slot for Ui display of the output.
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputUiSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputUiSrvGpu{};
    UiTextureID mOutputTextureId  = UiTextureID_Invalid;
    bool mUiSlotAllocated      = false;
    bool mDescriptorsAllocated    = false;

    UINT mWidth  = 0;
    UINT mHeight = 0;
    bool mIsInitialized = false;
    std::string mLastError;
};
