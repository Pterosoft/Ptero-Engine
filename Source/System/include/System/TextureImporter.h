#pragma once

#include <string>

namespace DirectX
{
    class ScratchImage;
}

// TextureImporter converts source image files (PNG, JPG, TGA, BMP, HDR, or already-DDS)
// into GPU-ready BC-compressed DDS files stored under Data/Textures.
// Compression format is chosen from the destination/source name so base color keeps
// higher quality and normal maps avoid low-quality BC1 artefacts.
class TextureImporter
{
public:
    // How the pixels are used, which picks the block format and colour space.
    enum class Role
    {
        Color,    // sRGB albedo / emissive: BC3 sRGB
        Linear,   // masks and packed data: BC3 linear
        Normal,   // tangent-space normals: BC5
    };

    // Import srcPath into destDdsPath.  destDdsPath must end with ".dds".
    // Returns true on success; on failure, LastError() contains a human-readable message.
    bool Import(const std::string& srcPath, const std::string& destDdsPath);

    // Encodes an already decoded image (any format DirectXTex can convert) into destDdsPath.
    bool ImportImage(const DirectX::ScratchImage& image, const std::string& destDdsPath, Role role);

    const std::string& LastError() const { return mLastError; }

private:
    static bool IsLikelyNormalMapPath(const std::string& path);
    static bool IsLikelyMaskTexturePath(const std::string& path);

    bool Encode(DirectX::ScratchImage& image, const std::string& destDdsPath, Role role);

    std::string mLastError;
};
