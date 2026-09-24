#pragma once

// The per-project packaging key: 32 random bytes generated once and stored in the repo at
// ProjectSettings/Packaging.key, next to Data/ and Binaries/. Both the editor (building and
// extracting .ppak files) and a packaged build's GameLauncher.exe (decrypting them at
// startup) need the same key, so it is generated once, committed like any other project
// config, and embedded into the shipped exe at package time rather than regenerated.

#include "System/PackageFormat.h"

#include <string>

namespace Packaging
{
    // `projectRoot` is the engine/project root that contains Data/, Binaries/ and (this
    // file) ProjectSettings/ - normally the same directory FindDataDirectory() walks up to.
    // Creates ProjectSettings/Packaging.key with fresh random bytes if it does not exist
    // yet. Returns false and fills outError only on an I/O failure.
    bool LoadOrCreateProjectKey(const std::wstring& projectRoot, Key& outKey, std::string& outError);
}
