#pragma once

// HeightmapImporter
// -----------------
// Loads a 16-bit unsigned little-endian .raw heightmap (width*height*2 bytes)
// from disk and produces two artefacts used by the terrain system:
//   * a CPU-side std::vector<uint16_t> of normalised samples (used to build
//     and edit the terrain mesh in the editor), and
//   * a sibling DDS file (R16_UNORM) the rest of the engine can GPU-load
//     through the standard TextureManager.
//
// The .raw format assumed is the common Unity/UE4 16-bit heightmap format
// (row-major, top row first, sample 0 = lowest elevation).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace HeightmapImporter
{
    // Resolve a path that may be relative to the project's Data/ directory
    // into an absolute path the OS can open.  Walks upward from the
    // current executable to find the Data/ root, then joins.  Returns an
    // empty path if no Data/ directory can be located.
    std::filesystem::path ResolveDataRelativePath(const std::string& path);

    // Convenience wrapper for the file-importer path -- if the input is
    // already absolute it is returned unchanged, otherwise the project
    // Data/ root is prepended.
    std::string ResolveDataRelativeToAbsolute(const std::string& path);

    // Read the entire .raw file at rawPath into samples.  The caller is
    // responsible for providing the width/height (the .raw header is just a
    // flat blob so we need a side channel).  The returned vector is
    // row-major: index = y * width + x, y = 0 is the top row.
    bool LoadRaw16(
        const std::string& rawPath,
        int width,
        int height,
        std::vector<std::uint16_t>& outSamples,
        std::string& outError);

    // Read an image (PNG/JPG/BMP/TIF/TGA/HDR/EXR/etc.) and collapse the
    // first channel (or luminance, for HDR) into a 0..65535 sample grid.
    // The returned vector is row-major: index = y * width + x.  Width and
    // height are taken from the loaded image.
    bool LoadImageToHeightmap(
        const std::string& imagePath,
        std::vector<std::uint16_t>& outSamples,
        int& outWidth,
        int& outHeight,
        std::string& outError);

    // Write a single-channel R16_UNORM DDS to ddsPath from the supplied
    // samples.  The DDS is *uncompressed* (R16_UNORM) so it can be sampled
    // losslessly on the GPU; runtime mip generation is the consumer's job.
    bool WriteDdsR16(
        const std::wstring& ddsPath,
        const std::vector<std::uint16_t>& samples,
        int width,
        int height,
        std::string& outError);

    // Compute the absolute DDS path that ConvertRaw16ToDds / Convert would
    // produce for the given source file.  Centralised so the editor can
    // show it in the modal without re-implementing the path logic.
    std::string DerivedDdsPath(const std::string& sourcePath);

    // Encode a pre-decoded heightmap (already collapsed to 0..65535
    // samples) into the DDS that lives next to the source file.  The
    // companion to LoadImageToHeightmap / LoadRaw16 for callers that have
    // cached the decode and want to skip a re-read.
    bool EncodeHeightmapToDds(
        const std::string& sourcePath,
        const std::vector<std::uint16_t>& samples,
        int width,
        int height,
        std::string& outError);

    // Convenience: do both at once and leave the DDS in the same directory
    // as the source file with a `.dds` suffix.  Returns the absolute DDS
    // path on success (empty string on failure).  The source can be either
    // a .raw heightmap (width/height required) or any image format
    // supported by DirectXTex (PNG, JPG, BMP, TIF, TGA, EXR/HDR).  For
    // image sources width/height are read from the file and may be left
    // at zero.
    std::string ConvertRaw16ToDds(
        const std::string& rawPath,
        int width,
        int height,
        std::string& outError);
}
