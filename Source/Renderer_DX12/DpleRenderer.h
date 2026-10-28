#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "DpleSettings.h"

#include <DirectXMath.h>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// DpleRenderer
// Deterministic Photoreal Lighting Enhancer, ported from the Unreal plugin of the same name.
// Deterministic compute passes that re-present lighting the renderer has already resolved:
// multiscale AO applied to the indirect share only, sun contact shadows, a per-material
// response, a micro-specular relit lobe and (optionally) frequency-separated detail.
//
// Where it runs: after TAA / SMAA / DLSS / FSR, before sharpening, bloom and the tonemapper
// - the same place the plugin hooks in Unreal (BL_SceneColorBeforeBloom). Linear HDR, at
// output resolution. After the upscale, because boosting detail before it means fighting
// the upscaler's own suppression of that band, and DPLE then owns the temporal stability of
// the signals it adds. Before tonemapping, because occlusion only means anything in linear.
//
// The consequence to keep in mind everywhere: the image is at output resolution and
// un-jittered, while the G-Buffer is at render resolution and still carries this frame's
// jitter. The shaders bridge the two through Dple_Common.hlsli.
//
// Per frame, all compute:
//   Prepare        working-res guide buffer (world normal, view depth)
//   MultiscaleAO   micro / contact / broad radii in one dispatch
//   ContactShadow  screen-space trace toward the sun
//   Temporal       reprojection + neighbourhood clamp; packs the four signals
//   Denoise x2     separable bilateral filter (after history is kept, never before)
//   Composite      bilateral upsample, material response, indirect-only AO, micro-specular
//   Detail         downsample chain + frequency recombination (optional)
// The result is copied back over the input image, so no pass downstream has to know DPLE
// ran - the same contract as SsrRenderer. That is safe for every image it can be handed:
// TAA copies its history before returning, and the upscalers keep history internally.
class DpleRenderer
{
public:
    struct FrameInputs
    {
        // The image to enhance in place. R16G16B16A16_FLOAT, PIXEL_SHADER_RESOURCE on entry,
        // left that way. Its size is the output resolution.
        ID3D12Resource* Image = nullptr;
        // True when Image is TAA's or an upscaler's output, which has the camera jitter
        // resolved out; G-Buffer lookups then add this frame's jitter. False for raw scene
        // colour, which is jittered exactly like the G-Buffer.
        bool ImageIsJitterResolved = false;

        // G-Buffer and depth, already readable from compute.
        D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE NormalSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE MaterialSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE AlbedoSrv{};
        UINT GBufferWidth = 0;
        UINT GBufferHeight = 0;

        // Row-vector matrices as DirectXMath builds them (not transposed).
        DirectX::XMFLOAT4X4 ViewProjection{};          // jittered: what the G-Buffer was drawn with
        DirectX::XMFLOAT4X4 ViewProjectionNoJitter{};
        DirectX::XMFLOAT4X4 ProjectionNoJitter{};
        DirectX::XMFLOAT3   CameraPosition{};
        DirectX::XMFLOAT3   CameraForward{ 0.0f, 1.0f, 0.0f };
        // This frame's jitter in render pixels, NDC +Y up (the engine's mCurrentCameraJitter).
        float JitterX = 0.0f;
        float JitterY = 0.0f;

        // Unit vector toward the sun, and how much it counts (0 = no sun to key off).
        DirectX::XMFLOAT3 SunDirection{ 0.0f, 0.0f, 1.0f };
        float SunWeight = 0.0f;

        float DeltaSeconds = 0.0f;
    };

    // Records the pass chain. Returns false (and leaves the image untouched) when the pass
    // could not run; HasInitFailed() then says whether it ever will.
    bool Dispatch(ID3D12GraphicsCommandList* commandList, const DpleSettings& settings, const FrameInputs& inputs);

    // Call on every frame the pass does not run, so re-enabling it does not reproject a
    // history that is several frames stale.
    void InvalidateHistory()
    {
        mHistoryValid = false;
        mHasPreviousCamera = false;
    }

    void Shutdown();

    bool HasInitFailed() const { return mInitFailed; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES       State = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE Srv{};
        D3D12_CPU_DESCRIPTOR_HANDLE UavCpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE Uav{};
        UINT Width = 0;
        UINT Height = 0;
    };

    // Every pass shares one root layout shape:
    //   [0]          CBV b0 (DpleConstants)
    //   [1]          8 root constants, b1 (DplePassConstants)
    //   [2 .. 2+S)   one single-SRV table per texture input, t0..t(S-1); t0-t3 are always
    //                depth, G-Buffer normal, material and albedo
    //   [2+S]        one UAV table, u0
    // One table per texture lets the ping-ponged history swap by handle each frame.
    struct Pass
    {
        DX12Shader Shader;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
        UINT SrvTables = 0;
    };

    // Mirrors DpleConstants in Dple_Common.hlsli.
    struct DpleCbData
    {
        DirectX::XMFLOAT4X4 InvViewProj{};
        DirectX::XMFLOAT4X4 ViewProj{};
        DirectX::XMFLOAT4X4 ViewProjNoJitter{};
        DirectX::XMFLOAT4X4 PrevViewProjNoJitter{};

        DirectX::XMFLOAT3 CameraPos{};      float SunWeight = 0.0f;
        DirectX::XMFLOAT3 CameraForward{};  float ProjScaleX = 1.0f;
        DirectX::XMFLOAT3 SunDirection{};   float ProjScaleY = 1.0f;
        DirectX::XMFLOAT2 JitterUV{};       DirectX::XMFLOAT2 GBufferSize{};
        DirectX::XMFLOAT2 InvGBufferSize{}; DirectX::XMFLOAT2 OutputSize{};
        DirectX::XMFLOAT2 InvOutputSize{};  DirectX::XMFLOAT2 InvWorkSize{};
        UINT WorkSizeX = 0;  UINT WorkSizeY = 0;  UINT FrameIndex = 0;  UINT DebugView = 0;
        UINT Flags = 0;      UINT HistoryValid = 0; UINT ContactShadowSteps = 10; INT DenoiseRadius = 0;

        float MicroRadius = 0.0f;               float ContactRadius = 0.0f;            float BroadRadius = 0.0f;              float AOBias = 0.0f;
        float AOPower = 1.0f;                   float MicroAOIntensity = 0.0f;         float ContactAOIntensity = 0.0f;       float BroadAOIntensity = 0.0f;
        float MaxCombinedAO = 0.0f;             float ContactShadowLength = 0.0f;      float ContactShadowThickness = 0.0f;   float ContactShadowIntensity = 0.0f;
        float SkinAOScale = 1.0f;               float SkinWarmth = 0.0f;               float FoliageAOScale = 1.0f;           float FoliageSaturation = 0.0f;
        float WetRoughnessThreshold = 0.0f;     float WetResponseStrength = 0.0f;      float SpecularOcclusionStrength = 0.0f; float MicroSpecularStrength = 0.0f;
        float MicroSpecularRoughnessMax = 1.0f; float MicroSpecularDetailScale = 1.0f; float IndirectFractionMin = 0.0f;      float IndirectFractionMax = 1.0f;
        float DirectLightWeight = 1.0f;         float HistoryWeightStable = 0.0f;      float HistoryWeightMoving = 0.0f;      float DisocclusionDepthTolerance = 0.05f;
        float NeighborhoodClampScale = 1.0f;    float DenoiseDepthTolerance = 0.05f;   float FineDetailStrength = 0.0f;       float StructureStrength = 0.0f;
        float MaxLuminanceChange = 0.0f;        float DetailMotionScale = 1.0f;        float Pad0 = 0.0f;                     float Pad1 = 0.0f;
    };

    // Mirrors DplePassConstants in Dple_Common.hlsli.
    struct PassConstants
    {
        INT   StepDirectionX = 0;  INT   StepDirectionY = 0;
        UINT  DestWidth = 0;       UINT  DestHeight = 0;
        float SourceWidth = 0.0f;  float SourceHeight = 0.0f;
        float SourceInvWidth = 0.0f; float SourceInvHeight = 0.0f;
    };

    // The engine keeps up to three frames in flight, so everything the CPU rewrites per
    // frame (constants, the input image's SRV) is ringed three deep.
    static constexpr UINT kFramesInFlight = 3;
    static constexpr UINT kCbStride = (sizeof(DpleCbData) + 255u) & ~255u;
    static constexpr UINT kDetailLevels = 4;

    bool EnsureInitialized();
    bool CreatePass(Pass& pass, const wchar_t* shaderFile, UINT srvTables);
    bool AllocateDescriptors();
    bool EnsureSize(UINT outputWidth, UINT outputHeight, UINT workWidth, UINT workHeight);
    void CreateTexture(Texture& texture, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name);

    void Require(Texture& texture, D3D12_RESOURCE_STATES state);
    void FlushBarriers(ID3D12GraphicsCommandList* commandList);
    void Bind(ID3D12GraphicsCommandList* commandList, const Pass& pass, D3D12_GPU_VIRTUAL_ADDRESS constants,
              const PassConstants& passConstants,
              std::initializer_list<D3D12_GPU_DESCRIPTOR_HANDLE> srvs,
              D3D12_GPU_DESCRIPTOR_HANDLE uav);

    float ComputeDetailMotionScale(const DpleSettings& settings, const FrameInputs& inputs) const;

    Pass mPreparePass;
    Pass mAmbientOcclusionPass;
    Pass mContactShadowPass;
    Pass mTemporalPass;
    Pass mDenoisePass;
    Pass mCompositePass;
    Pass mDownsamplePass;
    Pass mDetailPass;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    std::uint8_t* mMappedCb = nullptr;
    UINT          mFrameSlot = 0;

    // Working resolution.
    Texture mGuide[2];          // ping-ponged: [mBufferIndex] this frame, the other history
    Texture mOcclusion[2];      // ditto: temporal output = next frame's history
    Texture mAmbientOcclusion;
    Texture mContactShadow;
    Texture mDenoiseTemp;
    Texture mDenoised;
    UINT    mBufferIndex = 0;

    // Output resolution.
    Texture mComposite;
    Texture mDetail;
    Texture mDetailLevels[kDetailLevels];   // 1/2, 1/4, 1/8, 1/16

    // SRV of the caller's image, rewritten every frame into its own ring slot.
    D3D12_CPU_DESCRIPTOR_HANDLE mImageSrvCpu[kFramesInFlight]{};
    D3D12_GPU_DESCRIPTOR_HANDLE mImageSrv[kFramesInFlight]{};

    std::vector<D3D12_RESOURCE_BARRIER> mPendingBarriers;

    // Camera state from the last frame DPLE ran, for reprojection and motion suppression.
    DirectX::XMFLOAT4X4 mPrevViewProjNoJitter{};
    DirectX::XMFLOAT3   mPrevCameraPosition{};
    DirectX::XMFLOAT3   mPrevCameraForward{};
    bool mHasPreviousCamera = false;
    bool mHistoryValid = false;
    UINT mFrameIndex = 0;

    UINT mOutputWidth = 0;
    UINT mOutputHeight = 0;
    UINT mWorkWidth = 0;
    UINT mWorkHeight = 0;

    bool mPipelinesCreated = false;
    bool mDescriptorsAllocated = false;
    bool mInitFailed = false;
    std::string mLastError;
};
