#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "SkyGlareSettings.h"

#include "../QtUi/UiTypes.h"

#include <DirectXMath.h>
#include <string>

// SkyGlareRenderer - the glare around the sun and the moon, separate from the level's
// bloom.
//
// The sky draws the sun disc only bright enough to clip (kSunDiscRadianceScale), so
// the bloom treats it like any other bright pixel. This pass adds what the real sun
// does to an eye or a lens: its full energy - illuminance over one pixel's solid
// angle - spread by an analytic point spread function around its pixel (a power-law
// halo and the aperture's diffraction spikes, each normalised to unit energy on the
// CPU), dimmed by how much of the disc is visible. The moon gets the same treatment
// from the light its drawn disc sends, which gives it the soft glow a bright moon has
// in a dark sky. Being analytic, it costs one pass whatever the reach, and its shape
// is set by each body's SkyGlareSettings alone.
//
// Runs in the bloom stage, after the bloom and before the lens flare. Reads the
// bloomed image (and the image before bloom, for the cloud test) in
// PIXEL_SHADER_RESOURCE, writes its own output texture of the same size; all of them
// in PIXEL_SHADER_RESOURCE on return. Depth must be readable from compute.
class SkyGlareRenderer
{
public:
    enum SourceIndex : UINT { kSun = 0, kMoon = 1, kSourceCount = 2 };

    struct Source
    {
        const SkyGlareSettings* Settings = nullptr; // null or disabled = no glare
        DirectX::XMFLOAT3 Direction{};    // from the scene toward the body
        DirectX::XMFLOAT3 Illuminance{};  // the light it sends the camera, engine units
        float DiscRadius = 0.0f;          // angular radius as drawn, radians
        float DiscLuminance = 0.0f;       // what the cloud test expects of its disc
        // Judge the disc on its mean brightness rather than tap by tap: for the moon,
        // whose crescent is mostly dark. DiscLuminance is then the expected mean.
        bool  AverageDiscTest = false;
    };

    struct FrameInputs
    {
        ID3D12Resource* Input = nullptr;         // bloomed image
        ID3D12Resource* SceneColor = nullptr;    // the image before bloom; may be Input
        ID3D12Resource* Depth = nullptr;
        DXGI_FORMAT DepthSrvFormat = DXGI_FORMAT_R32_FLOAT;
        DirectX::XMFLOAT4X4 ViewProjection{};   // non-jittered, row-vector convention
        DirectX::XMFLOAT4X4 Projection{};       // non-jittered
        float DeltaSeconds = 0.0f;
        Source Sources[kSourceCount];
    };

    bool Initialize(UINT width, UINT height);
    void Shutdown();

    void Apply(ID3D12GraphicsCommandList* commandList, const FrameInputs& inputs);

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

    // Descriptor layout of one table in mHeap; registers as in SkyGlare.hlsl.
    enum Descriptor : UINT
    {
        SrvInput = 0, SrvSceneColor, SrvDepth,
        UavOutput, UavVisibility,
        DescriptorsPerTable
    };

    // Mirrors GlareSource in SkyGlare.hlsl.
    struct SourceCb
    {
        DirectX::XMFLOAT2 Pixel;
        float OffscreenFade;
        float Reset;
        DirectX::XMFLOAT3 Energy;
        float SampleRadius;
        float HaloCore;
        float HaloRadius;
        float HaloFalloff;
        float HaloWeight;
        float SpikeLength;
        float SpikeCore;
        float SpikeWeight;
        float SpikeCount;
        DirectX::XMFLOAT3 ChannelScale;
        float ExpectedDisc;
        float AverageDiscTest;
        float Pad[3];
        DirectX::XMFLOAT4 SpikeDirections[8];
    };

    // Mirrors SkyGlareConstants in SkyGlare.hlsl.
    struct SkyGlareCb
    {
        DirectX::XMFLOAT2 OutputSize;
        float VisibilityBlend;
        float SpikeWidth;
        SourceCb Sources[kSourceCount];
    };

    // What a source's PSF normalisations depend on; recomputed only when it changes.
    struct ShapeKey
    {
        float HaloCore = -1.0f, HaloRadius = 0, HaloFalloff = 0, SpikeLength = 0, SpikeCore = 0;
        int   SpikeCount = 0;
        bool operator==(const ShapeKey& o) const
        {
            return HaloCore == o.HaloCore && HaloRadius == o.HaloRadius && HaloFalloff == o.HaloFalloff
                && SpikeLength == o.SpikeLength && SpikeCore == o.SpikeCore && SpikeCount == o.SpikeCount;
        }
    };

    struct SourceState
    {
        ShapeKey Shape{};
        float HaloIntegral = 1.0f;   // of the halo over the image plane, in pixels
        float SpikeIntegral = 1.0f;  // of every spike's along-profile, both directions
        bool  VisibilityValid = false;
    };

    bool CreatePipelines();
    bool CreateOutput(UINT width, UINT height);
    // Fills one source's constants; false when it adds nothing this frame.
    bool PrepareSource(const Source& source, const DirectX::XMMATRIX& viewProjection,
                       float pixelsPerRadian, SourceState& state, SourceCb& cb) const;
    static void UpdateNormalisation(const ShapeKey& key, SourceState& state);
    D3D12_CPU_DESCRIPTOR_HANDLE HeapCpu(UINT table, UINT descriptor) const;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapGpu(UINT table, UINT descriptor) const;

    DX12Shader mVisibilityShader, mApplyShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mVisibilityPso, mApplyPso;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;   // ringed, kFrameSlots
    std::uint8_t* mMappedCb = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mHeap;
    UINT mDescriptorSize = 0;
    UINT64 mFrameCounter = 0;

    // The smoothed visibilities, one float per source, carried from frame to frame on
    // the GPU. A buffer, so it decays to COMMON after each submission and is promoted
    // again by the dispatches.
    Microsoft::WRL::ComPtr<ID3D12Resource> mVisibilityBuffer;
    SourceState mSources[kSourceCount];

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
