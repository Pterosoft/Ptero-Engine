#pragma once

// Which files inside a top-level Data/ subfolder are allowed into a shipped package.
// Whole-folder inclusion/exclusion (e.g. skipping fmod_project entirely) is a decision the
// Build Game dialog makes per folder; this only filters files *within* a folder that is
// already being packaged, per the user-specified rule: only cooked assets ship, never
// import sources, except the UI folder which ships as-is.

#include <string>

namespace Packaging
{
    // `topLevelFolderName` is the Data/ subfolder name (e.g. L"Textures", L"Geometry",
    // L"UI"), matched case-insensitively. `fileName` is just the file's name (with
    // extension), not a path.
    bool ShouldIncludeAsset(const std::wstring& topLevelFolderName, const std::wstring& fileName);
}
