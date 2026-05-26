#pragma once

#include <cstdint>
#include <type_traits>

// Version history:
//   1 - original format: header + vertices + indices
//   2 - added subMeshCount field and PteroSubMeshEntry table between the header and vertex data
static constexpr std::uint32_t kPteroMeshVersion = 2;

// Records a contiguous range of the shared index buffer that belongs to one FBX material slot.
// materialId is the zero-based FBX material index assigned to the polygons in this sub-mesh.
struct PteroSubMeshEntry
{
    std::uint32_t materialId = 0;
    std::uint32_t indexStart = 0;
    std::uint32_t indexCount = 0;
};

struct PteroMeshHeader
{
    char magic[4] = { 'P', 'T', 'R', 'O' };
    std::uint32_t version = kPteroMeshVersion;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    // Number of PteroSubMeshEntry records written immediately after this header.
    std::uint32_t subMeshCount = 0;
};

static_assert(std::is_trivially_copyable_v<PteroSubMeshEntry>, "PteroSubMeshEntry must stay trivially copyable for binary serialization.");
static_assert(std::is_standard_layout_v<PteroSubMeshEntry>, "PteroSubMeshEntry must stay standard layout for binary serialization.");
static_assert(std::is_trivially_copyable_v<PteroMeshHeader>, "PteroMeshHeader must stay trivially copyable for binary serialization.");
static_assert(std::is_standard_layout_v<PteroMeshHeader>, "PteroMeshHeader must stay standard layout for binary serialization.");
