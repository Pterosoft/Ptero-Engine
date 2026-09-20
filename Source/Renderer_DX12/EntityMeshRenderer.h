#pragma once

// EntityMeshRenderer
// Responsible for uploading and drawing every entity that has a MeshComponent
// with an assigned mesh.  In the deferred rendering pipeline this class acts
// as the G-Buffer geometry pass: it writes albedo, world-space normal, and
// material data into three MRT outputs (GBuffer.hlsl) without performing any
// lighting calculation.  Lighting is resolved separately by DeferredLightingPass.

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "TextureManager.h"
#include "Components.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cmath>
#include <memory>
#include <chrono>
#include <filesystem>
#include <string>
#include <map>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
}

class EntityMeshRenderer
{
public:
    // Call once after the DX12 device is ready.  commandList is used to record
    // the initial pipeline creation (no geometry upload happens here yet).
    bool Initialize(ID3D12GraphicsCommandList* commandList);

    // Called every frame before Render().  Gives the renderer the current entity
    // list so it can upload new meshes and retire stale GPU buffers.
    void SetEntities(std::vector<Entity>* entities);

    // Record draw calls for every entity that has a fully loaded mesh into the
    // G-Buffer (albedo, normal, material MRT).  The G-Buffer RTs must already be
    // bound as render targets before calling this (see DeferredLightingPass::BeginGeometryPass).
    // GBuffer.hlsl is used; no lighting is computed here.
    void Render(
        ID3D12GraphicsCommandList* commandList,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        DXGI_FORMAT albedoFormat,
        DXGI_FORMAT normalFormat,
        DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat,
        UINT msaaSampleCount = 1);

    // Render a depth-only pass using an external root signature and pipeline.
    // rootSignature/pipelineState must have a single root CBV at slot 0 (b0, VS)
    // for the per-entity MVP.  This reuses the GPU vertex/index buffers that
    // were already uploaded for the main colour pass.
    // lightViewProjection must be a column-major (pre-transposed) matrix.
    void RenderDepthOnly(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature*       rootSignature,
        ID3D12PipelineState*       pipelineState,
        const DirectX::XMFLOAT4X4& lightViewProjection);

    void RenderPointLightShadowDepth(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature*       rootSignature,
        ID3D12PipelineState*       pipelineState,
        const DirectX::XMFLOAT4X4& lightViewProjection,
        const DirectX::XMFLOAT3&   lightPosition,
        float                      farPlane);

    void Shutdown();

    // Describes the GPU-side vertex/index buffers for one entity mesh,
    // used by the DXR GI pass to build Bottom-Level Acceleration Structures (BLAS).
    struct GpuMeshInfo
    {
        ID3D12Resource* VertexBuffer = nullptr;
        ID3D12Resource* IndexBuffer  = nullptr;
        UINT            VertexCount  = 0;
        UINT            IndexCount   = 0;
        UINT            VertexStride = 0; // bytes per vertex
        // The source Mesh pointer, used as a BLAS cache key.
        const void*     MeshKey      = nullptr;
    };

    // GPU buffers for one LOD of one mesh asset, or false if that LOD has not
    // been uploaded yet. Keyed by the asset because the buffers are shared by
    // every entity placed from it.
    bool GetGpuMeshInfo(const Mesh* mesh, std::size_t lodIndex, GpuMeshInfo& outInfo) const;

    // Advance the constant-buffer frame slot. Call exactly once per frame,
    // before any of this renderer's passes record, so the shadow, depth and
    // G-Buffer passes all write into the same frame's copy.
    void BeginFrame()
    {
        ++mFrameCounter;
        mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;
        RetireExpiredBuffers();
        EvictUnusedMeshes();
    }

    bool ConsumeSceneContentChangedFlag()
    {
        const bool changed = mSceneContentChanged;
        mSceneContentChanged = false;
        return changed;
    }

    bool IsSceneContentDirty() const { return mSceneContentChanged; }

    void SetWireframeEnabled(bool enabled)
    {
        if (mWireframeEnabled != enabled)
        {
            mWireframeEnabled = enabled;
            mPipelineReady = false;
        }
    }

    bool IsWireframeEnabled() const
    {
        return mWireframeEnabled;
    }

    // Direct handles for the cvar registry, which writes the flag itself and then
    // asks for the pipeline rebuild that SetWireframeEnabled would have done.
    bool& GetWireframeEnabledRef() { return mWireframeEnabled; }
    void InvalidatePipeline() { mPipelineReady = false; }

    void SetTextureMipLODBias(float bias)
    {
        if ((std::fabs)(mTextureMipLODBias - bias) > 0.001f)
        {
            mTextureMipLODBias = bias;
            mPipelineReady = false;
        }
    }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

    // Writes the GPU address range of every mesh buffer currently in the cache.
    //
    // Called from the device-removed path. A page fault reports an address and
    // DRED will not say whose it is, so the only way to attribute it is to print
    // what lives where at the moment it happens. An address that falls inside a
    // range names the buffer; one that falls just past the end of a range is a
    // draw reading off the end of it, which is the more interesting answer.
    void LogLiveGpuBufferRanges() const;

private:
    // GPU-side buffers for one entity's mesh.
    struct EntityGpuMesh
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        // The staging buffers are not here on purpose. They used to be held for
        // the life of the entry, which doubled the memory every mesh cost - an
        // upload-heap copy of geometry nothing reads again after the first
        // frame. They go to the retire list instead, which frees them once the
        // copy they feed has certainly completed.
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        UINT                     IndexCount = 0;
        std::size_t              LodIndex = 0;
        // The MeshAsset pointer used when these buffers were built; used to
        // detect when the mesh has been replaced and buffers must be rebuilt.
        const Mesh*              SourceMesh = nullptr;
        // A strong reference to that same asset, held for exactly one reason:
        // the cache key is its address, and an address is only unique while the
        // object is alive. Pressing Play frees a whole level of meshes and
        // immediately allocates another level's worth, so the allocator hands a
        // new Mesh the address a cached entry is still filed under. The lookup
        // then hits, the draw binds the old mesh's index buffer, and the submesh
        // ranges - which come from the new asset, not from the cache - run off
        // the end of it into whatever follows. That is a GPU page fault inside
        // DrawIndexedInstanced, seconds after a level change, with nothing in
        // between to connect it to. Owning the asset makes the key unique for as
        // long as the entry exists, which is what the keying already assumed.
        std::shared_ptr<const Mesh> SourceMeshOwner;
        // Frame this entry was last drawn from, for evicting geometry a level
        // reload left behind.
        std::uint64_t            LastUsedFrame = 0;
    };

    // Constant buffer layout – must be 256-byte aligned.
    struct alignas(256) EntityConstants
    {
        DirectX::XMFLOAT4X4 MVP;
        DirectX::XMFLOAT4X4 Model;   // model-to-world matrix for shadow world-position
        std::byte            Padding[128]{};
    };
    static_assert(sizeof(EntityConstants) == 256);

    // Per-frame scene lighting constant buffer layout – must match SceneLighting in MeshEntity.hlsl.
    // Supports up to kMaxPointLights point lights packed alongside the directional sun/sky terms.
    struct alignas(256) LightingConstants
    {
        DirectX::XMFLOAT3 SunDirection; float Pad0 = 0;
        DirectX::XMFLOAT3 SunColor;     float Pad1 = 0;
        DirectX::XMFLOAT3 SkyAmbient;   float Pad2 = 0;
        int               NumPointLights = 0;
        float             PadLights[3]{};
        std::byte         Padding[64]{};
    };
    static_assert(sizeof(LightingConstants) == 256);

    // Shadow constant buffer layout – must match ShadowData in MeshEntity.hlsl.
    struct alignas(256) ShadowConstants
    {
        DirectX::XMFLOAT4X4 LightViewProj;  // column-major (pre-transposed)
        float               ShadowMapSize;  // texel resolution for PCF offset
        float               ShadowBias;     // depth comparison bias
        float               Pad[2];
        std::byte           Padding[128]{};
    };
    static_assert(sizeof(ShadowConstants) == 256);

    struct alignas(256) PointShadowFaceConstants
    {
        DirectX::XMFLOAT3 LightPosition;
        float             FarPlane = 1.0f;
        std::byte         Padding[240]{};
    };
    static_assert(sizeof(PointShadowFaceConstants) == 256);

    // Per-draw material constant buffer – must be 256-byte aligned.
    // Mirrors MaterialConstants in GBuffer.hlsl.
    struct alignas(256) MaterialConstants
    {
        DirectX::XMFLOAT4 BaseColorTint   = { 1.f, 1.f, 1.f, 1.f };
        DirectX::XMFLOAT3 EmissiveColor   = { 0.f, 0.f, 0.f };  float _Pad0 = 0.f;
        float  MetallicFactor   = 1.f;
        float  RoughnessFactor  = 1.f;
        float  NormalScale      = 1.f;
        float  AoStrength       = 1.f;
        int    HasNormalMap     = 0;
        int    HasMetallicMap   = 0;
        int    HasRoughnessMap  = 0;
        int    HasAoMap         = 0;
        int    HasEmissiveMap   = 0;
        int    HasPackedMaterialMap = 0;
        // Reflectivity; 0.5 is neutral. Occupies the first half of what used to be two
        // words of padding, so the row layout the shader expects is unchanged.
        float  SpecularFactor   = 0.5f;
        float  _Pad1            = 0.f;
        float  OpacityFactor    = 1.f;
        float  AlphaCutoff      = 0.5f;
        int    HasOpacityMap    = 0;
        int    UseAlphaCutout   = 0;
        int    UseTransparentBlend = 0;
        float  _Pad2[3]         = {};
        // UV transform: rotate about (0.5, 0.5), then scale by tiling, then offset.
        // The rotation is pre-resolved to sin/cos so the shader does no trigonometry.
        DirectX::XMFLOAT2 UvTiling = { 1.f, 1.f };
        DirectX::XMFLOAT2 UvOffset = { 0.f, 0.f };
        float  UvRotationSin    = 0.f;
        float  UvRotationCos    = 1.f;
        int    HasHeightMap     = 0;
        int    UseParallaxOcclusion = 0;
        float  ParallaxHeightScale  = 0.05f;
        int    ParallaxMinSteps     = 8;
        int    ParallaxMaxSteps     = 32;
        float  ParallaxFadeDistance = 30.f;
        // Needed to build the tangent-space view ray the parallax march walks along.
        DirectX::XMFLOAT3 CameraPositionWS = { 0.f, 0.f, 0.f };
        // Height value that sits at the polygon surface; 1 = white is the top of the
        // volume, which is what a plain 0-1 height map wants.
        float  ParallaxReferenceHeight = 1.f;
        std::byte Padding[80]{};
    };
    static_assert(sizeof(MaterialConstants) == 256);

    struct alignas(256) RainSurfaceConstants
    {
        float RainWetnessIntensity = 0.0f;
        float RainEnabled = 0.0f;
        float _Pad0 = 0.0f;
        float _Pad1 = 0.0f;
        std::byte Padding[240]{};
    };
    static_assert(sizeof(RainSurfaceConstants) == 256);

    // All texture paths for one sub-material, resolved to absolute paths.
    struct SubMaterialTextures
    {
        std::string baseColor;
        std::string normal;
        std::string packedMaterial;
        std::string metallic;
        std::string roughness;
        std::string ao;
        std::string emissive;
        std::string opacity;
        std::string height;
        // Scalar factors from JSON (default to 1 so they are safe even if not present).
        float baseColorTintR  = 1.f;
        float baseColorTintG  = 1.f;
        float baseColorTintB  = 1.f;
        float baseColorTintA  = 1.f;
        float metallicFactor  = 0.f;  // default: non-metallic (slider sits at 0)
        float roughnessFactor = 1.f;
        float specularFactor  = 0.5f; // neutral reflectivity (0.04 dielectric F0)
        float normalScale     = 1.f;
        float aoStrength      = 1.f;
        float opacityFactor   = 1.f;
        float alphaCutoff     = 0.5f;
        bool  useAlphaCutout  = false;
        bool  useTransparentBlend = false;
        // UV transform and parallax settings, straight from the material JSON.
        float uvTilingU       = 1.f;
        float uvTilingV       = 1.f;
        float uvOffsetU       = 0.f;
        float uvOffsetV       = 0.f;
        float uvRotationDegrees = 0.f;
        bool  useParallaxOcclusion = false;
        float parallaxHeightScale  = 0.05f;
        int   parallaxMinSteps     = 8;
        int   parallaxMaxSteps     = 32;
        float parallaxFadeDistance = 30.f;
        float parallaxReferenceHeight = 1.f;
    };

    bool CreatePipeline(DXGI_FORMAT albedoFormat, DXGI_FORMAT normalFormat,
                        DXGI_FORMAT materialFormat, DXGI_FORMAT depthFormat,
                        UINT msaaSampleCount);
    // Takes the asset by shared_ptr rather than raw pointer so the cache entry can
    // keep it alive; see EntityGpuMesh::SourceMeshOwner.
    bool EnsureEntityGpuMesh(
        ID3D12GraphicsCommandList* commandList,
        std::size_t entityIndex,
        const std::shared_ptr<Mesh>& mesh,
        std::size_t lodIndex);
    bool EnsureConstantBuffer(std::size_t requiredEntityCount);
    bool EnsureDepthPassConstantBuffer(std::size_t requiredEntityCount);
    bool EnsureMaterialConstantBuffer(std::size_t requiredDrawCount);
    bool EnsurePointShadowFaceConstantBuffer();
    bool EnsureRainSurfaceConstantBuffer();

    // Resolve all texture paths for every sub-material in a JSON file.
    //
    // Returns a reference into the cache below rather than a fresh map: this is
    // called once per entity per frame from the geometry pass, and the
    // underlying work - opening the JSON, parsing it, and probing the
    // filesystem for every texture path - is far too expensive to repeat at
    // frame rate. See mMaterialTextureCache.
    const std::unordered_map<uint32_t, SubMaterialTextures>&
        ResolveAllSubMaterialTextures(const std::string& materialPath) const;

    // The uncached parse behind it. Only reached on a cache miss or when the
    // material file has changed on disk.
    std::unordered_map<uint32_t, SubMaterialTextures>
        ParseSubMaterialTextures(const std::string& materialPath) const;

    // Parsed material, cached by the path the entity refers to.
    //
    // Materials are re-read when the file changes on disk so the editor keeps
    // live material editing, but the staleness check itself costs a filesystem
    // call, so it is throttled: an entry is only re-checked once the interval
    // below has elapsed. A quarter second is imperceptible when saving from the
    // material editor and reduces the geometry pass's filesystem traffic from
    // hundreds of calls per frame to effectively none.
    struct CachedMaterialTextures
    {
        std::unordered_map<uint32_t, SubMaterialTextures> Textures;
        std::filesystem::file_time_type LastWriteTime{};
        std::chrono::steady_clock::time_point LastCheckTime{};
        bool FileExists = false;
    };
    static constexpr std::chrono::milliseconds kMaterialRevalidateInterval{ 250 };
    mutable std::unordered_map<std::string, CachedMaterialTextures> mMaterialTextureCache;

    // Keyed by the mesh asset itself, never by entity index. Twenty entities
    // placed from one asset then share one set of GPU buffers instead of each
    // uploading a private copy of the same geometry - and, just as importantly,
    // deleting an entity cannot invalidate anyone else's entry. While this was
    // keyed by index, a delete shifted every later entity down one, so every
    // cached entry above the deletion suddenly named the wrong mesh and was
    // erased mid-frame while in-flight frames were still drawing from it.
    using MeshCacheKey = std::pair<const Mesh*, std::size_t>;   // asset, LOD
    std::map<MeshCacheKey, EntityGpuMesh> mGpuMeshes;
    std::unordered_set<std::size_t> mLoggedDrawEntities;

    // Per-frame constant storage.
    //
    // Every one of these buffers is an UPLOAD-heap block the CPU writes each
    // frame and the GPU reads while rasterising. The engine keeps three frames
    // in flight, so a single copy is overwritten while an earlier frame is
    // still drawing from it - and because these carry the MVP and model
    // matrices, a torn read rasterises geometry with a mixture of two frames'
    // transforms. That lands directly in the G-Buffer as wrong depth and wrong
    // normals, which anything reconstructing world position from it then
    // amplifies.
    //
    // It is invisible while the camera is still, because consecutive frames
    // then write identical matrices, and it is invisible whenever the CPU is
    // slow enough that it cannot run ahead of the GPU.
    //
    // Each buffer therefore holds kFramesInFlight copies of its slot range, and
    // the frame slot selects which copy this frame uses. BeginFrame() advances
    // it once per frame so every pass within a frame agrees.
    static constexpr std::size_t kFramesInFlight = 3;
    std::size_t                                  mFrameSlot = 0;

    // Growing a constant buffer used to flush the GPU mid-frame so the old one
    // could be released safely. That stall is what made adding or copying an
    // entity hitch, and a flush that timed out left the renderer convinced the
    // GPU was idle when it was not. Nothing has to wait: hand the replaced
    // buffer to this list instead and let it die once every frame that could
    // still be reading it has retired.
    struct RetiredBuffer
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        int FramesRemaining = 0;
        // Captured at retirement: once the ComPtr is released the resource cannot
        // be asked for its own address, and the address is what a page-fault
        // report gives us to match against.
        D3D12_GPU_VIRTUAL_ADDRESS Address = 0;
        UINT64 SizeBytes = 0;
        const char* What = "";
    };
    std::vector<RetiredBuffer> mRetiredBuffers;

    // Records the GPU address range as well as retiring the resource, and logs the
    // release when the countdown expires.
    //
    // A page fault reports an address. DRED is supposed to name the allocation it
    // fell in, but the name only survives if the driver kept it, and here it does
    // not - every crash so far has said "(unnamed)" for resources that carry debug
    // names. The address is therefore the only thing that identifies the buffer,
    // so both ends are logged and the fault address can simply be matched against
    // the ranges. Retirements are rare - a constant buffer growing, a mesh evicted
    // - so this costs nothing per frame.
    void RetireBuffer(Microsoft::WRL::ComPtr<ID3D12Resource> resource, const char* what);

    void RetireExpiredBuffers();

    // Geometry a level reload or a deleted entity left behind would otherwise
    // sit in VRAM for the rest of the session. Entries go through the retire
    // list rather than being destroyed here, because an in-flight frame may
    // still be drawing from them.
    static constexpr std::uint64_t kMeshEvictionFrames = 600;
    void EvictUnusedMeshes()
    {
        if (mFrameCounter % 120 != 0) return;
        for (auto it = mGpuMeshes.begin(); it != mGpuMeshes.end(); )
        {
            if (mFrameCounter - it->second.LastUsedFrame > kMeshEvictionFrames)
            {
                RetireBuffer(std::move(it->second.VertexBuffer), "evicted mesh vertex buffer");
                RetireBuffer(std::move(it->second.IndexBuffer), "evicted mesh index buffer");
                it = mGpuMeshes.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::uint64_t mFrameCounter = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource>      mConstantBuffer;
    EntityConstants*                             mMappedCB      = nullptr;
    std::size_t                                  mCBCapacity    = 0; // slots per frame

    // Per-draw material constants (one slot per submesh draw call per entity).
    Microsoft::WRL::ComPtr<ID3D12Resource>      mMaterialCB;
    MaterialConstants*                           mMappedMatCB      = nullptr;
    std::size_t                                  mMatCBCapacity    = 0;

    // Separate per-entity constant buffer for the depth-only shadow pass.
    // The shadow and main passes are recorded into the same command list, so they
    // cannot safely share one upload buffer without later CPU writes overwriting
    // constants that earlier draw calls still reference.
    Microsoft::WRL::ComPtr<ID3D12Resource>      mDepthPassConstantBuffer;
    EntityConstants*                             mMappedDepthPassCB = nullptr;
    std::size_t                                  mDepthPassCBCapacity = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource>      mPointShadowFaceConstantBuffer;
    PointShadowFaceConstants*                    mMappedPointShadowFaceCB = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource>      mRainSurfaceConstantBuffer;
    RainSurfaceConstants*                        mMappedRainSurfaceCB = nullptr;

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    std::vector<Entity>* mEntities = nullptr;

    // Texture loading and per-material-path SRV cache.
    TextureManager mTextureManager;

    // 1×1 white fallback texture used when an entity has no material or the texture is missing.
    Microsoft::WRL::ComPtr<ID3D12Resource> mFallbackTextureResource;
    D3D12_CPU_DESCRIPTOR_HANDLE            mFallbackCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE            mFallbackGpuHandle{};

    bool CreateFallbackTexture(ID3D12GraphicsCommandList* commandList);
    // Distance between LOD levels, and the fraction of it that must be crossed
    // before a switch is accepted. The margin is what stops an entity parked on
    // a boundary from flipping LOD every frame the camera moves.
    static constexpr float kLodDistanceStep = 25.0f;
    static constexpr float kLodHysteresis   = 0.15f;

    std::size_t SelectLodIndex(
        std::size_t entityIndex,
        const Entity& entity,
        const Mesh& mesh,
        const DirectX::XMFLOAT3& cameraPosition) const;

    // LOD chosen for each entity last frame, so selection can hysteresis rather
    // than oscillate. Mutable because selection is logically a const query.
    mutable std::unordered_map<std::size_t, std::size_t> mEntityLodState;

    // The LOD the camera pass last chose for this entity, or 0 if it has not been
    // drawn yet.
    //
    // The shadow and depth passes used to ask for LOD 0 unconditionally, so every
    // shadow-casting light rendered every entity at full detail, six times - once
    // per cube face - with no LOD, no frustum culling and no radius culling. A
    // single 600k-triangle prop therefore cost 3.6M triangles per light per frame
    // in the shadow pass alone, however far away or however small it was on screen.
    // Adding one such prop to a level was enough to push a frame past the two
    // second GPU watchdog and have Windows reset the driver, which surfaces as
    // DXGI_ERROR_DEVICE_HUNG in the middle of a long run of DrawIndexedInstanced.
    //
    // Rendering a caster at the detail the camera is already using for it is the
    // normal thing to do, and it is never more detail than the lit result can show.
    // Lagging a frame behind the camera pass does not matter: an LOD switch that is
    // invisible in the lit image is invisible in its shadow.
    std::size_t LastSelectedLod(std::size_t entityIndex) const
    {
        const auto it = mEntityLodState.find(entityIndex);
        return it != mEntityLodState.end() ? it->second : 0;
    }

    // Resolve a single-material file path to its base-color DDS path.
    // Returns an empty string if the material cannot be read or has no base-color texture.
    std::string ResolveBaseColorDdsPath(const std::string& materialRelativePath) const;

    // For a multi-material JSON path, returns a map of materialId -> absolute DDS path.
    // For a single-material JSON path, returns { {0, ddsPath} }.
    // Returns an empty map on failure.
    std::unordered_map<uint32_t, std::string> ResolveSubMaterialDdsPaths(const std::string& materialPath) const;

    // Upload buffer for the fallback texture — kept alive until the GPU finishes
    // consuming it (released after the first successful Render call).
    Microsoft::WRL::ComPtr<ID3D12Resource> mFallbackUploadBuffer;

    bool        mPipelineReady  = false;
    DXGI_FORMAT mAlbedoFormat   = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mNormalFormat   = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mMaterialFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT mDepthFormat    = DXGI_FORMAT_UNKNOWN;
    UINT        mMsaaSampleCount = 1;
    bool        mSceneContentChanged = false;
    bool        mWireframeEnabled = false;
    float       mTextureMipLODBias = -1.5f;
    std::string mLastError;

public:
    void SetRainSurfaceState(bool enabled, float wetnessIntensity);
};
