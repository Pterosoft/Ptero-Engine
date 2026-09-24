#pragma once

// The packaging key travels from the editor into a shipped GameLauncher.exe as a plain
// Win32 resource (RT_RCDATA), which any resource viewer can list. XORing it with a fixed
// mask baked into the engine source is not real protection - it is exactly as reversible
// as the rest of this obfuscation-not-DRM scheme - it just means "open it in Resource
// Hacker" does not immediately hand over the raw key. Both the editor (when it patches the
// resource into a built exe) and GameLauncher (when it reads the resource back at startup)
// use this same mask, so keep it in sync between them by only ever changing it here.

#include "System/PackageFormat.h"

namespace Packaging
{
    // XOR is its own inverse, so the same function obfuscates and deobfuscates.
    inline Key ObfuscateKey(const Key& key)
    {
        static constexpr unsigned char kMask[32] = {
            0x4B, 0x92, 0x1D, 0x77, 0xC3, 0x0A, 0x5E, 0xF1,
            0x88, 0x36, 0xA9, 0x64, 0x1F, 0xD2, 0x7B, 0x50,
            0xE4, 0x2C, 0x99, 0x0D, 0x63, 0xB7, 0x3A, 0xF8,
            0x16, 0xAC, 0x59, 0xD0, 0x8E, 0x41, 0xC6, 0x2F,
        };

        Key result{};
        for (size_t i = 0; i < result.size(); ++i)
            result[i] = static_cast<unsigned char>(key[i] ^ kMask[i]);
        return result;
    }
}
