#include "pch.h"

#include "PlayerCollision.h"

#include "TerrainRenderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace
{
    // Terrain rays march in steps of this length, then bisect the step that crossed the
    // surface. Much finer than any heightmap cell a level uses, and the character's rays
    // are short, so a march is only a handful of samples.
    constexpr float kTerrainStep = 0.25f;
    constexpr int kTerrainRefineSteps = 8;
}

void PlayerCollision::Build(const std::vector<Entity>& entities, const TerrainRenderer* terrain)
{
    Clear();

    // The whole level: a play session can walk anywhere.
    const XMFLOAT3 regionMin(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    const XMFLOAT3 regionMax(FLT_MAX, FLT_MAX, FLT_MAX);

    mMeshes.Build(entities, regionMin, regionMax, true);

    // Without a terrain entity every terrain query would walk the entity list for
    // nothing, several times per ray.
    const bool hasTerrain = std::any_of(entities.begin(), entities.end(),
                                        [](const Entity& entity) { return entity.HasTerrainComponent(); });
    mTerrain = hasTerrain ? terrain : nullptr;
}

void PlayerCollision::Clear()
{
    mMeshes.Clear();
    mTerrain = nullptr;
}

bool PlayerCollision::TraceCallback(void* user, const float origin[3], const float direction[3],
                                    float maxDistance, CharacterTraceHit* hit)
{
    const auto* self = static_cast<const PlayerCollision*>(user);
    return self != nullptr && hit != nullptr && self->Trace(origin, direction, maxDistance, *hit);
}

bool PlayerCollision::Trace(const float origin[3], const float direction[3], float maxDistance,
                            CharacterTraceHit& hit) const
{
    bool found = false;

    if (!mMeshes.IsEmpty())
    {
        GeometryRayHit meshHit;
        if (mMeshes.Raycast(XMFLOAT3(origin[0], origin[1], origin[2]),
                            XMFLOAT3(direction[0], direction[1], direction[2]), maxDistance, meshHit))
        {
            hit.Distance = meshHit.Distance;
            hit.Position[0] = meshHit.Position.x;
            hit.Position[1] = meshHit.Position.y;
            hit.Position[2] = meshHit.Position.z;
            hit.Normal[0] = meshHit.Normal.x;
            hit.Normal[1] = meshHit.Normal.y;
            hit.Normal[2] = meshHit.Normal.z;
            found = true;
        }
    }

    CharacterTraceHit terrainHit;
    if (TraceTerrain(origin, direction, found ? hit.Distance : maxDistance, terrainHit))
    {
        hit = terrainHit;
        found = true;
    }

    return found;
}

bool PlayerCollision::TraceTerrain(const float origin[3], const float direction[3], float maxDistance,
                                   CharacterTraceHit& hit) const
{
    if (mTerrain == nullptr || maxDistance <= 0.0f)
        return false;

    // Height of the ray above the terrain at distance t; negative once it is below.
    // Outside every terrain patch there is no terrain to hit.
    const auto clearance = [&](float t, float& out) -> bool
    {
        const float x = origin[0] + direction[0] * t;
        const float y = origin[1] + direction[1] * t;
        float height = 0.0f;
        if (!mTerrain->SampleHeightAt(XMFLOAT2(x, y), height))
            return false;
        out = origin[2] + direction[2] * t - height;
        return true;
    };

    float previousT = 0.0f;
    float previousClearance = 0.0f;
    bool previousValid = clearance(0.0f, previousClearance);
    // Starting underneath is not a hit: the ray is leaving the ground, not meeting it.
    if (previousValid && previousClearance < 0.0f)
        return false;

    const int steps = (std::max)(1, static_cast<int>(std::ceil(maxDistance / kTerrainStep)));
    for (int i = 1; i <= steps; ++i)
    {
        const float t = (std::min)(maxDistance, static_cast<float>(i) * kTerrainStep);
        float current = 0.0f;
        const bool valid = clearance(t, current);

        if (valid && previousValid && current < 0.0f)
        {
            // Crossed between previousT and t: narrow it down.
            float low = previousT;
            float high = t;
            for (int refine = 0; refine < kTerrainRefineSteps; ++refine)
            {
                const float middle = (low + high) * 0.5f;
                float middleClearance = 0.0f;
                if (clearance(middle, middleClearance) && middleClearance < 0.0f)
                    high = middle;
                else
                    low = middle;
            }

            hit.Distance = high;
            for (int a = 0; a < 3; ++a)
                hit.Position[a] = origin[a] + direction[a] * high;

            XMFLOAT3 normal(0.0f, 0.0f, 1.0f);
            mTerrain->SampleNormalAt(XMFLOAT2(hit.Position[0], hit.Position[1]), normal);
            hit.Normal[0] = normal.x;
            hit.Normal[1] = normal.y;
            hit.Normal[2] = normal.z;
            return true;
        }

        previousT = t;
        previousClearance = current;
        previousValid = valid;
    }

    return false;
}
