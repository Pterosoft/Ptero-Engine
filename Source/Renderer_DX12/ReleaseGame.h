#pragma once

// The editor side of the game-packaging pipeline: reads Data/, writes encrypted per-folder
// .ppak files, and turns a copy of GameLauncherTemplate.exe into a named, icon-branded,
// key-embedded game .exe. Driven from the Release menu in EditorMainMenu.cpp, always on a
// background thread since both packaging and copying DLLs can take a while.

#include <functional>
#include <string>
#include <vector>

namespace ReleaseGame
{
    struct BuildOptions
    {
        // The engine/project root - the directory containing Data/, Binaries/ and
        // ProjectSettings/. Same directory GetProjectDataDirectoryCached() resolves to
        // the parent of.
        std::wstring ProjectRoot;
        std::wstring GameName;
        // Empty = leave the template's placeholder icon alone.
        std::wstring IconSourcePath;
        std::wstring OutputDirectory;
        // Top-level Data/ subfolder names to include, e.g. L"Textures", L"UI".
        std::vector<std::wstring> IncludedFolders;
        bool BuildExe = true;
        bool BuildPackages = true;
        // Before gathering the shipped DLLs, compile Release|x64 for whichever of
        // Video/Audio/Game/Renderer_DX12/GameLauncher are missing or older than their
        // Debug build (invoking MSBuild directly - see EnsureReleaseBinariesBuilt).
        // Off falls back to the old check-and-warn-only behaviour.
        bool AutoBuildRelease = true;
    };

    using LogCallback = std::function<void(const std::string& line)>;

    // Runs everything `options` asks for. Returns true only if every requested step
    // succeeded; failures and warnings (e.g. a stale Release DLL) both go through onLog,
    // so the caller's progress panel shows a full account either way.
    bool Build(const BuildOptions& options, const LogCallback& onLog);

    // Decrypts+decompresses one .ppak using this project's ProjectSettings/Packaging.key,
    // for the Release menu's "Extract Package..." action.
    bool ExtractOnePackage(const std::wstring& projectRoot, const std::wstring& pakPath,
                           const std::wstring& destinationDirectory, std::string& outError);

    // The Data/ subfolders pre-checked in the Build Game dialog: everything the shipped
    // game actually reads at runtime. fmod_project (FMOD authoring source) is deliberately
    // not included here.
    const std::vector<std::wstring>& DefaultIncludedFolders();
}
