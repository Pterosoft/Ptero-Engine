#include "pch.h"
#include "EntityMeshRenderer.h"
#include "SubsurfaceScattering.h"

#include "System/DataFiles.h"
#include "System/Udim.h"

#include "System/PteroLog.h"

#include "d3dx12.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

// nlohmann/json for reading material .json files at runtime.
#include "..\SDKs\nlohmann\json.hpp"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace
{
    // Vertex layout that matches System/Mesh.h :: Vertex
    struct GpuVertex
    {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT3 Normal;
        DirectX::XMFLOAT2 TexCoord;
        DirectX::XMFLOAT4 Color;
    };
    static_assert(sizeof(GpuVertex) == sizeof(Vertex),
        "GpuVertex must match the Vertex layout in Mesh.h exactly.");

    void LogEntityMeshRendererDiagnostic(const std::string& message)
    {
        PteroLog::Write(PteroLog::Level::Debug, "Meshes", message.c_str());
    }

    bool CreateCommittedBuffer(
        ID3D12Device* device,
        UINT64 size,
        D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_STATES initialState,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outResource,
        // What DRED prints when a page fault lands in or near this allocation.
        // "(unnamed)" in a crash log identifies nothing, which is the difference
        // between reading the cause off the log and guessing at it.
        const wchar_t* debugName = nullptr)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type                 = heapType;
        heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask     = 1;
        heapProps.VisibleNodeMask      = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width              = size;
        desc.Height             = 1;
        desc.DepthOrArraySize   = 1;
        desc.MipLevels          = 1;
        desc.Format             = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count   = 1;
        desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&outResource))))
        {
            return false;
        }

        if (debugName != nullptr)
            outResource->SetName(debugName);
        return true;
    }

    std::filesystem::path FindProjectDataDirectory()
    {
        const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
        if (dataDirectory.empty() || DataFiles::IsPackaged())
            return dataDirectory;
        std::error_code errorCode;
        return std::filesystem::weakly_canonical(dataDirectory, errorCode);
    }

    // Canonical on disk; a packaged (virtual) path has nothing on disk to canonicalize.
    std::filesystem::path CanonicalDataPath(const std::filesystem::path& path)
    {
        if (DataFiles::IsPackaged())
            return path.lexically_normal();
        std::error_code errorCode;
        return std::filesystem::weakly_canonical(path, errorCode);
    }

    std::filesystem::path ResolveMaterialFilePath(const std::string& materialPath)
    {
        if (materialPath.empty())
            return {};

        std::filesystem::path path(materialPath);
        if (path.is_absolute() && DataFiles::IsFile(path))
            return CanonicalDataPath(path);

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (!dataDirectory.empty())
        {
            path = (dataDirectory / path).lexically_normal();
            if (DataFiles::IsFile(path))
                return CanonicalDataPath(path);
        }

        return {};
    }

}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool EntityMeshRenderer::Initialize(ID3D12GraphicsCommandList* commandList)
{
    mLastError.clear();

    // Create the 1×1 white fallback texture so slot 1 is always bindable.
    if (!CreateFallbackTexture(commandList))
    {
        // Non-fatal: we'll just skip texture binding if this failed.
        mFallbackGpuHandle = {};
    }

    // Pipeline is created lazily on the first Render() call once we know the
    // render-target formats.
    return true;
}

void EntityMeshRenderer::SetEntities(std::vector<Entity>* entities)
{
    if (mEntities != entities)
    {
        mSceneContentChanged = true;
        mLoggedDrawEntities.clear();
        // Entity indices are about to mean something different, so last frame's
        // LOD choices no longer apply to them.
        mEntityLodState.clear();
    }
    mEntities = entities;
}

void EntityMeshRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const XMMATRIX& viewProjection,
    const XMFLOAT3& cameraPosition,
    DXGI_FORMAT albedoFormat,
    DXGI_FORMAT normalFormat,
    DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat,
    UINT msaaSampleCount)
{
    if (commandList == nullptr || mEntities == nullptr || mEntities->empty())
    {
        return;
    }

    // (Re-)create the G-Buffer geometry pipeline if any format changed.
    if (!mPipelineReady
        || albedoFormat   != mAlbedoFormat
        || normalFormat   != mNormalFormat
        || materialFormat != mMaterialFormat
        || depthFormat    != mDepthFormat
        || msaaSampleCount != mMsaaSampleCount)
    {
        mAlbedoFormat   = albedoFormat;
        mNormalFormat   = normalFormat;
        mMaterialFormat = materialFormat;
        mDepthFormat    = depthFormat;
        mMsaaSampleCount = msaaSampleCount;
        if (!CreatePipeline(albedoFormat, normalFormat, materialFormat, depthFormat, msaaSampleCount))
            return;
    }

    // Count mesh entities for CB sizing.
    std::size_t meshEntityCount = 0;
    for (const Entity& entity : *mEntities)
    {
        if (entity.HasMeshComponent() && entity.Mesh.has_value() && entity.Mesh->MeshAsset)
            ++meshEntityCount;
    }

    if (meshEntityCount == 0)
        return;

    if (!EnsureConstantBuffer(meshEntityCount))
        return;

    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return;

    // Count total draw calls (one per submesh per entity, or one per UDIM tile of a submesh)
    // for material CB sizing. Takes the largest LOD so whichever LOD is drawn fits.
    std::size_t totalDraws = 0;
    for (const Entity& e : *mEntities)
    {
        if (!e.HasMeshComponent() || !e.Mesh.has_value() || !e.Mesh->MeshAsset) continue;
        std::size_t entityDraws = 1;
        for (const MeshLod& lod : e.Mesh->MeshAsset->GetLods())
        {
            std::size_t lodDraws = 0;
            for (const SubMesh& sub : lod.SubMeshes)
                lodDraws += (std::max)(std::size_t(1), sub.udimTiles.size());
            entityDraws = (std::max)(entityDraws, lodDraws);
        }
        totalDraws += entityDraws;
    }
    // Virtualized geometry may already have taken slots this frame.
    if (!EnsureMaterialConstantBuffer(mMaterialSlotCursor + totalDraws))
        return;
    if (!EnsureRainSurfaceConstantBuffer())
        return;

    ID3D12DescriptorHeap* sharedSrvHeap = DX12Context_GetSrvDescriptorHeap();
    if (sharedSrvHeap == nullptr)
        return;

    commandList->SetDescriptorHeaps(1, &sharedSrvHeap);
    commandList->SetGraphicsRootConstantBufferView(
        2, mRainSurfaceConstantBuffer->GetGPUVirtualAddress());

    // Which of the two G-Buffer pipelines is bound; switched per draw by bindTextures.
    bool tessellatedPipelineBound = false;

    std::size_t cbSlot  = 0;
    std::size_t matSlot = mMaterialSlotCursor;
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        Entity& entity = (*mEntities)[i];
        if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset)
            continue;
        if (IsDrawnVirtualized(i, kVirtualizedInGBuffer))
            continue;

        const Mesh* meshPtr = entity.Mesh->MeshAsset.get();
        const std::size_t selectedLodIndex = SelectLodIndex(i, entity, *meshPtr, cameraPosition);

        if (!EnsureEntityGpuMesh(commandList, i, entity.Mesh->MeshAsset, selectedLodIndex))
        {
            ++cbSlot;
            continue;
        }

        EntityGpuMesh& selectedGpuMesh = mGpuMeshes.at(MeshCacheKey{ meshPtr, selectedLodIndex });
        selectedGpuMesh.LastUsedFrame = mFrameCounter;
        if (selectedGpuMesh.IndexCount == 0)
        {
            ++cbSlot;
            continue;
        }

        const MeshLod& selectedLod = meshPtr->GetLod(selectedLodIndex);

        // Write per-entity MVP + model matrix into the constant buffer.
        const XMMATRIX model = entity.Transform.GetTransform();
        const XMMATRIX mvp   = XMMatrixTranspose(model * viewProjection);
        // Index into this frame's copy of the slot range, so the write cannot
        // land on constants an in-flight earlier frame is still drawing with.
        const std::size_t cbIndex = mFrameSlot * mCBCapacity + cbSlot;
        XMStoreFloat4x4(&mMappedCB[cbIndex].MVP,   mvp);
        XMStoreFloat4x4(&mMappedCB[cbIndex].Model, XMMatrixTranspose(model));

        const UINT64 cbOffset = static_cast<UINT64>(cbIndex) * sizeof(EntityConstants);
        // Slot 0: per-entity transform CBV (b0).
        commandList->SetGraphicsRootConstantBufferView(
            0, mConstantBuffer->GetGPUVirtualAddress() + cbOffset);

        commandList->IASetVertexBuffers(0, 1, &selectedGpuMesh.VertexBufferView);
        commandList->IASetIndexBuffer(&selectedGpuMesh.IndexBufferView);

        const std::string& materialPath = entity.Mesh->MaterialPath;
        const std::vector<SubMesh>& subMeshes = selectedLod.SubMeshes;

        if (mLoggedDrawEntities.emplace(i).second)
        {
            std::ostringstream logStream;
            logStream << "First draw for entityIndex=" << i
                      << ", entity='" << entity.Name << "'"
                      << ", lodIndex=" << selectedLodIndex
                      << ", vertexCount=" << selectedLod.Vertices.size()
                      << ", indexCount=" << selectedGpuMesh.IndexCount
                      << ", materialPath='" << materialPath << "'"
                      << ", subMeshCount=" << subMeshes.size();

            if (subMeshes.empty())
            {
                logStream << ", draw[0]={indexStart=0,indexCount=" << selectedGpuMesh.IndexCount << "}";
            }
            else
            {
                std::uint64_t summedSubMeshIndices = 0;
                bool validSubMeshRanges = true;
                const std::size_t loggedSubMeshCount = (std::min)(subMeshes.size(), static_cast<std::size_t>(8));
                for (std::size_t subMeshIndex = 0; subMeshIndex < subMeshes.size(); ++subMeshIndex)
                {
                    const SubMesh& subMesh = subMeshes[subMeshIndex];
                    summedSubMeshIndices += subMesh.indexCount;
                    if (static_cast<std::uint64_t>(subMesh.indexStart) + static_cast<std::uint64_t>(subMesh.indexCount)
                        > static_cast<std::uint64_t>(selectedGpuMesh.IndexCount))
                    {
                        validSubMeshRanges = false;
                    }

                    if (subMeshIndex < loggedSubMeshCount)
                    {
                        logStream << ", draw[" << subMeshIndex << "]={materialId=" << subMesh.materialId
                                  << ",indexStart=" << subMesh.indexStart
                                  << ",indexCount=" << subMesh.indexCount << "}";
                    }
                }

                if (subMeshes.size() > loggedSubMeshCount)
                {
                    logStream << ", additionalSubMeshes=" << (subMeshes.size() - loggedSubMeshCount);
                }

                logStream << ", summedSubMeshIndices=" << summedSubMeshIndices
                          << ", validSubMeshRanges=" << (validSubMeshRanges ? "true" : "false");
            }

            LogEntityMeshRendererDiagnostic(logStream.str());
        }

        // Load all texture paths for every sub-material in this entity's JSON.
        const auto& allTextures = ResolveAllSubMaterialTextures(materialPath);

        // Binds a sub-material (through the bind cache), switching between the
        // plain and tessellated pipelines as it asks.
        auto bindMaterial = [&](std::uint32_t materialId, std::uint32_t udimTile, MaterialConstants& matOut)
        {
            const bool tessellate = BindMaterialCached(commandList, materialPath, materialId, udimTile, matOut, cameraPosition);
            if (tessellate != tessellatedPipelineBound)
            {
                tessellatedPipelineBound = tessellate;
                commandList->SetPipelineState(tessellate ? mTessPipelineState.Get() : mPipelineState.Get());
                commandList->IASetPrimitiveTopology(tessellate
                    ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                    : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
        };

        if (!subMeshes.empty())
        {
            // Multi-material: draw each sub-mesh with its own textures and material params.
            for (const SubMesh& subMesh : subMeshes)
            {
                const std::size_t matIndex = mFrameSlot * mMatCBCapacity + matSlot;
                MaterialConstants& mat = mMappedMatCB[matIndex];
                mat = MaterialConstants{}; // reset to defaults

                const SubMaterialTextures* picked = PickSubMaterial(allTextures, subMesh.materialId);
                if (picked != nullptr && picked->hasUdim && !subMesh.udimTiles.empty())
                {
                    // UDIM set: one draw per tile, each with that tile's textures. The sampler
                    // wraps, so the tile's UVs (u in [n, n+1)) read its own 0-1 texture unchanged.
                    for (const SubMeshUdimTile& tileRange : subMesh.udimTiles)
                    {
                        const std::size_t tileMatIndex = mFrameSlot * mMatCBCapacity + matSlot;
                        MaterialConstants& tileMat = mMappedMatCB[tileMatIndex];
                        tileMat = MaterialConstants{};

                        bindMaterial(subMesh.materialId, tileRange.tile, tileMat);
                        commandList->SetGraphicsRootConstantBufferView(
                            1, mMaterialCB->GetGPUVirtualAddress() + tileMatIndex * sizeof(MaterialConstants));
                        commandList->DrawIndexedInstanced(tileRange.indexCount, 1, tileRange.indexStart, 0, 0);
                        ++matSlot;
                    }
                    continue;
                }

                bindMaterial(subMesh.materialId, 0u, mat);

                // Slot 1: per-draw material CBV (b1).
                commandList->SetGraphicsRootConstantBufferView(
                    1, mMaterialCB->GetGPUVirtualAddress() + matIndex * sizeof(MaterialConstants));
                commandList->DrawIndexedInstanced(subMesh.indexCount, 1, subMesh.indexStart, 0, 0);
                ++matSlot;
            }
        }
        else
        {
            // Single-material or no submesh info.
            const std::size_t matIndex = mFrameSlot * mMatCBCapacity + matSlot;
            MaterialConstants& mat = mMappedMatCB[matIndex];
            mat = MaterialConstants{};

            bindMaterial(0u, 0u, mat);

            commandList->SetGraphicsRootConstantBufferView(
                1, mMaterialCB->GetGPUVirtualAddress() + matIndex * sizeof(MaterialConstants));
            commandList->DrawIndexedInstanced(selectedGpuMesh.IndexCount, 1, 0, 0, 0);
            ++matSlot;
        }

        ++cbSlot;
    }

    mMaterialSlotCursor = matSlot;
}

const EntityMeshRenderer::SubMaterialTextures* EntityMeshRenderer::PickSubMaterial(
    const std::unordered_map<uint32_t, SubMaterialTextures>& allTextures, uint32_t materialId)
{
    // A present multi-material slot is authoritative even when it
    // intentionally has no textures. Falling back in that case leaks another
    // slot's opacity map across the mesh.
    const auto it = allTextures.find(materialId);
    if (it != allTextures.end())
        return &it->second;

    for (const auto& [resolvedMaterialId, textures] : allTextures)
    {
        if (!textures.baseColor.empty() || !textures.normal.empty() || !textures.metallic.empty()
            || !textures.roughness.empty() || !textures.ao.empty() || !textures.emissive.empty()
            || !textures.opacity.empty() || !textures.height.empty())
        {
            return &textures;
        }
    }
    return nullptr;
}

bool EntityMeshRenderer::CanDrawVirtualized(const std::string& materialPath) const
{
    for (const auto& [materialId, textures] : ResolveAllSubMaterialTextures(materialPath))
    {
        if (textures.useTessellation && !textures.height.empty())
            return false;
    }
    return true;
}

void EntityMeshRenderer::BindVirtualGeometryMaterial(
    ID3D12GraphicsCommandList* commandList,
    const std::string& materialPath,
    std::uint32_t materialId,
    std::uint32_t udimTile,
    const XMFLOAT3& cameraPosition)
{
    if (commandList == nullptr || !EnsureMaterialConstantBuffer(mMaterialSlotCursor + 1))
        return;

    const std::size_t matIndex = mFrameSlot * mMatCBCapacity + mMaterialSlotCursor;
    MaterialConstants& mat = mMappedMatCB[matIndex];
    mat = MaterialConstants{};

    // The virtualized pipeline has no tessellation stages; CanDrawVirtualized
    // keeps materials that need them away from it, so the request is moot.
    BindMaterialCached(commandList, materialPath, materialId, udimTile, mat, cameraPosition);
    commandList->SetGraphicsRootConstantBufferView(
        1, mMaterialCB->GetGPUVirtualAddress() + matIndex * sizeof(MaterialConstants));
    ++mMaterialSlotCursor;
}

D3D12_GPU_VIRTUAL_ADDRESS EntityMeshRenderer::GetRainSurfaceConstantsAddress()
{
    return EnsureRainSurfaceConstantBuffer() ? mRainSurfaceConstantBuffer->GetGPUVirtualAddress() : 0;
}

// Fills a sub-material's constants and binds its eight texture slots to root
// tables 3-10 (t0-t7), straight from the texture cache's pre-allocated
// descriptors, so nothing is copied. Shared by the ordinary G-Buffer draws
// and the virtualized ones, which use the same root parameter layout.
bool EntityMeshRenderer::BindSubMaterial(
    ID3D12GraphicsCommandList* commandList,
    const SubMaterialTextures& texPaths,
    MaterialConstants& matOut,
    const XMFLOAT3& cameraPosition)
{
    PreparedMaterial prepared;
    PrepareSubMaterial(texPaths, prepared);
    return ApplyPreparedMaterial(commandList, prepared, matOut, cameraPosition);
}

bool EntityMeshRenderer::BindMaterialCached(
    ID3D12GraphicsCommandList* commandList,
    const std::string& materialPath,
    std::uint32_t materialId,
    std::uint32_t udimTile,
    MaterialConstants& matOut,
    const XMFLOAT3& cameraPosition)
{
    const auto now = std::chrono::steady_clock::now();
    const BindCacheKeyView key{ materialPath, materialId, udimTile };
    auto it = mBindCache.find(key);
    // Rebuilt on the same interval the material and texture caches revalidate on,
    // so live material edits and texture hot reloads still come through.
    if (it == mBindCache.end() || now - it->second.BuiltAt >= kMaterialRevalidateInterval)
    {
        SubMaterialTextures texPaths;
        if (const SubMaterialTextures* picked = PickSubMaterial(ResolveAllSubMaterialTextures(materialPath), materialId))
            texPaths = *picked;

        // One UDIM tile's textures; tile 0 leaves the paths as they are.
        if (texPaths.hasUdim && udimTile != 0)
        {
            for (std::string* path : { &texPaths.baseColor, &texPaths.normal, &texPaths.packedMaterial,
                                       &texPaths.metallic, &texPaths.roughness, &texPaths.ao,
                                       &texPaths.emissive, &texPaths.opacity, &texPaths.height })
            {
                *path = Udim::Resolve(*path, udimTile);
            }
        }

        PreparedMaterial prepared;
        PrepareSubMaterial(texPaths, prepared);
        // Fallbacks bound only because textures are still streaming in must not
        // stick for the interval; the entry is rebuilt every frame until they land.
        prepared.BuiltAt = prepared.Incomplete ? std::chrono::steady_clock::time_point{} : now;
        if (it == mBindCache.end())
            it = mBindCache.emplace(BindCacheKey{ materialPath, materialId, udimTile }, std::move(prepared)).first;
        else
            it->second = std::move(prepared);
    }
    it->second.LastUsedFrame = mFrameCounter;
    return ApplyPreparedMaterial(commandList, it->second, matOut, cameraPosition);
}

void EntityMeshRenderer::PrepareSubMaterial(const SubMaterialTextures& texPaths, PreparedMaterial& out)
{
    MaterialConstants& matOut = out.Constants;
    matOut = MaterialConstants{};

    // Copy scalar parameters from the resolved material textures into the CB.
    matOut.BaseColorTint   = { texPaths.baseColorTintR, texPaths.baseColorTintG,
                               texPaths.baseColorTintB, texPaths.baseColorTintA };
    matOut.MetallicFactor  = texPaths.metallicFactor;
    matOut.RoughnessFactor = texPaths.roughnessFactor;
    matOut.SpecularFactor  = texPaths.specularFactor;
    matOut.NormalScale     = texPaths.normalScale;
    matOut.FlipNormalGreen = texPaths.flipNormalGreen ? 1 : 0;
    matOut.AoStrength      = texPaths.aoStrength;
    matOut.OpacityFactor   = texPaths.opacityFactor;
    matOut.AlphaCutoff     = texPaths.alphaCutoff;
    matOut.UseAlphaCutout  = texPaths.useAlphaCutout ? 1 : 0;
    matOut.UseTransparentBlend = texPaths.useTransparentBlend ? 1 : 0;

    matOut.UvTiling = { texPaths.uvTilingU, texPaths.uvTilingV };
    matOut.UvOffset = { texPaths.uvOffsetU, texPaths.uvOffsetV };
    const float uvRotationRadians = DirectX::XMConvertToRadians(texPaths.uvRotationDegrees);
    matOut.UvRotationSin = std::sin(uvRotationRadians);
    matOut.UvRotationCos = std::cos(uvRotationRadians);

    matOut.UseParallaxOcclusion = texPaths.useParallaxOcclusion ? 1 : 0;
    matOut.ParallaxHeightScale  = texPaths.parallaxHeightScale;
    matOut.ParallaxMinSteps     = (std::max)(1, texPaths.parallaxMinSteps);
    matOut.ParallaxMaxSteps     = (std::max)(matOut.ParallaxMinSteps, texPaths.parallaxMaxSteps);
    matOut.ParallaxFadeDistance = texPaths.parallaxFadeDistance;
    matOut.ParallaxReferenceHeight = texPaths.parallaxReferenceHeight;

    matOut.UseTessellation      = texPaths.useTessellation ? 1 : 0;
    matOut.TessMaxFactor        = texPaths.tessMaxFactor;
    matOut.TessTargetPixels     = texPaths.tessTargetPixels;
    matOut.TessFadeDistance     = texPaths.tessFadeDistance;
    matOut.DisplacementScale    = texPaths.displacementScale;
    matOut.DisplacementMidLevel = texPaths.displacementMidLevel;

    // Only opaque surfaces can be scattered: a blended one's diffuse is already
    // mixed with what is behind it by the time the subsurface pass could swap it.
    // The slot itself is acquired per draw (ApplyPreparedMaterial): slots are
    // handed out frame by frame.
    out.UseSubsurface = texPaths.useSubsurfaceScattering && !texPaths.useTransparentBlend;
    if (out.UseSubsurface)
    {
        out.SubsurfaceProfile.Color   = { texPaths.subsurfaceColorR, texPaths.subsurfaceColorG, texPaths.subsurfaceColorB };
        out.SubsurfaceProfile.Falloff = { texPaths.subsurfaceFalloffR, texPaths.subsurfaceFalloffG, texPaths.subsurfaceFalloffB };
        out.SubsurfaceProfile.RadiusMeters = (std::max)(texPaths.subsurfaceRadiusMm, 0.01f) * 0.001f;
        out.SubsurfaceProfile.Translucency = texPaths.subsurfaceTranslucency;
    }

    std::array<std::string, kTextureSlotCount> paths;
    if (ResolveTextureSlots(texPaths, paths))
        matOut.HasPackedMaterialMap = texPaths.packedLayout;

    int* flags[kTextureSlotCount] = {
        nullptr,               &matOut.HasNormalMap,    &matOut.HasMetallicMap,
        &matOut.HasRoughnessMap, &matOut.HasAoMap,       &matOut.HasEmissiveMap,
        &matOut.HasOpacityMap,   &matOut.HasHeightMap
    };

    out.Incomplete = false;
    for (std::size_t s = 0; s < kTextureSlotCount; ++s)
    {
        out.Handles[s] = mFallbackGpuHandle;
        out.Textures[s].reset();
        if (paths[s].empty())
            continue;
        // While a level streams in, the preloader reads textures a time-boxed slice
        // per frame; a draw reading every missing one itself would put the whole
        // level's textures back into a single frame.
        if (mDeferTextureLoads && !mTextureManager.IsCached(paths[s]))
        {
            out.Incomplete = true;
            continue;
        }
        if (auto gpuTex = mTextureManager.LoadDDS(paths[s], kTextureSlotSemantics[s]))
        {
            out.Handles[s] = gpuTex->GpuHandle;
            // Held so the descriptor cannot outlive its texture while cached.
            out.Textures[s] = std::move(gpuTex);
            if (flags[s]) *flags[s] = 1;
        }
    }

    // Tessellation needs something to displace by; without a height map the
    // plain pipeline draws the same surface for less.
    out.Tessellate = matOut.UseTessellation != 0
        && matOut.HasHeightMap != 0
        && mTessPipelineState != nullptr;
}

bool EntityMeshRenderer::ApplyPreparedMaterial(
    ID3D12GraphicsCommandList* commandList,
    const PreparedMaterial& prepared,
    MaterialConstants& matOut,
    const XMFLOAT3& cameraPosition)
{
    matOut = prepared.Constants;
    matOut.CameraPositionWS = cameraPosition;
    matOut.TessPixelScale   = mTessPixelScale;
    matOut.SubsurfaceSlot   = prepared.UseSubsurface ? SubsurfaceProfiles::Acquire(prepared.SubsurfaceProfile) : 0;

    for (std::size_t s = 0; s < kTextureSlotCount; ++s)
    {
        // Root slots 3–10 correspond to t0–t7.
        if (prepared.Handles[s].ptr != 0)
            commandList->SetGraphicsRootDescriptorTable(static_cast<UINT>(3 + s), prepared.Handles[s]);
    }
    return prepared.Tessellate;
}

bool EntityMeshRenderer::ResolveTextureSlots(
    const SubMaterialTextures& texPaths, std::array<std::string, kTextureSlotCount>& outPaths)
{
    // A packed map stands in for all three masks, but only when none is set on its own.
    const bool usePacked = !texPaths.packedMaterial.empty()
        && texPaths.metallic.empty()
        && texPaths.roughness.empty()
        && texPaths.ao.empty();
    outPaths = {
        texPaths.baseColor,
        texPaths.normal,
        usePacked ? texPaths.packedMaterial : texPaths.metallic,
        usePacked ? texPaths.packedMaterial : texPaths.roughness,
        usePacked ? texPaths.packedMaterial : texPaths.ao,
        texPaths.emissive,
        texPaths.opacity,
        texPaths.height,
    };
    return usePacked;
}

void EntityMeshRenderer::CollectTextureRequests(
    const std::vector<Entity>& entities, std::vector<TextureRequest>& outRequests) const
{
    outRequests.clear();
    std::unordered_set<std::string> seen;
    std::array<std::string, kTextureSlotCount> paths;

    const auto addSubMaterial = [&](const SubMaterialTextures& texPaths)
    {
        ResolveTextureSlots(texPaths, paths);
        for (std::size_t s = 0; s < kTextureSlotCount; ++s)
        {
            if (!paths[s].empty() && !mTextureManager.IsCached(paths[s]) && seen.insert(paths[s]).second)
                outRequests.push_back({ paths[s], kTextureSlotSemantics[s] });
        }
    };

    for (const Entity& entity : entities)
    {
        if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset)
            continue;

        const auto& allTextures = ResolveAllSubMaterialTextures(entity.Mesh->MaterialPath);
        if (allTextures.empty())
            continue;

        // The sub-materials the draws will actually pick, and for a UDIM set every
        // tile any LOD's geometry uses - the tiles, not the token, are what get loaded.
        std::map<std::uint32_t, std::set<std::uint32_t>> tilesByMaterial;
        const auto& lods = entity.Mesh->MeshAsset->GetLods();
        bool anySubMesh = false;
        for (const MeshLod& lod : lods)
        {
            for (const SubMesh& subMesh : lod.SubMeshes)
            {
                anySubMesh = true;
                std::set<std::uint32_t>& tiles = tilesByMaterial[subMesh.materialId];
                for (const SubMeshUdimTile& tileRange : subMesh.udimTiles)
                    tiles.insert(tileRange.tile);
            }
        }
        if (!anySubMesh)
            tilesByMaterial[0u];

        for (const auto& [materialId, tiles] : tilesByMaterial)
        {
            const SubMaterialTextures* picked = PickSubMaterial(allTextures, materialId);
            if (picked == nullptr)
                continue;
            if (!picked->hasUdim || tiles.empty())
            {
                addSubMaterial(*picked);
                continue;
            }
            for (std::uint32_t tile : tiles)
            {
                SubMaterialTextures tilePaths = *picked;
                for (std::string* path : { &tilePaths.baseColor, &tilePaths.normal, &tilePaths.packedMaterial,
                                           &tilePaths.metallic, &tilePaths.roughness, &tilePaths.ao,
                                           &tilePaths.emissive, &tilePaths.opacity, &tilePaths.height })
                {
                    *path = Udim::Resolve(*path, tile);
                }
                addSubMaterial(tilePaths);
            }
        }
    }
}

bool EntityMeshRenderer::GetGpuMeshInfo(const Mesh* mesh, std::size_t lodIndex, GpuMeshInfo& outInfo) const
{
    const auto it = mGpuMeshes.find(MeshCacheKey{ mesh, lodIndex });
    if (it == mGpuMeshes.end() || it->second.IndexCount == 0)
        return false;

    const EntityGpuMesh& gpuMesh = it->second;
    outInfo.VertexBuffer = gpuMesh.VertexBuffer.Get();
    outInfo.IndexBuffer  = gpuMesh.IndexBuffer.Get();
    outInfo.VertexCount  = static_cast<UINT>(gpuMesh.VertexBufferView.SizeInBytes / gpuMesh.VertexBufferView.StrideInBytes);
    outInfo.IndexCount   = gpuMesh.IndexCount;
    outInfo.VertexStride = gpuMesh.VertexBufferView.StrideInBytes;
    outInfo.MeshKey      = gpuMesh.SourceMesh;
    return true;
}

void EntityMeshRenderer::Shutdown()
{
    // Holds texture references; must go before the texture manager's cache does.
    mBindCache.clear();
    if (mConstantBuffer && mMappedCB != nullptr)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedCB = nullptr;
    }
    mConstantBuffer.Reset();
    mCBCapacity = 0;

    if (mMaterialCB && mMappedMatCB != nullptr)
    {
        mMaterialCB->Unmap(0, nullptr);
        mMappedMatCB = nullptr;
    }
    mMaterialCB.Reset();
    mMatCBCapacity = 0;

    if (mDepthPassConstantBuffer && mMappedDepthPassCB != nullptr)
    {
        mDepthPassConstantBuffer->Unmap(0, nullptr);
        mMappedDepthPassCB = nullptr;
    }
    mDepthPassConstantBuffer.Reset();
    mDepthPassCBCapacity = 0;

    if (mPointShadowFaceConstantBuffer && mMappedPointShadowFaceCB != nullptr)
    {
        mPointShadowFaceConstantBuffer->Unmap(0, nullptr);
        mMappedPointShadowFaceCB = nullptr;
    }
    mPointShadowFaceConstantBuffer.Reset();

    if (mRainSurfaceConstantBuffer && mMappedRainSurfaceCB != nullptr)
    {
        mRainSurfaceConstantBuffer->Unmap(0, nullptr);
        mMappedRainSurfaceCB = nullptr;
    }
    mRainSurfaceConstantBuffer.Reset();

    mGpuMeshes.clear();
    mRootSignature.Reset();
    mPipelineState.Reset();
    mVertexShader = DX12Shader{};
    mPixelShader  = DX12Shader{};
    mPipelineReady = false;

    // Release texture resources.
    mTextureManager.Shutdown();
    mFallbackUploadBuffer.Reset();
    mFallbackTextureResource.Reset();
    mFallbackCpuHandle = {};
    mFallbackGpuHandle = {};
}

void EntityMeshRenderer::LogLiveGpuBufferRanges() const
{
    PTERO_LOG_ERROR("Meshes", "Live mesh GPU buffers at the time of the fault (%llu entries):",
        static_cast<unsigned long long>(mGpuMeshes.size()));

    for (const auto& [key, gpuMesh] : mGpuMeshes)
    {
        if (!gpuMesh.VertexBuffer || !gpuMesh.IndexBuffer)
            continue;

        const D3D12_GPU_VIRTUAL_ADDRESS vbStart = gpuMesh.VertexBufferView.BufferLocation;
        const D3D12_GPU_VIRTUAL_ADDRESS ibStart = gpuMesh.IndexBufferView.BufferLocation;
        PTERO_LOG_ERROR("Meshes",
            "  mesh=%p lod=%llu indices=%u  VB 0x%llx..0x%llx (%u B)  IB 0x%llx..0x%llx (%u B)",
            static_cast<const void*>(key.first),
            static_cast<unsigned long long>(key.second),
            gpuMesh.IndexCount,
            static_cast<unsigned long long>(vbStart),
            static_cast<unsigned long long>(vbStart + gpuMesh.VertexBufferView.SizeInBytes),
            gpuMesh.VertexBufferView.SizeInBytes,
            static_cast<unsigned long long>(ibStart),
            static_cast<unsigned long long>(ibStart + gpuMesh.IndexBufferView.SizeInBytes),
            gpuMesh.IndexBufferView.SizeInBytes);
    }

    // The constant buffers are bound by every one of those draws, so their ranges
    // belong in the same picture.
    const auto logBuffer = [](const char* name, const Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
    {
        if (!resource) return;
        const D3D12_GPU_VIRTUAL_ADDRESS start = resource->GetGPUVirtualAddress();
        const UINT64 size = resource->GetDesc().Width;
        PTERO_LOG_ERROR("Meshes", "  %s 0x%llx..0x%llx (%llu B)", name,
            static_cast<unsigned long long>(start),
            static_cast<unsigned long long>(start + size),
            static_cast<unsigned long long>(size));
    };
    logBuffer("entity CB      ", mConstantBuffer);
    logBuffer("material CB    ", mMaterialCB);
    logBuffer("depth-pass CB  ", mDepthPassConstantBuffer);
    logBuffer("point-shadow CB", mPointShadowFaceConstantBuffer);
    logBuffer("rain surface CB", mRainSurfaceConstantBuffer);
}

void EntityMeshRenderer::RetireBuffer(Microsoft::WRL::ComPtr<ID3D12Resource> resource, const char* what)
{
    if (!resource) return;

    RetiredBuffer retired;
    retired.Address = resource->GetGPUVirtualAddress();
    retired.SizeBytes = resource->GetDesc().Width;
    retired.What = (what != nullptr) ? what : "";
    // One extra frame of slack over the number in flight, because the buffer
    // is retired part-way through a frame that has already recorded draws
    // referencing it.
    retired.FramesRemaining = static_cast<int>(kFramesInFlight) + 1;
    retired.Resource = std::move(resource);

    PTERO_LOG_DEBUG("Meshes",
        "Retiring %s at GPU VA 0x%llx..0x%llx (%llu bytes), releasing in %d frames (frame %llu).",
        retired.What,
        static_cast<unsigned long long>(retired.Address),
        static_cast<unsigned long long>(retired.Address + retired.SizeBytes),
        static_cast<unsigned long long>(retired.SizeBytes),
        retired.FramesRemaining,
        static_cast<unsigned long long>(mFrameCounter));

    mRetiredBuffers.push_back(std::move(retired));
}

void EntityMeshRenderer::RetireExpiredBuffers()
{
    for (auto it = mRetiredBuffers.begin(); it != mRetiredBuffers.end(); )
    {
        if (--it->FramesRemaining <= 0)
        {
            // Logged at the moment the memory actually goes back, which is the
            // event a later page fault in this range would be pointing at.
            PTERO_LOG_DEBUG("Meshes",
                "Releasing %s at GPU VA 0x%llx..0x%llx (frame %llu).",
                it->What,
                static_cast<unsigned long long>(it->Address),
                static_cast<unsigned long long>(it->Address + it->SizeBytes),
                static_cast<unsigned long long>(mFrameCounter));
            it = mRetiredBuffers.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void EntityMeshRenderer::SetRainSurfaceState(bool enabled, float wetnessIntensity)
{
    if (!EnsureRainSurfaceConstantBuffer() || mMappedRainSurfaceCB == nullptr)
    {
        return;
    }

    mMappedRainSurfaceCB->RainEnabled = enabled ? 1.0f : 0.0f;
    mMappedRainSurfaceCB->RainWetnessIntensity = wetnessIntensity;
}

void EntityMeshRenderer::RenderPointLightShadowDepth(
    ID3D12GraphicsCommandList* commandList,
    ID3D12RootSignature*       rootSignature,
    ID3D12PipelineState*       pipelineState,
    const DirectX::XMFLOAT4X4& lightViewProjection,
    const DirectX::XMFLOAT3&   lightPosition,
    float                      farPlane)
{
    if (!commandList || !rootSignature || !pipelineState || !mEntities) return;

    std::size_t meshCount = 0;
    for (const Entity& e : *mEntities)
        if (e.HasMeshComponent() && e.Mesh.has_value() && e.Mesh->MeshAsset) ++meshCount;
    if (meshCount == 0) return;

    if (!EnsureDepthPassConstantBuffer(meshCount) || !EnsurePointShadowFaceConstantBuffer())
        return;

    mMappedPointShadowFaceCB->LightPosition = lightPosition;
    mMappedPointShadowFaceCB->FarPlane = farPlane;

    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const XMMATRIX lightVP = XMMatrixTranspose(XMLoadFloat4x4(&lightViewProjection));

    std::size_t slot = 0;
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& entity = (*mEntities)[i];
        if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset)
            continue;
        if (IsDrawnVirtualized(i, kVirtualizedInShadows))
            continue;

        const Mesh* meshPtr = entity.Mesh->MeshAsset.get();
        // The detail the camera pass is using for this entity, not LOD 0. See
        // LastSelectedLod: asking for the finest LOD here made every shadow face
        // redraw every prop at full resolution.
        const std::size_t casterLod = LastSelectedLod(i);
        if (!EnsureEntityGpuMesh(commandList, i, entity.Mesh->MeshAsset, casterLod))
        {
            ++slot;
            continue;
        }

        auto it = mGpuMeshes.find(MeshCacheKey{ meshPtr, casterLod });
        if (it == mGpuMeshes.end() || it->second.IndexCount == 0)
        {
            ++slot;
            continue;
        }

        EntityGpuMesh& gpuMesh = it->second;
        gpuMesh.LastUsedFrame = mFrameCounter;

        const XMMATRIX model = entity.Transform.GetTransform();
        const XMMATRIX mvp   = XMMatrixTranspose(model * lightVP);
        const std::size_t depthIndex = mFrameSlot * mDepthPassCBCapacity + slot;

        XMStoreFloat4x4(&mMappedDepthPassCB[depthIndex].MVP, mvp);
        XMStoreFloat4x4(&mMappedDepthPassCB[depthIndex].Model, XMMatrixTranspose(model));

        commandList->SetGraphicsRootConstantBufferView(
            0,
            mDepthPassConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(depthIndex) * sizeof(EntityConstants));
        commandList->SetGraphicsRootConstantBufferView(
            1,
            mPointShadowFaceConstantBuffer->GetGPUVirtualAddress());

        commandList->IASetVertexBuffers(0, 1, &gpuMesh.VertexBufferView);
        commandList->IASetIndexBuffer(&gpuMesh.IndexBufferView);
        commandList->DrawIndexedInstanced(gpuMesh.IndexCount, 1, 0, 0, 0);

        ++slot;
    }
}

void EntityMeshRenderer::RenderDepthOnly(
    ID3D12GraphicsCommandList* commandList,
    ID3D12RootSignature*       rootSignature,
    ID3D12PipelineState*       pipelineState,
    const DirectX::XMFLOAT4X4& lightViewProjection)
{
    if (!commandList || !rootSignature || !pipelineState || !mEntities) return;

    std::size_t meshCount = 0;
    for (const Entity& e : *mEntities)
        if (e.HasMeshComponent() && e.Mesh.has_value() && e.Mesh->MeshAsset) ++meshCount;
    if (meshCount == 0) return;

    if (!EnsureDepthPassConstantBuffer(meshCount)) return;

    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const XMMATRIX lightVP = XMMatrixTranspose(XMLoadFloat4x4(&lightViewProjection));

    std::size_t slot = 0;
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& entity = (*mEntities)[i];
        if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset)
            continue;
        if (IsDrawnVirtualized(i, kVirtualizedInShadows))
            continue;

        const Mesh* meshPtr = entity.Mesh->MeshAsset.get();
        // The detail the camera pass is using for this entity, not LOD 0. See
        // LastSelectedLod: asking for the finest LOD here made every shadow face
        // redraw every prop at full resolution.
        const std::size_t casterLod = LastSelectedLod(i);
        if (!EnsureEntityGpuMesh(commandList, i, entity.Mesh->MeshAsset, casterLod))
        {
            ++slot;
            continue;
        }

        auto it = mGpuMeshes.find(MeshCacheKey{ meshPtr, casterLod });
        if (it == mGpuMeshes.end() || it->second.IndexCount == 0)
        {
            ++slot;
            continue;
        }

        EntityGpuMesh& gpuMesh = it->second;
        gpuMesh.LastUsedFrame = mFrameCounter;

        const XMMATRIX model = entity.Transform.GetTransform();
        const XMMATRIX mvp   = XMMatrixTranspose(model * lightVP);
        const std::size_t depthIndex = mFrameSlot * mDepthPassCBCapacity + slot;

        XMStoreFloat4x4(&mMappedDepthPassCB[depthIndex].MVP, mvp);

        commandList->SetGraphicsRootConstantBufferView(
            0,
            mDepthPassConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(depthIndex) * sizeof(EntityConstants));

        commandList->IASetVertexBuffers(0, 1, &gpuMesh.VertexBufferView);
        commandList->IASetIndexBuffer(&gpuMesh.IndexBufferView);
        commandList->DrawIndexedInstanced(gpuMesh.IndexCount, 1, 0, 0, 0);

        ++slot;
    }
}

void EntityMeshRenderer::RenderDepthOnlyPages(
    ID3D12GraphicsCommandList*         commandList,
    ID3D12RootSignature*               rootSignature,
    ID3D12PipelineState*               pipelineState,
    const std::vector<ShadowPageView>& pages,
    const std::vector<std::uint32_t>&  drawOffsets,
    const std::vector<std::uint32_t>&  drawEntities,
    std::vector<std::uint32_t>&        outNotReady,
    std::uint32_t&                     outDrawCount)
{
    outDrawCount = 0;
    if (!commandList || !rootSignature || !pipelineState || !mEntities || drawOffsets.size() != pages.size() + 1)
        return;

    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (std::size_t pageIndex = 0; pageIndex < pages.size(); ++pageIndex)
    {
        const std::uint32_t first = drawOffsets[pageIndex];
        const std::uint32_t last = drawOffsets[pageIndex + 1];
        if (first == last)
            continue;

        const ShadowPageView& page = pages[pageIndex];
        commandList->RSSetViewports(1, &page.Viewport);
        commandList->RSSetScissorRects(1, &page.Scissor);
        const XMMATRIX pageViewProjection = XMMatrixTranspose(XMLoadFloat4x4(&page.ViewProjectionTransposed));

        for (std::uint32_t draw = first; draw < last; ++draw)
        {
            const std::size_t i = drawEntities[draw];
            if (i >= mEntities->size())
                continue;
            const Entity& entity = (*mEntities)[i];
            if (!entity.HasMeshComponent() || !entity.Mesh->MeshAsset || IsDrawnVirtualized(i, kVirtualizedInShadows))
                continue;

            // The detail the camera pass is using, as for the other shadow passes.
            const std::size_t casterLod = LastSelectedLod(i);
            if (!EnsureEntityGpuMesh(commandList, i, entity.Mesh->MeshAsset, casterLod))
            {
                outNotReady.push_back(static_cast<std::uint32_t>(i));
                continue;
            }
            auto it = mGpuMeshes.find(MeshCacheKey{ entity.Mesh->MeshAsset.get(), casterLod });
            if (it == mGpuMeshes.end() || it->second.IndexCount == 0)
            {
                outNotReady.push_back(static_cast<std::uint32_t>(i));
                continue;
            }

            EntityGpuMesh& gpuMesh = it->second;
            gpuMesh.LastUsedFrame = mFrameCounter;

            XMFLOAT4X4 mvp;
            XMStoreFloat4x4(&mvp, XMMatrixTranspose(entity.Transform.GetTransform() * pageViewProjection));
            commandList->SetGraphicsRoot32BitConstants(0, 16, &mvp, 0);
            commandList->IASetVertexBuffers(0, 1, &gpuMesh.VertexBufferView);
            commandList->IASetIndexBuffer(&gpuMesh.IndexBufferView);
            commandList->DrawIndexedInstanced(gpuMesh.IndexCount, 1, 0, 0, 0);
            ++outDrawCount;
        }
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool EntityMeshRenderer::CreatePipeline(
    DXGI_FORMAT albedoFormat,
    DXGI_FORMAT normalFormat,
    DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat,
    UINT msaaSampleCount)
{
    mPipelineReady = false;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "EntityMeshRenderer: DX12 device is null.";
        return false;
    }

    // Compile the G-Buffer geometry shader (outputs albedo/normal/material MRT; no lighting).
    const ShaderCompileRequest vsRequest
    {
        L"Shaders\\GBuffer.hlsl",
        L"VSMain",
        L"vs_5_0",
        ShaderStage::Vertex
    };
    const ShaderCompileRequest psRequest
    {
        L"Shaders\\GBuffer.hlsl",
        L"PSMain",
        L"ps_5_0",
        ShaderStage::Pixel
    };

    if (!mVertexShader.Compile(vsRequest))
    {
        mLastError = std::string("GBuffer VS compile failed: ")
            + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }
    if (!mPixelShader.Compile(psRequest))
    {
        mLastError = std::string("GBuffer PS compile failed: ")
            + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // Root signature layout:
    //   slot 0 – root CBV  (b0, VS) per-entity MVP + model matrix
    //   slot 1 – root CBV  (b1, PS) per-draw material constants
    //   slot 2 – root CBV  (b2, PS) frame rain surface constants
    //   slots 3–10 – individual 1-SRV descriptor tables (PS):
    //              3=t0 baseColor, 4=t1 normal, 5=t2 metallic,
    //              6=t3 roughness, 7=t4 ao, 8=t5 emissive, 9=t6 opacity,
    //              10=t7 height (parallax occlusion)
    // Using individual tables means each texture
    // can be bound directly from its pre-allocated GPU handle without copying.
    D3D12_ROOT_PARAMETER rootParams[11]{};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0; // b0
    rootParams[0].Descriptor.RegisterSpace  = 0;
    // ALL rather than VERTEX/PIXEL: the tessellated pipeline's hull and domain
    // stages read the transforms, the material's tessellation settings and the
    // height map as well.
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1; // b1
    rootParams[1].Descriptor.RegisterSpace  = 0;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 2; // b2
    rootParams[2].Descriptor.RegisterSpace  = 0;
    rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // One D3D12_DESCRIPTOR_RANGE per texture slot (t0–t7).
    D3D12_DESCRIPTOR_RANGE srvRanges[8]{};
    for (int i = 0; i < 8; ++i)
    {
        srvRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[i].NumDescriptors                    = 1;
        srvRanges[i].BaseShaderRegister                = static_cast<UINT>(i); // t0..t7
        srvRanges[i].RegisterSpace                     = 0;
        srvRanges[i].OffsetInDescriptorsFromTableStart = 0;

        rootParams[3 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[3 + i].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[3 + i].DescriptorTable.pDescriptorRanges   = &srvRanges[i];
        // t7 (height) is displaced by the domain shader too.
        rootParams[3 + i].ShaderVisibility                    = (i == 7)
            ? D3D12_SHADER_VISIBILITY_ALL
            : D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // Static sampler s0: anisotropic wrap for material textures.
    D3D12_STATIC_SAMPLER_DESC staticSampler{};
    staticSampler.Filter           = D3D12_FILTER_ANISOTROPIC;
    staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSampler.MipLODBias       = mTextureMipLODBias;
    staticSampler.MaxAnisotropy    = 16;
    staticSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    staticSampler.MinLOD           = 0.0f;
    staticSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    staticSampler.ShaderRegister   = 0; // s0
    staticSampler.RegisterSpace    = 0;
    staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = static_cast<UINT>(std::size(rootParams));
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &staticSampler;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "EntityMeshRenderer: D3D12SerializeRootSignature failed.";
        return false;
    }
    if (FAILED(device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "EntityMeshRenderer: CreateRootSignature failed.";
        return false;
    }

    // Input layout matches System/Mesh.h :: Vertex
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mRootSignature.Get();
    psoDesc.VS                    = mVertexShader.GetBytecode();
    psoDesc.PS                    = mPixelShader.GetBytecode();
    psoDesc.SampleMask            = UINT_MAX;
    // G-Buffer MRT: three colour outputs (albedo, normal, material).
    psoDesc.NumRenderTargets      = 3;
    psoDesc.RTVFormats[0]         = albedoFormat;
    psoDesc.RTVFormats[1]         = normalFormat;
    psoDesc.RTVFormats[2]         = materialFormat;
    psoDesc.DSVFormat             = depthFormat;
    psoDesc.SampleDesc.Count      = msaaSampleCount;
    psoDesc.SampleDesc.Quality    = 0;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Opaque blend for all three G-Buffer outputs.
    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable           = FALSE;
    rtBlend.LogicOpEnable         = FALSE;
    rtBlend.SrcBlend              = D3D12_BLEND_ONE;
    rtBlend.DestBlend             = D3D12_BLEND_ZERO;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT i = 0; i < 3; ++i)
        psoDesc.BlendState.RenderTarget[i] = rtBlend;

    psoDesc.RasterizerState.FillMode              = mWireframeEnabled ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "EntityMeshRenderer: CreateGraphicsPipelineState failed.";
        return false;
    }

    // Tessellated variant.  Optional: if it cannot be built, tessellated
    // materials simply draw through the plain pipeline.
    mTessPipelineState.Reset();
    {
        const ShaderCompileRequest tessVsRequest{ L"Shaders\\GBuffer.hlsl", L"VSMainTess", L"vs_5_0", ShaderStage::Vertex };
        const ShaderCompileRequest hsRequest    { L"Shaders\\GBuffer.hlsl", L"HSMain",     L"hs_5_0", ShaderStage::Hull };
        const ShaderCompileRequest dsRequest    { L"Shaders\\GBuffer.hlsl", L"DSMain",     L"ds_5_0", ShaderStage::Domain };
        if (mTessVertexShader.Compile(tessVsRequest)
            && mHullShader.Compile(hsRequest)
            && mDomainShader.Compile(dsRequest))
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC tessDesc = psoDesc;
            tessDesc.VS = mTessVertexShader.GetBytecode();
            tessDesc.HS = mHullShader.GetBytecode();
            tessDesc.DS = mDomainShader.GetBytecode();
            tessDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            if (FAILED(device->CreateGraphicsPipelineState(&tessDesc, IID_PPV_ARGS(&mTessPipelineState))))
            {
                mTessPipelineState.Reset();
                OutputDebugStringA("EntityMeshRenderer: tessellated pipeline creation failed.\n");
            }
        }
        else
        {
            OutputDebugStringA("EntityMeshRenderer: tessellation shaders failed to compile.\n");
        }
    }

    mPipelineReady = true;
    mLastError.clear();
    return true;
}

bool EntityMeshRenderer::EnsureRainSurfaceConstantBuffer()
{
    if (mRainSurfaceConstantBuffer && mMappedRainSurfaceCB != nullptr)
    {
        return true;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask     = 1;
    uploadHeap.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = sizeof(RainSurfaceConstants);
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mRainSurfaceConstantBuffer))))
    {
        mLastError = "EntityMeshRenderer: Failed to create rain surface constant buffer.";
        return false;
    }

    mRainSurfaceConstantBuffer->SetName(L"EntityMesh_RainSurfaceCB");

    if (FAILED(mRainSurfaceConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedRainSurfaceCB))))
    {
        mLastError = "EntityMeshRenderer: Failed to map rain surface constant buffer.";
        return false;
    }

    *mMappedRainSurfaceCB = RainSurfaceConstants{};
    return true;
}

bool EntityMeshRenderer::EnsureEntityGpuMesh(
    ID3D12GraphicsCommandList* commandList,
    std::size_t entityIndex,
    const std::shared_ptr<Mesh>& meshAsset,
    std::size_t lodIndex)
{
    const Mesh* mesh = meshAsset.get();
    const MeshCacheKey cacheKey{ mesh, lodIndex };
    auto it = mGpuMeshes.find(cacheKey);
    if (it != mGpuMeshes.end())
    {
        // Buffers are already up to date for this asset and LOD, whichever
        // entity asked for them - as long as the entry really was built from
        // this asset. The entry owns the asset, so the address cannot have been
        // recycled under it; this checks the other way the two can drift, an
        // asset reimported in place and now a different size. The draw takes its
        // index ranges from the asset and its index buffer from here, so a
        // disagreement between them reads past the end of the buffer.
        // The owner comparison comes first so GetLod is only reached for the asset
        // this entry was actually built from, which is known to have the LOD.
        if (it->second.SourceMeshOwner == meshAsset &&
            it->second.IndexCount == static_cast<UINT>(mesh->GetLod(lodIndex).Indices.size()))
        {
            it->second.LastUsedFrame = mFrameCounter;
            return true;
        }

        // Stale. The buffers may still be named by in-flight frames, so they go
        // to the retire list rather than being freed here, and the entry is
        // rebuilt below.
        RetireBuffer(std::move(it->second.VertexBuffer), "stale mesh vertex buffer");
        RetireBuffer(std::move(it->second.IndexBuffer), "stale mesh index buffer");
        mGpuMeshes.erase(it);
    }

    // No stale-entry sweep here, deliberately. The key is the asset, so an
    // entry can never come to describe a different mesh than the one it was
    // built from, and nothing has to be thrown away to correct it. The sweep
    // that used to live here keyed on entity index, so deleting one entity
    // shifted every later index down and made this erase live buffers that
    // in-flight frames were still drawing from - a released resource under a
    // running command list, which the driver reports as a device hang. Entries
    // that really do fall out of use are retired by EvictUnusedMeshes, through
    // the frame-delayed retire list.

    const MeshLod* meshLod = mesh != nullptr ? &mesh->GetLod(lodIndex) : nullptr;

    if (mesh == nullptr || meshLod == nullptr || meshLod->Vertices.empty() || meshLod->Indices.empty())
    {
        std::ostringstream logStream;
        logStream << "Skipped GPU upload for entityIndex=" << entityIndex
                  << " because mesh data was missing or empty."
                  << " meshPtr=" << mesh;
        if (meshLod != nullptr)
        {
            logStream << ", lodIndex=" << lodIndex
                      << ", vertexCount=" << meshLod->Vertices.size()
                      << ", indexCount=" << meshLod->Indices.size();
        }
        LogEntityMeshRendererDiagnostic(logStream.str());
        return false;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    EntityGpuMesh gpuMesh;
    gpuMesh.SourceMesh = mesh;
    gpuMesh.SourceMeshOwner = meshAsset;
    gpuMesh.LodIndex = lodIndex;
    mSceneContentChanged = true;

    const UINT64 vbSize = meshLod->Vertices.size() * sizeof(Vertex);
    const UINT64 ibSize = meshLod->Indices.size()  * sizeof(std::uint32_t);

    // Vertex buffer: upload heap → default heap via CopyBufferRegion.
    if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST, gpuMesh.VertexBuffer, L"Mesh_VertexBuffer"))
    {
        return false;
    }
    // The staging buffer is needed only until the copy recorded below retires.
    // Holding it on the cache entry, as this used to, meant every mesh cost its
    // own size twice for the rest of the session.
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexUpload;
    if (!CreateCommittedBuffer(device, vbSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, vertexUpload, L"Mesh_VertexUpload"))
    {
        return false;
    }

    void* mappedVB = nullptr;
    if (FAILED(vertexUpload->Map(0, nullptr, &mappedVB)))
    {
        return false;
    }
    std::memcpy(mappedVB, meshLod->Vertices.data(), static_cast<std::size_t>(vbSize));
    vertexUpload->Unmap(0, nullptr);

    commandList->CopyBufferRegion(gpuMesh.VertexBuffer.Get(), 0, vertexUpload.Get(), 0, vbSize);
    RetireBuffer(std::move(vertexUpload), "mesh vertex staging buffer");
    auto vbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        gpuMesh.VertexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    commandList->ResourceBarrier(1, &vbBarrier);

    gpuMesh.VertexBufferView.BufferLocation = gpuMesh.VertexBuffer->GetGPUVirtualAddress();
    gpuMesh.VertexBufferView.StrideInBytes  = sizeof(Vertex);
    gpuMesh.VertexBufferView.SizeInBytes    = static_cast<UINT>(vbSize);

    // Index buffer
    if (!CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COPY_DEST, gpuMesh.IndexBuffer, L"Mesh_IndexBuffer"))
    {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> indexUpload;
    if (!CreateCommittedBuffer(device, ibSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, indexUpload, L"Mesh_IndexUpload"))
    {
        return false;
    }

    void* mappedIB = nullptr;
    if (FAILED(indexUpload->Map(0, nullptr, &mappedIB)))
    {
        return false;
    }
    std::memcpy(mappedIB, meshLod->Indices.data(), static_cast<std::size_t>(ibSize));
    indexUpload->Unmap(0, nullptr);

    commandList->CopyBufferRegion(gpuMesh.IndexBuffer.Get(), 0, indexUpload.Get(), 0, ibSize);
    RetireBuffer(std::move(indexUpload), "mesh index staging buffer");
    auto ibBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        gpuMesh.IndexBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_INDEX_BUFFER);
    commandList->ResourceBarrier(1, &ibBarrier);

    gpuMesh.IndexBufferView.BufferLocation = gpuMesh.IndexBuffer->GetGPUVirtualAddress();
    gpuMesh.IndexBufferView.Format         = DXGI_FORMAT_R32_UINT;
    gpuMesh.IndexBufferView.SizeInBytes    = static_cast<UINT>(ibSize);
    gpuMesh.IndexCount = static_cast<UINT>(meshLod->Indices.size());

    {
        std::ostringstream logStream;
        logStream << "Uploaded GPU mesh for entityIndex=" << entityIndex
                  << ", lodIndex=" << lodIndex
                  << ", vertexCount=" << meshLod->Vertices.size()
                  << ", indexCount=" << meshLod->Indices.size()
                  << ", vbBytes=" << vbSize
                  << ", ibBytes=" << ibSize;
        LogEntityMeshRendererDiagnostic(logStream.str());
    }

    gpuMesh.LastUsedFrame = mFrameCounter;
    mGpuMeshes.insert_or_assign(cacheKey, std::move(gpuMesh));
    return true;
}

std::size_t EntityMeshRenderer::SelectLodIndex(
    std::size_t entityIndex,
    const Entity& entity,
    const Mesh& mesh,
    const DirectX::XMFLOAT3& cameraPosition) const
{
    if (!entity.Mesh.has_value())
    {
        return 0;
    }

    const MeshComponent& meshComponent = *entity.Mesh;
    const std::size_t lodCount = mesh.GetLodCount();
    if (lodCount <= 1)
    {
        return 0;
    }

    if (meshComponent.DebugForcedLod >= 0)
    {
        return (std::min)(static_cast<std::size_t>(meshComponent.DebugForcedLod), lodCount - 1);
    }

    const DirectX::XMFLOAT3& position = entity.Transform.Position;
    const float dx = position.x - cameraPosition.x;
    const float dy = position.y - cameraPosition.y;
    const float dz = position.z - cameraPosition.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float scaledDistance = distance * (std::max)(meshComponent.LodUsageScale, 0.1f) * mLodDistanceScale;

    const std::size_t desiredLod =
        (std::min)(static_cast<std::size_t>(scaledDistance / kLodDistanceStep), lodCount - 1);

    // Hysteresis around the switch distance.
    //
    // Without it a bare threshold makes an entity parked near a boundary flip
    // LOD on sub-millimetre camera movement, every frame, for as long as the
    // camera is moving. That is not just a popping artifact: a different LOD is
    // different triangles, so the G-Buffer's depth and normals change with it,
    // and anything that reconstructs world position from them - ray traced GI
    // above all - sees its ray origins jump between two surfaces and flickers.
    // Deferred shading hides it because both LODs carry the same albedo.
    //
    // The dead band is asymmetric by construction: a level is only entered once
    // clearly past its threshold, and only left once clearly back inside the
    // previous one, so no single distance can satisfy both tests.
    const auto previousIt = mEntityLodState.find(entityIndex);
    if (previousIt != mEntityLodState.end())
    {
        const std::size_t previousLod = (std::min)(previousIt->second, lodCount - 1);

        if (desiredLod > previousLod)
        {
            // Going coarser: require the distance to be past the boundary by
            // the margin before accepting it.
            const float switchDistance =
                static_cast<float>(previousLod + 1) * kLodDistanceStep * (1.0f + kLodHysteresis);
            if (scaledDistance < switchDistance)
            {
                mEntityLodState[entityIndex] = previousLod;
                return previousLod;
            }
        }
        else if (desiredLod < previousLod)
        {
            // Going finer: require the distance to be back inside the previous
            // level's threshold by the same margin.
            const float switchDistance =
                static_cast<float>(previousLod) * kLodDistanceStep * (1.0f - kLodHysteresis);
            if (scaledDistance > switchDistance)
            {
                mEntityLodState[entityIndex] = previousLod;
                return previousLod;
            }
        }
    }

    mEntityLodState[entityIndex] = desiredLod;
    return desiredLod;
}

bool EntityMeshRenderer::EnsureConstantBuffer(std::size_t requiredEntityCount)
{
    if (requiredEntityCount <= mCBCapacity)
    {
        return true;
    }

    // The outgoing buffer goes to the retire list rather than forcing a GPU
    // flush here. Unmap is a CPU-side operation and the resource stays alive
    // through the retained reference, so draws already recorded against it keep
    // reading valid memory.
    if (mConstantBuffer)
    {
        if (mMappedCB != nullptr)
        {
            mConstantBuffer->Unmap(0, nullptr);
            mMappedCB = nullptr;
        }
        RetireBuffer(std::move(mConstantBuffer), "entity constant buffer");
    }
    mConstantBuffer.Reset();
    mCBCapacity = 0;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    // Allocate with some headroom so frequent entity additions don't cause re-allocs.
    const std::size_t newCapacity = requiredEntityCount + 16;
    const UINT64 cbSize = static_cast<UINT64>(newCapacity) * kFramesInFlight * sizeof(EntityConstants);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask     = 1;
    uploadHeap.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = cbSize;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mConstantBuffer))))
    {
        mLastError = "EntityMeshRenderer: Failed to create constant buffer.";
        return false;
    }

    mConstantBuffer->SetName(L"EntityMesh_EntityCB");

    if (FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedCB))))
    {
        mLastError = "EntityMeshRenderer: Failed to map constant buffer.";
        return false;
    }

    PTERO_LOG_INFO("Meshes", "Entity constant buffer grown to %llu slots x %llu frames (%llu bytes).",
                   static_cast<unsigned long long>(newCapacity),
                   static_cast<unsigned long long>(kFramesInFlight),
                   static_cast<unsigned long long>(cbSize));
    mCBCapacity = newCapacity;
    return true;
}

bool EntityMeshRenderer::EnsureDepthPassConstantBuffer(std::size_t requiredEntityCount)
{
    if (requiredEntityCount <= mDepthPassCBCapacity)
    {
        return true;
    }

    if (mDepthPassConstantBuffer)
    {
        if (mMappedDepthPassCB != nullptr)
        {
            mDepthPassConstantBuffer->Unmap(0, nullptr);
            mMappedDepthPassCB = nullptr;
        }
        RetireBuffer(std::move(mDepthPassConstantBuffer), "depth-pass constant buffer");
    }
    mDepthPassConstantBuffer.Reset();
    mDepthPassCBCapacity = 0;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    const std::size_t newCapacity = requiredEntityCount + 16;
    const UINT64 cbSize = static_cast<UINT64>(newCapacity) * kFramesInFlight * sizeof(EntityConstants);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask     = 1;
    uploadHeap.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = cbSize;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mDepthPassConstantBuffer))))
    {
        mLastError = "EntityMeshRenderer: Failed to create shadow-pass constant buffer.";
        return false;
    }

    mDepthPassConstantBuffer->SetName(L"EntityMesh_DepthPassCB");

    if (FAILED(mDepthPassConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedDepthPassCB))))
    {
        mLastError = "EntityMeshRenderer: Failed to map shadow-pass constant buffer.";
        return false;
    }

    PTERO_LOG_INFO("Meshes", "Shadow-pass constant buffer grown to %llu slots x %llu frames.",
                   static_cast<unsigned long long>(newCapacity),
                   static_cast<unsigned long long>(kFramesInFlight));
    mDepthPassCBCapacity = newCapacity;
    return true;
}

bool EntityMeshRenderer::EnsurePointShadowFaceConstantBuffer()
{
    if (mPointShadowFaceConstantBuffer && mMappedPointShadowFaceCB != nullptr)
    {
        return true;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask     = 1;
    uploadHeap.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = sizeof(PointShadowFaceConstants);
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mPointShadowFaceConstantBuffer))))
    {
        mLastError = "EntityMeshRenderer: Failed to create point-shadow face constant buffer.";
        return false;
    }

    mPointShadowFaceConstantBuffer->SetName(L"EntityMesh_PointShadowFaceCB");

    if (FAILED(mPointShadowFaceConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedPointShadowFaceCB))))
    {
        mLastError = "EntityMeshRenderer: Failed to map point-shadow face constant buffer.";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// CreateFallbackTexture
// Uploads a single 1×1 opaque-white RGBA8 pixel as a committed GPU texture so
// entities without a material always have something valid bound at t0.
// ---------------------------------------------------------------------------
bool EntityMeshRenderer::CreateFallbackTexture(ID3D12GraphicsCommandList* commandList)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    // Allocate an SRV descriptor slot for the fallback.
    if (!DX12Context_AllocateSrvDescriptor(&mFallbackCpuHandle, &mFallbackGpuHandle))
    {
        mLastError = "EntityMeshRenderer: SRV heap full, cannot allocate fallback texture slot.";
        return false;
    }

    // Describe a 1×1 R8G8B8A8_UNORM texture.
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width              = 1;
    texDesc.Height             = 1;
    texDesc.DepthOrArraySize   = 1;
    texDesc.MipLevels          = 1;
    texDesc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count   = 1;
    texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE,
        &texDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&mFallbackTextureResource))))
    {
        mLastError = "EntityMeshRenderer: Failed to create fallback texture resource.";
        return false;
    }

    // Upload heap buffer for the single white pixel.
    const UINT64 uploadSize = GetRequiredIntermediateSize(mFallbackTextureResource.Get(), 0, 1);
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    CD3DX12_RESOURCE_DESC uploadBufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);

    if (FAILED(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &uploadBufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mFallbackUploadBuffer))))
    {
        mLastError = "EntityMeshRenderer: Failed to create fallback upload buffer.";
        return false;
    }

    // White pixel: R=255 G=255 B=255 A=255
    const uint32_t whitePixel = 0xFFFFFFFFu;
    D3D12_SUBRESOURCE_DATA subData{};
    subData.pData      = &whitePixel;
    subData.RowPitch   = 4;
    subData.SlicePitch = 4;

    UpdateSubresources(commandList, mFallbackTextureResource.Get(), mFallbackUploadBuffer.Get(),
        0, 0, 1, &subData);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        mFallbackTextureResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);

    // mFallbackUploadBuffer is kept as a member and released after the first Render()
    // GPU flush so the GPU has finished reading from it.

    // Create the SRV.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(mFallbackTextureResource.Get(), &srvDesc, mFallbackCpuHandle);

    return true;
}

// ---------------------------------------------------------------------------
// ResolveBaseColorDdsPath
// Reads a material .json file (path is absolute or relative-to-Data) and
// returns the absolute path of the base-color DDS, or "" on failure.
// ---------------------------------------------------------------------------
std::string EntityMeshRenderer::ResolveBaseColorDdsPath(const std::string& materialPath) const
{
    if (materialPath.empty()) return {};

    // Try the path as-is first (already absolute), then try relative to Data/.
    auto tryLoad = [](const std::filesystem::path& p) -> std::string
    {
        try
        {
            std::string text;
            if (!DataFiles::ReadText(p, text)) return {};
            const nlohmann::json j = nlohmann::json::parse(text);
            auto textures = j.find("textures");
            if (textures == j.end()) return {};
            auto bc = textures->find("baseColor");
            if (bc == textures->end()) return {};
            return bc->get<std::string>();
        }
        catch (...) { return {}; }
    };

    std::string relPath = tryLoad(std::filesystem::path(materialPath));

    if (relPath.empty()) return {};

    // relPath is relative to Data/ — resolve to an absolute path.
    // Walk up from the material file to find the Data directory.
    std::filesystem::path matDir = std::filesystem::path(materialPath).parent_path();
    std::filesystem::path current = matDir;
    while (!current.empty())
    {
        // Check if this directory contains a "Data" sibling that holds our relPath.
        std::filesystem::path candidate = current.parent_path() / "Data" / relPath;
        if (DataFiles::Exists(candidate))
            return candidate.string();

        // Also check if current itself is Data/.
        candidate = current / relPath;
        if (current.filename() == "Data" && DataFiles::Exists(candidate))
            return candidate.string();

        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }

    return {};
}

// ---------------------------------------------------------------------------
// EnsureMaterialConstantBuffer
// Grows or creates the per-draw material CB so it has at least requiredDrawCount
// aligned 256-byte slots.
// ---------------------------------------------------------------------------
bool EntityMeshRenderer::EnsureMaterialConstantBuffer(std::size_t requiredDrawCount)
{
    if (requiredDrawCount <= mMatCBCapacity)
        return true;

    if (mMaterialCB)
    {
        if (mMappedMatCB != nullptr)
        {
            mMaterialCB->Unmap(0, nullptr);
            mMappedMatCB = nullptr;
        }
        RetireBuffer(std::move(mMaterialCB), "material constant buffer");
    }
    mMaterialCB.Reset();
    mMatCBCapacity = 0;

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    const std::size_t newCapacity = requiredDrawCount + 64;
    const UINT64 size = static_cast<UINT64>(newCapacity) * kFramesInFlight * sizeof(MaterialConstants);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type                 = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask     = 1;
    uploadHeap.VisibleNodeMask      = 1;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mMaterialCB))))
    {
        mLastError = "EntityMeshRenderer: Failed to create material constant buffer.";
        return false;
    }
    mMaterialCB->SetName(L"EntityMesh_MaterialCB");

    if (FAILED(mMaterialCB->Map(0, nullptr, reinterpret_cast<void**>(&mMappedMatCB))))
    {
        mLastError = "EntityMeshRenderer: Failed to map material constant buffer.";
        return false;
    }
    PTERO_LOG_INFO("Meshes", "Material constant buffer grown to %llu draws x %llu frames.",
                   static_cast<unsigned long long>(newCapacity),
                   static_cast<unsigned long long>(kFramesInFlight));
    mMatCBCapacity = newCapacity;
    return true;
}

// ---------------------------------------------------------------------------
// ResolveAllSubMaterialTextures
// Reads a material JSON file and returns a map of materialId ->
// SubMaterialTextures (all texture paths resolved to absolute paths, plus
// scalar factors stored in the struct fields).
//
// The result is cached per material path. The geometry pass calls this once per
// entity per frame, and doing the work each time meant opening and parsing the
// JSON plus a filesystem probe for every texture path - hundreds of blocking
// metadata syscalls per frame across a scene, which does not show up as CPU
// load because the thread spends it waiting. The cache is revalidated against
// the file's write time, throttled so the check itself does not reintroduce the
// per-frame filesystem traffic it exists to remove.
// ---------------------------------------------------------------------------
const std::unordered_map<uint32_t, EntityMeshRenderer::SubMaterialTextures>&
EntityMeshRenderer::ResolveAllSubMaterialTextures(const std::string& materialPath) const
{
    static const std::unordered_map<uint32_t, SubMaterialTextures> kEmptyResult;
    if (materialPath.empty()) return kEmptyResult;

    const auto now = std::chrono::steady_clock::now();

    if (auto cached = mMaterialTextureCache.find(materialPath); cached != mMaterialTextureCache.end())
    {
        CachedMaterialTextures& entry = cached->second;

        if (now - entry.LastCheckTime < kMaterialRevalidateInterval)
        {
            return entry.Textures;
        }

        entry.LastCheckTime = now;

        // Due a revalidation: one filesystem call, and only if the file has
        // actually changed do we pay for the reparse.
        std::error_code writeTimeError;
        const std::filesystem::path absolutePath = ResolveMaterialFilePath(materialPath);
        const bool exists = !absolutePath.empty();
        const auto writeTime = exists
            ? std::filesystem::last_write_time(absolutePath, writeTimeError)
            : std::filesystem::file_time_type{};

        if (exists == entry.FileExists && (writeTimeError || writeTime == entry.LastWriteTime))
        {
            return entry.Textures;
        }

        entry.FileExists = exists;
        entry.LastWriteTime = writeTimeError ? std::filesystem::file_time_type{} : writeTime;
        entry.Textures = ParseSubMaterialTextures(materialPath);
        return entry.Textures;
    }

    CachedMaterialTextures entry;
    entry.LastCheckTime = now;
    {
        std::error_code writeTimeError;
        const std::filesystem::path absolutePath = ResolveMaterialFilePath(materialPath);
        entry.FileExists = !absolutePath.empty();
        if (entry.FileExists)
        {
            const auto writeTime = std::filesystem::last_write_time(absolutePath, writeTimeError);
            entry.LastWriteTime = writeTimeError ? std::filesystem::file_time_type{} : writeTime;
        }
    }
    entry.Textures = ParseSubMaterialTextures(materialPath);

    return mMaterialTextureCache.insert_or_assign(materialPath, std::move(entry)).first->second.Textures;
}

// ---------------------------------------------------------------------------
// ParseSubMaterialTextures
// The uncached parse. Only reached on a cache miss or when the material file
// has changed on disk.
// ---------------------------------------------------------------------------
std::unordered_map<uint32_t, EntityMeshRenderer::SubMaterialTextures>
EntityMeshRenderer::ParseSubMaterialTextures(const std::string& materialPath) const
{
    std::unordered_map<uint32_t, SubMaterialTextures> result;
    if (materialPath.empty()) return result;

    // Walks up from nearPath to locate the Data/ root and resolve relPath from there.
    auto resolveDataRelativePath = [](const std::string& relPath,
                                      const std::filesystem::path& nearPath) -> std::string
    {
        if (relPath.empty()) return {};
        std::filesystem::path current = nearPath.parent_path();
        while (!current.empty())
        {
            std::filesystem::path candidate = current.parent_path() / "Data" / relPath;
            if (DataFiles::Exists(candidate)) return candidate.string();
            candidate = current / relPath;
            if (current.filename() == "Data" && DataFiles::Exists(candidate))
                return candidate.string();
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        return {};
    };

    // Extracts all texture paths and scalar factors from a single sub-material JSON node.
    auto texFromNode = [&](const nlohmann::json& node, const std::filesystem::path& matFile) -> SubMaterialTextures
    {
        SubMaterialTextures t;
        auto texIt = node.find("textures");
        if (texIt != node.end())
        {
            auto resolve = [&](const char* key) -> std::string
            {
                auto it = texIt->find(key);
                if (it == texIt->end() || !it->is_string()) return {};
                const std::string relPath = it->get<std::string>();
                if (!Udim::HasToken(relPath))
                    return resolveDataRelativePath(relPath, matFile);

                // UDIM set: resolve through the first tile that exists on disk, then put the
                // token back so the geometry pass can substitute each tile it draws.
                for (std::uint32_t tile = Udim::kFirstTile; tile <= Udim::kLastTile; ++tile)
                {
                    const std::string resolved = resolveDataRelativePath(Udim::Resolve(relPath, tile), matFile);
                    if (resolved.empty()) continue;

                    const std::string tileText = std::to_string(tile);
                    const std::size_t tailLength = relPath.size() - relPath.rfind(Udim::kToken);
                    // The tile number sits where the token was, counted from the end of the path.
                    const std::size_t pos = resolved.size() - (tailLength - std::char_traits<char>::length(Udim::kToken) + tileText.size());
                    std::string templatePath = resolved;
                    templatePath.replace(pos, tileText.size(), Udim::kToken);
                    t.hasUdim = true;
                    if (t.udimFirstTile == 0 || tile < t.udimFirstTile) t.udimFirstTile = tile;
                    return templatePath;
                }
                return {};
            };
            t.baseColor  = resolve("baseColor");
            t.normal     = resolve("normal");
            t.packedMaterial = resolve("metallicRoughness");
            if (t.packedMaterial.empty())
            {
                t.packedMaterial = resolve("orm");
                t.packedLayout = 2;
            }
            t.metallic   = resolve("metallic");
            t.roughness  = resolve("roughness");
            t.ao         = resolve("ao");
            if (t.ao.empty())
            {
                t.ao = resolve("ambientOcclusion");
            }
            t.emissive   = resolve("emissive");
            t.opacity    = resolve("opacity");
            t.height     = resolve("height");
        }

        // Read scalar parameters; use safe value() calls with sensible defaults.
        t.metallicFactor  = node.value("metallicFactor",          1.f);
        t.roughnessFactor = node.value("roughnessFactor",         1.f);
        // Materials authored before the specular slider existed carry no key; 0.5 is the
        // value that reproduces exactly how they used to shade.
        t.specularFactor  = node.value("specularFactor",           0.5f);
        t.normalScale     = node.value("normalScale",             1.f);
        t.flipNormalGreen = node.value("normalFlipGreen",         false);
        // JSON uses "ambientOcclusionStrength" for the AO multiplier.
        t.aoStrength      = node.value("ambientOcclusionStrength", 1.f);
        t.opacityFactor   = node.value("opacity",                  1.f);
        t.alphaCutoff     = node.value("alphaCutoff",              0.5f);
        t.useAlphaCutout  = node.value("useAlphaCutout",           false);
        t.useTransparentBlend = node.value("useTransparentBlend",  false);

        t.uvRotationDegrees     = node.value("uvRotationDegrees",    0.f);
        t.useParallaxOcclusion  = node.value("useParallaxOcclusion", false);
        t.parallaxHeightScale   = node.value("heightScale",          0.05f);
        t.parallaxMinSteps      = node.value("parallaxMinSteps",     8);
        t.parallaxMaxSteps      = node.value("parallaxMaxSteps",     32);
        t.parallaxFadeDistance  = node.value("parallaxFadeDistance", 30.f);
        t.parallaxReferenceHeight = node.value("heightReference",     1.f);

        t.useTessellation       = node.value("useTessellation",          false);
        t.tessMaxFactor         = node.value("tessellationMaxFactor",    16.f);
        t.tessTargetPixels      = node.value("tessellationTargetPixels", 8.f);
        t.tessFadeDistance      = node.value("tessellationFadeDistance", 60.f);
        t.displacementScale     = node.value("displacementScale",        0.05f);
        t.displacementMidLevel  = node.value("displacementMidLevel",     0.5f);

        t.useSubsurfaceScattering = node.value("useSubsurfaceScattering", false);
        t.subsurfaceRadiusMm      = node.value("subsurfaceRadiusMm",      3.f);
        t.subsurfaceTranslucency  = node.value("subsurfaceTranslucency",  0.8f);

        // UV tiling and offset are stored as two-element arrays [u, v].
        auto readFloat2 = [&node](const char* key, float& outU, float& outV)
        {
            auto it = node.find(key);
            if (it != node.end() && it->is_array() && it->size() >= 2)
            {
                outU = (*it)[0].get<float>();
                outV = (*it)[1].get<float>();
            }
        };
        readFloat2("uvTiling", t.uvTilingU, t.uvTilingV);
        readFloat2("uvOffset", t.uvOffsetU, t.uvOffsetV);

        auto readFloat3 = [&node](const char* key, float& outR, float& outG, float& outB)
        {
            auto it = node.find(key);
            if (it != node.end() && it->is_array() && it->size() >= 3)
            {
                outR = (*it)[0].get<float>();
                outG = (*it)[1].get<float>();
                outB = (*it)[2].get<float>();
            }
        };
        readFloat3("subsurfaceColor", t.subsurfaceColorR, t.subsurfaceColorG, t.subsurfaceColorB);
        readFloat3("subsurfaceFalloff", t.subsurfaceFalloffR, t.subsurfaceFalloffG, t.subsurfaceFalloffB);

        // Base color tint is stored as an RGBA array [r,g,b,a] in 0–1 range.
        auto tintIt = node.find("baseColorTint");
        if (tintIt != node.end() && tintIt->is_array() && tintIt->size() >= 4)
        {
            t.baseColorTintR = (*tintIt)[0].get<float>();
            t.baseColorTintG = (*tintIt)[1].get<float>();
            t.baseColorTintB = (*tintIt)[2].get<float>();
            t.baseColorTintA = (*tintIt)[3].get<float>();
        }
        return t;
    };

    try
    {
        std::filesystem::path matFilePath = ResolveMaterialFilePath(materialPath);
        if (matFilePath.empty())
            return result;

        std::string matText;
        if (!DataFiles::ReadText(matFilePath, matText)) return result;
        const nlohmann::json j = nlohmann::json::parse(matText);

        const std::string type = j.value("type", std::string{});
        if (type == "MultiMaterial")
        {
            auto subMatsIt = j.find("subMaterials");
            if (subMatsIt != j.end() && subMatsIt->is_array())
            {
                uint32_t id = 0;
                for (const auto& subMat : *subMatsIt)
                {
                    result[id] = texFromNode(subMat, matFilePath);
                    ++id;
                }
            }
        }
        else
        {
            result[0] = texFromNode(j, matFilePath);
        }
    }
    catch (...) {}

    return result;
}

// ---------------------------------------------------------------------------
// ResolveSubMaterialDdsPaths (kept for backward compat with RTGI path)
// ---------------------------------------------------------------------------
std::unordered_map<uint32_t, std::string>
EntityMeshRenderer::ResolveSubMaterialDdsPaths(const std::string& materialPath) const
{
    std::unordered_map<uint32_t, std::string> result;
    const auto& all = ResolveAllSubMaterialTextures(materialPath);
    for (const auto& [id, tex] : all)
        result[id] = tex.hasUdim ? Udim::Resolve(tex.baseColor, tex.udimFirstTile) : tex.baseColor;
    return result;
}
