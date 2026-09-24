#include "pch.h"
#include "System/DataFiles.h"

#include "VegetationRenderer.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    constexpr std::uint8_t kWhitePixel[4] = { 255, 255, 255, 255 };

    // 5 uints: IndexCountPerInstance, InstanceCount, StartIndexLocation,
    // BaseVertexLocation, StartInstanceLocation.
    constexpr UINT kDrawArgStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    static_assert(kDrawArgStride == 20);

    // Byte offset of InstanceCount within the argument struct; the cull shader
    // does its InterlockedAdd here.
    constexpr UINT kInstanceCountByteOffset = 4;

    bool CreateCommittedBuffer(
        ID3D12Device*         device,
        UINT64                byteSize,
        D3D12_HEAP_TYPE       heapType,
        D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_FLAGS  flags,
        ComPtr<ID3D12Resource>& outResource)
    {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type                 = heapType;
        heapProperties.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProperties.CreationNodeMask     = 1;
        heapProperties.VisibleNodeMask      = 1;

        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize, flags);

        return SUCCEEDED(device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&outResource)));
    }

    std::filesystem::path FindProjectDataDirectory()
    {
        // Walks up from the exe to Data/ - or, in a packaged game, the virtual Data
        // root the .ppak archives serve (see System/DataFiles.h).
        return DataFiles::FindDataDirectory();
    }

    std::filesystem::path ResolveDataRelativePath(const std::string& relativePath)
    {
        if (relativePath.empty())
            return {};

        std::filesystem::path path(relativePath);
        std::error_code errorCode;
        if (path.is_absolute() && DataFiles::Exists(path))
            return std::filesystem::weakly_canonical(path, errorCode);

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (!dataDirectory.empty())
        {
            path = (dataDirectory / path).lexically_normal();
            if (DataFiles::Exists(path))
                return std::filesystem::weakly_canonical(path, errorCode);
        }

        return {};
    }

    // Material textures are stored relative to the material file, matching how
    // EntityMeshRenderer and TerrainRenderer resolve theirs.
    std::string ResolveTextureNearMaterial(
        const std::filesystem::path& materialFilePath,
        const std::string&           texturePath)
    {
        if (texturePath.empty())
            return {};

        std::error_code errorCode;
        std::filesystem::path path(texturePath);
        if (path.is_absolute() && DataFiles::Exists(path))
            return std::filesystem::weakly_canonical(path, errorCode).string();

        const std::filesystem::path beside = (materialFilePath.parent_path() / path).lexically_normal();
        if (DataFiles::Exists(beside))
            return std::filesystem::weakly_canonical(beside, errorCode).string();

        const std::filesystem::path fromData = ResolveDataRelativePath(texturePath);
        return fromData.empty() ? std::string{} : fromData.string();
    }

    // Extract the six world-space frustum planes from a view-projection matrix
    // (Gribb-Hartmann), normalised and pointing inward.
    void ExtractFrustumPlanes(const XMMATRIX& viewProjection, XMFLOAT4 outPlanes[6])
    {
        XMFLOAT4X4 m{};
        XMStoreFloat4x4(&m, viewProjection);

        // left, right, bottom, top, near, far
        const XMFLOAT4 raw[6] =
        {
            XMFLOAT4(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41),
            XMFLOAT4(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41),
            XMFLOAT4(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42),
            XMFLOAT4(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42),
            XMFLOAT4(m._13,         m._23,         m._33,         m._43),
            XMFLOAT4(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43),
        };

        for (int i = 0; i < 6; ++i)
        {
            const float length = std::sqrt(raw[i].x * raw[i].x + raw[i].y * raw[i].y + raw[i].z * raw[i].z);
            const float inverse = (length > 1e-6f) ? (1.0f / length) : 1.0f;
            outPlanes[i] = XMFLOAT4(raw[i].x * inverse, raw[i].y * inverse, raw[i].z * inverse, raw[i].w * inverse);
        }
    }

    // Immutable copy of the terrain, handed to worker threads.
    //
    // The scatter runs off the main thread while the terrain brush can be
    // editing the live heightmap, so reading TerrainRenderer directly from a
    // job would be a data race.  One snapshot is shared by every job started
    // from the same terrain revision, so the copy cost is paid once per edit
    // rather than once per area.
    class TerrainSnapshot final : public IVegetationTerrainSource
    {
    public:
        explicit TerrainSnapshot(std::vector<TerrainRenderer::PatchData> patches)
            : mPatches(std::move(patches))
        {
        }

        bool SampleSurface(const XMFLOAT2& worldXY, VegetationSurfaceSample& outSample) const override
        {
            for (const TerrainRenderer::PatchData& patch : mPatches)
            {
                if (patch.Width <= 1 || patch.Height <= 1)
                    continue;

                const float halfSize = patch.WorldSize * 0.5f;
                const float localX = worldXY.x - patch.Origin.x;
                const float localY = worldXY.y - patch.Origin.y;

                if (localX < -halfSize || localX > halfSize || localY < -halfSize || localY > halfSize)
                    continue;

                const float cellX = ((localX + halfSize) / patch.WorldSize) * static_cast<float>(patch.Width  - 1);
                const float cellY = ((localY + halfSize) / patch.WorldSize) * static_cast<float>(patch.Height - 1);

                const int x0 = (std::max)(0, (std::min)(patch.Width  - 1, static_cast<int>(std::floor(cellX))));
                const int y0 = (std::max)(0, (std::min)(patch.Height - 1, static_cast<int>(std::floor(cellY))));
                const int x1 = (std::min)(patch.Width  - 1, x0 + 1);
                const int y1 = (std::min)(patch.Height - 1, y0 + 1);
                const float tx = cellX - static_cast<float>(x0);
                const float ty = cellY - static_cast<float>(y0);

                const auto sampleAt = [&](int x, int y) -> float
                {
                    return static_cast<float>(patch.Samples[static_cast<std::size_t>(y) * patch.Width + x]) / 65535.0f;
                };

                const float h0 = sampleAt(x0, y0) * (1.0f - tx) + sampleAt(x1, y0) * tx;
                const float h1 = sampleAt(x0, y1) * (1.0f - tx) + sampleAt(x1, y1) * tx;
                const float normalised = h0 * (1.0f - ty) + h1 * ty;

                outSample.Height = normalised * patch.HeightScale + patch.HeightOffset + patch.Origin.z;

                // Central-difference normal, matching TerrainRenderer::SampleNormalAt.
                const int cx = (std::max)(0, (std::min)(patch.Width  - 1, static_cast<int>(cellX + 0.5f)));
                const int cy = (std::max)(0, (std::min)(patch.Height - 1, static_cast<int>(cellY + 0.5f)));
                const int xm = (std::max)(0, cx - 1);
                const int xp = (std::min)(patch.Width  - 1, cx + 1);
                const int ym = (std::max)(0, cy - 1);
                const int yp = (std::min)(patch.Height - 1, cy + 1);

                const float hL = sampleAt(xm, cy) * patch.HeightScale;
                const float hR = sampleAt(xp, cy) * patch.HeightScale;
                const float hD = sampleAt(cx, ym) * patch.HeightScale;
                const float hU = sampleAt(cx, yp) * patch.HeightScale;

                const float xStep = patch.WorldSize / static_cast<float>(patch.Width  - 1);
                const float yStep = patch.WorldSize / static_cast<float>(patch.Height - 1);
                const float dx = static_cast<float>(xp - xm) * xStep;
                const float dy = static_cast<float>(yp - ym) * yStep;

                XMFLOAT3 normal(
                    (dx > 0.0f) ? -(hR - hL) / dx : 0.0f,
                    (dy > 0.0f) ? -(hU - hD) / dy : 0.0f,
                    1.0f);
                XMStoreFloat3(&outSample.Normal, XMVector3Normalize(XMLoadFloat3(&normal)));

                if (!patch.LayerWeights.empty())
                {
                    const XMVECTOR row0 = XMVectorLerp(
                        XMLoadFloat4(&patch.LayerWeights[static_cast<std::size_t>(y0) * patch.Width + x0]),
                        XMLoadFloat4(&patch.LayerWeights[static_cast<std::size_t>(y0) * patch.Width + x1]), tx);
                    const XMVECTOR row1 = XMVectorLerp(
                        XMLoadFloat4(&patch.LayerWeights[static_cast<std::size_t>(y1) * patch.Width + x0]),
                        XMLoadFloat4(&patch.LayerWeights[static_cast<std::size_t>(y1) * patch.Width + x1]), tx);
                    XMStoreFloat4(&outSample.LayerWeights, XMVectorLerp(row0, row1, ty));
                    outSample.HasLayerWeights = true;
                }
                else
                {
                    outSample.HasLayerWeights = false;
                }

                return true;
            }

            return false;
        }

    private:
        std::vector<TerrainRenderer::PatchData> mPatches;
    };

    // Shared across all scatter jobs started from the same terrain revision.
    std::shared_ptr<TerrainSnapshot> gTerrainSnapshot;
    std::uint64_t                    gTerrainSnapshotRevision = ~0ull;

    // Hash helper for the rule fingerprint.
    inline void HashBytes(std::uint64_t& hash, const void* data, std::size_t size)
    {
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 0x100000001b3ull;   // FNV-1a prime
        }
    }

    template <typename T>
    inline void HashValue(std::uint64_t& hash, const T& value)
    {
        HashBytes(hash, &value, sizeof(T));
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

bool VegetationRenderer::Initialize(ID3D12GraphicsCommandList* commandList)
{
    if (mInitialized)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "VegetationRenderer::Initialize: device is null.";
        return false;
    }

    if (!CreateFallbackTexture(commandList))
        return false;

    if (!CreateBillboardQuad(commandList))
        return false;

    if (!EnsureConstantBuffers(16))
        return false;

    if (!CreateCommandSignature())
        return false;

    if (!CreateCullPipeline())
        return false;

    if (!CreateInteractionPipeline())
        return false;

    if (!EnsureInteractionResources())
        return false;

    mInitialized = true;
    return true;
}

void VegetationRenderer::Shutdown()
{
    // Worker threads hold references to the snapshot and to nothing GPU-side,
    // but they must still finish before the objects they captured go away.
    for (auto& entry : mAreas)
    {
        if (entry.second.ScatterJob.valid())
            entry.second.ScatterJob.wait();
    }

    mAreas.clear();
    mSharedMeshes.clear();
    mMaterials.clear();
    mFlatLayers.clear();

    if (mMappedPassCb && mPassCb)         mPassCb->Unmap(0, nullptr);
    if (mMappedLayerCb && mLayerCb)       mLayerCb->Unmap(0, nullptr);
    if (mMappedCullCb && mCullCb)         mCullCb->Unmap(0, nullptr);
    if (mMappedInteractionCb && mInteractionCb) mInteractionCb->Unmap(0, nullptr);
    if (mIndirectArgResetPtr && mIndirectArgReset) mIndirectArgReset->Unmap(0, nullptr);
    if (mInteractorBufferPtr && mInteractorBuffer) mInteractorBuffer->Unmap(0, nullptr);
    if (mInteractorVelocityBufferPtr && mInteractorVelocityBuffer) mInteractorVelocityBuffer->Unmap(0, nullptr);

    mMappedPassCb = nullptr;
    mMappedLayerCb = nullptr;
    mMappedCullCb = nullptr;
    mMappedInteractionCb = nullptr;
    mIndirectArgResetPtr = nullptr;
    mInteractorBufferPtr = nullptr;
    mInteractorVelocityBufferPtr = nullptr;

    mPassCb.Reset();
    mLayerCb.Reset();
    mCullCb.Reset();
    mInteractionCb.Reset();
    mVisibleIndexBuffer.Reset();
    mIndirectArgBuffer.Reset();
    mIndirectArgReset.Reset();
    mInteractorBuffer.Reset();
    mInteractorVelocityBuffer.Reset();
    mInteractionMap[0].Reset();
    mInteractionMap[1].Reset();
    mCommandSignature.Reset();
    mGraphicsRootSignature.Reset();
    mGraphicsPipelineState.Reset();
    mShadowPipelineState.Reset();
    mMotionPipelineState.Reset();
    mBillboardPipelineState.Reset();
    mBillboardVertexBuffer.Reset();
    mBillboardVertexUpload.Reset();
    mBillboardIndexBuffer.Reset();
    mBillboardIndexUpload.Reset();
    mBillboardTextures.clear();
    mCullRootSignature.Reset();
    mCullPipelineState.Reset();
    mInteractionRootSignature.Reset();
    mInteractionPipelineState.Reset();
    mFallbackTextureResource.Reset();
    mFallbackUploadBuffer.Reset();

    mTextureManager.Shutdown();

    gTerrainSnapshot.reset();
    gTerrainSnapshotRevision = ~0ull;

    mInitialized = false;
    mPipelineReady = false;
    mShadowPipelineReady = false;
}

void VegetationRenderer::SetEntities(std::vector<Entity>* entities)
{
    if (mEntities != entities)
    {
        // Entity indices are the area key, so a different list invalidates
        // every cached scatter.
        for (auto& entry : mAreas)
        {
            if (entry.second.ScatterJob.valid())
                entry.second.ScatterJob.wait();
        }
        mAreas.clear();
        mLayoutDirty = true;
    }

    mEntities = entities;
}

void VegetationRenderer::RequestRegenerate(std::size_t entityIndex)
{
    auto it = mAreas.find(entityIndex);
    if (it != mAreas.end())
        it->second.ForceRegenerate = true;
}

void VegetationRenderer::RequestRegenerateAll()
{
    for (auto& entry : mAreas)
        entry.second.ForceRegenerate = true;
}

bool VegetationRenderer::GetAreaStats(
    std::size_t             entityIndex,
    std::uint32_t&          outInstanceCount,
    VegetationScatterStats& outStats,
    bool&                   outScatterInProgress) const
{
    const auto it = mAreas.find(entityIndex);
    if (it == mAreas.end())
        return false;

    outInstanceCount     = it->second.InstanceCount;
    outStats             = it->second.Stats;
    outScatterInProgress = it->second.ScatterInProgress;
    return true;
}

// ---------------------------------------------------------------------------
// Scatter scheduling
// ---------------------------------------------------------------------------

std::uint64_t VegetationRenderer::ComputeRuleHash(
    const VegetationAreaComponent& area,
    const TransformComponent&      transform)
{
    // FNV-1a over every input the scatter reads.  Cheaper and less error-prone
    // than tracking a dirty flag on each individual field in the editor UI,
    // and it cannot miss an edit path.
    std::uint64_t hash = 0xcbf29ce484222325ull;

    HashValue(hash, transform.Position);
    HashValue(hash, transform.Rotation);
    HashValue(hash, area.Shape);
    HashValue(hash, area.ExtentX);
    HashValue(hash, area.ExtentY);
    HashValue(hash, area.ExtentZ);
    HashValue(hash, area.Seed);
    HashValue(hash, area.SnapToTerrain);
    HashValue(hash, area.SnapToGeometry);
    HashValue(hash, area.SnapMaxDistance);
    HashValue(hash, area.IsExclusionVolume);

    for (const XMFLOAT2& point : area.PolygonPoints)
        HashValue(hash, point);

    for (const VegetationLayer& layer : area.Layers)
    {
        HashBytes(hash, layer.MeshPath.data(), layer.MeshPath.size());
        HashBytes(hash, layer.MaterialPath.data(), layer.MaterialPath.size());
        HashValue(hash, layer.Enabled);
        HashValue(hash, layer.Density);
        HashValue(hash, layer.MinScale);
        HashValue(hash, layer.MaxScale);
        HashValue(hash, layer.RandomYaw);
        HashValue(hash, layer.MaxTiltDegrees);
        HashValue(hash, layer.AlignToNormal);
        HashValue(hash, layer.MaxSlopeDegrees);
        HashValue(hash, layer.MinAltitude);
        HashValue(hash, layer.MaxAltitude);
        HashValue(hash, layer.SinkOffset);
        HashValue(hash, layer.TerrainLayerMask);
        HashValue(hash, layer.TerrainLayerThreshold);
        HashValue(hash, layer.CollisionRadius);
    }

    for (const VegetationInstanceOverride& ov : area.Overrides)
    {
        HashValue(hash, ov.InstanceId);
        HashValue(hash, ov.Removed);
        HashValue(hash, ov.HasTransform);
        HashValue(hash, ov.Position);
        HashValue(hash, ov.RotationZ);
        HashValue(hash, ov.Scale);
    }

    return hash;
}

std::vector<VegetationExclusionVolume> VegetationRenderer::GatherExclusionVolumes() const
{
    std::vector<VegetationExclusionVolume> volumes;
    if (mEntities == nullptr)
        return volumes;

    for (const Entity& entity : *mEntities)
    {
        if (!entity.HasVegetationAreaComponent())
            continue;

        const VegetationAreaComponent& area = *entity.VegetationArea;
        if (!area.IsExclusionVolume)
            continue;

        VegetationExclusionVolume volume;
        volume.Shape         = area.Shape;
        volume.ExtentX       = area.ExtentX;
        volume.ExtentY       = area.ExtentY;
        volume.ExtentZ       = area.ExtentZ;
        volume.PolygonPoints = area.PolygonPoints;

        // Same convention as the scatter: rotation and translation only, no
        // scale, so the extents stay literal metres.
        const XMMATRIX localToWorld =
            PteroTransform::ComposeRotation(entity.Transform.Rotation)
            * XMMatrixTranslation(
                entity.Transform.Position.x,
                entity.Transform.Position.y,
                entity.Transform.Position.z);

        volume.WorldToLocal = XMMatrixInverse(nullptr, localToWorld);
        volumes.push_back(std::move(volume));
    }

    return volumes;
}

void VegetationRenderer::StartScatter(std::size_t entityIndex, AreaState& state, const Entity& entity)
{
    const VegetationAreaComponent area = *entity.VegetationArea;   // copied for the worker
    const XMFLOAT3 position = entity.Transform.Position;
    const XMFLOAT3 rotation = entity.Transform.Rotation;

    // Snapshot the terrain once per revision and share it across every job.
    const std::uint64_t revision = (mTerrainRenderer != nullptr)
        ? mTerrainRenderer->GetTerrainRevision()
        : 0;

    if (mTerrainRenderer != nullptr && (!gTerrainSnapshot || gTerrainSnapshotRevision != revision))
    {
        std::vector<TerrainRenderer::PatchData> patches;
        mTerrainRenderer->ExportPatches(patches);
        gTerrainSnapshot = std::make_shared<TerrainSnapshot>(std::move(patches));
        gTerrainSnapshotRevision = revision;
    }

    std::shared_ptr<TerrainSnapshot> terrain = gTerrainSnapshot;

    // The mesh BVH is built here, on the main thread, because it reads the
    // entity list and the loaded mesh assets.  It is then owned solely by the
    // job.
    auto geometry = std::make_shared<GeometryRaycaster>();
    if (area.SnapToGeometry && mEntities != nullptr)
    {
        XMFLOAT3 boundsMin{};
        XMFLOAT3 boundsMax{};
        VegetationScatter::ComputeWorldBounds(area, position, rotation, boundsMin, boundsMax);

        // Extend the search box downward so geometry below the volume can still
        // catch instances dropped from its ceiling.
        boundsMin.z -= area.SnapMaxDistance;
        geometry->Build(*mEntities, boundsMin, boundsMax);
    }

    std::vector<VegetationExclusionVolume> exclusions = GatherExclusionVolumes();

    state.ScatterJob = std::async(
        std::launch::async,
        [area, position, rotation, terrain, geometry, exclusions]()
        {
            return VegetationScatter::Generate(
                area, position, rotation,
                terrain.get(),
                geometry->IsEmpty() ? nullptr : geometry.get(),
                exclusions);
        });

    state.ScatterInProgress = true;
    state.RuleHash        = ComputeRuleHash(area, entity.Transform);
    state.TerrainRevision = revision;
    state.ForceRegenerate = false;
}

void VegetationRenderer::ScheduleStaleScatters()
{
    if (mEntities == nullptr)
        return;

    const std::uint64_t terrainRevision = (mTerrainRenderer != nullptr)
        ? mTerrainRenderer->GetTerrainRevision()
        : 0;

    // Drop state for entities that no longer carry an area.
    for (auto it = mAreas.begin(); it != mAreas.end(); )
    {
        const bool stillValid =
            it->first < mEntities->size() && (*mEntities)[it->first].HasVegetationAreaComponent();

        if (!stillValid && !it->second.ScatterInProgress)
        {
            it = mAreas.erase(it);
            mLayoutDirty = true;
        }
        else
        {
            ++it;
        }
    }

    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& entity = (*mEntities)[i];
        if (!entity.HasVegetationAreaComponent())
            continue;

        AreaState& state = mAreas[i];
        if (state.ScatterInProgress)
            continue;

        const std::uint64_t ruleHash = ComputeRuleHash(*entity.VegetationArea, entity.Transform);

        const bool stale = state.ForceRegenerate
                        || state.RuleHash != ruleHash
                        || state.TerrainRevision != terrainRevision;

        if (stale)
            StartScatter(i, state, entity);
    }
}

void VegetationRenderer::PollScatterJobs()
{
    for (auto& entry : mAreas)
    {
        AreaState& state = entry.second;
        if (!state.ScatterInProgress || !state.ScatterJob.valid())
            continue;

        if (state.ScatterJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            continue;

        VegetationScatterResult result = state.ScatterJob.get();
        state.ScatterInProgress = false;
        state.Stats = result.Stats;

        // Pack into the GPU layout.  Scale folds in the engine's fixed mesh
        // world scale, the same 10x correction TransformComponent applies, so
        // vegetation matches the size of an equivalent placed mesh entity.
        state.PendingUpload.clear();
        state.PendingUpload.reserve(result.Instances.size());
        for (const VegetationInstance& instance : result.Instances)
        {
            GpuInstance gpu;
            gpu.Position  = instance.Position;
            gpu.RotationZ = instance.RotationZ;
            gpu.UpAxis    = instance.UpAxis;
            gpu.Scale     = instance.Scale * TransformComponent::MeshWorldScale;
            state.PendingUpload.push_back(gpu);
        }

        state.InstanceCount = static_cast<std::uint32_t>(result.Instances.size());
        state.HasPendingUpload = true;

        // Keep a CPU copy only when this area feeds the ray tracing scene.
        state.CpuInstances.clear();
        state.CpuInstances.shrink_to_fit();
        if (entry.first < mEntities->size())
        {
            const Entity& rtEntity = (*mEntities)[entry.first];
            if (rtEntity.HasVegetationAreaComponent())
            {
                bool needsCpuCopy = false;
                for (const VegetationLayer& layer : rtEntity.VegetationArea->Layers)
                {
                    if (layer.ContributeToRayTracing)
                    {
                        needsCpuCopy = true;
                        break;
                    }
                }

                if (needsCpuCopy)
                    state.CpuInstances = state.PendingUpload;
            }
        }

        // Rebuild the per-layer ranges from the scatter's layer offsets.
        state.LayerRanges.clear();
        if (entry.first < mEntities->size())
        {
            const Entity& entity = (*mEntities)[entry.first];
            if (entity.HasVegetationAreaComponent())
            {
                const VegetationAreaComponent& area = *entity.VegetationArea;
                for (std::size_t layerIndex = 0; layerIndex + 1 < result.LayerOffsets.size(); ++layerIndex)
                {
                    const std::uint32_t first = result.LayerOffsets[layerIndex];
                    const std::uint32_t count = result.LayerOffsets[layerIndex + 1] - first;
                    if (count == 0 || layerIndex >= area.Layers.size())
                        continue;

                    LayerDrawRange range;
                    range.InstanceFirst        = first;
                    range.InstanceCount        = count;
                    range.MeshPath             = area.Layers[layerIndex].MeshPath;
                    range.MaterialPath         = area.Layers[layerIndex].MaterialPath;
                    range.BillboardTexturePath = area.Layers[layerIndex].BillboardTexturePath;
                    state.LayerRanges.push_back(std::move(range));
                }
            }
        }

        mLayoutDirty = true;
    }
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------

void VegetationRenderer::Update(
    ID3D12GraphicsCommandList* commandList,
    const XMFLOAT3&            cameraPosition,
    float                      deltaSeconds)
{
    if (!mInitialized || mEntities == nullptr)
        return;

    (void)cameraPosition;

    // Keep last frame's clock so the motion vector pass can re-evaluate the
    // bend at exactly the time the previous frame drew it.
    mPreviousTimeSeconds = mTimeSeconds;
    mTimeSeconds += deltaSeconds;

    PollScatterJobs();
    ScheduleStaleScatters();

    // Upload any scatter that finished, and make sure its meshes and materials
    // are resident before the layout is rebuilt around them.
    for (auto& entry : mAreas)
    {
        AreaState& state = entry.second;

        if (state.HasPendingUpload)
        {
            if (EnsureInstanceBuffer(state, state.InstanceCount))
                UploadPendingInstances(commandList, state);
        }

        for (LayerDrawRange& range : state.LayerRanges)
        {
            if (!range.MeshPath.empty())
                EnsureSharedMesh(commandList, range.MeshPath);
            if (!range.MaterialPath.empty())
                ResolveLayerMaterial(range.MaterialPath);

            if (!range.BillboardTexturePath.empty()
                && mBillboardTextures.find(range.BillboardTexturePath) == mBillboardTextures.end())
            {
                const std::filesystem::path resolved = ResolveDataRelativePath(range.BillboardTexturePath);
                // Cached even on failure, so a missing card is not retried
                // every frame; the layer simply culls instead of billboarding.
                mBillboardTextures[range.BillboardTexturePath] = resolved.empty()
                    ? nullptr
                    : mTextureManager.LoadDDS(resolved.string(), TextureSemantic::Color);
            }
        }
    }

    if (mLayoutDirty)
    {
        RebuildDrawLayout();
        mLayoutDirty = false;
    }

    RefreshIndirectArgTemplate();
}

bool VegetationRenderer::EnsureInstanceBuffer(AreaState& state, std::uint32_t instanceCount)
{
    if (instanceCount == 0)
        return true;

    if (state.InstanceCapacity >= instanceCount && state.InstanceBuffer)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Grow with headroom so nudging a density slider does not reallocate on
    // every frame of the drag.
    const std::uint32_t capacity = (std::max)(instanceCount + instanceCount / 4u, 1024u);
    const UINT64 byteSize = static_cast<UINT64>(capacity) * sizeof(GpuInstance);

    // Retiring the old resources here relies on the caller having flushed the
    // GPU; scatter uploads only happen on edits, so a wait is acceptable.
    DX12Context_WaitForGPU();

    state.InstanceBuffer.Reset();
    state.InstanceUpload.Reset();

    if (!CreateCommittedBuffer(device, byteSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, state.InstanceBuffer))
    {
        mLastError = "VegetationRenderer: failed to create the instance buffer.";
        return false;
    }

    if (!CreateCommittedBuffer(device, byteSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, state.InstanceUpload))
    {
        mLastError = "VegetationRenderer: failed to create the instance upload buffer.";
        return false;
    }

    state.InstanceCapacity = capacity;
    return true;
}

void VegetationRenderer::UploadPendingInstances(
    ID3D12GraphicsCommandList* commandList,
    AreaState&                 state)
{
    if (!state.HasPendingUpload || commandList == nullptr)
        return;

    state.HasPendingUpload = false;

    if (state.PendingUpload.empty() || !state.InstanceBuffer || !state.InstanceUpload)
        return;

    const UINT64 byteSize = static_cast<UINT64>(state.PendingUpload.size()) * sizeof(GpuInstance);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(state.InstanceUpload->Map(0, &readRange, &mapped)) || mapped == nullptr)
        return;

    std::memcpy(mapped, state.PendingUpload.data(), static_cast<std::size_t>(byteSize));
    state.InstanceUpload->Unmap(0, nullptr);

    const D3D12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        state.InstanceBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->ResourceBarrier(1, &toCopy);

    commandList->CopyBufferRegion(state.InstanceBuffer.Get(), 0, state.InstanceUpload.Get(), 0, byteSize);

    const D3D12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(
        state.InstanceBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toRead);

    // The staging copy is no longer needed; the next scatter recreates it.
    state.PendingUpload.clear();
    state.PendingUpload.shrink_to_fit();
}

// ---------------------------------------------------------------------------
// Mesh and material residency
// ---------------------------------------------------------------------------

bool VegetationRenderer::EnsureSharedMesh(
    ID3D12GraphicsCommandList* commandList,
    const std::string&         meshPath)
{
    auto it = mSharedMeshes.find(meshPath);
    if (it != mSharedMeshes.end() && it->second.Ready)
        return true;

    if (!mMeshResolver)
        return false;

    SharedMesh& shared = mSharedMeshes[meshPath];
    if (!shared.Asset)
        shared.Asset = mMeshResolver(meshPath);

    if (!shared.Asset)
        return false;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || commandList == nullptr)
        return false;

    const std::size_t lodCount =
        (std::min)(shared.Asset->GetLodCount(), static_cast<std::size_t>(kMaxLodsPerLayer));

    shared.Lods.clear();
    shared.Lods.resize(lodCount);

    float maxRadiusSq = 0.0f;

    for (std::size_t lod = 0; lod < lodCount; ++lod)
    {
        const MeshLod& source = shared.Asset->GetLod(lod);
        if (source.Vertices.empty() || source.Indices.empty())
            continue;

        LodBuffers& buffers = shared.Lods[lod];

        const UINT64 vertexBytes = static_cast<UINT64>(source.Vertices.size()) * sizeof(Vertex);
        const UINT64 indexBytes  = static_cast<UINT64>(source.Indices.size())  * sizeof(std::uint32_t);

        if (!CreateCommittedBuffer(device, vertexBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, buffers.VertexBuffer) ||
            !CreateCommittedBuffer(device, vertexBytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, buffers.VertexUpload) ||
            !CreateCommittedBuffer(device, indexBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, buffers.IndexBuffer) ||
            !CreateCommittedBuffer(device, indexBytes, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, buffers.IndexUpload))
        {
            mLastError = "VegetationRenderer: failed to allocate mesh buffers for " + meshPath;
            return false;
        }

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };

        if (SUCCEEDED(buffers.VertexUpload->Map(0, &readRange, &mapped)) && mapped)
        {
            std::memcpy(mapped, source.Vertices.data(), static_cast<std::size_t>(vertexBytes));
            buffers.VertexUpload->Unmap(0, nullptr);
        }

        if (SUCCEEDED(buffers.IndexUpload->Map(0, &readRange, &mapped)) && mapped)
        {
            std::memcpy(mapped, source.Indices.data(), static_cast<std::size_t>(indexBytes));
            buffers.IndexUpload->Unmap(0, nullptr);
        }

        const D3D12_RESOURCE_BARRIER toCopy[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(buffers.VertexBuffer.Get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(buffers.IndexBuffer.Get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        commandList->ResourceBarrier(2, toCopy);

        commandList->CopyBufferRegion(buffers.VertexBuffer.Get(), 0, buffers.VertexUpload.Get(), 0, vertexBytes);
        commandList->CopyBufferRegion(buffers.IndexBuffer.Get(), 0, buffers.IndexUpload.Get(), 0, indexBytes);

        const D3D12_RESOURCE_BARRIER toRead[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(buffers.VertexBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            CD3DX12_RESOURCE_BARRIER::Transition(buffers.IndexBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
        };
        commandList->ResourceBarrier(2, toRead);

        buffers.VertexBufferView.BufferLocation = buffers.VertexBuffer->GetGPUVirtualAddress();
        buffers.VertexBufferView.SizeInBytes    = static_cast<UINT>(vertexBytes);
        buffers.VertexBufferView.StrideInBytes  = sizeof(Vertex);

        buffers.IndexBufferView.BufferLocation = buffers.IndexBuffer->GetGPUVirtualAddress();
        buffers.IndexBufferView.SizeInBytes    = static_cast<UINT>(indexBytes);
        buffers.IndexBufferView.Format         = DXGI_FORMAT_R32_UINT;

        buffers.IndexCount = static_cast<std::uint32_t>(source.Indices.size());

        // LOD0 defines the bound; coarser LODs are contained within it.
        if (lod == 0)
        {
            float maxHorizontal = 0.0f;
            float minZ =  FLT_MAX;
            float maxZ = -FLT_MAX;

            for (const Vertex& vertex : source.Vertices)
            {
                const float radiusSq =
                    vertex.Position.x * vertex.Position.x +
                    vertex.Position.y * vertex.Position.y +
                    vertex.Position.z * vertex.Position.z;
                maxRadiusSq = (std::max)(maxRadiusSq, radiusSq);

                const float horizontal = std::sqrt(
                    vertex.Position.x * vertex.Position.x +
                    vertex.Position.y * vertex.Position.y);
                maxHorizontal = (std::max)(maxHorizontal, horizontal);

                minZ = (std::min)(minZ, vertex.Position.z);
                maxZ = (std::max)(maxZ, vertex.Position.z);
            }

            // Size the billboard card to the mesh's own silhouette, so
            // swapping to it does not change how much screen space the plant
            // occupies.
            shared.BillboardHalfWidth = (maxHorizontal > 0.0f) ? maxHorizontal : 1.0f;
            shared.BillboardHeight    = (maxZ > minZ) ? (maxZ - minZ) : 2.0f;
        }
    }

    // The cull shader multiplies this by the instance scale, which already
    // carries MeshWorldScale, so the radius stays in raw mesh units here.
    shared.BoundingRadius = (maxRadiusSq > 0.0f) ? std::sqrt(maxRadiusSq) : 1.0f;
    shared.Ready = true;
    mLayoutDirty = true;
    return true;
}

const VegetationRenderer::LayerMaterial& VegetationRenderer::ResolveLayerMaterial(
    const std::string& materialPath)
{
    auto it = mMaterials.find(materialPath);
    if (it != mMaterials.end() && it->second.Resolved)
        return it->second;

    LayerMaterial& material = mMaterials[materialPath];
    material.Resolved = true;

    const std::filesystem::path resolved = ResolveDataRelativePath(materialPath);
    if (resolved.empty())
        return material;

    DataFiles::InputFile stream(resolved);
    if (!stream)
        return material;

    nlohmann::json json;
    try
    {
        stream >> json;
    }
    catch (const std::exception&)
    {
        return material;
    }

    material.AlphaCutoff     = json.value("alphaCutoff", 0.5f);
    material.MetallicFactor  = json.value("metallicFactor", 0.0f);
    material.RoughnessFactor = json.value("roughnessFactor", 1.0f);
    material.NormalScale     = json.value("normalScale", 1.0f);
    material.AoStrength      = json.value("ambientOcclusionStrength", 1.0f);

    const auto tintIt = json.find("baseColorTint");
    if (tintIt != json.end() && tintIt->is_array() && tintIt->size() >= 4)
    {
        material.BaseColorTint = XMFLOAT4(
            (*tintIt)[0].get<float>(), (*tintIt)[1].get<float>(),
            (*tintIt)[2].get<float>(), (*tintIt)[3].get<float>());
    }

    const auto texturesIt = json.find("textures");
    if (texturesIt == json.end() || !texturesIt->is_object())
        return material;

    const auto loadSlot = [&](const char* key, TextureSemantic semantic) -> std::shared_ptr<GpuTexture>
    {
        const auto slotIt = texturesIt->find(key);
        if (slotIt == texturesIt->end() || !slotIt->is_string())
            return nullptr;

        const std::string path = ResolveTextureNearMaterial(resolved, slotIt->get<std::string>());
        if (path.empty())
            return nullptr;

        return mTextureManager.LoadDDS(path, semantic);
    };

    material.BaseColor = loadSlot("baseColor", TextureSemantic::Color);
    material.Normal    = loadSlot("normal",    TextureSemantic::Normal);
    material.Ao        = loadSlot("ambientOcclusion", TextureSemantic::MaterialMask);

    // A combined metallic-roughness map takes priority, matching how the rest
    // of the engine reads these materials.
    if (auto packed = loadSlot("metallicRoughness", TextureSemantic::MaterialMask))
    {
        material.Metallic  = packed;
        material.Roughness = packed;
        material.HasPackedMaterialMap = true;
    }
    else
    {
        material.Metallic  = loadSlot("metallic",  TextureSemantic::MaterialMask);
        material.Roughness = loadSlot("roughness", TextureSemantic::MaterialMask);
    }

    return material;
}

// ---------------------------------------------------------------------------
// Draw layout
// ---------------------------------------------------------------------------

void VegetationRenderer::RebuildDrawLayout()
{
    mFlatLayers.clear();
    mTotalInstanceCount = 0;
    mHasRayTracedLayers = false;

    if (mEntities != nullptr)
    {
        for (const Entity& entity : *mEntities)
        {
            if (!entity.HasVegetationAreaComponent())
                continue;

            for (const VegetationLayer& layer : entity.VegetationArea->Layers)
            {
                if (layer.ContributeToRayTracing)
                {
                    mHasRayTracedLayers = true;
                    break;
                }
            }

            if (mHasRayTracedLayers)
                break;
        }
    }

    std::uint32_t argSlot     = 0;
    std::uint32_t visibleBase = 0;

    for (auto& entry : mAreas)
    {
        AreaState& state = entry.second;

        for (std::uint32_t layerIndex = 0; layerIndex < state.LayerRanges.size(); ++layerIndex)
        {
            LayerDrawRange& range = state.LayerRanges[layerIndex];

            const auto meshIt = mSharedMeshes.find(range.MeshPath);
            if (meshIt == mSharedMeshes.end() || !meshIt->second.Ready || meshIt->second.Lods.empty())
                continue;

            // The billboard, when present, occupies the last LOD slot.  Mesh
            // LODs give up a slot to make room rather than raising the cap,
            // because the cull constants pack the per-LOD tables as fixed
            // four-wide vectors.
            range.HasBillboard = !range.BillboardTexturePath.empty();

            const std::uint32_t maxMeshLods =
                kMaxLodsPerLayer - (range.HasBillboard ? 1u : 0u);
            const std::uint32_t meshLodCount = (std::min)(
                static_cast<std::uint32_t>(meshIt->second.Lods.size()), maxMeshLods);

            range.LodCount = meshLodCount + (range.HasBillboard ? 1u : 0u);
            range.ArgSlot  = argSlot;
            // Each LOD gets a worst-case slice, since any instance could land
            // in any LOD depending on where the camera is.
            range.VisibleBase = visibleBase;

            argSlot     += range.LodCount;
            visibleBase += range.InstanceCount * range.LodCount;

            mTotalInstanceCount += range.InstanceCount;

            mFlatLayers.push_back(FlatLayer{ entry.first, layerIndex });
        }
    }

    mIndirectArgSlotCount = argSlot;

    EnsureCullResources(visibleBase, argSlot);
    EnsureConstantBuffers(mFlatLayers.size());
}

bool VegetationRenderer::EnsureCullResources(std::uint32_t visibleCapacity, std::uint32_t drawSlots)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    if (visibleCapacity > mVisibleIndexCapacity)
    {
        DX12Context_WaitForGPU();

        const std::uint32_t capacity = (std::max)(visibleCapacity + visibleCapacity / 4u, 4096u);
        const UINT64 byteSize = static_cast<UINT64>(capacity) * sizeof(std::uint32_t);

        mVisibleIndexBuffer.Reset();
        // Created in the state DispatchCull expects to find it in, so the
        // barrier sequence is identical on the first frame and every frame
        // after a reallocation.
        if (!CreateCommittedBuffer(device, byteSize, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, mVisibleIndexBuffer))
        {
            mLastError = "VegetationRenderer: failed to create the visible index buffer.";
            return false;
        }

        mVisibleIndexCapacity = capacity;
    }

    if (drawSlots > mIndirectArgSlotCapacity)
    {
        DX12Context_WaitForGPU();

        const std::uint32_t capacity = (std::max)(drawSlots + 8u, 32u);
        const UINT64 byteSize = static_cast<UINT64>(capacity) * kDrawArgStride;

        if (mIndirectArgResetPtr && mIndirectArgReset)
        {
            mIndirectArgReset->Unmap(0, nullptr);
            mIndirectArgResetPtr = nullptr;
        }

        mIndirectArgBuffer.Reset();
        mIndirectArgReset.Reset();

        if (!CreateCommittedBuffer(device, byteSize, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, mIndirectArgBuffer) ||
            !CreateCommittedBuffer(device, byteSize, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                D3D12_RESOURCE_FLAG_NONE, mIndirectArgReset))
        {
            mLastError = "VegetationRenderer: failed to create the indirect argument buffers.";
            return false;
        }

        const D3D12_RANGE readRange{ 0, 0 };
        if (FAILED(mIndirectArgReset->Map(0, &readRange, &mIndirectArgResetPtr)))
        {
            mLastError = "VegetationRenderer: failed to map the indirect argument template.";
            return false;
        }

        std::memset(mIndirectArgResetPtr, 0, static_cast<std::size_t>(byteSize));
        mIndirectArgSlotCapacity = capacity;
    }

    return true;
}

void VegetationRenderer::RefreshIndirectArgTemplate()
{
    if (mIndirectArgResetPtr == nullptr)
        return;

    // Rewrite the static half of every draw argument.  InstanceCount stays at
    // zero: the cull shader accumulates into it, and copying this template
    // over the live buffer each frame is what resets the counters.
    auto* args = static_cast<D3D12_DRAW_INDEXED_ARGUMENTS*>(mIndirectArgResetPtr);

    for (const FlatLayer& flat : mFlatLayers)
    {
        const auto areaIt = mAreas.find(flat.EntityIndex);
        if (areaIt == mAreas.end() || flat.LayerIndex >= areaIt->second.LayerRanges.size())
            continue;

        const LayerDrawRange& range = areaIt->second.LayerRanges[flat.LayerIndex];

        const auto meshIt = mSharedMeshes.find(range.MeshPath);
        if (meshIt == mSharedMeshes.end() || !meshIt->second.Ready)
            continue;

        const std::uint32_t meshLodCount = range.LodCount - (range.HasBillboard ? 1u : 0u);

        for (std::uint32_t lod = 0; lod < range.LodCount; ++lod)
        {
            const std::uint32_t slot = range.ArgSlot + lod;
            if (slot >= mIndirectArgSlotCapacity)
                break;

            const bool isBillboard = range.HasBillboard && (lod == meshLodCount);

            args[slot].IndexCountPerInstance = isBillboard
                ? kBillboardIndexCount
                : ((lod < meshIt->second.Lods.size()) ? meshIt->second.Lods[lod].IndexCount : 0);
            args[slot].InstanceCount         = 0;
            args[slot].StartIndexLocation    = 0;
            args[slot].BaseVertexLocation    = 0;
            args[slot].StartInstanceLocation = 0;
        }
    }
}

bool VegetationRenderer::EnsureConstantBuffers(std::size_t layerDrawCount)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const D3D12_RANGE readRange{ 0, 0 };

    if (!mPassCb)
    {
        // One slot per pass, for the same reason the layer constants are
        // partitioned: all three passes go into a single command list.
        if (!CreateCommittedBuffer(device, sizeof(PassConstants) * kPassCount, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mPassCb))
        {
            mLastError = "VegetationRenderer: failed to create the pass constant buffer.";
            return false;
        }

        if (FAILED(mPassCb->Map(0, &readRange, reinterpret_cast<void**>(&mMappedPassCb))))
            return false;
    }

    if (!mInteractionCb)
    {
        if (!CreateCommittedBuffer(device, sizeof(InteractionConstants), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mInteractionCb))
        {
            mLastError = "VegetationRenderer: failed to create the interaction constant buffer.";
            return false;
        }

        if (FAILED(mInteractionCb->Map(0, &readRange, reinterpret_cast<void**>(&mMappedInteractionCb))))
            return false;
    }

    const std::size_t required = (std::max)(layerDrawCount, static_cast<std::size_t>(16));

    // Every (pass, layer, LOD) triple needs its own slot.
    const std::size_t layerSlots = required * kMaxLodsPerLayer * kPassCount;
    mLayerCbStride = required;

    if (mLayerCbCapacity < layerSlots)
    {
        DX12Context_WaitForGPU();

        if (mMappedLayerCb && mLayerCb)
        {
            mLayerCb->Unmap(0, nullptr);
            mMappedLayerCb = nullptr;
        }
        mLayerCb.Reset();

        if (!CreateCommittedBuffer(device, layerSlots * sizeof(LayerConstants), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mLayerCb))
        {
            mLastError = "VegetationRenderer: failed to create the layer constant buffer.";
            return false;
        }

        if (FAILED(mLayerCb->Map(0, &readRange, reinterpret_cast<void**>(&mMappedLayerCb))))
            return false;

        mLayerCbCapacity = layerSlots;
    }

    if (mCullCbCapacity < required)
    {
        DX12Context_WaitForGPU();

        if (mMappedCullCb && mCullCb)
        {
            mCullCb->Unmap(0, nullptr);
            mMappedCullCb = nullptr;
        }
        mCullCb.Reset();

        if (!CreateCommittedBuffer(device, required * sizeof(CullConstants), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mCullCb))
        {
            mLastError = "VegetationRenderer: failed to create the cull constant buffer.";
            return false;
        }

        if (FAILED(mCullCb->Map(0, &readRange, reinterpret_cast<void**>(&mMappedCullCb))))
            return false;

        mCullCbCapacity = required;
    }

    return true;
}

void VegetationRenderer::FillLayerConstants(
    const VegetationLayer& layer,
    const LayerMaterial&   material,
    const SharedMesh&      mesh,
    LayerConstants&        outConstants)
{
    outConstants.BaseColorTint        = material.BaseColorTint;
    outConstants.AlphaCutoff          = material.AlphaCutoff;
    outConstants.RoughnessFactor      = material.RoughnessFactor;
    outConstants.MetallicFactor       = material.MetallicFactor;
    outConstants.NormalScale          = material.NormalScale;
    outConstants.AoStrength           = material.AoStrength;
    // Leaves transmit light, trunks and rocks do not, so only the layers
    // authored as foliage get the backlit lift.
    outConstants.Translucency         = (layer.BendModel == VegetationBendModel::None) ? 0.0f : 0.35f;
    outConstants.WindInfluence        = layer.WindInfluence;
    outConstants.Stiffness            = layer.Stiffness;
    outConstants.FlutterAmount        = layer.FlutterAmount;
    outConstants.InteractionInfluence = layer.InteractionInfluence;
    outConstants.BendModel            = static_cast<int>(layer.BendModel);
    outConstants.HasNormalMap         = material.Normal    ? 1 : 0;
    outConstants.HasMetallicMap       = material.Metallic  ? 1 : 0;
    outConstants.HasRoughnessMap      = material.Roughness ? 1 : 0;
    outConstants.HasAoMap             = material.Ao        ? 1 : 0;
    outConstants.HasPackedMaterialMap = material.HasPackedMaterialMap ? 1 : 0;
    outConstants.BillboardHalfWidth   = mesh.BillboardHalfWidth * layer.BillboardScale;
    outConstants.BillboardHeight      = mesh.BillboardHeight    * layer.BillboardScale;
    outConstants.InstanceOffset       = 0;
}

void VegetationRenderer::FillPassConstants(
    const XMMATRIX& viewProjection,
    const XMFLOAT3& cameraPosition,
    PassConstants&  outConstants) const
{
    XMStoreFloat4x4(&outConstants.ViewProj, XMMatrixTranspose(viewProjection));
    outConstants.CameraPosition = cameraPosition;
    outConstants.Time = mTimeSeconds;

    const WindSettings defaultWind{};
    const WindSettings& wind = (mWindSettings != nullptr) ? *mWindSettings : defaultWind;

    outConstants.WindParams0 = XMFLOAT4(
        wind.DirectionRadians, wind.Strength, wind.GustAmplitude, wind.GustFrequency);
    outConstants.WindParams1 = XMFLOAT4(
        wind.GustWavelength, wind.VegetationBendScale, wind.FlutterFrequency,
        wind.Enabled ? 1.0f : 0.0f);
    outConstants.InteractionParams = XMFLOAT4(
        mInteractionCentre.x, mInteractionCentre.y, kInteractionWorldSize,
        mPreviousTimeSeconds);

    // Overwritten by the motion vector pass, which is the only consumer.
    XMStoreFloat4x4(&outConstants.PrevViewProj, XMMatrixIdentity());
}

void VegetationRenderer::CollectRayTracingBatches(
    const XMFLOAT3&               cameraPosition,
    float                         maxDistance,
    std::vector<RayTracingBatch>& outBatches) const
{
    outBatches.clear();

    if (!mHasRayTracedLayers || mEntities == nullptr)
        return;

    const float maxDistanceSq = maxDistance * maxDistance;

    for (const auto& entry : mAreas)
    {
        const AreaState& state = entry.second;
        if (state.CpuInstances.empty() || entry.first >= mEntities->size())
            continue;

        const Entity& entity = (*mEntities)[entry.first];
        if (!entity.HasVegetationAreaComponent())
            continue;

        const VegetationAreaComponent& area = *entity.VegetationArea;

        for (std::size_t layerIndex = 0; layerIndex < state.LayerRanges.size(); ++layerIndex)
        {
            if (layerIndex >= area.Layers.size())
                continue;

            const VegetationLayer& layer = area.Layers[layerIndex];
            if (!layer.ContributeToRayTracing)
                continue;

            const LayerDrawRange& range = state.LayerRanges[layerIndex];
            if (range.InstanceCount == 0)
                continue;

            const auto meshIt = mSharedMeshes.find(range.MeshPath);
            if (meshIt == mSharedMeshes.end() || !meshIt->second.Asset)
                continue;

            RayTracingBatch batch;
            batch.MeshAsset    = meshIt->second.Asset.get();
            batch.MaterialPath = range.MaterialPath;

            const std::uint32_t last =
                (std::min)(range.InstanceFirst + range.InstanceCount,
                           static_cast<std::uint32_t>(state.CpuInstances.size()));

            for (std::uint32_t i = range.InstanceFirst; i < last; ++i)
            {
                const GpuInstance& instance = state.CpuInstances[i];

                const float dx = instance.Position.x - cameraPosition.x;
                const float dy = instance.Position.y - cameraPosition.y;
                const float dz = instance.Position.z - cameraPosition.z;
                if ((dx * dx + dy * dy + dz * dz) > maxDistanceSq)
                    continue;

                // Rebuild the same basis the vertex shader uses, so the traced
                // geometry sits exactly where the rasterised geometry does.
                // Deliberately unbent: the acceleration structure is rebuilt
                // per frame but re-fitting it to the wind every frame would
                // cost far more than the lighting difference is worth.
                const XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&instance.UpAxis));
                const XMVECTOR reference = (std::fabs(instance.UpAxis.z) > 0.999f)
                    ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
                    : XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

                const XMVECTOR right   = XMVector3Normalize(XMVector3Cross(reference, up));
                const XMVECTOR forward = XMVector3Cross(up, right);

                const float sinYaw = std::sin(instance.RotationZ);
                const float cosYaw = std::cos(instance.RotationZ);

                const XMVECTOR rotatedRight =
                    XMVectorAdd(XMVectorScale(right, cosYaw), XMVectorScale(forward, sinYaw));
                const XMVECTOR rotatedForward =
                    XMVectorSubtract(XMVectorScale(forward, cosYaw), XMVectorScale(right, sinYaw));

                XMMATRIX world = XMMatrixIdentity();
                world.r[0] = XMVectorScale(rotatedRight,   instance.Scale);
                world.r[1] = XMVectorScale(rotatedForward, instance.Scale);
                world.r[2] = XMVectorScale(up,             instance.Scale);
                world.r[3] = XMVectorSet(
                    instance.Position.x, instance.Position.y, instance.Position.z, 1.0f);

                XMFLOAT4X4 transform{};
                XMStoreFloat4x4(&transform, world);
                batch.Transforms.push_back(transform);
            }

            if (!batch.Transforms.empty())
                outBatches.push_back(std::move(batch));
        }
    }
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------

bool VegetationRenderer::CreateFallbackTexture(ID3D12GraphicsCommandList* commandList)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || commandList == nullptr)
        return false;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width            = 1;
    textureDesc.Height           = 1;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels        = 1;
    textureDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mFallbackTextureResource))))
    {
        mLastError = "VegetationRenderer: failed to create the fallback texture.";
        return false;
    }

    const UINT64 uploadSize = GetRequiredIntermediateSize(mFallbackTextureResource.Get(), 0, 1);
    if (!CreateCommittedBuffer(device, uploadSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mFallbackUploadBuffer))
    {
        return false;
    }

    D3D12_SUBRESOURCE_DATA subresource{};
    subresource.pData      = kWhitePixel;
    subresource.RowPitch   = sizeof(kWhitePixel);
    subresource.SlicePitch = sizeof(kWhitePixel);

    UpdateSubresources(commandList, mFallbackTextureResource.Get(),
        mFallbackUploadBuffer.Get(), 0, 0, 1, &subresource);

    const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        mFallbackTextureResource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);

    if (!DX12Context_AllocateSrvDescriptor(&mFallbackCpuHandle, &mFallbackGpuHandle))
    {
        mLastError = "VegetationRenderer: failed to allocate the fallback texture descriptor.";
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(mFallbackTextureResource.Get(), &srvDesc, mFallbackCpuHandle);

    return true;
}

bool VegetationRenderer::CreateBillboardQuad(ID3D12GraphicsCommandList* commandList)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || commandList == nullptr)
        return false;

    // A unit card in the XY plane: X spans -0.5..0.5 and Y spans 0..1, so Y
    // doubles as normalised height above the base and can drive the trunk bend
    // the same way vertex-colour alpha does on the mesh path.
    const Vertex quadVertices[4] =
    {
        { { -0.5f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f }, { 1.f, 1.f, 1.f, 0.0f } },
        { {  0.5f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f }, { 1.f, 1.f, 1.f, 0.0f } },
        { {  0.5f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f }, { 1.f, 1.f, 1.f, 1.0f } },
        { { -0.5f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f }, { 1.f, 1.f, 1.f, 1.0f } },
    };

    const std::uint32_t quadIndices[kBillboardIndexCount] = { 0, 1, 2, 0, 2, 3 };

    const UINT64 vertexBytes = sizeof(quadVertices);
    const UINT64 indexBytes  = sizeof(quadIndices);

    if (!CreateCommittedBuffer(device, vertexBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, mBillboardVertexBuffer) ||
        !CreateCommittedBuffer(device, vertexBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mBillboardVertexUpload) ||
        !CreateCommittedBuffer(device, indexBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, mBillboardIndexBuffer) ||
        !CreateCommittedBuffer(device, indexBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mBillboardIndexUpload))
    {
        mLastError = "VegetationRenderer: failed to create the billboard quad buffers.";
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, 0 };

    if (SUCCEEDED(mBillboardVertexUpload->Map(0, &readRange, &mapped)) && mapped)
    {
        std::memcpy(mapped, quadVertices, static_cast<std::size_t>(vertexBytes));
        mBillboardVertexUpload->Unmap(0, nullptr);
    }

    if (SUCCEEDED(mBillboardIndexUpload->Map(0, &readRange, &mapped)) && mapped)
    {
        std::memcpy(mapped, quadIndices, static_cast<std::size_t>(indexBytes));
        mBillboardIndexUpload->Unmap(0, nullptr);
    }

    const D3D12_RESOURCE_BARRIER toCopy[2] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(mBillboardVertexBuffer.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        CD3DX12_RESOURCE_BARRIER::Transition(mBillboardIndexBuffer.Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    commandList->ResourceBarrier(2, toCopy);

    commandList->CopyBufferRegion(mBillboardVertexBuffer.Get(), 0, mBillboardVertexUpload.Get(), 0, vertexBytes);
    commandList->CopyBufferRegion(mBillboardIndexBuffer.Get(), 0, mBillboardIndexUpload.Get(), 0, indexBytes);

    const D3D12_RESOURCE_BARRIER toRead[2] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(mBillboardVertexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        CD3DX12_RESOURCE_BARRIER::Transition(mBillboardIndexBuffer.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    commandList->ResourceBarrier(2, toRead);

    mBillboardVertexView.BufferLocation = mBillboardVertexBuffer->GetGPUVirtualAddress();
    mBillboardVertexView.SizeInBytes    = static_cast<UINT>(vertexBytes);
    mBillboardVertexView.StrideInBytes  = sizeof(Vertex);

    mBillboardIndexView.BufferLocation = mBillboardIndexBuffer->GetGPUVirtualAddress();
    mBillboardIndexView.SizeInBytes    = static_cast<UINT>(indexBytes);
    mBillboardIndexView.Format         = DXGI_FORMAT_R32_UINT;

    return true;
}

bool VegetationRenderer::CreateBillboardPipeline(
    DXGI_FORMAT albedoFormat,
    DXGI_FORMAT normalFormat,
    DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat,
    UINT        msaaSampleCount)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || !mGraphicsRootSignature)
        return false;

    const ShaderCompileRequest vsRequest{ L"Shaders\\VegetationBillboard.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    const ShaderCompileRequest psRequest{ L"Shaders\\VegetationBillboard.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };

    if (!mBillboardVertexShader.Compile(vsRequest) || !mBillboardPixelShader.Compile(psRequest))
    {
        mLastError = std::string("Vegetation billboard shader compile failed: ")
            + (mBillboardVertexShader.GetLastErrorMessage()
                ? mBillboardVertexShader.GetLastErrorMessage()
                : (mBillboardPixelShader.GetLastErrorMessage() ? mBillboardPixelShader.GetLastErrorMessage() : "unknown"));
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mGraphicsRootSignature.Get();
    psoDesc.VS                    = mBillboardVertexShader.GetBytecode();
    psoDesc.PS                    = mBillboardPixelShader.GetBytecode();
    psoDesc.InputLayout           = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 3;
    psoDesc.RTVFormats[0]         = albedoFormat;
    psoDesc.RTVFormats[1]         = normalFormat;
    psoDesc.RTVFormats[2]         = materialFormat;
    psoDesc.DSVFormat             = depthFormat;
    psoDesc.SampleDesc.Count      = msaaSampleCount;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.SrcBlend              = D3D12_BLEND_ONE;
    rtBlend.DestBlend             = D3D12_BLEND_ZERO;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (UINT i = 0; i < 3; ++i)
        psoDesc.BlendState.RenderTarget[i] = rtBlend;

    psoDesc.BlendState.AlphaToCoverageEnable = (msaaSampleCount > 1);

    // The card is built facing the camera, so which side is "front" depends on
    // where the camera is; culling either face would make it vanish.
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mBillboardPipelineState))))
    {
        mLastError = "VegetationRenderer: billboard CreateGraphicsPipelineState failed.";
        return false;
    }

    mBillboardPipelineReady = true;
    return true;
}

bool VegetationRenderer::CreateCommandSignature()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // The signature carries only the draw itself.  Everything that varies per
    // layer (root constants, textures, buffer bindings) is set on the command
    // list before each ExecuteIndirect, which keeps the signature valid across
    // every layer without needing a root-signature-aware signature.
    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{};
    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
    signatureDesc.ByteStride       = kDrawArgStride;
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs   = &argumentDesc;

    if (FAILED(device->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&mCommandSignature))))
    {
        mLastError = "VegetationRenderer: CreateCommandSignature failed.";
        return false;
    }

    return true;
}

bool VegetationRenderer::CreateGraphicsPipeline(
    DXGI_FORMAT albedoFormat,
    DXGI_FORMAT normalFormat,
    DXGI_FORMAT materialFormat,
    DXGI_FORMAT depthFormat,
    UINT        msaaSampleCount)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const ShaderCompileRequest vsRequest{ L"Shaders\\Vegetation.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    const ShaderCompileRequest psRequest{ L"Shaders\\Vegetation.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };

    if (!mVertexShader.Compile(vsRequest))
    {
        mLastError = std::string("Vegetation VS compile failed: ")
            + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    if (!mPixelShader.Compile(psRequest))
    {
        mLastError = std::string("Vegetation PS compile failed: ")
            + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    if (!mGraphicsRootSignature)
    {
        // Root layout:
        //   0      CBV b0  - per-frame pass constants
        //   1      CBV b1  - per-layer constants
        //   2      SRV t6  - instance buffer, as a root descriptor so each area
        //                    can bind its own without allocating a descriptor
        //   3      SRV t7  - compacted visible index list
        //   4..8   tables t0..t4 - material textures
        //   9      table  t5     - interaction map, read in the vertex shader
        //
        // The instance buffers sit at t6/t7 in register space 0 rather than in
        // a separate space, because register spaces require shader model 5.1
        // and the startup shader cache warmup compiles every VSMain/PSMain at
        // 5.0.  Keeping to one space lets these shaders be precompiled with
        // everything else instead of stalling on first draw.
        D3D12_ROOT_PARAMETER rootParams[10]{};

        rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[1].Descriptor.ShaderRegister = 1;
        rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[2].Descriptor.ShaderRegister = 6;
        rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        rootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[3].Descriptor.ShaderRegister = 7;
        rootParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        // Single-descriptor tables, one per slot, so each texture can be bound
        // straight from its TextureManager handle without needing a contiguous
        // block per material.  Same approach as EntityMeshRenderer.
        D3D12_DESCRIPTOR_RANGE srvRanges[6]{};
        for (int i = 0; i < 6; ++i)
        {
            srvRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[i].NumDescriptors                    = 1;
            srvRanges[i].BaseShaderRegister                = static_cast<UINT>(i);
            srvRanges[i].RegisterSpace                     = 0;
            srvRanges[i].OffsetInDescriptorsFromTableStart = 0;

            rootParams[4 + i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParams[4 + i].DescriptorTable.NumDescriptorRanges = 1;
            rootParams[4 + i].DescriptorTable.pDescriptorRanges   = &srvRanges[i];
            // t5 is the interaction map and is sampled in the vertex shader;
            // t0..t4 are material textures used in the pixel shader.
            rootParams[4 + i].ShaderVisibility = (i == 5)
                ? D3D12_SHADER_VISIBILITY_VERTEX
                : D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_STATIC_SAMPLER_DESC samplers[2]{};

        samplers[0].Filter           = D3D12_FILTER_ANISOTROPIC;
        samplers[0].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].MaxAnisotropy    = 8;
        samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[0].MinLOD           = 0.0f;
        samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister   = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        samplers[1].Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[1].AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].MaxAnisotropy    = 1;
        samplers[1].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[1].MinLOD           = 0.0f;
        samplers[1].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister   = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = static_cast<UINT>(std::size(rootParams));
        rsDesc.pParameters       = rootParams;
        rsDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
        rsDesc.pStaticSamplers   = samplers;
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
        {
            mLastError = "VegetationRenderer: D3D12SerializeRootSignature failed.";
            if (errors)
                mLastError += std::string(" ") + static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }

        if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&mGraphicsRootSignature))))
        {
            mLastError = "VegetationRenderer: CreateRootSignature failed.";
            return false;
        }
    }

    // Matches System/Mesh.h :: Vertex, the same layout every other mesh pass
    // uses.  Instance data comes from a structured buffer, not a second stream.
    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mGraphicsRootSignature.Get();
    psoDesc.VS                    = mVertexShader.GetBytecode();
    psoDesc.PS                    = mPixelShader.GetBytecode();
    psoDesc.InputLayout           = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 3;
    psoDesc.RTVFormats[0]         = albedoFormat;
    psoDesc.RTVFormats[1]         = normalFormat;
    psoDesc.RTVFormats[2]         = materialFormat;
    psoDesc.DSVFormat             = depthFormat;
    psoDesc.SampleDesc.Count      = msaaSampleCount;
    psoDesc.SampleDesc.Quality    = 0;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Opaque blend for all three G-Buffer outputs, matching EntityMeshRenderer.
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

    // Alpha-to-coverage turns the alpha test into a soft edge under MSAA,
    // which matters more for foliage than for anything else in the scene:
    // hard-clipped leaf silhouettes are the classic source of shimmer.
    psoDesc.BlendState.AlphaToCoverageEnable = (msaaSampleCount > 1);

    // Foliage is built from flat cards, so both faces have to draw or every
    // leaf would be invisible from one side.  The pixel shader flips the
    // normal on backfaces to compensate.
    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FillMode              = mWireframeEnabled
        ? D3D12_FILL_MODE_WIREFRAME
        : D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGraphicsPipelineState))))
    {
        mLastError = "VegetationRenderer: CreateGraphicsPipelineState failed.";
        return false;
    }

    mAlbedoFormat    = albedoFormat;
    mNormalFormat    = normalFormat;
    mMaterialFormat  = materialFormat;
    mDepthFormat     = depthFormat;
    mMsaaSampleCount = msaaSampleCount;
    mPipelineReady   = true;
    return true;
}

bool VegetationRenderer::CreateShadowPipeline(DXGI_FORMAT depthFormat)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || !mGraphicsRootSignature)
        return false;

    const ShaderCompileRequest vsRequest{ L"Shaders\\VegetationShadow.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    const ShaderCompileRequest psRequest{ L"Shaders\\VegetationShadow.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };

    if (!mShadowVertexShader.Compile(vsRequest) || !mShadowPixelShader.Compile(psRequest))
    {
        mLastError = std::string("Vegetation shadow shader compile failed: ")
            + (mShadowVertexShader.GetLastErrorMessage()
                ? mShadowVertexShader.GetLastErrorMessage()
                : (mShadowPixelShader.GetLastErrorMessage() ? mShadowPixelShader.GetLastErrorMessage() : "unknown"));
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mGraphicsRootSignature.Get();
    psoDesc.VS                    = mShadowVertexShader.GetBytecode();
    // The pixel shader exists only to run the alpha-test clip; without it the
    // shadow of a leaf card would be a solid rectangle.
    psoDesc.PS                    = mShadowPixelShader.GetBytecode();
    psoDesc.InputLayout           = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 0;
    psoDesc.DSVFormat             = depthFormat;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    psoDesc.DepthStencilState.DepthEnable    = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mShadowPipelineState))))
    {
        mLastError = "VegetationRenderer: shadow CreateGraphicsPipelineState failed.";
        return false;
    }

    mShadowDepthFormat   = depthFormat;
    mShadowPipelineReady = true;
    return true;
}

bool VegetationRenderer::CreateMotionPipeline(DXGI_FORMAT targetFormat, DXGI_FORMAT depthFormat)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || !mGraphicsRootSignature)
        return false;

    const ShaderCompileRequest vsRequest{ L"Shaders\\VegetationMotion.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    const ShaderCompileRequest psRequest{ L"Shaders\\VegetationMotion.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };

    if (!mMotionVertexShader.Compile(vsRequest) || !mMotionPixelShader.Compile(psRequest))
    {
        mLastError = std::string("Vegetation motion shader compile failed: ")
            + (mMotionVertexShader.GetLastErrorMessage()
                ? mMotionVertexShader.GetLastErrorMessage()
                : (mMotionPixelShader.GetLastErrorMessage() ? mMotionPixelShader.GetLastErrorMessage() : "unknown"));
        return false;
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mGraphicsRootSignature.Get();
    psoDesc.VS                    = mMotionVertexShader.GetBytecode();
    psoDesc.PS                    = mMotionPixelShader.GetBytecode();
    psoDesc.InputLayout           = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = targetFormat;
    psoDesc.DSVFormat             = depthFormat;
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.SrcBlend              = D3D12_BLEND_ONE;
    rtBlend.DestBlend             = D3D12_BLEND_ZERO;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = rtBlend;

    psoDesc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable       = TRUE;

    // Depth test against the already-populated depth buffer, but no writes:
    // the geometry pass owns depth, and this pass only needs to know which
    // foliage pixels survived.
    psoDesc.DepthStencilState.DepthEnable    = (depthFormat != DXGI_FORMAT_UNKNOWN);
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mMotionPipelineState))))
    {
        mLastError = "VegetationRenderer: motion CreateGraphicsPipelineState failed.";
        return false;
    }

    mMotionTargetFormat  = targetFormat;
    mMotionDepthFormat   = depthFormat;
    mMotionPipelineReady = true;
    return true;
}

bool VegetationRenderer::CreateCullPipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const ShaderCompileRequest request{ L"Shaders\\VegetationCull.hlsl", L"CSMain", L"cs_5_0", ShaderStage::Compute };
    if (!mCullShader.Compile(request))
    {
        mLastError = std::string("Vegetation cull CS compile failed: ")
            + (mCullShader.GetLastErrorMessage() ? mCullShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    D3D12_ROOT_PARAMETER rootParams[4]{};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[2].Descriptor.ShaderRegister = 0;
    rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParams[3].Descriptor.ShaderRegister = 1;
    rootParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = static_cast<UINT>(std::size(rootParams));
    rsDesc.pParameters   = rootParams;
    rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "VegetationRenderer: cull D3D12SerializeRootSignature failed.";
        return false;
    }

    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&mCullRootSignature))))
    {
        mLastError = "VegetationRenderer: cull CreateRootSignature failed.";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mCullRootSignature.Get();
    psoDesc.CS             = mCullShader.GetBytecode();

    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mCullPipelineState))))
    {
        mLastError = "VegetationRenderer: cull CreateComputePipelineState failed.";
        return false;
    }

    return true;
}

bool VegetationRenderer::CreateInteractionPipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const ShaderCompileRequest request{ L"Shaders\\VegetationInteraction.hlsl", L"CSMain", L"cs_5_0", ShaderStage::Compute };
    if (!mInteractionShader.Compile(request))
    {
        mLastError = std::string("Vegetation interaction CS compile failed: ")
            + (mInteractionShader.GetLastErrorMessage() ? mInteractionShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE prevRange{};
    prevRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    prevRange.NumDescriptors     = 1;
    prevRange.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE outputRange{};
    outputRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors     = 1;
    outputRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER rootParams[5]{};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;

    rootParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges   = &prevRange;

    rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[2].Descriptor.ShaderRegister = 1;

    rootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[3].Descriptor.ShaderRegister = 2;

    rootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges   = &outputRange;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy  = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD         = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = static_cast<UINT>(std::size(rootParams));
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers   = &sampler;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "VegetationRenderer: interaction D3D12SerializeRootSignature failed.";
        return false;
    }

    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&mInteractionRootSignature))))
    {
        mLastError = "VegetationRenderer: interaction CreateRootSignature failed.";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mInteractionRootSignature.Get();
    psoDesc.CS             = mInteractionShader.GetBytecode();

    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mInteractionPipelineState))))
    {
        mLastError = "VegetationRenderer: interaction CreateComputePipelineState failed.";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Interaction map
// ---------------------------------------------------------------------------

bool VegetationRenderer::EnsureInteractionResources()
{
    if (mInteractionMap[0] && mInteractionMap[1])
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width            = kInteractionMapSize;
    textureDesc.Height           = kInteractionMapSize;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels        = 1;
    textureDesc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mInteractionMap[i]))))
        {
            mLastError = "VegetationRenderer: failed to create the interaction map.";
            return false;
        }

        mInteractionState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        if (!DX12Context_AllocateSrvDescriptor(&mInteractionSrvCpu[i], &mInteractionSrvGpu[i]) ||
            !DX12Context_AllocateSrvDescriptor(&mInteractionUavCpu[i], &mInteractionUavGpu[i]))
        {
            mLastError = "VegetationRenderer: failed to allocate interaction map descriptors.";
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels     = 1;
        device->CreateShaderResourceView(mInteractionMap[i].Get(), &srvDesc, mInteractionSrvCpu[i]);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(mInteractionMap[i].Get(), nullptr, &uavDesc, mInteractionUavCpu[i]);
    }

    // Interactor lists live in UPLOAD memory and are rewritten every frame;
    // there are at most kMaxInteractors of them, so a copy to VRAM would cost
    // more than the read.
    const UINT64 listBytes = static_cast<UINT64>(kMaxInteractors) * sizeof(XMFLOAT4);

    if (!CreateCommittedBuffer(device, listBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mInteractorBuffer) ||
        !CreateCommittedBuffer(device, listBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, mInteractorVelocityBuffer))
    {
        mLastError = "VegetationRenderer: failed to create the interactor buffers.";
        return false;
    }

    const D3D12_RANGE readRange{ 0, 0 };
    if (FAILED(mInteractorBuffer->Map(0, &readRange, &mInteractorBufferPtr)) ||
        FAILED(mInteractorVelocityBuffer->Map(0, &readRange, &mInteractorVelocityBufferPtr)))
    {
        mLastError = "VegetationRenderer: failed to map the interactor buffers.";
        return false;
    }

    std::memset(mInteractorBufferPtr, 0, static_cast<std::size_t>(listBytes));
    std::memset(mInteractorVelocityBufferPtr, 0, static_cast<std::size_t>(listBytes));

    return true;
}

void VegetationRenderer::SubmitInteractor(const VegetationInteractor& interactor)
{
    if (mPendingInteractors.size() < kMaxInteractors)
        mPendingInteractors.push_back(interactor);
}

void VegetationRenderer::DispatchInteraction(
    ID3D12GraphicsCommandList* commandList,
    const XMFLOAT3&            cameraPosition,
    float                      deltaSeconds)
{
    if (!mInitialized || commandList == nullptr || !mInteractionPipelineState)
    {
        mPendingInteractors.clear();
        return;
    }

    if (!EnsureInteractionResources())
    {
        mPendingInteractors.clear();
        return;
    }

    mInteractionPreviousCentre = mInteractionCentre;
    mInteractionCentre = XMFLOAT2(cameraPosition.x, cameraPosition.y);

    const int readIndex  = mInteractionWriteIndex;
    const int writeIndex = 1 - mInteractionWriteIndex;

    // Upload this frame's interactors.
    auto* positions  = static_cast<XMFLOAT4*>(mInteractorBufferPtr);
    auto* velocities = static_cast<XMFLOAT4*>(mInteractorVelocityBufferPtr);
    const std::uint32_t count = static_cast<std::uint32_t>(
        (std::min)(mPendingInteractors.size(), static_cast<std::size_t>(kMaxInteractors)));

    for (std::uint32_t i = 0; i < count; ++i)
    {
        const VegetationInteractor& interactor = mPendingInteractors[i];
        positions[i] = XMFLOAT4(
            interactor.Position.x, interactor.Position.y, interactor.Position.z, interactor.Radius);
        velocities[i] = XMFLOAT4(
            interactor.Velocity.x, interactor.Velocity.y, interactor.Velocity.z, interactor.Strength);
    }

    mMappedInteractionCb->Centre          = mInteractionCentre;
    mMappedInteractionCb->WorldSize       = kInteractionWorldSize;
    mMappedInteractionCb->Resolution      = static_cast<float>(kInteractionMapSize);
    mMappedInteractionCb->DeltaTime       = deltaSeconds;
    mMappedInteractionCb->InteractorCount = count;

    // On the very first dispatch the "previous" texture holds whatever was in
    // memory.  Placing the previous centre far outside the map makes every
    // reprojected lookup fall out of bounds, which the shader reads as zero --
    // a clean start without needing a separate clear pass.
    mMappedInteractionCb->PreviousCentre = mInteractionCleared
        ? mInteractionPreviousCentre
        : XMFLOAT2(mInteractionCentre.x + kInteractionWorldSize * 4.0f,
                   mInteractionCentre.y + kInteractionWorldSize * 4.0f);
    mInteractionCleared = true;

    // The read texture must be readable as an SRV and the write texture as a
    // UAV; they swap roles every frame.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    UINT barrierCount = 0;

    if (mInteractionState[readIndex] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
            mInteractionMap[readIndex].Get(),
            mInteractionState[readIndex],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        mInteractionState[readIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    if (mInteractionState[writeIndex] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
            mInteractionMap[writeIndex].Get(),
            mInteractionState[writeIndex],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        mInteractionState[writeIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    if (barrierCount > 0)
        commandList->ResourceBarrier(barrierCount, barriers);

    ID3D12DescriptorHeap* srvHeap = DX12Context_GetSrvDescriptorHeap();
    if (srvHeap == nullptr)
    {
        mPendingInteractors.clear();
        return;
    }

    commandList->SetDescriptorHeaps(1, &srvHeap);
    commandList->SetComputeRootSignature(mInteractionRootSignature.Get());
    commandList->SetPipelineState(mInteractionPipelineState.Get());

    commandList->SetComputeRootConstantBufferView(0, mInteractionCb->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(1, mInteractionSrvGpu[readIndex]);
    commandList->SetComputeRootShaderResourceView(2, mInteractorBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(3, mInteractorVelocityBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(4, mInteractionUavGpu[writeIndex]);

    const UINT groups = (kInteractionMapSize + 7) / 8;
    commandList->Dispatch(groups, groups, 1);

    // Make the freshly written map readable by the vertex shaders that sample
    // it during the draw passes.
    const D3D12_RESOURCE_BARRIER toRead = CD3DX12_RESOURCE_BARRIER::Transition(
        mInteractionMap[writeIndex].Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toRead);
    mInteractionState[writeIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    mInteractionWriteIndex = writeIndex;
    mPendingInteractors.clear();
}

// ---------------------------------------------------------------------------
// Cull
// ---------------------------------------------------------------------------

void VegetationRenderer::DispatchCull(
    ID3D12GraphicsCommandList* commandList,
    const XMMATRIX&            viewProjection,
    const XMFLOAT3&            cameraPosition)
{
    if (!mInitialized || commandList == nullptr || !mCullPipelineState)
        return;

    if (mFlatLayers.empty() || !mVisibleIndexBuffer || !mIndirectArgBuffer)
        return;

    // Reset every draw's InstanceCount by copying the template over the live
    // buffer.  Cheaper and simpler than a clear dispatch, and it refreshes the
    // static argument fields at the same time.
    {
        const D3D12_RESOURCE_BARRIER toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
            mIndirectArgBuffer.Get(),
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
            D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &toCopy);

        commandList->CopyBufferRegion(
            mIndirectArgBuffer.Get(), 0,
            mIndirectArgReset.Get(), 0,
            static_cast<UINT64>(mIndirectArgSlotCapacity) * kDrawArgStride);

        const D3D12_RESOURCE_BARRIER barriers[2] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(
                mIndirectArgBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(
                mVisibleIndexBuffer.Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        commandList->ResourceBarrier(2, barriers);
    }

    XMFLOAT4 frustumPlanes[6]{};
    ExtractFrustumPlanes(viewProjection, frustumPlanes);

    commandList->SetComputeRootSignature(mCullRootSignature.Get());
    commandList->SetPipelineState(mCullPipelineState.Get());

    for (std::size_t drawIndex = 0; drawIndex < mFlatLayers.size(); ++drawIndex)
    {
        const FlatLayer& flat = mFlatLayers[drawIndex];

        const auto areaIt = mAreas.find(flat.EntityIndex);
        if (areaIt == mAreas.end() || flat.LayerIndex >= areaIt->second.LayerRanges.size())
            continue;

        const AreaState&      state = areaIt->second;
        const LayerDrawRange& range = state.LayerRanges[flat.LayerIndex];

        if (range.InstanceCount == 0 || !state.InstanceBuffer)
            continue;

        const auto meshIt = mSharedMeshes.find(range.MeshPath);
        if (meshIt == mSharedMeshes.end() || !meshIt->second.Ready)
            continue;

        // The layer's authored rules live on the component, which is where the
        // cull distances and fade come from.
        if (flat.EntityIndex >= mEntities->size())
            continue;
        const Entity& entity = (*mEntities)[flat.EntityIndex];
        if (!entity.HasVegetationAreaComponent())
            continue;
        const VegetationAreaComponent& area = *entity.VegetationArea;
        if (flat.LayerIndex >= area.Layers.size())
            continue;
        const VegetationLayer& layer = area.Layers[flat.LayerIndex];

        CullConstants& constants = mMappedCullCb[drawIndex];
        for (int i = 0; i < 6; ++i)
            constants.FrustumPlanes[i] = frustumPlanes[i];

        constants.CameraPosition = cameraPosition;
        constants.CullDistance   = layer.CullDistance;
        constants.InstanceFirst  = range.InstanceFirst;
        constants.InstanceCount  = range.InstanceCount;
        constants.LodCount       = range.LodCount;
        constants.FadeFraction   = layer.FadeFraction;
        constants.BoundingRadius = meshIt->second.BoundingRadius;

        // Split the layer's cull distance across its LODs.  Weighting the
        // near bands tighter keeps the detailed mesh close to the camera,
        // which is where the instance count is lowest.
        const float span = layer.CullDistance;
        constants.LodDistances = XMFLOAT4(
            span * 0.18f, span * 0.42f, span * 0.72f, span);

        for (std::uint32_t lod = 0; lod < kMaxLodsPerLayer; ++lod)
        {
            const std::uint32_t clampedLod = (std::min)(lod, range.LodCount - 1);
            reinterpret_cast<std::uint32_t*>(&constants.LodOutputBase)[lod] =
                range.VisibleBase + clampedLod * range.InstanceCount;
            reinterpret_cast<std::uint32_t*>(&constants.LodArgOffset)[lod] =
                (range.ArgSlot + clampedLod) * kDrawArgStride;
        }

        const D3D12_GPU_VIRTUAL_ADDRESS cullCbAddress =
            mCullCb->GetGPUVirtualAddress() + drawIndex * sizeof(CullConstants);

        commandList->SetComputeRootConstantBufferView(0, cullCbAddress);
        commandList->SetComputeRootShaderResourceView(1, state.InstanceBuffer->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(2, mVisibleIndexBuffer->GetGPUVirtualAddress());
        commandList->SetComputeRootUnorderedAccessView(3, mIndirectArgBuffer->GetGPUVirtualAddress());

        const UINT groups = (range.InstanceCount + 63) / 64;
        commandList->Dispatch(groups, 1, 1);
    }

    const D3D12_RESOURCE_BARRIER toRead[2] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(
            mIndirectArgBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        CD3DX12_RESOURCE_BARRIER::Transition(
            mVisibleIndexBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    commandList->ResourceBarrier(2, toRead);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void VegetationRenderer::Render(
    ID3D12GraphicsCommandList* commandList,
    const XMMATRIX&            viewProjection,
    const XMFLOAT3&            cameraPosition,
    DXGI_FORMAT                albedoFormat,
    DXGI_FORMAT                normalFormat,
    DXGI_FORMAT                materialFormat,
    DXGI_FORMAT                depthFormat,
    UINT                       msaaSampleCount)
{
    if (!mInitialized || commandList == nullptr || mFlatLayers.empty())
        return;

    const bool formatsChanged =
        !mPipelineReady ||
        mAlbedoFormat    != albedoFormat  ||
        mNormalFormat    != normalFormat  ||
        mMaterialFormat  != materialFormat||
        mDepthFormat     != depthFormat   ||
        mMsaaSampleCount != msaaSampleCount;

    if (formatsChanged)
    {
        if (!CreateGraphicsPipeline(albedoFormat, normalFormat, materialFormat, depthFormat, msaaSampleCount))
            return;

        // Shares the graphics root signature, so it has to be rebuilt whenever
        // the mesh pipeline is.  Failure here is non-fatal: layers just draw
        // their mesh LODs and cull, with no billboard stage.
        mBillboardPipelineReady = false;
        CreateBillboardPipeline(albedoFormat, normalFormat, materialFormat, depthFormat, msaaSampleCount);
    }

    if (!mVisibleIndexBuffer || !mIndirectArgBuffer || !mCommandSignature)
        return;

    ID3D12DescriptorHeap* srvHeap = DX12Context_GetSrvDescriptorHeap();
    if (srvHeap == nullptr)
        return;

    FillPassConstants(viewProjection, cameraPosition, mMappedPassCb[kPassColour]);

    commandList->SetDescriptorHeaps(1, &srvHeap);
    commandList->SetGraphicsRootSignature(mGraphicsRootSignature.Get());
    commandList->SetPipelineState(mGraphicsPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(
        0, mPassCb->GetGPUVirtualAddress() + kPassColour * sizeof(PassConstants));
    commandList->SetGraphicsRootDescriptorTable(9, mInteractionSrvGpu[mInteractionWriteIndex]);

    for (std::size_t drawIndex = 0; drawIndex < mFlatLayers.size(); ++drawIndex)
    {
        const FlatLayer& flat = mFlatLayers[drawIndex];

        const auto areaIt = mAreas.find(flat.EntityIndex);
        if (areaIt == mAreas.end() || flat.LayerIndex >= areaIt->second.LayerRanges.size())
            continue;

        const AreaState&      state = areaIt->second;
        const LayerDrawRange& range = state.LayerRanges[flat.LayerIndex];

        if (range.InstanceCount == 0 || !state.InstanceBuffer)
            continue;

        const auto meshIt = mSharedMeshes.find(range.MeshPath);
        if (meshIt == mSharedMeshes.end() || !meshIt->second.Ready)
            continue;

        if (flat.EntityIndex >= mEntities->size())
            continue;
        const Entity& entity = (*mEntities)[flat.EntityIndex];
        if (!entity.HasVegetationAreaComponent())
            continue;
        const VegetationAreaComponent& area = *entity.VegetationArea;
        if (flat.LayerIndex >= area.Layers.size())
            continue;
        const VegetationLayer& layer = area.Layers[flat.LayerIndex];

        const LayerMaterial& material = ResolveLayerMaterial(range.MaterialPath);

        // Written into LOD 0's slot, then copied into each LOD's own slot
        // below.  Every LOD needs a distinct slot because they differ by
        // InstanceOffset and all their draws are recorded before any of them
        // execute.
        LayerConstants& constants = mMappedLayerCb[LayerCbSlot(kPassColour, drawIndex, 0)];
        FillLayerConstants(layer, material, meshIt->second, constants);

        commandList->SetGraphicsRootShaderResourceView(2, state.InstanceBuffer->GetGPUVirtualAddress());
        commandList->SetGraphicsRootShaderResourceView(3, mVisibleIndexBuffer->GetGPUVirtualAddress());

        const auto bindTexture = [&](UINT slot, const std::shared_ptr<GpuTexture>& texture)
        {
            commandList->SetGraphicsRootDescriptorTable(
                slot, (texture && texture->IsValid()) ? texture->GpuHandle : mFallbackGpuHandle);
        };

        bindTexture(4, material.BaseColor);
        bindTexture(5, material.Normal);
        bindTexture(6, material.Metallic);
        bindTexture(7, material.Roughness);
        bindTexture(8, material.Ao);

        const std::uint32_t meshLodCount = range.LodCount - (range.HasBillboard ? 1u : 0u);

        for (std::uint32_t lod = 0; lod < meshLodCount; ++lod)
        {
            if (lod >= meshIt->second.Lods.size())
                break;

            const LodBuffers& buffers = meshIt->second.Lods[lod];
            if (buffers.IndexCount == 0)
                continue;

            const std::size_t slot = LayerCbSlot(kPassColour, drawIndex, lod);
            if (slot != LayerCbSlot(kPassColour, drawIndex, 0))
                mMappedLayerCb[slot] = constants;

            // Each LOD reads its own slice of the compacted visible list.
            mMappedLayerCb[slot].InstanceOffset = range.VisibleBase + lod * range.InstanceCount;

            commandList->SetGraphicsRootConstantBufferView(
                1, mLayerCb->GetGPUVirtualAddress() + slot * sizeof(LayerConstants));

            commandList->IASetVertexBuffers(0, 1, &buffers.VertexBufferView);
            commandList->IASetIndexBuffer(&buffers.IndexBufferView);

            commandList->ExecuteIndirect(
                mCommandSignature.Get(),
                1,
                mIndirectArgBuffer.Get(),
                static_cast<UINT64>(range.ArgSlot + lod) * kDrawArgStride,
                nullptr,
                0);
        }

        // The billboard LOD swaps in a different pipeline and the shared quad,
        // so it is issued after the mesh LODs to keep the PSO switch to one per
        // layer rather than one per LOD.
        if (range.HasBillboard && mBillboardPipelineState)
        {
            const auto billboardIt = mBillboardTextures.find(range.BillboardTexturePath);
            if (billboardIt != mBillboardTextures.end()
                && billboardIt->second && billboardIt->second->IsValid())
            {
                commandList->SetPipelineState(mBillboardPipelineState.Get());
                commandList->SetGraphicsRootDescriptorTable(4, billboardIt->second->GpuHandle);

                const std::size_t slot = LayerCbSlot(kPassColour, drawIndex, meshLodCount);
                mMappedLayerCb[slot] = constants;
                mMappedLayerCb[slot].InstanceOffset = range.VisibleBase + meshLodCount * range.InstanceCount;

                commandList->SetGraphicsRootConstantBufferView(
                    1, mLayerCb->GetGPUVirtualAddress() + slot * sizeof(LayerConstants));

                commandList->IASetVertexBuffers(0, 1, &mBillboardVertexView);
                commandList->IASetIndexBuffer(&mBillboardIndexView);

                commandList->ExecuteIndirect(
                    mCommandSignature.Get(),
                    1,
                    mIndirectArgBuffer.Get(),
                    static_cast<UINT64>(range.ArgSlot + meshLodCount) * kDrawArgStride,
                    nullptr,
                    0);

                // Restore the mesh pipeline for the next layer.
                commandList->SetPipelineState(mGraphicsPipelineState.Get());
            }
        }
    }
}

void VegetationRenderer::RenderMotionVectors(
    ID3D12GraphicsCommandList* commandList,
    const XMMATRIX&            viewProjection,
    const XMMATRIX&            previousViewProjection,
    const XMFLOAT3&            cameraPosition,
    DXGI_FORMAT                targetFormat,
    DXGI_FORMAT                depthFormat)
{
    if (!mInitialized || commandList == nullptr || mFlatLayers.empty())
        return;

    // The graphics root signature is shared, so the colour pipeline has to
    // exist before the motion one can be built against it.
    if (!mPipelineReady || !mGraphicsRootSignature)
        return;

    if (!mMotionPipelineReady || mMotionTargetFormat != targetFormat || mMotionDepthFormat != depthFormat)
    {
        if (!CreateMotionPipeline(targetFormat, depthFormat))
            return;
    }

    if (!mVisibleIndexBuffer || !mIndirectArgBuffer || !mCommandSignature)
        return;

    ID3D12DescriptorHeap* srvHeap = DX12Context_GetSrvDescriptorHeap();
    if (srvHeap == nullptr)
        return;

    FillPassConstants(viewProjection, cameraPosition, mMappedPassCb[kPassMotion]);
    XMStoreFloat4x4(&mMappedPassCb[kPassMotion].PrevViewProj, XMMatrixTranspose(previousViewProjection));

    commandList->SetDescriptorHeaps(1, &srvHeap);
    commandList->SetGraphicsRootSignature(mGraphicsRootSignature.Get());
    commandList->SetPipelineState(mMotionPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(
        0, mPassCb->GetGPUVirtualAddress() + kPassMotion * sizeof(PassConstants));
    commandList->SetGraphicsRootDescriptorTable(9, mInteractionSrvGpu[mInteractionWriteIndex]);

    for (std::size_t drawIndex = 0; drawIndex < mFlatLayers.size(); ++drawIndex)
    {
        const FlatLayer& flat = mFlatLayers[drawIndex];

        const auto areaIt = mAreas.find(flat.EntityIndex);
        if (areaIt == mAreas.end() || flat.LayerIndex >= areaIt->second.LayerRanges.size())
            continue;

        const AreaState&      state = areaIt->second;
        const LayerDrawRange& range = state.LayerRanges[flat.LayerIndex];

        if (range.InstanceCount == 0 || !state.InstanceBuffer)
            continue;

        const auto meshIt = mSharedMeshes.find(range.MeshPath);
        if (meshIt == mSharedMeshes.end() || !meshIt->second.Ready)
            continue;

        const LayerMaterial& material = ResolveLayerMaterial(range.MaterialPath);

        commandList->SetGraphicsRootShaderResourceView(2, state.InstanceBuffer->GetGPUVirtualAddress());
        commandList->SetGraphicsRootShaderResourceView(3, mVisibleIndexBuffer->GetGPUVirtualAddress());
        commandList->SetGraphicsRootDescriptorTable(
            4, (material.BaseColor && material.BaseColor->IsValid())
                ? material.BaseColor->GpuHandle
                : mFallbackGpuHandle);

        // Mesh LODs only.  The billboard card is deliberately left out: at the
        // distance it is drawn, its bend contributes well under a pixel of
        // movement, and giving it camera-only motion vectors is both cheaper
        // and closer to correct than a second pipeline for the same result.
        const std::uint32_t meshLodCount = range.LodCount - (range.HasBillboard ? 1u : 0u);

        for (std::uint32_t lod = 0; lod < meshLodCount; ++lod)
        {
            if (lod >= meshIt->second.Lods.size())
                break;

            const LodBuffers& buffers = meshIt->second.Lods[lod];
            if (buffers.IndexCount == 0)
                continue;

            // Copied from the colour pass's slot, which Render() filled earlier
            // in this same command list, so the bend parameters are guaranteed
            // identical -- any divergence would show up as ghosting.
            const std::size_t slot = LayerCbSlot(kPassMotion, drawIndex, lod);
            mMappedLayerCb[slot] = mMappedLayerCb[LayerCbSlot(kPassColour, drawIndex, lod)];
            mMappedLayerCb[slot].InstanceOffset = range.VisibleBase + lod * range.InstanceCount;

            commandList->SetGraphicsRootConstantBufferView(
                1, mLayerCb->GetGPUVirtualAddress() + slot * sizeof(LayerConstants));

            commandList->IASetVertexBuffers(0, 1, &buffers.VertexBufferView);
            commandList->IASetIndexBuffer(&buffers.IndexBufferView);

            commandList->ExecuteIndirect(
                mCommandSignature.Get(),
                1,
                mIndirectArgBuffer.Get(),
                static_cast<UINT64>(range.ArgSlot + lod) * kDrawArgStride,
                nullptr,
                0);
        }
    }
}

void VegetationRenderer::RenderShadowDepth(
    ID3D12GraphicsCommandList* commandList,
    const XMFLOAT4X4&          lightViewProjection,
    DXGI_FORMAT                depthFormat)
{
    if (!mInitialized || commandList == nullptr || mFlatLayers.empty())
        return;

    if (!mShadowPipelineReady || mShadowDepthFormat != depthFormat)
    {
        if (!CreateShadowPipeline(depthFormat))
            return;
    }

    if (!mVisibleIndexBuffer || !mIndirectArgBuffer || !mCommandSignature)
        return;

    ID3D12DescriptorHeap* srvHeap = DX12Context_GetSrvDescriptorHeap();
    if (srvHeap == nullptr)
        return;

    // The shadow pass reuses the pass constant buffer with the light's matrix
    // swapped in.  Everything else -- wind, time, interaction -- must stay
    // identical to the colour pass so the bend matches exactly.
    FillPassConstants(XMMatrixIdentity(), XMFLOAT3(0.0f, 0.0f, 0.0f), mMappedPassCb[kPassShadow]);
    mMappedPassCb[kPassShadow].ViewProj = lightViewProjection;

    commandList->SetDescriptorHeaps(1, &srvHeap);
    commandList->SetGraphicsRootSignature(mGraphicsRootSignature.Get());
    commandList->SetPipelineState(mShadowPipelineState.Get());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootConstantBufferView(
        0, mPassCb->GetGPUVirtualAddress() + kPassShadow * sizeof(PassConstants));
    commandList->SetGraphicsRootDescriptorTable(9, mInteractionSrvGpu[mInteractionWriteIndex]);

    for (std::size_t drawIndex = 0; drawIndex < mFlatLayers.size(); ++drawIndex)
    {
        const FlatLayer& flat = mFlatLayers[drawIndex];

        const auto areaIt = mAreas.find(flat.EntityIndex);
        if (areaIt == mAreas.end() || flat.LayerIndex >= areaIt->second.LayerRanges.size())
            continue;

        const AreaState&      state = areaIt->second;
        const LayerDrawRange& range = state.LayerRanges[flat.LayerIndex];

        if (range.InstanceCount == 0 || !state.InstanceBuffer)
            continue;

        const auto meshIt = mSharedMeshes.find(range.MeshPath);
        if (meshIt == mSharedMeshes.end() || !meshIt->second.Ready)
            continue;

        if (flat.EntityIndex >= mEntities->size())
            continue;
        const Entity& entity = (*mEntities)[flat.EntityIndex];
        if (!entity.HasVegetationAreaComponent())
            continue;
        const VegetationAreaComponent& area = *entity.VegetationArea;
        if (flat.LayerIndex >= area.Layers.size() || !area.Layers[flat.LayerIndex].CastShadows)
            continue;

        const LayerMaterial& material = ResolveLayerMaterial(range.MaterialPath);

        commandList->SetGraphicsRootShaderResourceView(2, state.InstanceBuffer->GetGPUVirtualAddress());
        commandList->SetGraphicsRootShaderResourceView(3, mVisibleIndexBuffer->GetGPUVirtualAddress());
        commandList->SetGraphicsRootDescriptorTable(
            4, (material.BaseColor && material.BaseColor->IsValid())
                ? material.BaseColor->GpuHandle
                : mFallbackGpuHandle);

        // Only the nearest LOD casts.  Shadows from a distant billboard are
        // not worth the draw, and the difference is invisible at range.
        const LodBuffers& buffers = meshIt->second.Lods[0];
        if (buffers.IndexCount == 0)
            continue;

        // The shadow pass owns its own constant slots.  It runs before the
        // colour pass, so it fills them itself rather than reusing anything the
        // colour pass will write later in the same command list.
        const std::size_t slot = LayerCbSlot(kPassShadow, drawIndex, 0);
        FillLayerConstants(area.Layers[flat.LayerIndex], material, meshIt->second, mMappedLayerCb[slot]);
        mMappedLayerCb[slot].InstanceOffset = range.VisibleBase;

        commandList->SetGraphicsRootConstantBufferView(
            1, mLayerCb->GetGPUVirtualAddress() + slot * sizeof(LayerConstants));

        commandList->IASetVertexBuffers(0, 1, &buffers.VertexBufferView);
        commandList->IASetIndexBuffer(&buffers.IndexBufferView);

        commandList->ExecuteIndirect(
            mCommandSignature.Get(),
            1,
            mIndirectArgBuffer.Get(),
            static_cast<UINT64>(range.ArgSlot) * kDrawArgStride,
            nullptr,
            0);
    }
}
