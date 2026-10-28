#pragma once

// PhysicsWorld.h - Physics.dll: rigid body simulation for the engine, backed by Jolt Physics.
//
// Jolt never leaves this DLL. Its headers, allocator, job system and math types are all
// private to Physics.dll, so the engine only has to agree with it on the plain data below:
// floats, integers and pointers, no STL and no Jolt types. That keeps the DLL free to be
// built with whatever Jolt configuration it likes (SIMD level, asserts) without the caller
// having to match it.
//
// World space is the engine's: metres, left-handed, Z up. Gravity defaults to -Z.
// Rotations are unit quaternions (x, y, z, w) describing the same linear map that
// DirectXMath's XMQuaternion* functions produce, so XMQuaternionRotationMatrix of an
// entity's rotation can be handed over unchanged.
//
// Typical use for a play session:
//   PhysicsWorld world;
//   world.Initialize(settings, error, sizeof(error));
//   world.CreateBody(...) for every collider and rigid body
//   world.OptimizeBroadPhase();
//   every frame: MoveKinematic(...) for moved kinematic bodies, Step(dt), GetBodyTransform(...)
//   world.Shutdown();

#include <cstddef>
#include <cstdint>

#ifdef PHYSICS_EXPORTS
#define PHYSICS_API __declspec(dllexport)
#else
#define PHYSICS_API __declspec(dllimport)
#endif

// How a body moves. Static bodies never move; kinematic bodies are moved by the caller
// (MoveKinematic) and push dynamic bodies out of the way without being pushed back;
// dynamic bodies are driven by the simulation.
enum class PhysicsMotionType : std::int32_t
{
    Static = 0,
    Kinematic = 1,
    Dynamic = 2,
};

// Which bodies a body collides with and which a query sees.
//   Static  - the level: collides with Moving and Player.
//   Moving  - kinematic and dynamic bodies: collides with everything.
//   Player  - the character's proxy: collides with Moving only, so it shoves crates but
//             never fights the level the character's own movement already handles.
enum class PhysicsLayer : std::int32_t
{
    Static = 0,
    Moving = 1,
    Player = 2,
};

// Query masks for CastRay.
enum PhysicsQueryMask : std::uint32_t
{
    PhysicsQueryStatic = 1u << 0,
    PhysicsQueryMoving = 1u << 1,
    PhysicsQueryPlayer = 1u << 2,
    PhysicsQueryAll = PhysicsQueryStatic | PhysicsQueryMoving | PhysicsQueryPlayer,
};

enum class PhysicsShapeType : std::int32_t
{
    Box = 0,         // HalfExtents
    Sphere = 1,      // Radius
    Capsule = 2,     // Radius + HalfHeight (of the cylinder part), along the local Z axis
    ConvexHull = 3,  // Vertices; the hull of the points is taken, indices are ignored
    TriangleMesh = 4 // Vertices + Indices; static or kinematic bodies only
};

// One collision shape in the body's local space. Several of them make a compound.
struct PhysicsShapeDesc
{
    PhysicsShapeType Type = PhysicsShapeType::Box;

    float HalfExtents[3]{ 0.5f, 0.5f, 0.5f };
    float Radius = 0.5f;
    float HalfHeight = 0.5f;

    // Tightly packed xyz triples, in body-local space with any scale already applied.
    // Borrowed for the duration of CreateBody only.
    const float* Vertices = nullptr;
    std::uint32_t VertexCount = 0;
    // Triangle list into Vertices. Front faces are the side (v1 - v0) x (v2 - v0) points to;
    // Jolt does not collide with back faces, so callers should hand outward-facing triangles.
    const std::uint32_t* Indices = nullptr;
    std::uint32_t IndexCount = 0;

    // Placement of this shape inside the body.
    float LocalPosition[3]{ 0.0f, 0.0f, 0.0f };
    float LocalRotation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
};

// A built, reference-counted collision shape that many bodies can share - e.g. every
// instance of one rock mesh - so the expensive part (a triangle mesh's tree, a hull) is
// built once. Release it with PhysicsWorld::ReleaseShape; bodies keep their own reference.
struct PhysicsShapeHandle
{
    void* Value = nullptr;

    bool IsValid() const { return Value != nullptr; }
};

struct PhysicsBodyDesc
{
    PhysicsMotionType Motion = PhysicsMotionType::Static;
    PhysicsLayer Layer = PhysicsLayer::Static;

    float Position[3]{ 0.0f, 0.0f, 0.0f };
    float Rotation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };

    // Either a shared shape from CreateShape...
    PhysicsShapeHandle Shape;
    // ...or the shapes to build for this body alone. More than one makes a compound.
    const PhysicsShapeDesc* Shapes = nullptr;
    std::uint32_t ShapeCount = 0;
    // Scale applied on top of the shape, per body. Non-uniform and negative (mirrored)
    // scales are fine; spheres and capsules can only scale uniformly and take the closest.
    float Scale[3]{ 1.0f, 1.0f, 1.0f };

    // Dynamic bodies only. Mass in kg; 0 derives it from the shape volume at the density of
    // water (1000 kg/m^3), which is rarely what anyone wants for a big prop.
    float Mass = 0.0f;
    float Friction = 0.5f;
    float Restitution = 0.0f;
    float LinearDamping = 0.05f;
    float AngularDamping = 0.05f;
    float GravityFactor = 1.0f;
    // Continuous collision detection for small, fast bodies so they do not tunnel.
    bool ContinuousCollision = false;
    bool StartAsleep = false;
    // Exerts no forces: other bodies pass through it.
    bool IsSensor = false;

    // Handed back by queries, e.g. the entity index the body stands for.
    std::uint64_t UserData = 0;
};

struct PhysicsBodyHandle
{
    std::uint32_t Value = 0xffffffffu;

    bool IsValid() const { return Value != 0xffffffffu; }
    bool operator==(const PhysicsBodyHandle& other) const { return Value == other.Value; }
    bool operator!=(const PhysicsBodyHandle& other) const { return Value != other.Value; }
};

// A height field on a regular grid in the XY plane, Z up. Sample (column, row) sits at
// world (OriginX + column * CellSize, OriginY + row * CellSize, Heights[row * SampleCount + column]).
// SampleCount must be a power of two of at least 4; heights are absolute world Z.
struct PhysicsHeightFieldDesc
{
    float OriginX = 0.0f;
    float OriginY = 0.0f;
    float CellSize = 1.0f;
    std::uint32_t SampleCount = 0;
    const float* Heights = nullptr;
    float Friction = 0.6f;
    std::uint64_t UserData = 0;
};

struct PhysicsRayHit
{
    float Distance = 0.0f;
    float Position[3]{};
    // Surface normal, flipped to face back along the ray.
    float Normal[3]{ 0.0f, 0.0f, 1.0f };
    PhysicsBodyHandle Body;
    std::uint64_t UserData = 0;
};

// Two bodies that started touching during the latest Step(). One per pair per Step(),
// keeping the hardest impact, so a crate sliding across many triangles of a floor is
// still one hit rather than one per triangle.
struct PhysicsContact
{
    PhysicsBodyHandle BodyA;
    PhysicsBodyHandle BodyB;
    std::uint64_t UserDataA = 0;
    std::uint64_t UserDataB = 0;
    float Position[3]{};
    // From A towards B.
    float Normal[3]{ 0.0f, 0.0f, 1.0f };
    // How fast the two were closing along the normal, in m/s: 0 for a gentle touch,
    // which is what tells an impact sound or damage apart from a body settling.
    float ImpactSpeed = 0.0f;
};

struct PhysicsWorldSettings
{
    float Gravity[3]{ 0.0f, 0.0f, -9.81f };
    std::uint32_t MaxBodies = 65536;
    std::uint32_t MaxBodyPairs = 65536;
    std::uint32_t MaxContactConstraints = 16384;
    // Worker threads for the simulation. 0 picks hardware concurrency - 1, capped at 8.
    std::uint32_t WorkerThreads = 0;
    // The simulation runs at this fixed rate and Step() catches up in whole steps.
    float FixedTimeStep = 1.0f / 60.0f;
    // A long hitch would otherwise make the next frame simulate seconds of catch-up.
    std::uint32_t MaxStepsPerFrame = 4;
};

struct PhysicsWorldStats
{
    std::uint32_t BodyCount = 0;
    std::uint32_t ActiveBodyCount = 0;
    std::uint32_t StepsLastFrame = 0;
    float StepMilliseconds = 0.0f;
};

class PHYSICS_API PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Creates the Jolt physics system. On failure returns false and writes a message.
    bool Initialize(const PhysicsWorldSettings& settings, char* errorBuffer = nullptr, std::size_t errorBufferSize = 0);
    // Destroys every body and the system. Safe to call when not initialised.
    void Shutdown();
    bool IsInitialized() const;

    // Removes every body but keeps the system, for a fresh play session.
    void Clear();

    void SetGravity(const float gravity[3]);

    // Builds a shape to share between bodies (see PhysicsShapeHandle). `motion` decides
    // what a TriangleMesh becomes: a dynamic body cannot use one and gets its hull.
    // Returns an invalid handle when nothing usable could be built.
    PhysicsShapeHandle CreateShape(const PhysicsShapeDesc* shapes, std::uint32_t shapeCount, PhysicsMotionType motion);
    void ReleaseShape(PhysicsShapeHandle shape);

    PhysicsBodyHandle CreateBody(const PhysicsBodyDesc& desc);
    PhysicsBodyHandle CreateHeightField(const PhysicsHeightFieldDesc& desc);
    void DestroyBody(PhysicsBodyHandle body);

    // Call once after adding many bodies at once; it makes the first queries and steps fast.
    void OptimizeBroadPhase();

    // Advances the simulation by deltaSeconds of wall time, in fixed steps.
    void Step(float deltaSeconds);

    // Pose of the body origin (not the centre of mass). The simulation advances in fixed
    // steps, so at any frame rate but the step rate a body only moves on some frames;
    // `interpolated` blends between the last two steps by the time left over, which is
    // what should be drawn. Leave it false for gameplay decisions.
    bool GetBodyTransform(PhysicsBodyHandle body, float outPosition[3], float outRotation[4],
                          bool interpolated = false) const;
    // Teleports the body. Velocities are kept; activate wakes a sleeping body.
    void SetBodyTransform(PhysicsBodyHandle body, const float position[3], const float rotation[4], bool activate = true);
    // Moves a kinematic body so it arrives at the pose by the end of the next Step(), with
    // the velocity that pushes dynamic bodies along the way rather than teleporting
    // through them. The body comes to rest there unless it is moved again.
    void MoveKinematic(PhysicsBodyHandle body, const float position[3], const float rotation[4]);

    bool IsBodyActive(PhysicsBodyHandle body) const;
    PhysicsMotionType GetMotionType(PhysicsBodyHandle body) const;
    std::uint64_t GetUserData(PhysicsBodyHandle body) const;

    void GetLinearVelocity(PhysicsBodyHandle body, float outVelocity[3]) const;
    void SetLinearVelocity(PhysicsBodyHandle body, const float velocity[3]);
    void GetAngularVelocity(PhysicsBodyHandle body, float outVelocity[3]) const;
    void SetAngularVelocity(PhysicsBodyHandle body, const float velocity[3]);
    // Force in N, applied for the next step at the centre of mass.
    void AddForce(PhysicsBodyHandle body, const float force[3]);
    // Impulse in N*s at a world point, or at the centre of mass when point is null.
    void AddImpulse(PhysicsBodyHandle body, const float impulse[3], const float* worldPoint = nullptr);

    // Nearest hit along origin + direction * t, t in [0, maxDistance]. direction must be
    // normalised. `ignore` excludes one body, e.g. the caster's own proxy.
    bool CastRay(const float origin[3], const float direction[3], float maxDistance, PhysicsRayHit& outHit,
                 std::uint32_t queryMask = PhysicsQueryAll, PhysicsBodyHandle ignore = {}) const;

    // Collisions that began during the latest Step(), cleared by the next one. Copies up
    // to maxCount into `out` and returns how many there are in all; pass null to count.
    std::uint32_t GetContacts(PhysicsContact* out, std::uint32_t maxCount) const;

    PhysicsWorldStats GetStats() const;

private:
    bool InitializeSystem(const PhysicsWorldSettings& settings, char* errorBuffer, std::size_t errorBufferSize);

    struct Impl;
    Impl* mImpl = nullptr;
};
