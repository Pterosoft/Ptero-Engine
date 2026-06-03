#include "pch.h"

#include "System/AssetManager.h"

#include "System/FbxCompiler.h"
#include "System/PteroMeshFormat.h"
#include "System/TextureImporter.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
    struct LegacyPteroMeshHeader
    {
        char magic[4] = { 'P', 'T', 'R', 'O' };
        std::uint32_t version = 2;
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t subMeshCount = 0;
    };

    void LogAssetManagerDiagnostic(const std::string& message)
    {
        OutputDebugStringA("[AssetManager] ");
        OutputDebugStringA(message.c_str());
        OutputDebugStringA("\n");
    }

    std::filesystem::path NormalizeExistingPath(const std::filesystem::path& path)
    {
        std::error_code errorCode;
        std::filesystem::path normalizedPath = path.lexically_normal();
        if (std::filesystem::exists(normalizedPath, errorCode))
        {
            const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(normalizedPath, errorCode);
            if (!errorCode)
            {
                return canonicalPath;
            }
        }

        return normalizedPath;
    }

    bool IsPathInsideRoot(const std::filesystem::path& rootPath, const std::filesystem::path& candidatePath)
    {
        const std::filesystem::path normalizedRootPath = rootPath.lexically_normal();
        const std::filesystem::path normalizedCandidatePath = candidatePath.lexically_normal();

        auto rootIterator = normalizedRootPath.begin();
        auto candidateIterator = normalizedCandidatePath.begin();
        for (; rootIterator != normalizedRootPath.end(); ++rootIterator, ++candidateIterator)
        {
            if (candidateIterator == normalizedCandidatePath.end() || *rootIterator != *candidateIterator)
            {
                return false;
            }
        }

        return true;
    }

    std::filesystem::path FindProjectDataDirectory()
    {
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD characterCount = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (characterCount == 0 || characterCount == std::size(executablePath))
        {
            return {};
        }

        // Walk upward from the editor executable until the repository Data folder is found.
        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        while (!currentPath.empty())
        {
            const std::filesystem::path dataDirectory = currentPath / "Data";
            if (std::filesystem::exists(dataDirectory) && std::filesystem::is_directory(dataDirectory))
            {
                return dataDirectory;
            }

            const std::filesystem::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
            {
                break;
            }

            currentPath = parentPath;
        }

        return {};
    }

    std::filesystem::path ResolveAssetPathFromDataDirectory(const std::string& assetPath)
    {
        if (assetPath.empty())
        {
            return {};
        }

        const std::filesystem::path candidatePath(assetPath);
        if (candidatePath.is_absolute())
        {
            return NormalizeExistingPath(candidatePath);
        }

        std::error_code errorCode;
        if (std::filesystem::exists(candidatePath, errorCode))
        {
            return NormalizeExistingPath(candidatePath);
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (dataDirectory.empty())
        {
            return candidatePath.lexically_normal();
        }

        const std::filesystem::path dataRelativeCandidate = (dataDirectory / candidatePath).lexically_normal();
        if (std::filesystem::exists(dataRelativeCandidate, errorCode))
        {
            return NormalizeExistingPath(dataRelativeCandidate);
        }

        auto pathPart = candidatePath.begin();
        if (pathPart != candidatePath.end() && _stricmp(pathPart->string().c_str(), "Data") == 0)
        {
            const std::filesystem::path projectRelativeCandidate = (dataDirectory.parent_path() / candidatePath).lexically_normal();
            if (std::filesystem::exists(projectRelativeCandidate, errorCode))
            {
                return NormalizeExistingPath(projectRelativeCandidate);
            }
        }

        return candidatePath.lexically_normal();
    }

    bool ReadMeshLodPayload(
        std::ifstream& inputStream,
        const std::uint32_t vertexCount,
        const std::uint32_t indexCount,
        const std::uint32_t subMeshCount,
        MeshLod& outLod,
        std::string& errorMessage)
    {
        outLod = {};

        if (subMeshCount > 0)
        {
            std::vector<PteroSubMeshEntry> entries(subMeshCount);
            inputStream.read(
                reinterpret_cast<char*>(entries.data()),
                static_cast<std::streamsize>(entries.size() * sizeof(PteroSubMeshEntry)));
            if (!inputStream)
            {
                errorMessage = "Failed to read the sub-mesh table from the cooked mesh.";
                return false;
            }

            outLod.SubMeshes.reserve(entries.size());
            for (const PteroSubMeshEntry& entry : entries)
            {
                SubMesh sm{};
                sm.materialId = entry.materialId;
                sm.indexStart = entry.indexStart;
                sm.indexCount = entry.indexCount;
                outLod.SubMeshes.push_back(sm);
            }
        }

        outLod.Vertices.resize(vertexCount);
        outLod.Indices.resize(indexCount);

        if (!outLod.Vertices.empty())
        {
            inputStream.read(
                reinterpret_cast<char*>(outLod.Vertices.data()),
                static_cast<std::streamsize>(outLod.Vertices.size() * sizeof(Vertex)));
        }

        if (!outLod.Indices.empty())
        {
            inputStream.read(
                reinterpret_cast<char*>(outLod.Indices.data()),
                static_cast<std::streamsize>(outLod.Indices.size() * sizeof(std::uint32_t)));
        }

        if (!inputStream)
        {
            errorMessage = "The cooked mesh file ended before all vertex and index data was read.";
            return false;
        }

        return true;
    }
}

bool AssetManager::ImportTextureToDataDirectory(
    const std::string& sourceTexturePath,
    const std::string& targetDirectoryRelativeToData,
    std::string* importedTexturePath)
{
    mLastErrorMessage.clear();

    if (sourceTexturePath.empty())
    {
        mLastErrorMessage = "The source texture path is empty.";
        return false;
    }

    const std::filesystem::path sourcePath(sourceTexturePath);
    if (!std::filesystem::exists(sourcePath) || !std::filesystem::is_regular_file(sourcePath))
    {
        mLastErrorMessage = "The source texture file does not exist.";
        return false;
    }

    const std::filesystem::path dataDirectory = FindProjectDataDirectory();
    if (dataDirectory.empty())
    {
        mLastErrorMessage = "Failed to locate the project's Data directory.";
        return false;
    }

    const std::filesystem::path targetDirectory = targetDirectoryRelativeToData.empty()
        ? dataDirectory / "Textures"
        : (dataDirectory / std::filesystem::path(targetDirectoryRelativeToData)).lexically_normal();

    if (!IsPathInsideRoot(dataDirectory, targetDirectory))
    {
        mLastErrorMessage = "The target texture folder must stay inside the Data directory.";
        return false;
    }

    std::filesystem::create_directories(targetDirectory);

    const std::filesystem::path destinationTexturePath = targetDirectory / (sourcePath.stem().string() + ".dds");

    // If the source is already a DDS inside Data, just return its canonical path without re-importing.
    const bool sourceAlreadyInDataAsDds = _stricmp(sourcePath.extension().string().c_str(), ".dds") == 0
        && IsPathInsideRoot(dataDirectory, sourcePath);

    if (sourceAlreadyInDataAsDds)
    {
        if (importedTexturePath != nullptr)
        {
            *importedTexturePath = std::filesystem::weakly_canonical(sourcePath).string();
        }
        return true;
    }

    TextureImporter textureImporter;
    if (!textureImporter.Import(sourcePath.string(), destinationTexturePath.string()))
    {
        mLastErrorMessage = textureImporter.LastError().empty()
            ? "Failed to convert the source texture into DDS."
            : textureImporter.LastError();
        return false;
    }

    if (importedTexturePath != nullptr)
    {
        *importedTexturePath = destinationTexturePath.string();
    }

    return true;
}

bool AssetManager::ImportFbxToDataDirectory(
    const std::string& sourceFbxPath,
    const std::string& targetDirectoryRelativeToData,
    std::string* importedFbxPath)
{
    mLastErrorMessage.clear();

    if (sourceFbxPath.empty())
    {
        mLastErrorMessage = "The source FBX path is empty.";
        return false;
    }

    const std::filesystem::path sourcePath(sourceFbxPath);
    if (!std::filesystem::exists(sourcePath) || !std::filesystem::is_regular_file(sourcePath))
    {
        mLastErrorMessage = "The source FBX file does not exist.";
        return false;
    }

    const std::filesystem::path dataDirectory = FindProjectDataDirectory();
    if (dataDirectory.empty())
    {
        mLastErrorMessage = "Failed to locate the project's Data directory.";
        return false;
    }

    const std::filesystem::path targetDirectory = targetDirectoryRelativeToData.empty()
        ? dataDirectory / "Imported"
        : (dataDirectory / std::filesystem::path(targetDirectoryRelativeToData)).lexically_normal();

    if (!IsPathInsideRoot(dataDirectory, targetDirectory))
    {
        mLastErrorMessage = "The target import folder must stay inside the Data directory.";
        return false;
    }

    std::filesystem::create_directories(targetDirectory);

    const std::filesystem::path destinationFbxPath = targetDirectory / sourcePath.filename();

    // Copy the imported source asset into Data so the editor can browse project-owned content in one place.
    if (!std::filesystem::exists(destinationFbxPath) || !std::filesystem::equivalent(sourcePath, destinationFbxPath))
    {
        std::filesystem::copy_file(sourcePath, destinationFbxPath, std::filesystem::copy_options::overwrite_existing);
    }

    const std::filesystem::path destinationPteroPath = destinationFbxPath.string() + ".ptero";
    if (!FbxCompiler::CompileFbxToPtero(destinationFbxPath.string(), destinationPteroPath.string()))
    {
        mLastErrorMessage = "Failed to compile the imported FBX file into a cooked .ptero mesh inside Data.";
        return false;
    }

    // Auto-import textures referenced by the generated multi-material JSON.
    // FbxCompiler writes the JSON to Data/MultiMaterials/<stem>.json.
    // Each sub-material "textures.baseColor" holds a Data-relative path like
    // "Textures/<name>.dds".  We resolve the original source texture (same
    // filename, any extension) next to the source FBX and import it.
    {
        const std::string stem = destinationFbxPath.stem().string();
        const std::filesystem::path multiMatJson = dataDirectory / "MultiMaterials" / (stem + ".json");
        if (std::filesystem::exists(multiMatJson))
        {
            try
            {
                std::ifstream jsonFile(multiMatJson);
                nlohmann::json j;
                jsonFile >> j;

                auto subMatsIt = j.find("subMaterials");
                if (subMatsIt != j.end() && subMatsIt->is_array())
                {
                    // Collect unique base names we need to import.
                    for (const auto& subMat : *subMatsIt)
                    {
                        auto texIt = subMat.find("textures");
                        if (texIt == subMat.end()) continue;
                        auto bcIt = texIt->find("baseColor");
                        if (bcIt == texIt->end()) continue;

                        const std::string dataRelDds = bcIt->get<std::string>();
                        if (dataRelDds.empty()) continue;

                        // The DDS path is e.g. "Textures/sponza_ceiling_a_diff.dds".
                        // Look for the original texture (any common image extension)
                        // next to the source FBX file and next to the destination FBX.
                        const std::filesystem::path ddsFilename = std::filesystem::path(dataRelDds).filename();
                        const std::string stemName = ddsFilename.stem().string();

                        static const char* const kExts[] = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tiff", ".hdr", ".dds" };

                        auto tryImportTexture = [&](const std::filesystem::path& dir) -> bool
                        {
                            for (const char* ext : kExts)
                            {
                                const std::filesystem::path candidate = dir / (stemName + ext);
                                if (std::filesystem::exists(candidate))
                                {
                                    // Import into Data/Textures/ — errors here are non-fatal.
                                    ImportTextureToDataDirectory(candidate.string(), "Textures");
                                    return true;
                                }
                            }
                            return false;
                        };

                        // Try source FBX directory first, then destination directory.
                        if (!tryImportTexture(std::filesystem::path(sourceFbxPath).parent_path()))
                        {
                            tryImportTexture(destinationFbxPath.parent_path());
                        }
                    }
                }
            }
            catch (...) {}
        }
    }

    mMeshCache.erase(destinationFbxPath.string());

    if (importedFbxPath != nullptr)
    {
        *importedFbxPath = destinationFbxPath.string();
    }

    return true;
}

std::shared_ptr<Mesh> AssetManager::GetMesh(const std::string& fbxFilePath)
{
    mLastErrorMessage.clear();

    if (fbxFilePath.empty())
    {
        mLastErrorMessage = "The FBX file path is empty.";
        return nullptr;
    }

    if (const auto cachedMesh = mMeshCache.find(fbxFilePath); cachedMesh != mMeshCache.end())
    {
        std::ostringstream logStream;
        logStream << "Cache hit for '" << fbxFilePath << "'"
                  << ", vertices=" << cachedMesh->second->GetVertices().size()
                  << ", indices=" << cachedMesh->second->GetIndices().size()
                  << ", subMeshes=" << cachedMesh->second->GetSubMeshes().size();
        LogAssetManagerDiagnostic(logStream.str());
        return cachedMesh->second;
    }

    const std::filesystem::path resolvedInputPath = ResolveAssetPathFromDataDirectory(fbxFilePath);
    const std::string resolvedMeshPath = resolvedInputPath.string();
    if (!resolvedMeshPath.empty())
    {
        if (const auto cachedMesh = mMeshCache.find(resolvedMeshPath); cachedMesh != mMeshCache.end())
        {
            mMeshCache.emplace(fbxFilePath, cachedMesh->second);
            std::ostringstream logStream;
            logStream << "Cache hit for resolved path '" << resolvedMeshPath << "' (requested '" << fbxFilePath << "')"
                      << ", vertices=" << cachedMesh->second->GetVertices().size()
                      << ", indices=" << cachedMesh->second->GetIndices().size()
                      << ", subMeshes=" << cachedMesh->second->GetSubMeshes().size();
            LogAssetManagerDiagnostic(logStream.str());
            return cachedMesh->second;
        }
    }

    // If the caller supplied a .ptero path directly, load it as-is without cooking.
    // Otherwise treat it as an FBX path and derive/cook the .ptero alongside it.
    const bool isNativePtero = _stricmp(resolvedInputPath.extension().string().c_str(), ".ptero") == 0;

    const std::string pteroPath = isNativePtero ? resolvedMeshPath : resolvedMeshPath + ".ptero";

    {
        std::ostringstream logStream;
        logStream << "Loading mesh request='" << fbxFilePath
                  << "', resolvedInput='" << resolvedMeshPath
                  << "', cookedPath='" << pteroPath
                  << "', isNativePtero=" << (isNativePtero ? "true" : "false");
        LogAssetManagerDiagnostic(logStream.str());
    }

    if (!isNativePtero && !std::filesystem::exists(pteroPath))
    {
        // Bake the FBX on demand so runtime loads hit the compact cooked mesh format after the first import.
        if (!FbxCompiler::CompileFbxToPtero(resolvedMeshPath, pteroPath))
        {
            mLastErrorMessage = "Failed to compile the FBX file into a cooked .ptero mesh.";
            return nullptr;
        }
    }

    std::ifstream inputStream(pteroPath, std::ios::binary);
    if (!inputStream)
    {
        mLastErrorMessage = "Failed to open the cooked .ptero mesh from disk.";
        return nullptr;
    }

    LegacyPteroMeshHeader legacyHeader{};
    inputStream.read(reinterpret_cast<char*>(&legacyHeader), sizeof(legacyHeader));
    if (!inputStream || std::memcmp(legacyHeader.magic, "PTRO", 4) != 0)
    {
        mLastErrorMessage = "The cooked mesh header is invalid.";
        return nullptr;
    }

    std::uint32_t lodCount = 1;
    if (legacyHeader.version >= 3)
    {
        inputStream.read(reinterpret_cast<char*>(&lodCount), sizeof(lodCount));
        if (!inputStream)
        {
            mLastErrorMessage = "The cooked mesh LOD header is invalid.";
            return nullptr;
        }
    }

    {
        std::ostringstream logStream;
        logStream << "Read cooked mesh header from '" << pteroPath << "'"
                  << ": version=" << legacyHeader.version
                  << ", vertexCount=" << legacyHeader.vertexCount
                  << ", indexCount=" << legacyHeader.indexCount
                  << ", subMeshCount=" << legacyHeader.subMeshCount
                  << ", lodCount=" << lodCount;
        LogAssetManagerDiagnostic(logStream.str());
    }

    // If the cached .ptero was baked by an older compiler that lacks sub-mesh support, re-cook it now.
    // Native .ptero files have no source FBX available, so skip re-cooking and just report the version mismatch.
    if (legacyHeader.version != kPteroMeshVersion)
    {
        if (isNativePtero)
        {
            mLastErrorMessage = "The .ptero file was created by an older version of the compiler and cannot be re-cooked without a source FBX.";
            return nullptr;
        }

        inputStream.close();
        if (!FbxCompiler::CompileFbxToPtero(resolvedMeshPath, pteroPath))
        {
            mLastErrorMessage = "The cooked mesh is outdated and re-compilation failed.";
            return nullptr;
        }

        inputStream.open(pteroPath, std::ios::binary);
        if (!inputStream)
        {
            mLastErrorMessage = "Failed to re-open the re-cooked .ptero mesh from disk.";
            return nullptr;
        }

        inputStream.read(reinterpret_cast<char*>(&legacyHeader), sizeof(legacyHeader));
        if (!inputStream || std::memcmp(legacyHeader.magic, "PTRO", 4) != 0 || legacyHeader.version != kPteroMeshVersion)
        {
            mLastErrorMessage = "Re-cooked mesh header is still invalid.";
            return nullptr;
        }

        lodCount = 1;
        if (legacyHeader.version >= 3)
        {
            inputStream.read(reinterpret_cast<char*>(&lodCount), sizeof(lodCount));
            if (!inputStream)
            {
                mLastErrorMessage = "Re-cooked mesh LOD header is invalid.";
                return nullptr;
            }
        }
    }

    lodCount = (std::max)(lodCount, 1u);
    std::vector<MeshLod> lods;
    lods.reserve(lodCount);

    MeshLod baseLod;
    if (!ReadMeshLodPayload(inputStream, legacyHeader.vertexCount, legacyHeader.indexCount, legacyHeader.subMeshCount, baseLod, mLastErrorMessage))
    {
        return nullptr;
    }

    for (const SubMesh& subMesh : baseLod.SubMeshes)
    {
        std::ostringstream logStream;
        const bool isRangeValid = static_cast<std::uint64_t>(subMesh.indexStart) + static_cast<std::uint64_t>(subMesh.indexCount)
            <= static_cast<std::uint64_t>(baseLod.Indices.size());
        logStream << "SubMesh materialId=" << subMesh.materialId
                  << ", indexStart=" << subMesh.indexStart
                  << ", indexCount=" << subMesh.indexCount
                  << ", validRange=" << (isRangeValid ? "true" : "false");
        LogAssetManagerDiagnostic(logStream.str());
    }

    lods.push_back(std::move(baseLod));

    for (std::uint32_t lodIndex = 1; lodIndex < lodCount; ++lodIndex)
    {
        PteroLodEntry lodEntry{};
        inputStream.read(reinterpret_cast<char*>(&lodEntry), sizeof(lodEntry));
        if (!inputStream)
        {
            mLastErrorMessage = "Failed to read an additional mesh LOD entry from the cooked mesh.";
            return nullptr;
        }

        MeshLod lod;
        if (!ReadMeshLodPayload(inputStream, lodEntry.vertexCount, lodEntry.indexCount, lodEntry.subMeshCount, lod, mLastErrorMessage))
        {
            return nullptr;
        }

        lods.push_back(std::move(lod));
    }

    auto mesh = std::make_shared<Mesh>(std::move(lods));
    if (!mesh->UploadToGpu())
    {
        mLastErrorMessage = "The mesh data loaded, but the GPU upload step failed.";
        return nullptr;
    }

    {
        std::ostringstream logStream;
        logStream << "Loaded mesh successfully from '" << pteroPath << "'"
                  << ", vertices=" << mesh->GetVertices().size()
                  << ", indices=" << mesh->GetIndices().size()
                  << ", subMeshes=" << mesh->GetSubMeshes().size()
                  << ", lods=" << mesh->GetLodCount()
                  << ", uploadedToGpu=" << (mesh->IsUploadedToGpu() ? "true" : "false");
        LogAssetManagerDiagnostic(logStream.str());
    }

    mMeshCache.emplace(fbxFilePath, mesh);
    if (!resolvedMeshPath.empty() && resolvedMeshPath != fbxFilePath)
    {
        mMeshCache.emplace(resolvedMeshPath, mesh);
    }
    return mesh;
}
