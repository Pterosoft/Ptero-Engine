#include "pch.h"

#include "System/SystemAssetApi.h"

#include "System/AssetManager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    AssetManager gAssetManager;

    void WriteStatusMessage(char* destination, const int destinationCapacity, const std::string& message)
    {
        if (destination == nullptr || destinationCapacity <= 0)
        {
            return;
        }

        const size_t copyLength = (std::min)(message.size(), static_cast<size_t>(destinationCapacity - 1));
        std::memcpy(destination, message.data(), copyLength);
        destination[copyLength] = '\0';
    }
}

extern "C" SYSTEM_ASSET_API bool __stdcall System_TryLoadMeshFromFile(
    const char* fbxFilePath,
    char* statusMessage,
    int statusMessageCapacity)
{
    if (fbxFilePath == nullptr || fbxFilePath[0] == '\0')
    {
        WriteStatusMessage(statusMessage, statusMessageCapacity, "Enter an FBX file path before loading a mesh.");
        return false;
    }

    const std::shared_ptr<Mesh> mesh = gAssetManager.GetMesh(fbxFilePath);
    if (mesh == nullptr)
    {
        const std::string failureMessage = gAssetManager.GetLastErrorMessage().empty()
            ? "The System asset manager failed to load the mesh."
            : gAssetManager.GetLastErrorMessage();
        WriteStatusMessage(statusMessage, statusMessageCapacity, failureMessage);
        return false;
    }

    char summary[512]{};
    std::snprintf(
        summary,
        sizeof(summary),
        "Loaded mesh successfully. Vertices: %zu, Indices: %zu",
        mesh->GetVertices().size(),
        mesh->GetIndices().size());
    WriteStatusMessage(statusMessage, statusMessageCapacity, summary);
    return true;
}

extern "C" SYSTEM_ASSET_API bool __stdcall System_ImportFbxToData(
    const char* sourceFbxPath,
    const char* targetDirectoryRelativeToData,
    char* statusMessage,
    int statusMessageCapacity)
{
    if (sourceFbxPath == nullptr || sourceFbxPath[0] == '\0')
    {
        WriteStatusMessage(statusMessage, statusMessageCapacity, "Choose an FBX file to import.");
        return false;
    }

    std::string importedFbxPath;
    const std::string targetDirectory = targetDirectoryRelativeToData != nullptr
        ? std::string(targetDirectoryRelativeToData)
        : std::string{};

    if (!gAssetManager.ImportFbxToDataDirectory(
        sourceFbxPath,
        targetDirectory,
        &importedFbxPath))
    {
        const std::string failureMessage = gAssetManager.GetLastErrorMessage().empty()
            ? "The System asset manager failed to import the FBX file into Data."
            : gAssetManager.GetLastErrorMessage();
        WriteStatusMessage(statusMessage, statusMessageCapacity, failureMessage);
        return false;
    }

    WriteStatusMessage(statusMessage, statusMessageCapacity, "Imported FBX into Data: " + importedFbxPath);
    return true;
}

extern "C" SYSTEM_ASSET_API bool __stdcall System_ImportTextureToData(
    const char* sourceTexturePath,
    const char* targetDirectoryRelativeToData,
    char* statusMessage,
    int statusMessageCapacity)
{
    if (sourceTexturePath == nullptr || sourceTexturePath[0] == '\0')
    {
        WriteStatusMessage(statusMessage, statusMessageCapacity, "Choose a texture file to import.");
        return false;
    }

    std::string importedTexturePath;
    const std::string targetDirectory = targetDirectoryRelativeToData != nullptr
        ? std::string(targetDirectoryRelativeToData)
        : std::string{};

    if (!gAssetManager.ImportTextureToDataDirectory(
        sourceTexturePath,
        targetDirectory,
        &importedTexturePath))
    {
        const std::string failureMessage = gAssetManager.GetLastErrorMessage().empty()
            ? "The System asset manager failed to import the texture into Data."
            : gAssetManager.GetLastErrorMessage();
        WriteStatusMessage(statusMessage, statusMessageCapacity, failureMessage);
        return false;
    }

    WriteStatusMessage(statusMessage, statusMessageCapacity, "Imported texture into Data: " + importedTexturePath);
    return true;
}
