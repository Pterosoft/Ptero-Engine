#pragma once

// VirtualShadowMapRenderer
// ------------------------
// Virtual shadow maps for the sun and for shadow-casting local lights, all paged into
// one physical depth texture (the pool):
//
//   Sun          a clipmap of up to 16 levels, each a 16384^2 virtual depth map
//                centred on the camera at twice the texel size of the one before.
//   Local lights point, spot and rect lights each get a slot (up to 16): a virtual
//                cube of six 4096^2 faces with six mips.
//
// Only the 128^2 pages that pixels on screen actually sample are backed by pool tiles,
// and a rendered page stays cached until something moves through it, the light moves
// or turns, or it goes unused long enough to be evicted.
// Data/Shaders/VirtualShadowMap.hlsli describes the addressing.
//
// A frame, driven by DX12SceneRenderer:
//
//   BeginFrame     read back the page requests an earlier frame's marking pass wrote,
//                  slide each sun level's window with the camera, give each shadowed
//                  light a slot, invalidate pages under anything that moved, allocate
//                  pool tiles for newly requested pages and pick this frame's pages to
//                  render (budgeted, coarse first). Produces GetPageViews(), which
//                  virtualized geometry culls for.
//   RenderPages    upload the page table, clear the chosen tiles and draw every caster
//                  into each: entity meshes (culled per page on the CPU), vegetation,
//                  and virtualized geometry (culled per page on the GPU, one cull view
//                  per page, at the page's own resolution).
//   MarkRequests   after the G-Buffer: every pixel sets the request bit of the pages
//                  the lighting pass will read for it; the bits are copied to a
//                  readback ring and consumed by BeginFrame kFramesInFlight frames later.
//
// Requests arrive a few frames late, so a page the camera just uncovered is missing
// for a frame or two. The lighting then reads the next coarser resident level or mip,
// and every pixel also requests the page two levels up for exactly that purpose. So that
// there is always something coarser to fall back to - even for a view the camera has
// only just turned to - a 3 x 3 block of pages around the camera stays resident on every
// coarse sun level, and each light keeps its coarsest mips on all six faces (which is
// also what the fog falls back to in the air, where no surface asks for anything).
//
// Page management is on the CPU because the casters are drawn with the ordinary
// hardware depth pipelines - alpha-tested vegetation, mesh-shader clusters - into a
// viewport per page, and the CPU has to know the pages to set those viewports.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "ShadowPageView.h"
#include "VirtualShadowMapConstants.h"
#include "VirtualShadowMapSettings.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class EntityMeshRenderer;
class VegetationRenderer;
class VirtualGeometryRenderer;
class Mesh;
struct Entity;

class VirtualShadowMapRenderer
{
public:
    // Sun clipmap.
    static constexpr int      kMaxLevels     = 16;
    static constexpr int      kLevelPages    = 128;   // per axis
    static constexpr int      kPagesPerLevel = kLevelPages * kLevelPages;
    static constexpr UINT     kPageTexels    = 128;
    static constexpr int      kDirectionalPages = kMaxLevels * kPagesPerLevel;

    // Local lights. Must match VirtualShadowMap.hlsli.
    static constexpr int      kMaxLocalLights   = 16;
    static constexpr int      kLocalResolution  = 4096;
    static constexpr int      kLocalMips        = 6;
    static constexpr int      kLocalFacePages   = 1365;   // 32^2 + 16^2 + 8^2 + 4^2 + 2^2 + 1
    static constexpr int      kLocalLightPages  = 6 * kLocalFacePages;
    static constexpr float    kLocalNear        = 0.05f;

    static constexpr int      kTotalPages = kDirectionalPages + kMaxLocalLights * kLocalLightPages;

    // Virtualized geometry packs the view index into 8 bits and keeps a few views for
    // the camera and point lights, which caps the pages one frame can render.
    static constexpr int      kMaxPagesPerFrameLimit = 240;

    bool Initialize(const VirtualShadowMapSettings& settings);
    void Shutdown();
    bool IsInitialized() const { return mInitialized; }
    const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

    // A shadow-casting point, spot or rect light.
    struct LocalLight
    {
        // Identifies the light from frame to frame; a light that keeps its key keeps
        // its slot and its cached pages.
        std::uint64_t     Key = 0;
        DirectX::XMFLOAT3 Position{};
        float             Radius = 1.0f;
        // Where it emits: the axis, and the cosine off it below which it emits
        // nothing (-2 for all around), so pages it could not light are never asked for.
        DirectX::XMFLOAT3 Direction{ 0.0f, 0.0f, -1.0f };
        float             EmitCosine = -2.0f;
    };

    struct FrameInputs
    {
        std::vector<Entity>* Entities = nullptr;
        // Which entities virtualized geometry draws in shadows (bit 1), by index; an
        // entity changing hands changes its caster geometry.
        const std::vector<std::uint8_t>* VirtualizedMask = nullptr;
        DirectX::XMFLOAT3 CameraPosition{};
        float FovYRadians = 1.0f;
        UINT  ViewportHeight = 1;

        // The sun, when there is one to shadow.
        bool SunEnabled = false;
        DirectX::XMFLOAT3 SunDirection{};   // from the sun toward the scene

        // Shadowed local lights, in the order GetLocalSlots() answers for. Only the
        // first kMaxLocalLights get a slot.
        std::vector<LocalLight> LocalLights;

        // Changes whenever vegetation that casts shadows changed shape or material.
        std::uint64_t VegetationSignature = 0;
    };

    // Whether the map is in use this frame. When false, nothing else records and the
    // passes should use the ordinary shadow maps.
    bool BeginFrame(const FrameInputs& inputs, const VirtualShadowMapSettings& settings);
    bool IsActiveThisFrame() const { return mActiveThisFrame; }
    // Whether local lights are shadowed through the map this frame.
    bool AreLocalLightsActive() const { return mActiveThisFrame && mGpuConstants.LocalEnabled != 0; }

    // Per FrameInputs::LocalLights entry, its slot in the map (what the lighting reads
    // as the light's shadow index), or -1.
    const std::vector<int>& GetLocalSlots() const { return mLocalSlotOfInput; }

    // Instead of BeginFrame on a frame the map is not used. Casters are not tracked
    // while it is off, so the cache is dropped when it comes back.
    void SkipFrame()
    {
        mActiveThisFrame = false;
        mPageViews.clear();
        mLocalSlotOfInput.clear();
        mGpuConstants.Active = 0;
        mGpuConstants.Enabled = 0;
        mGpuConstants.LocalEnabled = 0;
        mInvalidateAllRequested = true;
    }

    // The pages BeginFrame chose to render this frame.
    const std::vector<ShadowPageView>& GetPageViews() const { return mPageViews; }

    void RenderPages(
        ID3D12GraphicsCommandList* commandList,
        EntityMeshRenderer& entities,
        VegetationRenderer& vegetation,
        VirtualGeometryRenderer& virtualGeometry);

    // After the G-Buffer pass. depthSrv and normalSrv must be readable from the pixel
    // shader. invViewProjection is Transpose(inverse(viewProj)), as the lighting uses it.
    void MarkRequests(
        ID3D12GraphicsCommandList*  commandList,
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
        D3D12_GPU_DESCRIPTOR_HANDLE normalSrv,
        const DirectX::XMFLOAT4X4&  invViewProjection,
        const DirectX::XMFLOAT3&    cameraPosition,
        UINT width,
        UINT height);

    // For the passes that sample the map.
    const VsmGpuConstants& GetGpuConstants() const { return mGpuConstants; }
    D3D12_GPU_VIRTUAL_ADDRESS GetPageTableAddress() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetPoolSrv() const { return mPoolSrvGpu; }

    // Drops every cached page (a level load, a global change the renderer cannot see).
    void InvalidateAll() { mInvalidateAllRequested = true; }

    struct Statistics
    {
        std::uint32_t ResidentPages = 0;
        std::uint32_t PoolPages = 0;
        std::uint32_t RequestedPages = 0;      // in the latest readback
        std::uint32_t RenderedThisFrame = 0;
        std::uint32_t WaitingPages = 0;        // dirty, requested, over this frame's budget
        std::uint32_t PoolOverflow = 0;        // requests that found no tile
        std::uint32_t CasterDraws = 0;         // entity draws this frame
        std::uint32_t LocalLights = 0;         // lights holding a slot
    };
    const Statistics& GetStatistics() const { return mStatistics; }

private:
    struct PageEntry
    {
        int  GlobalX = 0;             // sun: page coordinates; local: page within its mip
        int  GlobalY = 0;
        int  Tile = -1;               // -1 = no pool tile
        bool Rendered = false;        // the tile holds depth for this page
        bool Dirty = false;           // needs (re)rendering
        std::uint64_t LastRequested = 0;
    };

    struct LevelWindow
    {
        int  OriginX = 0;
        int  OriginY = 0;
        bool Valid = false;
    };

    struct LocalSlot
    {
        bool              Used = false;
        bool              Seen = false;
        std::uint64_t     Key = 0;
        DirectX::XMFLOAT3 Position{};
        float             Radius = 1.0f;
        DirectX::XMFLOAT3 Direction{ 0.0f, 0.0f, -1.0f };
        float             EmitCosine = -2.0f;
    };

    struct LocalPageAddress
    {
        int Slot = 0, Face = 0, Mip = 0, X = 0, Y = 0;
    };

    struct TrackedCaster
    {
        std::uint64_t Hash = 0;
        bool Valid = false;
        DirectX::XMFLOAT3 Min{};
        DirectX::XMFLOAT3 Max{};
    };

    struct ReadbackSlot
    {
        bool Pending = false;
        bool SunMarked = false;
        int  LevelCount = 0;
        int  OriginX[kMaxLevels]{};
        int  OriginY[kMaxLevels]{};
        // The light each local slot held when the marking pass ran.
        bool          LocalUsed[kMaxLocalLights]{};
        std::uint64_t LocalKeys[kMaxLocalLights]{};
    };

    struct MeshBounds
    {
        std::weak_ptr<Mesh> Owner;
        DirectX::XMFLOAT3 Min{};
        DirectX::XMFLOAT3 Max{};
    };

    // Per-frame caster record for page culling.
    struct Caster
    {
        std::uint32_t EntityIndex = 0;
        float MinX = 0.0f, MinY = 0.0f, MaxX = 0.0f, MaxY = 0.0f;   // sun light space
        DirectX::XMFLOAT3 Min{};                                   // world
        DirectX::XMFLOAT3 Max{};
    };

    bool CreatePool(int physicalPages);
    bool CreateBuffers();
    bool CreatePipelines(float slopeScaledDepthBias);

    void ResetAllPages();
    void ResetSunPages();
    void ResetLocalSlot(int slot);
    void DirtyLocalSlot(int slot);
    void DirtySunPages();
    void FreePage(PageEntry& page);
    void UpdateLightFrame(const FrameInputs& inputs, const VirtualShadowMapSettings& settings);
    void UpdateWindows(const FrameInputs& inputs);
    void UpdateLocalSlots(const FrameInputs& inputs, const VirtualShadowMapSettings& settings);
    void TrackCasters(const FrameInputs& inputs);
    void InvalidateBox(const DirectX::XMFLOAT3& boxMin, const DirectX::XMFLOAT3& boxMax);
    void ConsumeRequests(std::size_t slot);
    void RequestPage(int pageIndex, int globalX, int globalY);
    void RequestResidentFallbacks(const FrameInputs& inputs);
    void AllocateRequested();
    void ChoosePagesToRender(const VirtualShadowMapSettings& settings);
    void BuildPageViews();
    void BuildCasterDraws();
    void UpdateGpuConstants(const FrameInputs& inputs, const VirtualShadowMapSettings& settings);

    bool GetMeshBounds(const std::shared_ptr<Mesh>& mesh, DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax);
    DirectX::XMFLOAT3 ToLightSpace(const DirectX::XMFLOAT3& world) const;
    float TexelSize(int level) const;
    int  PageIndex(int level, int globalX, int globalY) const
    {
        return level * kPagesPerLevel + (globalY & (kLevelPages - 1)) * kLevelPages + (globalX & (kLevelPages - 1));
    }
    static int LocalPageIndex(int slot, int face, int mip, int x, int y);
    static LocalPageAddress DecodeLocalPage(int pageIndex);
    static bool IsLocalPage(int pageIndex) { return pageIndex >= kDirectionalPages; }
    // Higher = rendered sooner: the coarse levels and mips everything finer falls back on.
    static int CoarseRank(int pageIndex);
    void WritePageTableEntry(int pageIndex);

    bool mInitialized = false;
    bool mActiveThisFrame = false;
    bool mSunActive = false;
    std::string mLastError;

    // --- GPU ------------------------------------------------------------------
    static constexpr std::size_t kFramesInFlight = 3;

    Microsoft::WRL::ComPtr<ID3D12Resource>       mPool;
    D3D12_RESOURCE_STATES                        mPoolState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE                  mPoolDsv{};
    D3D12_CPU_DESCRIPTOR_HANDLE                  mPoolSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE                  mPoolSrvGpu{};
    int  mPoolPagesX = 0;
    int  mPoolPagesY = 0;
    int  mPoolPageCount = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mPageTable;          // DEFAULT, StructuredBuffer<uint>
    Microsoft::WRL::ComPtr<ID3D12Resource> mPageTableUpload;    // kFramesInFlight copies
    std::byte* mPageTableUploadMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mRequests;           // DEFAULT, UAV bitmask
    Microsoft::WRL::ComPtr<ID3D12Resource> mRequestsZero;       // UPLOAD, never written after creation
    Microsoft::WRL::ComPtr<ID3D12Resource> mRequestsReadback;   // kFramesInFlight copies
    const std::uint32_t* mRequestsReadbackMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mMarkConstants;      // kFramesInFlight slots
    std::byte* mMarkConstantsMapped = nullptr;

    DX12Shader mMarkVertexShader;
    DX12Shader mMarkPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mMarkRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mMarkPipeline;

    DX12Shader mDepthVertexShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mDepthRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mDepthPipeline;
    float mPipelineSlopeBias = -1.0f;

    // --- Page state --------------------------------------------------------------
    std::vector<PageEntry>     mPages;          // kTotalPages
    std::vector<std::uint32_t> mPageTableCpu;   // what the GPU table should hold
    bool                       mPageTableDirty = true;
    std::vector<int>           mFreeTiles;
    std::vector<int>           mTileOwner;      // tile -> page index, or -1
    std::vector<int>           mAllocationRequests;
    std::vector<int>           mPagesToRender;
    LevelWindow                mWindows[kMaxLevels];

    // Sun light frame.
    bool              mLightValid = false;
    DirectX::XMFLOAT3 mLightDirection{};
    DirectX::XMFLOAT3 mAnchor{};
    DirectX::XMFLOAT3 mAxisX{}, mAxisY{}, mAxisZ{};
    float             mDepthNear = 0.0f;
    float             mDepthFar = 1.0f;

    // Local lights.
    LocalSlot         mLocalSlots[kMaxLocalLights];
    std::vector<int>  mLocalSlotOfInput;

    // Settings the cached pages were rendered with.
    float mBuiltFirstTexel = 0.0f;
    int   mBuiltLevelCount = 0;
    float mBuiltDepthRange = 0.0f;
    int   mLevelCount = 0;
    float mFirstTexel = 1.0f;

    std::uint64_t mFrame = 0;
    std::size_t   mFrameSlot = 0;
    ReadbackSlot  mReadbackSlots[kFramesInFlight];
    bool          mInvalidateAllRequested = true;

    std::vector<TrackedCaster> mTrackedCasters;
    std::uint64_t              mVegetationSignature = 0;
    std::vector<std::uint32_t> mCastersNotReady;   // skipped by the last page render
    std::unordered_map<const Mesh*, MeshBounds> mMeshBounds;

    // This frame's output.
    std::vector<Caster>         mCasters;
    std::vector<ShadowPageView> mPageViews;
    std::vector<std::uint32_t>  mEntityDrawEntities;   // flattened, per page
    std::vector<std::uint32_t>  mEntityDrawOffsets;    // mPageViews.size() + 1
    VsmGpuConstants             mGpuConstants{};
    Statistics                  mStatistics{};
};
