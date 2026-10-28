#pragma once

#include "System/Mesh.h"

#include <memory>
#include <string>
#include <unordered_map>

class AssetManager
{
public:
    bool ImportFbxToDataDirectory(
        const std::string& sourceFbxPath,
        const std::string& targetDirectoryRelativeToData = {},
        std::string* importedFbxPath = nullptr);
    bool ImportTextureToDataDirectory(
        const std::string& sourceTexturePath,
        const std::string& targetDirectoryRelativeToData = {},
        std::string* importedTexturePath = nullptr);
    // Imports an Unreal Engine 5 .uasset (static mesh, texture or material). Assets already
    // inside Data are converted in place; others land in targetDirectoryRelativeToData.
    bool ImportUnrealAssetToDataDirectory(
        const std::string& sourceUassetPath,
        const std::string& targetDirectoryRelativeToData = {},
        std::string* summary = nullptr);
    std::shared_ptr<Mesh> GetMesh(const std::string& fbxFilePath);
    // Drops the cached copy so the next GetMesh reads the cooked files again (after a
    // re-cook). Entities keep the old geometry until their MeshAsset is reset.
    void ForgetMesh(const std::string& meshPath);

    const std::string& GetLastErrorMessage() const
    {
        return mLastErrorMessage;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<Mesh>> mMeshCache;
    std::string mLastErrorMessage;
};
