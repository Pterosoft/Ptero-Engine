#pragma once

#include <string>

// TextureImporter converts source image files (PNG, JPG, TGA, BMP, HDR, or already-DDS)
// into GPU-ready BC-compressed DDS files stored under Data/Textures.
// Compression format is chosen from the destination/source name so base color keeps
// higher quality and normal maps avoid low-quality BC1 artefacts.
class TextureImporter
{
public:
    // Import srcPath into destDdsPath.  destDdsPath must end with ".dds".
    // Returns true on success; on failure, LastError() contains a human-readable message.
    bool Import(const std::string& srcPath, const std::string& destDdsPath);

    const std::string& LastError() const { return mLastError; }

private:
    static bool IsLikelyNormalMapPath(const std::string& path);
    static bool IsLikelyMaskTexturePath(const std::string& path);

    std::string mLastError;
};
