#pragma once

#include "UnrealPackage.h"

#include "System/Mesh.h"
#include "System/PteroMeshFormat.h"

#include <string>
#include <vector>

namespace Ptero::Unreal
{
    struct MeshMaterialSlot
    {
        std::string SlotName;
        // Object path of the assigned material ("/Game/Materials/MI_Rock.MI_Rock"); empty
        // when the slot has none.
        std::string MaterialPath;
    };

    // A static mesh's LOD 0 source geometry converted into Ptero's frame: metres, Z up,
    // left-handed, the same orientation an FBX of the model gets from the FBX importer.
    struct StaticMeshData
    {
        std::string Name;
        std::vector<Vertex> Vertices;
        std::vector<std::uint32_t> Indices;
        // One entry per material slot that has triangles; materialId is the slot index.
        std::vector<PteroSubMeshEntry> SubMeshes;
        std::vector<MeshMaterialSlot> Slots;
    };

    bool ReadStaticMesh(const Package& package, StaticMeshData& mesh, std::string& error);
}
