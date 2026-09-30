#pragma once

// PlayerCollision
// ---------------
// The world as the player character feels it: every static mesh in the level plus the
// terrain heightmaps, behind the one ray query CharacterMovement needs.
//
// Built once when a play session starts. Meshes come from GeometryRaycaster - collision
// hulls when the asset has them, its finest LOD otherwise - so an entity the game moves
// during play keeps colliding where it stood at the start. Terrain is not copied into the
// BVH: its heightmap is sampled live, which is both cheaper and exact.
//
// Water and vegetation-area volumes are not surfaces and are skipped, as the scatter
// skips them.

#include "GeometryRaycaster.h"
#include "System/CharacterMovement.h"

#include <vector>

class TerrainRenderer;

class PlayerCollision
{
public:
    void Build(const std::vector<Entity>& entities, const TerrainRenderer* terrain);
    void Clear();

    bool Trace(const float origin[3], const float direction[3], float maxDistance, CharacterTraceHit& hit) const;

    // CharacterTraceFn adapter; `user` is the PlayerCollision.
    static bool TraceCallback(void* user, const float origin[3], const float direction[3],
                              float maxDistance, CharacterTraceHit* hit);

    std::size_t GetTriangleCount() const { return mMeshes.GetTriangleCount(); }

private:
    bool TraceTerrain(const float origin[3], const float direction[3], float maxDistance, CharacterTraceHit& hit) const;

    GeometryRaycaster mMeshes;
    const TerrainRenderer* mTerrain = nullptr;
};
