#pragma once

#include "System/Mesh.h"
#include "System/PteroMeshFormat.h"

#include <string>
#include <vector>

class FbxCompiler final
{
public:
    FbxCompiler() = delete;

    static bool CompileFbxToPtero(const std::string& fbxPath, const std::string& pteroOutPath);

    // Writes already converted geometry (Ptero frame) as a .ptero with generated LODs; used
    // by importers that do not go through the FBX SDK.
    static bool WritePteroMesh(
        const std::string& pteroOutPath,
        std::vector<Vertex> vertices,
        std::vector<std::uint32_t> indices,
        std::vector<PteroSubMeshEntry> subMeshes);
    static bool GenerateLodsForPtero(const std::string& pteroPath);

    // Re-cooks "<name>.fbx.ptero" from "<name>.fbx" when it was written by an older
    // compiler. Anything that rewrites a .ptero in place - LOD or collision generation -
    // calls this first, or it would stamp the current version on vertices still in the
    // old frame. True when the file is current (already, or after re-cooking).
    static bool EnsureCurrentPtero(const std::string& pteroPath);
};
