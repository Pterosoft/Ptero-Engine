#include "pch.h"

#include "PhysicsWorld.h"

// Jolt.h has to come before every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace JPH;

namespace
{
    // ---- Jolt process globals --------------------------------------------------------
    // The allocator hooks, the factory and the type registry belong to the process, not to
    // a world, so they are set up by the first world and torn down by the last one.

    std::mutex gJoltGlobalsMutex;
    int gJoltGlobalsUsers = 0;

    void JoltTrace(const char* format, ...)
    {
        char message[1024];
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);

        OutputDebugStringA("[Physics] ");
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }

#ifdef JPH_ENABLE_ASSERTS
    bool JoltAssertFailed(const char* expression, const char* message, const char* file, uint line)
    {
        char text[1024];
        std::snprintf(text, sizeof(text), "[Physics] Jolt assert %s:%u: (%s) %s\n",
            file, line, expression, message != nullptr ? message : "");
        OutputDebugStringA(text);
        // Report and carry on: breaking into the debugger mid play-session is never what
        // the person pressing Play wanted.
        return false;
    }
#endif

    void AcquireJoltGlobals()
    {
        std::lock_guard<std::mutex> lock(gJoltGlobalsMutex);
        if (gJoltGlobalsUsers++ > 0)
            return;

        RegisterDefaultAllocator();
        Trace = JoltTrace;
        JPH_IF_ENABLE_ASSERTS(AssertFailed = JoltAssertFailed;)
        Factory::sInstance = new Factory();
        RegisterTypes();
    }

    void ReleaseJoltGlobals()
    {
        std::lock_guard<std::mutex> lock(gJoltGlobalsMutex);
        if (--gJoltGlobalsUsers > 0)
            return;

        UnregisterTypes();
        delete Factory::sInstance;
        Factory::sInstance = nullptr;
    }

    // ---- Layers --------------------------------------------------------------------------

    namespace ObjectLayers
    {
        constexpr ObjectLayer Static = 0;
        constexpr ObjectLayer Moving = 1;
        constexpr ObjectLayer Player = 2;
        constexpr ObjectLayer Count = 3;
    }

    namespace BroadPhaseLayers
    {
        constexpr BroadPhaseLayer Static(0);
        constexpr BroadPhaseLayer Moving(1);
        constexpr uint Count = 2;
    }

    ObjectLayer ToObjectLayer(PhysicsLayer layer)
    {
        switch (layer)
        {
        case PhysicsLayer::Moving: return ObjectLayers::Moving;
        case PhysicsLayer::Player: return ObjectLayers::Player;
        default:                   return ObjectLayers::Static;
        }
    }

    class BroadPhaseLayerMap final : public BroadPhaseLayerInterface
    {
    public:
        uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }

        BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override
        {
            return layer == ObjectLayers::Static ? BroadPhaseLayers::Static : BroadPhaseLayers::Moving;
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        const char* GetBroadPhaseLayerName(BroadPhaseLayer layer) const override
        {
            return layer == BroadPhaseLayers::Static ? "Static" : "Moving";
        }
#endif
    };

    // Static only meets Moving; Player only meets Moving; Moving meets everything.
    bool LayersCollide(ObjectLayer a, ObjectLayer b)
    {
        if (a == ObjectLayers::Moving || b == ObjectLayers::Moving)
            return true;
        return false;
    }

    class ObjectVsBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter
    {
    public:
        bool ShouldCollide(ObjectLayer layer, BroadPhaseLayer broadPhaseLayer) const override
        {
            if (layer == ObjectLayers::Moving)
                return true;
            return broadPhaseLayer == BroadPhaseLayers::Moving;
        }
    };

    class ObjectPairFilter final : public ObjectLayerPairFilter
    {
    public:
        bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override { return LayersCollide(a, b); }
    };

    // CastRay's query mask, as a Jolt object layer filter.
    class QueryMaskFilter final : public ObjectLayerFilter
    {
    public:
        explicit QueryMaskFilter(std::uint32_t mask) : mMask(mask) {}

        bool ShouldCollide(ObjectLayer layer) const override
        {
            switch (layer)
            {
            case ObjectLayers::Static: return (mMask & PhysicsQueryStatic) != 0;
            case ObjectLayers::Moving: return (mMask & PhysicsQueryMoving) != 0;
            case ObjectLayers::Player: return (mMask & PhysicsQueryPlayer) != 0;
            default:                   return false;
            }
        }

    private:
        std::uint32_t mMask;
    };

    // ---- Conversions ---------------------------------------------------------------------

    Vec3 ToVec3(const float v[3]) { return Vec3(v[0], v[1], v[2]); }

    Quat ToQuat(const float q[4])
    {
        const Quat quat(q[0], q[1], q[2], q[3]);
        // A zero or denormal quaternion from a careless caller would poison the body.
        const float length = quat.Length();
        return length > 1.0e-6f ? quat / length : Quat::sIdentity();
    }

    void StoreVec3(Vec3Arg v, float out[3])
    {
        out[0] = v.GetX();
        out[1] = v.GetY();
        out[2] = v.GetZ();
    }

    void StoreQuat(QuatArg q, float out[4])
    {
        out[0] = q.GetX();
        out[1] = q.GetY();
        out[2] = q.GetZ();
        out[3] = q.GetW();
    }

    BodyID ToBodyId(PhysicsBodyHandle handle) { return BodyID(handle.Value); }
    PhysicsBodyHandle ToHandle(const BodyID& id)
    {
        PhysicsBodyHandle handle;
        handle.Value = id.GetIndexAndSequenceNumber();
        return handle;
    }

    EMotionType ToMotionType(PhysicsMotionType motion)
    {
        switch (motion)
        {
        case PhysicsMotionType::Kinematic: return EMotionType::Kinematic;
        case PhysicsMotionType::Dynamic:   return EMotionType::Dynamic;
        default:                           return EMotionType::Static;
        }
    }

    // A convex radius rounds a shape's corners and is what keeps convex-convex collision
    // fast and stable, but it may not exceed the shape itself.
    float ConvexRadiusFor(float smallestHalfExtent)
    {
        return (std::min)(cDefaultConvexRadius, 0.5f * smallestHalfExtent);
    }

    std::vector<Vec3> CollectPoints(const PhysicsShapeDesc& desc)
    {
        std::vector<Vec3> points;
        if (desc.Vertices == nullptr)
            return points;
        points.reserve(desc.VertexCount);
        for (std::uint32_t i = 0; i < desc.VertexCount; ++i)
        {
            const float* p = desc.Vertices + static_cast<std::size_t>(i) * 3;
            if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
                points.emplace_back(p[0], p[1], p[2]);
        }
        return points;
    }

    // The box that bounds a point cloud, for hulls too flat or too small to be one.
    ShapeRefC BoundingBoxShape(const std::vector<Vec3>& points)
    {
        if (points.empty())
            return nullptr;

        Vec3 minimum = points.front();
        Vec3 maximum = points.front();
        for (const Vec3& point : points)
        {
            minimum = Vec3::sMin(minimum, point);
            maximum = Vec3::sMax(maximum, point);
        }

        // A flat hull still needs some thickness to collide with.
        const Vec3 halfExtent = Vec3::sMax(0.5f * (maximum - minimum), Vec3::sReplicate(0.01f));
        BoxShapeSettings box(halfExtent, ConvexRadiusFor(halfExtent.ReduceMin()));
        const ShapeSettings::ShapeResult boxResult = box.Create();
        if (boxResult.HasError())
            return nullptr;

        const Vec3 center = 0.5f * (minimum + maximum);
        if (center.IsNearZero())
            return boxResult.Get();

        RotatedTranslatedShapeSettings placed(center, Quat::sIdentity(), boxResult.Get());
        const ShapeSettings::ShapeResult placedResult = placed.Create();
        return placedResult.HasError() ? nullptr : placedResult.Get();
    }

    ShapeRefC BuildConvexHull(const PhysicsShapeDesc& desc)
    {
        const std::vector<Vec3> points = CollectPoints(desc);
        if (points.size() < 4)
            return BoundingBoxShape(points);

        // Jolt caps a hull's vertex count; it simplifies anything larger itself.
        ConvexHullShapeSettings hull(points.data(), static_cast<int>(points.size()));
        const ShapeSettings::ShapeResult result = hull.Create();
        if (result.HasError())
        {
            // Coplanar or degenerate input, e.g. a decal quad or a single wall card.
            return BoundingBoxShape(points);
        }
        return result.Get();
    }

    ShapeRefC BuildTriangleMesh(const PhysicsShapeDesc& desc)
    {
        if (desc.Vertices == nullptr || desc.Indices == nullptr || desc.IndexCount < 3)
            return nullptr;

        VertexList vertices;
        vertices.reserve(desc.VertexCount);
        for (std::uint32_t i = 0; i < desc.VertexCount; ++i)
        {
            const float* p = desc.Vertices + static_cast<std::size_t>(i) * 3;
            vertices.push_back(Float3(p[0], p[1], p[2]));
        }

        IndexedTriangleList triangles;
        triangles.reserve(desc.IndexCount / 3);
        for (std::uint32_t i = 0; i + 2 < desc.IndexCount; i += 3)
        {
            const std::uint32_t a = desc.Indices[i];
            const std::uint32_t b = desc.Indices[i + 1];
            const std::uint32_t c = desc.Indices[i + 2];
            if (a >= desc.VertexCount || b >= desc.VertexCount || c >= desc.VertexCount)
                continue;
            if (a == b || b == c || a == c)
                continue;
            triangles.push_back(IndexedTriangle(a, b, c));
        }

        if (triangles.empty())
            return nullptr;

        MeshShapeSettings mesh(std::move(vertices), std::move(triangles));
        const ShapeSettings::ShapeResult result = mesh.Create();
        if (result.HasError())
        {
            Trace("Triangle mesh shape failed: %s", result.GetError().c_str());
            return nullptr;
        }
        return result.Get();
    }

    // One shape at the body origin, before its LocalPosition / LocalRotation.
    ShapeRefC BuildBareShape(const PhysicsShapeDesc& desc, bool dynamic)
    {
        switch (desc.Type)
        {
        case PhysicsShapeType::Box:
        {
            const Vec3 halfExtent = Vec3::sMax(ToVec3(desc.HalfExtents), Vec3::sReplicate(0.01f));
            BoxShapeSettings box(halfExtent, ConvexRadiusFor(halfExtent.ReduceMin()));
            const ShapeSettings::ShapeResult result = box.Create();
            return result.HasError() ? nullptr : result.Get();
        }
        case PhysicsShapeType::Sphere:
        {
            SphereShapeSettings sphere((std::max)(desc.Radius, 0.01f));
            const ShapeSettings::ShapeResult result = sphere.Create();
            return result.HasError() ? nullptr : result.Get();
        }
        case PhysicsShapeType::Capsule:
        {
            // Jolt's capsule runs along its Y axis; the engine's along Z (up).
            CapsuleShapeSettings capsule((std::max)(desc.HalfHeight, 0.001f), (std::max)(desc.Radius, 0.01f));
            const ShapeSettings::ShapeResult capsuleResult = capsule.Create();
            if (capsuleResult.HasError())
                return nullptr;
            RotatedTranslatedShapeSettings upright(Vec3::sZero(),
                Quat::sRotation(Vec3::sAxisX(), 0.5f * JPH_PI), capsuleResult.Get());
            const ShapeSettings::ShapeResult result = upright.Create();
            return result.HasError() ? nullptr : result.Get();
        }
        case PhysicsShapeType::ConvexHull:
            return BuildConvexHull(desc);
        case PhysicsShapeType::TriangleMesh:
            // A triangle soup has no volume, so it has no mass or inertia, and Jolt cannot
            // collide it with other meshes: a dynamic body gets its hull instead.
            return dynamic ? BuildConvexHull(desc) : BuildTriangleMesh(desc);
        }
        return nullptr;
    }

    bool IsIdentityPlacement(const PhysicsShapeDesc& desc)
    {
        const Vec3 position = ToVec3(desc.LocalPosition);
        const Quat rotation = ToQuat(desc.LocalRotation);
        return position.IsNearZero() && rotation.IsClose(Quat::sIdentity());
    }

    ShapeRefC BuildShapeList(const PhysicsShapeDesc* shapes, std::uint32_t shapeCount, bool dynamic)
    {
        if (shapes == nullptr || shapeCount == 0)
            return nullptr;

        if (shapeCount == 1)
        {
            const PhysicsShapeDesc& only = shapes[0];
            ShapeRefC shape = BuildBareShape(only, dynamic);
            if (shape == nullptr || IsIdentityPlacement(only))
                return shape;

            RotatedTranslatedShapeSettings placed(ToVec3(only.LocalPosition), ToQuat(only.LocalRotation), shape);
            const ShapeSettings::ShapeResult result = placed.Create();
            return result.HasError() ? nullptr : result.Get();
        }

        StaticCompoundShapeSettings compound;
        int added = 0;
        for (std::uint32_t i = 0; i < shapeCount; ++i)
        {
            const PhysicsShapeDesc& part = shapes[i];
            ShapeRefC shape = BuildBareShape(part, dynamic);
            if (shape == nullptr)
                continue;
            compound.AddShape(ToVec3(part.LocalPosition), ToQuat(part.LocalRotation), shape);
            ++added;
        }

        if (added == 0)
            return nullptr;

        const ShapeSettings::ShapeResult result = compound.Create();
        if (result.HasError())
        {
            Trace("Compound shape failed: %s", result.GetError().c_str());
            return nullptr;
        }
        return result.Get();
    }

    // The per-body scale on top of a (possibly shared) shape. Jolt bakes it into the shape
    // where it can and wraps it otherwise; spheres and capsules take the nearest uniform scale.
    ShapeRefC ApplyScale(const ShapeRefC& shape, const float scale[3])
    {
        // A zero axis would collapse the shape and trip Jolt's asserts.
        Vec3 safeScale = ToVec3(scale);
        for (int axis = 0; axis < 3; ++axis)
        {
            const float value = safeScale[axis];
            if (!std::isfinite(value) || std::abs(value) < 1.0e-3f)
                safeScale.SetComponent(axis, value < 0.0f ? -1.0e-3f : 1.0e-3f);
        }

        if (safeScale.IsClose(Vec3::sOne(), 1.0e-10f))
            return shape;

        const Shape::ShapeResult result = shape->ScaleShape(safeScale);
        if (!result.HasError())
            return result.Get();
        return new ScaledShape(shape, shape->MakeScaleValid(safeScale));
    }

    // Jolt cannot give a triangle mesh mass or collide it with other meshes.
    bool ContainsMesh(const Shape* shape)
    {
        if (shape->GetSubType() == EShapeSubType::Mesh || shape->GetSubType() == EShapeSubType::HeightField)
            return true;
        if (shape->GetType() == EShapeType::Decorated)
            return ContainsMesh(static_cast<const DecoratedShape*>(shape)->GetInnerShape());
        if (shape->GetType() == EShapeType::Compound)
        {
            const CompoundShape* compound = static_cast<const CompoundShape*>(shape);
            for (const CompoundShape::SubShape& part : compound->GetSubShapes())
                if (ContainsMesh(part.mShape))
                    return true;
        }
        return false;
    }

    // Collects the contacts that begin during a step. Jolt calls it from its worker
    // threads, hence the lock; pairs are merged so each reports once per Step().
    class ContactCollector final : public ContactListener
    {
    public:
        void OnContactAdded(const Body& body1, const Body& body2, const ContactManifold& manifold,
                            ContactSettings&) override
        {
            const RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
            const Vec3 normal = manifold.mWorldSpaceNormal; // from body 1 towards body 2
            const Vec3 closing = body2.GetPointVelocity(point) - body1.GetPointVelocity(point);
            const float impactSpeed = (std::max)(0.0f, -closing.Dot(normal));

            PhysicsContact contact;
            contact.BodyA = ToHandle(body1.GetID());
            contact.BodyB = ToHandle(body2.GetID());
            contact.UserDataA = body1.GetUserData();
            contact.UserDataB = body2.GetUserData();
            StoreVec3(Vec3(point), contact.Position);
            StoreVec3(normal, contact.Normal);
            contact.ImpactSpeed = impactSpeed;

            const std::uint64_t pair = (static_cast<std::uint64_t>(contact.BodyA.Value) << 32) | contact.BodyB.Value;

            std::lock_guard<std::mutex> lock(mMutex);
            const auto found = mPairIndex.find(pair);
            if (found != mPairIndex.end())
            {
                if (impactSpeed > mContacts[found->second].ImpactSpeed)
                    mContacts[found->second] = contact;
                return;
            }
            // A collapsing tower of thousands of crates would otherwise grow this without
            // bound; nobody reacts to more hits than this in one frame.
            if (mContacts.size() >= kMaxContactsPerStep)
                return;
            mPairIndex.emplace(pair, mContacts.size());
            mContacts.push_back(contact);
        }

        void Reset()
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mContacts.clear();
            mPairIndex.clear();
        }

        // Only read between steps, when no worker is adding.
        const std::vector<PhysicsContact>& Contacts() const { return mContacts; }

    private:
        static constexpr std::size_t kMaxContactsPerStep = 4096;

        std::mutex mMutex;
        std::vector<PhysicsContact> mContacts;
        std::unordered_map<std::uint64_t, std::size_t> mPairIndex;
    };

    void WriteError(char* buffer, std::size_t size, const char* message)
    {
        if (buffer != nullptr && size > 0)
            std::snprintf(buffer, size, "%s", message);
    }
}

struct PhysicsWorld::Impl
{
    PhysicsWorldSettings Settings;

    BroadPhaseLayerMap BroadPhaseLayers;
    ObjectVsBroadPhaseFilter ObjectVsBroadPhase;
    ObjectPairFilter ObjectPairs;
    ContactCollector Contacts;

    std::unique_ptr<TempAllocatorImpl> TempAllocator;
    std::unique_ptr<JobSystemThreadPool> JobSystem;
    std::unique_ptr<PhysicsSystem> System;

    float Accumulator = 0.0f;

    // Kinematic bodies waiting to be carried to a pose over the next Step().
    struct KinematicTarget
    {
        RVec3 Position;
        Quat Rotation;
    };
    std::unordered_map<std::uint32_t, KinematicTarget> KinematicTargets;

    // Poses of the bodies that were moving, taken just before the latest step, for
    // GetBodyTransform's interpolation.
    struct Pose
    {
        RVec3 Position;
        Quat Rotation;
    };
    std::unordered_map<std::uint32_t, Pose> PreviousPoses;
    BodyIDVector ActiveBodies;

    PhysicsWorldStats Stats;

    BodyInterface& Bodies() { return System->GetBodyInterface(); }
    const BodyInterface& Bodies() const { return System->GetBodyInterface(); }
};

PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
}

bool PhysicsWorld::Initialize(const PhysicsWorldSettings& settings, char* errorBuffer, std::size_t errorBufferSize)
{
    Shutdown();

    AcquireJoltGlobals();
    return InitializeSystem(settings, errorBuffer, errorBufferSize);
}

bool PhysicsWorld::InitializeSystem(const PhysicsWorldSettings& settings, char* errorBuffer, std::size_t errorBufferSize)
try
{
    auto impl = std::make_unique<Impl>();
    impl->Settings = settings;
    impl->Settings.FixedTimeStep = (std::clamp)(settings.FixedTimeStep, 1.0f / 480.0f, 1.0f / 15.0f);
    impl->Settings.MaxStepsPerFrame = (std::max)(1u, settings.MaxStepsPerFrame);

    // Scratch memory for one step; Jolt falls back to the heap with a warning if a huge
    // pile-up ever needs more.
    impl->TempAllocator = std::make_unique<TempAllocatorImpl>(32 * 1024 * 1024);

    std::uint32_t threads = settings.WorkerThreads;
    if (threads == 0)
    {
        const unsigned hardware = std::thread::hardware_concurrency();
        threads = (std::clamp)(hardware > 1 ? hardware - 1 : 1u, 1u, 8u);
    }
    impl->JobSystem = std::make_unique<JobSystemThreadPool>(cMaxPhysicsJobs, cMaxPhysicsBarriers, static_cast<int>(threads));

    impl->System = std::make_unique<PhysicsSystem>();
    impl->System->Init(
        (std::max)(settings.MaxBodies, 16u),
        0, // default number of body mutexes
        (std::max)(settings.MaxBodyPairs, 16u),
        (std::max)(settings.MaxContactConstraints, 16u),
        impl->BroadPhaseLayers,
        impl->ObjectVsBroadPhase,
        impl->ObjectPairs);
    impl->System->SetGravity(ToVec3(settings.Gravity));
    impl->System->SetContactListener(&impl->Contacts);

    mImpl = impl.release();
    return true;
}
catch (const std::exception& exception)
{
    // Out of memory for the scratch allocator, or the worker threads could not start.
    char message[512];
    std::snprintf(message, sizeof(message), "Jolt physics could not be initialised: %s", exception.what());
    WriteError(errorBuffer, errorBufferSize, message);
    ReleaseJoltGlobals();
    return false;
}

void PhysicsWorld::Shutdown()
{
    if (mImpl == nullptr)
        return;

    Clear();
    // The system first: it may still reference the job system's threads.
    mImpl->System.reset();
    mImpl->JobSystem.reset();
    mImpl->TempAllocator.reset();
    delete mImpl;
    mImpl = nullptr;

    ReleaseJoltGlobals();
}

bool PhysicsWorld::IsInitialized() const
{
    return mImpl != nullptr;
}

void PhysicsWorld::Clear()
{
    if (mImpl == nullptr)
        return;

    BodyIDVector bodies;
    mImpl->System->GetBodies(bodies);
    if (!bodies.empty())
    {
        BodyInterface& bodyInterface = mImpl->Bodies();
        // RemoveBodies asserts on bodies that were never added; all of ours were.
        bodyInterface.RemoveBodies(bodies.data(), static_cast<int>(bodies.size()));
        bodyInterface.DestroyBodies(bodies.data(), static_cast<int>(bodies.size()));
    }

    mImpl->KinematicTargets.clear();
    mImpl->PreviousPoses.clear();
    mImpl->Contacts.Reset();
    mImpl->Accumulator = 0.0f;
    mImpl->Stats = {};
}

void PhysicsWorld::SetGravity(const float gravity[3])
{
    if (mImpl != nullptr)
        mImpl->System->SetGravity(ToVec3(gravity));
}

PhysicsShapeHandle PhysicsWorld::CreateShape(const PhysicsShapeDesc* shapes, std::uint32_t shapeCount, PhysicsMotionType motion)
{
    if (mImpl == nullptr)
        return {};

    const ShapeRefC shape = BuildShapeList(shapes, shapeCount, motion == PhysicsMotionType::Dynamic);
    if (shape == nullptr)
        return {};

    // The handle owns one reference until ReleaseShape.
    shape->AddRef();
    PhysicsShapeHandle handle;
    handle.Value = const_cast<Shape*>(shape.GetPtr());
    return handle;
}

void PhysicsWorld::ReleaseShape(PhysicsShapeHandle shape)
{
    if (shape.IsValid())
        static_cast<const Shape*>(shape.Value)->Release();
}

PhysicsBodyHandle PhysicsWorld::CreateBody(const PhysicsBodyDesc& desc)
{
    if (mImpl == nullptr)
        return {};

    const bool dynamic = desc.Motion == PhysicsMotionType::Dynamic;
    ShapeRefC shape = desc.Shape.IsValid()
        ? ShapeRefC(static_cast<const Shape*>(desc.Shape.Value))
        : BuildShapeList(desc.Shapes, desc.ShapeCount, dynamic);
    if (shape == nullptr)
        return {};

    if (dynamic && ContainsMesh(shape))
    {
        Trace("A dynamic body cannot use a triangle mesh or height field shape; build its shape as dynamic.");
        return {};
    }

    shape = ApplyScale(shape, desc.Scale);

    BodyCreationSettings settings(shape, RVec3(ToVec3(desc.Position)), ToQuat(desc.Rotation),
        ToMotionType(desc.Motion), ToObjectLayer(desc.Layer));
    settings.mFriction = (std::max)(desc.Friction, 0.0f);
    settings.mRestitution = (std::clamp)(desc.Restitution, 0.0f, 1.0f);
    settings.mLinearDamping = (std::max)(desc.LinearDamping, 0.0f);
    settings.mAngularDamping = (std::max)(desc.AngularDamping, 0.0f);
    settings.mGravityFactor = desc.GravityFactor;
    settings.mIsSensor = desc.IsSensor;
    settings.mUserData = desc.UserData;
    if (desc.ContinuousCollision)
        settings.mMotionQuality = EMotionQuality::LinearCast;

    if (desc.Motion == PhysicsMotionType::Dynamic && desc.Mass > 0.0f)
    {
        settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.Mass;
    }
    else if (desc.Motion == PhysicsMotionType::Kinematic)
    {
        // A kinematic body never responds to forces, so its mass only matters to Jolt's
        // bookkeeping - and a triangle mesh has none it could calculate.
        settings.mOverrideMassProperties = EOverrideMassProperties::MassAndInertiaProvided;
        settings.mMassPropertiesOverride.mMass = 1.0f;
        settings.mMassPropertiesOverride.mInertia = Mat44::sIdentity();
    }

    const EActivation activation =
        (desc.Motion == PhysicsMotionType::Static || desc.StartAsleep) ? EActivation::DontActivate : EActivation::Activate;

    const BodyID id = mImpl->Bodies().CreateAndAddBody(settings, activation);
    if (id.IsInvalid())
    {
        Trace("Body could not be created: MaxBodies (%u) reached.", mImpl->Settings.MaxBodies);
        return {};
    }
    return ToHandle(id);
}

PhysicsBodyHandle PhysicsWorld::CreateHeightField(const PhysicsHeightFieldDesc& desc)
{
    if (mImpl == nullptr || desc.Heights == nullptr || desc.SampleCount < 4 || desc.CellSize <= 0.0f)
        return {};
    // Jolt wants SampleCount / BlockSize (2) to be at least 2 and is best with powers of two.
    if ((desc.SampleCount & (desc.SampleCount - 1)) != 0)
        return {};

    // Jolt height fields are Y-up: sample (x, y) lies at offset + scale * (x, height, y).
    // A +90 degree turn about X takes local (x, h, z) to world (x, -z, h), so the field
    // is laid out with its local Z running towards world -Y from the far (max Y) edge.
    const std::uint32_t count = desc.SampleCount;
    std::vector<float> samples(static_cast<std::size_t>(count) * count);
    for (std::uint32_t row = 0; row < count; ++row)
    {
        const std::uint32_t joltRow = count - 1 - row;
        const float* source = desc.Heights + static_cast<std::size_t>(row) * count;
        std::copy(source, source + count, samples.begin() + static_cast<std::size_t>(joltRow) * count);
    }

    HeightFieldShapeSettings field(samples.data(), Vec3::sZero(), Vec3(desc.CellSize, 1.0f, desc.CellSize), count);
    const ShapeSettings::ShapeResult result = field.Create();
    if (result.HasError())
    {
        Trace("Height field shape failed: %s", result.GetError().c_str());
        return {};
    }

    const float farEdgeY = desc.OriginY + static_cast<float>(count - 1) * desc.CellSize;
    BodyCreationSettings settings(result.Get(), RVec3(desc.OriginX, farEdgeY, 0.0f),
        Quat::sRotation(Vec3::sAxisX(), 0.5f * JPH_PI), EMotionType::Static, ObjectLayers::Static);
    settings.mFriction = (std::max)(desc.Friction, 0.0f);
    settings.mUserData = desc.UserData;

    const BodyID id = mImpl->Bodies().CreateAndAddBody(settings, EActivation::DontActivate);
    return id.IsInvalid() ? PhysicsBodyHandle{} : ToHandle(id);
}

void PhysicsWorld::DestroyBody(PhysicsBodyHandle body)
{
    if (mImpl == nullptr || !body.IsValid())
        return;

    BodyInterface& bodyInterface = mImpl->Bodies();
    const BodyID id = ToBodyId(body);
    if (!bodyInterface.IsAdded(id))
        return;
    bodyInterface.RemoveBody(id);
    bodyInterface.DestroyBody(id);
    mImpl->KinematicTargets.erase(body.Value);
    mImpl->PreviousPoses.erase(body.Value);
}

void PhysicsWorld::OptimizeBroadPhase()
{
    if (mImpl != nullptr)
        mImpl->System->OptimizeBroadPhase();
}

void PhysicsWorld::Step(float deltaSeconds)
{
    if (mImpl == nullptr)
        return;

    const auto start = std::chrono::steady_clock::now();
    const float fixedStep = mImpl->Settings.FixedTimeStep;
    const std::uint32_t maxSteps = mImpl->Settings.MaxStepsPerFrame;

    if (std::isfinite(deltaSeconds) && deltaSeconds > 0.0f)
        mImpl->Accumulator += deltaSeconds;

    // Every Step() reports only its own collisions, including one that ran no fixed step.
    mImpl->Contacts.Reset();

    std::uint32_t steps = static_cast<std::uint32_t>(mImpl->Accumulator / fixedStep);
    if (steps > maxSteps)
    {
        // Falling behind (a hitch, a breakpoint): simulate what we can and let the rest go
        // rather than spiralling into ever longer catch-up frames.
        steps = maxSteps;
        mImpl->Accumulator = static_cast<float>(steps) * fixedStep;
    }

    BodyInterface& bodyInterface = mImpl->Bodies();
    for (std::uint32_t step = 0; step < steps; ++step)
    {
        // Spread each kinematic move over the steps left this frame, so the body arrives
        // at its target on the last one instead of overshooting it.
        const float remaining = static_cast<float>(steps - step) * fixedStep;
        for (const auto& [value, target] : mImpl->KinematicTargets)
        {
            PhysicsBodyHandle handle;
            handle.Value = value;
            bodyInterface.MoveKinematic(ToBodyId(handle), target.Position, target.Rotation, remaining);
        }

        if (step + 1 == steps)
        {
            mImpl->PreviousPoses.clear();
            mImpl->System->GetActiveBodies(EBodyType::RigidBody, mImpl->ActiveBodies);
            for (const BodyID& id : mImpl->ActiveBodies)
            {
                Impl::Pose pose;
                bodyInterface.GetPositionAndRotation(id, pose.Position, pose.Rotation);
                mImpl->PreviousPoses.emplace(id.GetIndexAndSequenceNumber(), pose);
            }
        }

        mImpl->System->Update(fixedStep, 1, mImpl->TempAllocator.get(), mImpl->JobSystem.get());
        mImpl->Accumulator -= fixedStep;
    }

    if (steps > 0)
    {
        // Arrived: stop there until moved again. A kinematic body otherwise keeps its
        // velocity forever and drifts away.
        for (const auto& [value, target] : mImpl->KinematicTargets)
        {
            PhysicsBodyHandle handle;
            handle.Value = value;
            bodyInterface.SetLinearAndAngularVelocity(ToBodyId(handle), Vec3::sZero(), Vec3::sZero());
        }
        mImpl->KinematicTargets.clear();
    }

    mImpl->Stats.StepsLastFrame = steps;
    mImpl->Stats.StepMilliseconds =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool PhysicsWorld::GetBodyTransform(PhysicsBodyHandle body, float outPosition[3], float outRotation[4],
                                    bool interpolated) const
{
    if (mImpl == nullptr || !body.IsValid())
        return false;

    const BodyID id = ToBodyId(body);
    const BodyInterface& bodyInterface = mImpl->Bodies();
    if (!bodyInterface.IsAdded(id))
        return false;

    RVec3 position;
    Quat rotation;
    bodyInterface.GetPositionAndRotation(id, position, rotation);

    if (interpolated)
    {
        const auto previous = mImpl->PreviousPoses.find(body.Value);
        if (previous != mImpl->PreviousPoses.end())
        {
            // The accumulator holds the time since the latest step, so this pose is that
            // far between the step before it and the latest one: one step behind real time,
            // which is the price of never showing a body somewhere it has not been.
            const float alpha = (std::clamp)(mImpl->Accumulator / mImpl->Settings.FixedTimeStep, 0.0f, 1.0f);
            position = previous->second.Position + (position - previous->second.Position) * alpha;
            rotation = previous->second.Rotation.SLERP(rotation, alpha).Normalized();
        }
    }

    StoreVec3(Vec3(position), outPosition);
    StoreQuat(rotation, outRotation);
    return true;
}

void PhysicsWorld::SetBodyTransform(PhysicsBodyHandle body, const float position[3], const float rotation[4], bool activate)
{
    if (mImpl == nullptr || !body.IsValid())
        return;

    mImpl->KinematicTargets.erase(body.Value);
    // A teleport must not be drawn sliding there from the old pose.
    mImpl->PreviousPoses.erase(body.Value);
    mImpl->Bodies().SetPositionAndRotation(ToBodyId(body), RVec3(ToVec3(position)), ToQuat(rotation),
        activate ? EActivation::Activate : EActivation::DontActivate);
}

void PhysicsWorld::MoveKinematic(PhysicsBodyHandle body, const float position[3], const float rotation[4])
{
    if (mImpl == nullptr || !body.IsValid())
        return;

    mImpl->KinematicTargets[body.Value] = Impl::KinematicTarget{ RVec3(ToVec3(position)), ToQuat(rotation) };
}

bool PhysicsWorld::IsBodyActive(PhysicsBodyHandle body) const
{
    return mImpl != nullptr && body.IsValid() && mImpl->Bodies().IsActive(ToBodyId(body));
}

PhysicsMotionType PhysicsWorld::GetMotionType(PhysicsBodyHandle body) const
{
    if (mImpl == nullptr || !body.IsValid())
        return PhysicsMotionType::Static;

    switch (mImpl->Bodies().GetMotionType(ToBodyId(body)))
    {
    case EMotionType::Kinematic: return PhysicsMotionType::Kinematic;
    case EMotionType::Dynamic:   return PhysicsMotionType::Dynamic;
    default:                     return PhysicsMotionType::Static;
    }
}

std::uint64_t PhysicsWorld::GetUserData(PhysicsBodyHandle body) const
{
    return (mImpl != nullptr && body.IsValid()) ? mImpl->Bodies().GetUserData(ToBodyId(body)) : 0;
}

void PhysicsWorld::GetLinearVelocity(PhysicsBodyHandle body, float outVelocity[3]) const
{
    const Vec3 velocity = (mImpl != nullptr && body.IsValid())
        ? mImpl->Bodies().GetLinearVelocity(ToBodyId(body)) : Vec3::sZero();
    StoreVec3(velocity, outVelocity);
}

void PhysicsWorld::SetLinearVelocity(PhysicsBodyHandle body, const float velocity[3])
{
    if (mImpl != nullptr && body.IsValid())
        mImpl->Bodies().SetLinearVelocity(ToBodyId(body), ToVec3(velocity));
}

void PhysicsWorld::GetAngularVelocity(PhysicsBodyHandle body, float outVelocity[3]) const
{
    const Vec3 velocity = (mImpl != nullptr && body.IsValid())
        ? mImpl->Bodies().GetAngularVelocity(ToBodyId(body)) : Vec3::sZero();
    StoreVec3(velocity, outVelocity);
}

void PhysicsWorld::SetAngularVelocity(PhysicsBodyHandle body, const float velocity[3])
{
    if (mImpl != nullptr && body.IsValid())
        mImpl->Bodies().SetAngularVelocity(ToBodyId(body), ToVec3(velocity));
}

void PhysicsWorld::AddForce(PhysicsBodyHandle body, const float force[3])
{
    if (mImpl != nullptr && body.IsValid())
        mImpl->Bodies().AddForce(ToBodyId(body), ToVec3(force), EActivation::Activate);
}

void PhysicsWorld::AddImpulse(PhysicsBodyHandle body, const float impulse[3], const float* worldPoint)
{
    if (mImpl == nullptr || !body.IsValid())
        return;

    BodyInterface& bodyInterface = mImpl->Bodies();
    const BodyID id = ToBodyId(body);
    if (worldPoint != nullptr)
        bodyInterface.AddImpulse(id, ToVec3(impulse), RVec3(ToVec3(worldPoint)));
    else
        bodyInterface.AddImpulse(id, ToVec3(impulse));
    // An impulse on a sleeping body would otherwise wait for something else to wake it.
    bodyInterface.ActivateBody(id);
}

bool PhysicsWorld::CastRay(const float origin[3], const float direction[3], float maxDistance, PhysicsRayHit& outHit,
                           std::uint32_t queryMask, PhysicsBodyHandle ignore) const
{
    if (mImpl == nullptr || !(maxDistance > 0.0f) || queryMask == 0)
        return false;

    const Vec3 rayDirection = ToVec3(direction);
    const RRayCast ray(RVec3(ToVec3(origin)), rayDirection * maxDistance);

    // Like the engine's own raycaster: meshes are hit from either side, because imported
    // geometry cannot be trusted to have consistent winding.
    RayCastSettings settings;
    settings.SetBackFaceMode(EBackFaceMode::CollideWithBackFaces);
    settings.mTreatConvexAsSolid = true;

    ClosestHitCollisionCollector<CastRayCollector> collector;
    const QueryMaskFilter layerFilter(queryMask);
    IgnoreSingleBodyFilter ignoreFilter(ToBodyId(ignore));
    const BodyFilter acceptAll;
    const BodyFilter& bodyFilter = ignore.IsValid() ? static_cast<const BodyFilter&>(ignoreFilter) : acceptAll;

    mImpl->System->GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layerFilter, bodyFilter);
    if (!collector.HadHit())
        return false;

    const RayCastResult& hit = collector.mHit;
    const RVec3 position = ray.GetPointOnRay(hit.mFraction);

    Vec3 normal = Vec3::sAxisZ();
    {
        BodyLockRead lock(mImpl->System->GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
        {
            const Body& body = lock.GetBody();
            normal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, position);
            outHit.UserData = body.GetUserData();
        }
    }
    if (normal.Dot(rayDirection) > 0.0f)
        normal = -normal;

    outHit.Distance = hit.mFraction * maxDistance;
    StoreVec3(Vec3(position), outHit.Position);
    StoreVec3(normal, outHit.Normal);
    outHit.Body = ToHandle(hit.mBodyID);
    return true;
}

std::uint32_t PhysicsWorld::GetContacts(PhysicsContact* out, std::uint32_t maxCount) const
{
    if (mImpl == nullptr)
        return 0;

    const std::vector<PhysicsContact>& contacts = mImpl->Contacts.Contacts();
    if (out != nullptr)
        std::copy_n(contacts.begin(), (std::min)(static_cast<std::size_t>(maxCount), contacts.size()), out);
    return static_cast<std::uint32_t>(contacts.size());
}

PhysicsWorldStats PhysicsWorld::GetStats() const
{
    if (mImpl == nullptr)
        return {};

    PhysicsWorldStats stats = mImpl->Stats;
    stats.BodyCount = mImpl->System->GetNumBodies();
    stats.ActiveBodyCount = mImpl->System->GetNumActiveBodies(EBodyType::RigidBody);
    return stats;
}
