#pragma once

#include "Components.h"

#include <cstdint>
#include <random>
#include <unordered_set>
#include <vector>

// Gives every entity a unique, non-zero Entity::Id.
//
// Ids are assigned lazily instead of at each of the many places an entity is created -
// the Add menus, paste, duplicate, prefab drops, undo snapshots. Any entity still at 0 gets
// a fresh id, and so does the second holder of an id that is already taken: copying an
// entity copies its id, and the copy is always the later one in the list, so the original
// keeps the id that graphs point at. Called after a level loads, before it saves, before
// play starts and whenever the Node Graph lists the level's entities.
//
// Ids stay below 2^53 because the node graph carries them in a double, which holds every
// integer up to there exactly. Random rather than sequential so two levels, or an entity
// pasted between them, are unlikely to collide.
//
// Returns true when anything changed, i.e. the level now has ids it has not saved.
inline bool EnsureEntityIds(std::vector<Entity>& entities)
{
    static std::mt19937_64 generator{ std::random_device{}() };
    constexpr std::uint64_t kIdMask = (std::uint64_t(1) << 53) - 1;

    std::unordered_set<std::uint64_t> taken;
    taken.reserve(entities.size());

    bool changed = false;
    for (Entity& entity : entities)
    {
        if (entity.Id != 0 && entity.Id <= kIdMask && taken.insert(entity.Id).second)
        {
            continue;
        }

        std::uint64_t id = 0;
        do
        {
            id = generator() & kIdMask;
        } while (id == 0 || taken.count(id) != 0);

        entity.Id = id;
        taken.insert(id);
        changed = true;
    }

    return changed;
}
