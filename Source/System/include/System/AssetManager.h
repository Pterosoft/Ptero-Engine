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
    std::shared_ptr<Mesh> GetMesh(const std::string& fbxFilePath);

    const std::string& GetLastErrorMessage() const
    {
        return mLastErrorMessage;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<Mesh>> mMeshCache;
    std::string mLastErrorMessage;
};
