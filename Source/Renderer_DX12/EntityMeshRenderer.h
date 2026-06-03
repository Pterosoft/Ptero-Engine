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
#include <memory>
#include <string>
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
        DXGI_FORMAT albedoFormat,
        DXGI_FORMAT normalFormat,
        DXGI_FORMAT materialFormat,
        DXGI_FORMAT depthFormat);

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

    // Returns GPU mesh info for the entity at the given index.
    // Returns false if the entity does not have an uploaded mesh yet.
    bool GetGpuMeshInfo(std::size_t entityIndex, GpuMeshInfo& outInfo) const;

    bool ConsumeSceneContentChangedFlag()
    {
        const bool changed = mSceneContentChanged;
        mSceneContentChanged = false;
        return changed;
    }

    bool IsSceneContentDirty() const { return mSceneContentChanged; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // GPU-side buffers for one entity's mesh.
    struct EntityGpuMesh
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexUpload;   // kept alive until upload fence
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexUpload;
        D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
        D3D12_INDEX_BUFFER_VIEW  IndexBufferView{};
        UINT                     IndexCount = 0;
        // The MeshAsset pointer used when these buffers were built; used to
        // detect when the mesh has been replaced and buffers must be rebuilt.
        const Mesh*              SourceMesh = nullptr;
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
        float  _Pad1[2]         = {};
        std::byte Padding[160]{};
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
        // Scalar factors from JSON (default to 1 so they are safe even if not present).
        float baseColorTintR  = 1.f;
        float baseColorTintG  = 1.f;
        float baseColorTintB  = 1.f;
        float baseColorTintA  = 1.f;
        float metallicFactor  = 0.f;  // default: non-metallic (slider sits at 0)
        float roughnessFactor = 1.f;
        float normalScale     = 1.f;
        float aoStrength      = 1.f;
    };

    bool CreatePipeline(DXGI_FORMAT albedoFormat, DXGI_FORMAT normalFormat,
                        DXGI_FORMAT materialFormat, DXGI_FORMAT depthFormat);
    bool EnsureEntityGpuMesh(
        ID3D12GraphicsCommandList* commandList,
        std::size_t entityIndex,
        const Mesh* mesh);
    bool EnsureConstantBuffer(std::size_t requiredEntityCount);
    bool EnsureDepthPassConstantBuffer(std::size_t requiredEntityCount);
    bool EnsureMaterialConstantBuffer(std::size_t requiredDrawCount);
    bool EnsurePointShadowFaceConstantBuffer();
    bool EnsureRainSurfaceConstantBuffer();

    // Resolve all texture paths for every sub-material in a JSON file.
    std::unordered_map<uint32_t, SubMaterialTextures>
        ResolveAllSubMaterialTextures(const std::string& materialPath) const;

    // key = entity index
    std::unordered_map<std::size_t, EntityGpuMesh> mGpuMeshes;
    std::unordered_set<std::size_t> mLoggedDrawEntities;

    Microsoft::WRL::ComPtr<ID3D12Resource>      mConstantBuffer;
    EntityConstants*                             mMappedCB      = nullptr;
    std::size_t                                  mCBCapacity    = 0; // slots allocated

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
    bool        mSceneContentChanged = false;
    std::string mLastError;

public:
    void SetRainSurfaceState(bool enabled, float wetnessIntensity);
};

