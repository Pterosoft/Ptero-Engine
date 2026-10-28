#pragma once

// ScenePhysics
// ------------
// The level as the rigid body simulation (Physics.dll, Jolt) sees it during a play session.
//
// Built once when play starts, from the entities as they are at that moment:
//   - every mesh entity becomes a static collider (its collision hulls when the asset has
//     them, its exact finest-LOD triangles otherwise), shared between instances of a mesh;
//   - an entity with a RigidBodyComponent gets the shape and motion it asks for;
//   - each terrain patch becomes a height field.
// Water and vegetation-area volumes are not surfaces and are skipped, as PlayerCollision
// skips them.
//
// Each frame Update() carries kinematic bodies to wherever the game put their entities,
// steps the simulation, and writes the dynamic bodies' poses back into their entities -
// which is all the renderer needs to draw them moving. The play session runs on a copy of
// the level, so none of that is ever saved.
//
// The player is not a physics body in the usual sense: CharacterMovement moves itself with
// rays against PlayerCollision. A kinematic capsule follows the player's eye so it shoves
// dynamic bodies out of the way, and TraceMovingBodies() lets the character's rays find
// the bodies PlayerCollision cannot know about, so a crate can be stood on.

#include "Components.h"
#include "PhysicsWorld.h"
#include "System/CharacterMovement.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

class TerrainRenderer;

class ScenePhysics
{
public:
    ScenePhysics() = default;
    ~ScenePhysics();

    ScenePhysics(const ScenePhysics&) = delete;
    ScenePhysics& operator=(const ScenePhysics&) = delete;

    // Replaces whatever was built before. Returns false when Physics.dll could not start;
    // the session then plays without rigid bodies.
    bool Build(std::vector<Entity>& entities, const TerrainRenderer& terrain);
    void Clear();

    bool IsActive() const { return mWorld.IsInitialized() && mEntities != nullptr; }

    void Update(float deltaSeconds);

    // Moves the player's proxy capsule so that its top sits just above `eye`.
    void SetPlayerEye(const float eye[3]);
    void RemovePlayer();

    // Nearest dynamic or kinematic body along the ray, ignoring the player's own proxy.
    // Same contract as CharacterTraceFn.
    bool TraceMovingBodies(const float origin[3], const float direction[3], float maxDistance,
                           CharacterTraceHit& hit) const;

    // ---- Gameplay -------------------------------------------------------------------
    // Entities are addressed by Entity::Id. Forces, impulses and velocities only act on a
    // Dynamic rigid body; they return false for anything else (no Rigid Body, Static or
    // Kinematic - move those with their transform instead) or when physics is not running.

    // Impulse in N*s (kg*m/s): a kick. Divide by the mass for the change in velocity.
    bool AddImpulse(std::uint64_t entityId, const float impulse[3]);
    // Force in N, for the next step only: call it every frame for a steady push.
    bool AddForce(std::uint64_t entityId, const float force[3]);
    bool SetVelocity(std::uint64_t entityId, const float velocity[3]);
    // Also reads Kinematic bodies, which the simulation gives the speed they are moved at.
    bool GetVelocity(std::uint64_t entityId, float outVelocity[3]) const;
    // Radians per second about the world X, Y and Z axes.
    bool SetAngularVelocity(std::uint64_t entityId, const float velocity[3]);

    struct RayHit
    {
        float Distance = 0.0f;
        float Position[3]{};
        float Normal[3]{ 0.0f, 0.0f, 1.0f };
        // The entity hit: a mesh, a rigid body or a terrain. 0 for none.
        std::uint64_t EntityId = 0;
    };
    // Everything solid - level meshes, terrain and rigid bodies - except the player's own
    // proxy, which would otherwise be the first thing a ray from the eye hits.
    bool Raycast(const float origin[3], const float direction[3], float maxDistance, RayHit& hit) const;

    // A collision that began during the latest Update(). Entity ids are 0 for the
    // player's proxy.
    struct Hit
    {
        std::uint64_t EntityA = 0;
        std::uint64_t EntityB = 0;
        bool PlayerInvolved = false;
        float Position[3]{};
        float Normal[3]{ 0.0f, 0.0f, 1.0f }; // from A towards B
        float ImpactSpeed = 0.0f;
    };
    const std::vector<Hit>& GetHits() const { return mHits; }

    std::size_t GetRigidBodyCount() const { return mLinks.size(); }
    std::size_t GetColliderCount() const { return mColliderCount; }
    PhysicsWorldStats GetStats() const { return mWorld.GetStats(); }

    // True for an entity whose rigid body moves during play. PlayerCollision leaves these
    // out of its static BVH, where they would stay behind as invisible walls.
    static bool IsMovingRigidBody(const Entity& entity)
    {
        return entity.RigidBody.has_value() && entity.RigidBody->Motion != RigidBodyMotion::Static;
    }

private:
    // An entity driven by, or driving, a body.
    struct Link
    {
        std::size_t EntityIndex = 0;
        std::uint64_t EntityId = 0;
        PhysicsBodyHandle Body;
        RigidBodyMotion Motion = RigidBodyMotion::Dynamic;
        // The transform as last synchronised. An entity found anywhere else has been moved
        // by the game since, which teleports a dynamic body or steers a kinematic one.
        DirectX::XMFLOAT3 Position{};
        DirectX::XMFLOAT3 Rotation{};
        // Whether the body was simulating last frame: a body that has just gone to sleep
        // gets one last write, of its exact resting pose rather than an interpolated one.
        bool WasActive = true;
    };

    Entity* ResolveEntity(Link& link);
    // The dynamic body standing for an entity; invalid when there is none.
    PhysicsBodyHandle FindDynamicBody(std::uint64_t entityId) const;
    void CollectHits();
    bool AddEntity(std::size_t entityIndex, Entity& entity);
    void AddTerrain(const TerrainRenderer& terrain);
    // Shared, unscaled shape of a mesh asset: cached per asset, kind and dynamic-ness.
    PhysicsShapeHandle GetMeshShape(const Mesh& mesh, RigidBodyShape kind, bool dynamic);

    PhysicsWorld mWorld;
    std::vector<Entity>* mEntities = nullptr;
    std::vector<Link> mLinks;
    std::size_t mColliderCount = 0;
    std::map<std::tuple<const Mesh*, int, bool>, PhysicsShapeHandle> mShapeCache;

    std::vector<Hit> mHits;
    std::vector<PhysicsContact> mContactScratch;

    PhysicsBodyHandle mPlayer;
    bool mPlayerPlaced = false;
    float mLastPlayerCenter[3]{};
};
