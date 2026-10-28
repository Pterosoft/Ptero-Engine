#include "pch.h"

#include "ScenePhysics.h"

#include "EntityIds.h"
#include "TerrainRenderer.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>

using namespace DirectX;

namespace
{
    // The player's proxy: CharacterMovementSettings' standing capsule, hanging below the eye.
    // Its bottom is lifted a little off the feet so that standing on a crate does not
    // press the capsule into it - the character's own floor probe already keeps it up there.
    constexpr float kPlayerRadius = 0.35f;
    constexpr float kPlayerFootClearance = 0.1f;
    constexpr float kPlayerHeight = 1.8f - kPlayerFootClearance;
    constexpr float kPlayerEyeHeight = 1.65f - kPlayerFootClearance;
    // Further than this in one frame is a respawn or a cut, not a walk: teleport rather
    // than sweep the capsule through everything in between.
    constexpr float kPlayerTeleportDistance = 3.0f;

    // Finest height field the terrain gets. A 4k heightmap would cost seconds to build and
    // a lot of memory, and half-metre cells are plenty for props to land on.
    constexpr std::uint32_t kMaxHeightFieldSamples = 2048;

    // Same conversion every renderer system uses (PteroTransform::ComposeRotation).
    void EulerToQuaternion(const XMFLOAT3& euler, float out[4])
    {
        XMFLOAT4 q;
        XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionRotationMatrix(PteroTransform::ComposeRotation(euler))));
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    }

    // Inverse of ComposeRotation, which is Rx(x) * Ry(y) * Rz(z) on row vectors:
    // m02 = -sin y, m12 = sin x cos y, m22 = cos x cos y, m01 = cos y sin z, m00 = cos y cos z
    // (see PteroTransform::ConvertLegacyRotation for the same derivation).
    XMFLOAT3 QuaternionToEuler(const float q[4])
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(XMVectorSet(q[0], q[1], q[2], q[3])));

        const float sinY = std::clamp(-m._13, -1.0f, 1.0f);
        const float cosY = std::sqrt((std::max)(0.0f, 1.0f - sinY * sinY));

        XMFLOAT3 euler{};
        euler.y = std::asin(sinY);
        if (cosY > 1.0e-6f)
        {
            euler.x = std::atan2(m._23, m._33);
            euler.z = std::atan2(m._12, m._11);
        }
        else
        {
            // Gimbal lock: X and Z turn about the same axis here; fold it all into Z.
            euler.x = 0.0f;
            euler.z = std::atan2(-m._21, m._22);
        }
        return euler;
    }

    bool SameFloat3(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    bool IsSurfaceMeshEntity(const Entity& entity)
    {
        return entity.Mesh.has_value() && !entity.Terrain && !entity.Water && !entity.VegetationArea;
    }

    // Scale as the renderer applies it (TransformComponent::GetTransform).
    XMFLOAT3 EntityScale(const Entity& entity)
    {
        constexpr float s = TransformComponent::MeshWorldScale;
        return XMFLOAT3(entity.Transform.Scale.x * s, entity.Transform.Scale.y * s, entity.Transform.Scale.z * s);
    }

    // Local bounds of the finest LOD, with the entity's scale applied.
    bool ScaledMeshBounds(const Mesh& mesh, const XMFLOAT3& scale, XMFLOAT3& outMin, XMFLOAT3& outMax)
    {
        if (mesh.GetLodCount() == 0 || mesh.GetLod(0).Vertices.empty())
            return false;

        outMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        outMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const Vertex& vertex : mesh.GetLod(0).Vertices)
        {
            const XMFLOAT3 p(vertex.Position.x * scale.x, vertex.Position.y * scale.y, vertex.Position.z * scale.z);
            outMin = XMFLOAT3((std::min)(outMin.x, p.x), (std::min)(outMin.y, p.y), (std::min)(outMin.z, p.z));
            outMax = XMFLOAT3((std::max)(outMax.x, p.x), (std::max)(outMax.y, p.y), (std::max)(outMax.z, p.z));
        }
        return true;
    }

    void AppendPositions(const std::vector<Vertex>& vertices, std::vector<float>& out)
    {
        out.reserve(out.size() + vertices.size() * 3);
        for (const Vertex& vertex : vertices)
        {
            out.push_back(vertex.Position.x);
            out.push_back(vertex.Position.y);
            out.push_back(vertex.Position.z);
        }
    }

    // Jolt does not collide with the back of a triangle, so a floor whose triangles face
    // down lets everything fall through it. The importer's winding is not something to rely
    // on (GeometryRaycaster treats triangles as double-sided for the same reason), so ask
    // the mesh's own normals which side is out and turn the triangles to match.
    void OrientTrianglesOutward(const MeshLod& lod, std::vector<std::uint32_t>& indices)
    {
        double agreement = 0.0;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const XMVECTOR p0 = XMLoadFloat3(&lod.Vertices[indices[i]].Position);
            const XMVECTOR p1 = XMLoadFloat3(&lod.Vertices[indices[i + 1]].Position);
            const XMVECTOR p2 = XMLoadFloat3(&lod.Vertices[indices[i + 2]].Position);
            const XMVECTOR n = XMVectorAdd(XMLoadFloat3(&lod.Vertices[indices[i]].Normal),
                XMVectorAdd(XMLoadFloat3(&lod.Vertices[indices[i + 1]].Normal), XMLoadFloat3(&lod.Vertices[indices[i + 2]].Normal)));
            // Area-weighted: the cross product's length is twice the triangle's area.
            agreement += XMVectorGetX(XMVector3Dot(XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0)), n));
        }

        if (agreement < 0.0)
        {
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
                std::swap(indices[i + 1], indices[i + 2]);
        }
    }
}

ScenePhysics::~ScenePhysics()
{
    Clear();
}

bool ScenePhysics::Build(std::vector<Entity>& entities, const TerrainRenderer& terrain)
{
    Clear();

    const auto start = std::chrono::steady_clock::now();

    char error[512] = {};
    if (!mWorld.Initialize(PhysicsWorldSettings{}, error, sizeof(error)))
    {
        PTERO_LOG_WARNING("Physics", "%s Playing without rigid bodies.", error);
        return false;
    }

    // Links hold ids, so the body can still find its entity if the list shifts mid-session.
    EnsureEntityIds(entities);
    mEntities = &entities;

    for (std::size_t i = 0; i < entities.size(); ++i)
        AddEntity(i, entities[i]);

    AddTerrain(terrain);

    mWorld.OptimizeBroadPhase();

    const float buildMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    PTERO_LOG_INFO("Physics", "Jolt world: %zu rigid bodies, %zu static colliders (%zu shared shapes), built in %.1f ms.",
        mLinks.size(), mColliderCount, mShapeCache.size(), buildMs);
    return true;
}

void ScenePhysics::Clear()
{
    // Bodies hold their own shape references, so the cache can go in any order.
    for (auto& [key, shape] : mShapeCache)
        mWorld.ReleaseShape(shape);
    mShapeCache.clear();
    // The whole world goes, worker threads included: the editor has no use for them
    // between sessions, and they must not still be running when the renderer unloads.
    mWorld.Shutdown();

    mLinks.clear();
    mHits.clear();
    mColliderCount = 0;
    mEntities = nullptr;
    mPlayer = {};
    mPlayerPlaced = false;
}

bool ScenePhysics::AddEntity(std::size_t entityIndex, Entity& entity)
{
    if (!IsSurfaceMeshEntity(entity))
        return false;

    const Mesh* mesh = entity.Mesh->MeshAsset.get();
    const RigidBodyComponent defaults{ RigidBodyMotion::Static, RigidBodyShape::Auto };
    const RigidBodyComponent& rb = entity.RigidBody ? *entity.RigidBody : defaults;

    // A plain static mesh with nothing loaded has nothing to collide with; a rigid body
    // still gets a stand-in box so the game's logic around it keeps working.
    if (mesh == nullptr && !entity.RigidBody)
        return false;

    PhysicsBodyDesc desc;
    switch (rb.Motion)
    {
    case RigidBodyMotion::Dynamic:
        desc.Motion = PhysicsMotionType::Dynamic;
        desc.Layer = PhysicsLayer::Moving;
        break;
    case RigidBodyMotion::Kinematic:
        desc.Motion = PhysicsMotionType::Kinematic;
        desc.Layer = PhysicsLayer::Moving;
        break;
    default:
        desc.Motion = PhysicsMotionType::Static;
        desc.Layer = PhysicsLayer::Static;
        break;
    }
    const bool dynamic = desc.Motion == PhysicsMotionType::Dynamic;

    desc.Position[0] = entity.Transform.Position.x;
    desc.Position[1] = entity.Transform.Position.y;
    desc.Position[2] = entity.Transform.Position.z;
    EulerToQuaternion(entity.Transform.Rotation, desc.Rotation);

    const XMFLOAT3 scale = EntityScale(entity);

    // Primitives are fitted to the scaled bounds here, per body; mesh-derived shapes are
    // shared per asset and scaled by the body.
    PhysicsShapeDesc primitive;
    const bool wantsPrimitive = rb.Shape == RigidBodyShape::Box || rb.Shape == RigidBodyShape::Sphere ||
                                rb.Shape == RigidBodyShape::Capsule || mesh == nullptr;
    if (wantsPrimitive)
    {
        XMFLOAT3 boundsMin(-0.5f * std::abs(scale.x), -0.5f * std::abs(scale.y), -0.5f * std::abs(scale.z));
        XMFLOAT3 boundsMax(0.5f * std::abs(scale.x), 0.5f * std::abs(scale.y), 0.5f * std::abs(scale.z));
        if (mesh != nullptr)
            ScaledMeshBounds(*mesh, scale, boundsMin, boundsMax);

        const XMFLOAT3 halfExtents(0.5f * (boundsMax.x - boundsMin.x), 0.5f * (boundsMax.y - boundsMin.y),
                                   0.5f * (boundsMax.z - boundsMin.z));
        primitive.LocalPosition[0] = 0.5f * (boundsMin.x + boundsMax.x);
        primitive.LocalPosition[1] = 0.5f * (boundsMin.y + boundsMax.y);
        primitive.LocalPosition[2] = 0.5f * (boundsMin.z + boundsMax.z);

        switch (rb.Shape)
        {
        case RigidBodyShape::Sphere:
            primitive.Type = PhysicsShapeType::Sphere;
            primitive.Radius = (std::max)({ halfExtents.x, halfExtents.y, halfExtents.z });
            break;
        case RigidBodyShape::Capsule:
            primitive.Type = PhysicsShapeType::Capsule;
            primitive.Radius = (std::max)(halfExtents.x, halfExtents.y);
            primitive.HalfHeight = (std::max)(halfExtents.z - primitive.Radius, 0.01f);
            break;
        default:
            primitive.Type = PhysicsShapeType::Box;
            primitive.HalfExtents[0] = halfExtents.x;
            primitive.HalfExtents[1] = halfExtents.y;
            primitive.HalfExtents[2] = halfExtents.z;
            break;
        }

        desc.Shapes = &primitive;
        desc.ShapeCount = 1;
    }
    else
    {
        desc.Shape = GetMeshShape(*mesh, rb.Shape, dynamic);
        if (!desc.Shape.IsValid())
            return false;
        desc.Scale[0] = scale.x;
        desc.Scale[1] = scale.y;
        desc.Scale[2] = scale.z;
    }

    desc.Mass = rb.Mass;
    desc.Friction = rb.Friction;
    desc.Restitution = rb.Restitution;
    desc.LinearDamping = rb.LinearDamping;
    desc.AngularDamping = rb.AngularDamping;
    desc.GravityFactor = rb.GravityScale;
    desc.ContinuousCollision = rb.ContinuousCollision;
    desc.StartAsleep = rb.StartAsleep;
    desc.UserData = entity.Id;

    const PhysicsBodyHandle body = mWorld.CreateBody(desc);
    if (!body.IsValid())
    {
        PTERO_LOG_WARNING("Physics", "'%s' could not be added to the simulation.", entity.Name.c_str());
        return false;
    }

    if (desc.Motion == PhysicsMotionType::Static)
    {
        ++mColliderCount;
        return true;
    }

    Link link;
    link.EntityIndex = entityIndex;
    link.EntityId = entity.Id;
    link.Body = body;
    link.Motion = rb.Motion;
    link.Position = entity.Transform.Position;
    link.Rotation = entity.Transform.Rotation;
    mLinks.push_back(link);
    return true;
}

PhysicsShapeHandle ScenePhysics::GetMeshShape(const Mesh& mesh, RigidBodyShape kind, bool dynamic)
{
    // Resolve Auto first so equivalent requests share one entry.
    if (kind == RigidBodyShape::Auto && !mesh.HasCollisionHulls())
        kind = dynamic ? RigidBodyShape::ConvexHull : RigidBodyShape::TriangleMesh;
    if (kind == RigidBodyShape::TriangleMesh && dynamic)
        kind = RigidBodyShape::ConvexHull;

    const auto key = std::make_tuple(&mesh, static_cast<int>(kind), dynamic);
    if (const auto found = mShapeCache.find(key); found != mShapeCache.end())
        return found->second;

    const PhysicsMotionType motion = dynamic ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static;
    PhysicsShapeHandle shape;

    if (kind == RigidBodyShape::Auto)
    {
        // The convex decomposition (Generate Collisions): one hull per piece, which is
        // both cheaper and better behaved than the render triangles, moving or not.
        const std::vector<CollisionHull>& hulls = mesh.GetCollisionHulls();
        std::vector<std::vector<float>> points(hulls.size());
        std::vector<PhysicsShapeDesc> parts;
        parts.reserve(hulls.size());
        for (std::size_t i = 0; i < hulls.size(); ++i)
        {
            for (const XMFLOAT3& vertex : hulls[i].Vertices)
            {
                points[i].push_back(vertex.x);
                points[i].push_back(vertex.y);
                points[i].push_back(vertex.z);
            }
            if (points[i].empty())
                continue;

            PhysicsShapeDesc part;
            part.Type = PhysicsShapeType::ConvexHull;
            part.Vertices = points[i].data();
            part.VertexCount = static_cast<std::uint32_t>(points[i].size() / 3);
            parts.push_back(part);
        }
        shape = mWorld.CreateShape(parts.data(), static_cast<std::uint32_t>(parts.size()), motion);
    }
    else if (kind == RigidBodyShape::ConvexHull && mesh.GetLodCount() > 0)
    {
        // The hull of the coarsest LOD is all but the hull of the finest, from far fewer points.
        std::vector<float> points;
        AppendPositions(mesh.GetLod(mesh.GetLodCount() - 1).Vertices, points);

        PhysicsShapeDesc hull;
        hull.Type = PhysicsShapeType::ConvexHull;
        hull.Vertices = points.data();
        hull.VertexCount = static_cast<std::uint32_t>(points.size() / 3);
        shape = mWorld.CreateShape(&hull, 1, motion);
    }
    else if (kind == RigidBodyShape::TriangleMesh && mesh.GetLodCount() > 0)
    {
        // The finest LOD, as PlayerCollision uses: a coarse LOD melts a stair into a ramp.
        const MeshLod& lod = mesh.GetLod(0);
        std::vector<float> points;
        AppendPositions(lod.Vertices, points);

        std::vector<std::uint32_t> indices;
        indices.reserve(lod.Indices.size());
        for (std::size_t i = 0; i + 2 < lod.Indices.size(); i += 3)
        {
            const std::uint32_t a = lod.Indices[i], b = lod.Indices[i + 1], c = lod.Indices[i + 2];
            if (a < lod.Vertices.size() && b < lod.Vertices.size() && c < lod.Vertices.size())
                indices.insert(indices.end(), { a, b, c });
        }
        OrientTrianglesOutward(lod, indices);

        PhysicsShapeDesc triangles;
        triangles.Type = PhysicsShapeType::TriangleMesh;
        triangles.Vertices = points.data();
        triangles.VertexCount = static_cast<std::uint32_t>(points.size() / 3);
        triangles.Indices = indices.data();
        triangles.IndexCount = static_cast<std::uint32_t>(indices.size());
        shape = mWorld.CreateShape(&triangles, 1, motion);
    }

    // Cache failures too, so a broken asset is reported by its bodies once, not rebuilt
    // for every instance.
    mShapeCache.emplace(key, shape);
    return shape;
}

void ScenePhysics::AddTerrain(const TerrainRenderer& terrain)
{
    std::vector<TerrainRenderer::PatchData> patches;
    terrain.ExportPatches(patches);

    for (const TerrainRenderer::PatchData& patch : patches)
    {
        if (patch.Width < 2 || patch.Height < 2 || patch.WorldSize <= 0.0f)
            continue;

        // Jolt wants a square, power-of-two grid; resample the heightmap onto the nearest
        // one with at least as many cells (TerrainGeometry::WorldToCell's mapping).
        const std::uint32_t cells = static_cast<std::uint32_t>((std::max)(patch.Width, patch.Height) - 1);
        std::uint32_t count = 4;
        while (count < cells && count < kMaxHeightFieldSamples)
            count *= 2;

        const float cellSize = patch.WorldSize / static_cast<float>(count - 1);
        std::vector<float> heights(static_cast<std::size_t>(count) * count);
        for (std::uint32_t row = 0; row < count; ++row)
        {
            const float v = static_cast<float>(row) / static_cast<float>(count - 1);
            const float cellY = v * static_cast<float>(patch.Height - 1);
            const int y0 = (std::min)(static_cast<int>(cellY), patch.Height - 1);
            const int y1 = (std::min)(y0 + 1, patch.Height - 1);
            const float ty = cellY - static_cast<float>(y0);

            for (std::uint32_t column = 0; column < count; ++column)
            {
                const float u = static_cast<float>(column) / static_cast<float>(count - 1);
                const float cellX = u * static_cast<float>(patch.Width - 1);
                const int x0 = (std::min)(static_cast<int>(cellX), patch.Width - 1);
                const int x1 = (std::min)(x0 + 1, patch.Width - 1);
                const float tx = cellX - static_cast<float>(x0);

                const auto sample = [&](int x, int y) {
                    return static_cast<float>(patch.Samples[static_cast<std::size_t>(y) * patch.Width + x]) / 65535.0f;
                };
                const float h0 = sample(x0, y0) * (1.0f - tx) + sample(x1, y0) * tx;
                const float h1 = sample(x0, y1) * (1.0f - tx) + sample(x1, y1) * tx;
                const float normalised = h0 * (1.0f - ty) + h1 * ty;

                heights[static_cast<std::size_t>(row) * count + column] =
                    normalised * patch.HeightScale + patch.HeightOffset + patch.Origin.z;
            }
        }

        PhysicsHeightFieldDesc field;
        field.OriginX = patch.Origin.x - 0.5f * patch.WorldSize;
        field.OriginY = patch.Origin.y - 0.5f * patch.WorldSize;
        field.CellSize = cellSize;
        field.SampleCount = count;
        field.Heights = heights.data();
        if (patch.EntityIndex < mEntities->size())
            field.UserData = (*mEntities)[patch.EntityIndex].Id;

        if (mWorld.CreateHeightField(field).IsValid())
            ++mColliderCount;
        else
            PTERO_LOG_WARNING("Physics", "A terrain patch could not be turned into a height field.");
    }
}

Entity* ScenePhysics::ResolveEntity(Link& link)
{
    if (mEntities == nullptr)
        return nullptr;

    if (link.EntityIndex < mEntities->size() && (*mEntities)[link.EntityIndex].Id == link.EntityId)
        return &(*mEntities)[link.EntityIndex];

    // The list changed under us (an entity added or removed during play): find it again.
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        if ((*mEntities)[i].Id == link.EntityId)
        {
            link.EntityIndex = i;
            return &(*mEntities)[i];
        }
    }
    return nullptr;
}

void ScenePhysics::Update(float deltaSeconds)
{
    if (!IsActive())
        return;

    // The game may have moved any of these since the last frame (Set Transform, a
    // sequence). A kinematic body follows its entity; a dynamic one is teleported there.
    for (std::size_t i = 0; i < mLinks.size();)
    {
        Link& link = mLinks[i];
        Entity* entity = ResolveEntity(link);
        if (entity == nullptr)
        {
            // Deleted during play.
            mWorld.DestroyBody(link.Body);
            mLinks.erase(mLinks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }

        if (!SameFloat3(entity->Transform.Position, link.Position) || !SameFloat3(entity->Transform.Rotation, link.Rotation))
        {
            const float position[3]{ entity->Transform.Position.x, entity->Transform.Position.y, entity->Transform.Position.z };
            float rotation[4];
            EulerToQuaternion(entity->Transform.Rotation, rotation);
            if (link.Motion == RigidBodyMotion::Kinematic)
                mWorld.MoveKinematic(link.Body, position, rotation);
            else
                mWorld.SetBodyTransform(link.Body, position, rotation);
            link.Position = entity->Transform.Position;
            link.Rotation = entity->Transform.Rotation;
        }
        ++i;
    }

    mWorld.Step(deltaSeconds);
    CollectHits();

    for (Link& link : mLinks)
    {
        if (link.Motion != RigidBodyMotion::Dynamic)
            continue;

        const bool active = mWorld.IsBodyActive(link.Body);
        const bool justFellAsleep = link.WasActive && !active;
        link.WasActive = active;
        if (!active && !justFellAsleep)
            continue;

        // Drawn between the last two fixed steps, so motion is smooth at any frame rate.
        Entity* entity = ResolveEntity(link);
        float position[3];
        float rotation[4];
        if (entity == nullptr || !mWorld.GetBodyTransform(link.Body, position, rotation, active))
            continue;

        entity->Transform.Position = XMFLOAT3(position[0], position[1], position[2]);
        entity->Transform.Rotation = QuaternionToEuler(rotation);
        link.Position = entity->Transform.Position;
        link.Rotation = entity->Transform.Rotation;
    }
}

void ScenePhysics::SetPlayerEye(const float eye[3])
{
    if (!IsActive())
        return;

    // Capsule centre: half its height above its bottom, which is kPlayerEyeHeight below the eye.
    const float center[3]{ eye[0], eye[1], eye[2] - kPlayerEyeHeight + 0.5f * kPlayerHeight };
    const float upright[4]{ 0.0f, 0.0f, 0.0f, 1.0f };

    if (!mPlayer.IsValid())
    {
        PhysicsShapeDesc capsule;
        capsule.Type = PhysicsShapeType::Capsule;
        capsule.Radius = kPlayerRadius;
        capsule.HalfHeight = 0.5f * kPlayerHeight - kPlayerRadius;

        PhysicsBodyDesc desc;
        desc.Motion = PhysicsMotionType::Kinematic;
        desc.Layer = PhysicsLayer::Player;
        desc.Shapes = &capsule;
        desc.ShapeCount = 1;
        desc.Position[0] = center[0];
        desc.Position[1] = center[1];
        desc.Position[2] = center[2];
        mPlayer = mWorld.CreateBody(desc);
        mPlayerPlaced = mPlayer.IsValid();
        std::copy(center, center + 3, mLastPlayerCenter);
        return;
    }

    const float dx = center[0] - mLastPlayerCenter[0];
    const float dy = center[1] - mLastPlayerCenter[1];
    const float dz = center[2] - mLastPlayerCenter[2];
    if (!mPlayerPlaced || dx * dx + dy * dy + dz * dz > kPlayerTeleportDistance * kPlayerTeleportDistance)
        mWorld.SetBodyTransform(mPlayer, center, upright, false);
    else
        mWorld.MoveKinematic(mPlayer, center, upright);

    mPlayerPlaced = true;
    std::copy(center, center + 3, mLastPlayerCenter);
}

void ScenePhysics::RemovePlayer()
{
    if (mPlayer.IsValid())
        mWorld.DestroyBody(mPlayer);
    mPlayer = {};
    mPlayerPlaced = false;
}

void ScenePhysics::CollectHits()
{
    mHits.clear();

    const std::uint32_t count = mWorld.GetContacts(nullptr, 0);
    if (count == 0)
        return;
    mContactScratch.resize(count);
    mWorld.GetContacts(mContactScratch.data(), count);

    mHits.reserve(count);
    for (const PhysicsContact& contact : mContactScratch)
    {
        Hit hit;
        hit.EntityA = contact.UserDataA;
        hit.EntityB = contact.UserDataB;
        hit.PlayerInvolved = (mPlayer.IsValid() && (contact.BodyA == mPlayer || contact.BodyB == mPlayer));
        std::copy(contact.Position, contact.Position + 3, hit.Position);
        std::copy(contact.Normal, contact.Normal + 3, hit.Normal);
        hit.ImpactSpeed = contact.ImpactSpeed;
        mHits.push_back(hit);
    }
}

PhysicsBodyHandle ScenePhysics::FindDynamicBody(std::uint64_t entityId) const
{
    if (!IsActive() || entityId == 0)
        return {};
    for (const Link& link : mLinks)
        if (link.EntityId == entityId)
            return link.Motion == RigidBodyMotion::Dynamic ? link.Body : PhysicsBodyHandle{};
    return {};
}

bool ScenePhysics::AddImpulse(std::uint64_t entityId, const float impulse[3])
{
    const PhysicsBodyHandle body = FindDynamicBody(entityId);
    if (!body.IsValid())
        return false;
    mWorld.AddImpulse(body, impulse);
    return true;
}

bool ScenePhysics::AddForce(std::uint64_t entityId, const float force[3])
{
    const PhysicsBodyHandle body = FindDynamicBody(entityId);
    if (!body.IsValid())
        return false;
    mWorld.AddForce(body, force);
    return true;
}

bool ScenePhysics::SetVelocity(std::uint64_t entityId, const float velocity[3])
{
    const PhysicsBodyHandle body = FindDynamicBody(entityId);
    if (!body.IsValid())
        return false;
    mWorld.SetLinearVelocity(body, velocity);
    return true;
}

bool ScenePhysics::GetVelocity(std::uint64_t entityId, float outVelocity[3]) const
{
    if (!IsActive() || entityId == 0)
        return false;
    for (const Link& link : mLinks)
    {
        if (link.EntityId == entityId)
        {
            mWorld.GetLinearVelocity(link.Body, outVelocity);
            return true;
        }
    }
    return false;
}

bool ScenePhysics::SetAngularVelocity(std::uint64_t entityId, const float velocity[3])
{
    const PhysicsBodyHandle body = FindDynamicBody(entityId);
    if (!body.IsValid())
        return false;
    mWorld.SetAngularVelocity(body, velocity);
    return true;
}

bool ScenePhysics::Raycast(const float origin[3], const float direction[3], float maxDistance, RayHit& hit) const
{
    if (!IsActive())
        return false;

    // Callers hand over any direction; the world wants a unit one.
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (!(length > 1.0e-6f))
        return false;
    const float unit[3]{ direction[0] / length, direction[1] / length, direction[2] / length };

    PhysicsRayHit rayHit;
    if (!mWorld.CastRay(origin, unit, maxDistance, rayHit, PhysicsQueryStatic | PhysicsQueryMoving, mPlayer))
        return false;

    hit.Distance = rayHit.Distance;
    std::copy(rayHit.Position, rayHit.Position + 3, hit.Position);
    std::copy(rayHit.Normal, rayHit.Normal + 3, hit.Normal);
    hit.EntityId = rayHit.UserData;
    return true;
}

bool ScenePhysics::TraceMovingBodies(const float origin[3], const float direction[3], float maxDistance,
                                     CharacterTraceHit& hit) const
{
    if (!IsActive() || mLinks.empty())
        return false;

    PhysicsRayHit rayHit;
    if (!mWorld.CastRay(origin, direction, maxDistance, rayHit, PhysicsQueryMoving, mPlayer))
        return false;

    hit.Distance = rayHit.Distance;
    std::copy(rayHit.Position, rayHit.Position + 3, hit.Position);
    std::copy(rayHit.Normal, rayHit.Normal + 3, hit.Normal);
    return true;
}
