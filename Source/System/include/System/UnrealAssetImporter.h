#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Imports Unreal Engine 5 editor assets (.uasset, as sold on Fab) without Unreal:
// static meshes become .ptero geometry with a multi-material next to them, textures become
// BC-compressed .dds, and materials / material instances become Ptero material JSON with
// their textures wired to the matching slots.
namespace UnrealAssetImporter
{
    struct Result
    {
        std::string Summary;
        std::vector<std::string> Warnings;
        std::vector<std::filesystem::path> WrittenFiles;
    };

    // uassetPath: the asset to import. When it lives inside dataDirectory its outputs are
    // written next to it; otherwise into targetDirectory (absolute, inside dataDirectory),
    // with the textures it pulls in under targetDirectory/Textures.
    bool Import(
        const std::filesystem::path& uassetPath,
        const std::filesystem::path& dataDirectory,
        const std::filesystem::path& targetDirectory,
        Result& result,
        std::string& error);

    // Class of the package's main asset ("StaticMesh", "Texture2D", "MaterialInstanceConstant",
    // "Material", "World", ...); empty when the file is not a readable package.
    std::string PeekAssetClass(const std::filesystem::path& uassetPath);

    // True when this class is something Import() handles.
    bool IsImportableClass(const std::string& assetClass);

    // Whether an asset inside Data already has its converted output next to it.
    bool HasImportedOutput(const std::filesystem::path& uassetPath);

    // Unreal's editor assets are Oodle-compressed; false (with the reason) when no Oodle
    // decompressor could be found on this machine.
    bool IsDecompressorAvailable(std::string* reason = nullptr);
}
