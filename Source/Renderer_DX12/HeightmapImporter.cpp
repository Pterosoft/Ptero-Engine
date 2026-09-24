#include "pch.h"
#include "System/DataFiles.h"

#include "HeightmapImporter.h"

// DirectXTex provides the in-memory image container + DDS writer used below.
#include "..\SDKs\DirectXTex\DirectXTex\DirectXTex.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{
    // Walk upward from the current executable to find the project's
    // Data/ directory.  Mirrors the helper used elsewhere in the renderer
    // so terrain files stored under Data/ are loadable regardless of the
    // editor's CWD.
    std::filesystem::path FindProjectDataDirectory()
    {
        // Walks up from the exe to Data/ - or, in a packaged game, the virtual Data
        // root the .ppak archives serve (see System/DataFiles.h).
        return DataFiles::FindDataDirectory();
    }
}

namespace HeightmapImporter
{
    std::filesystem::path ResolveDataRelativePath(const std::string& path)
    {
        if (path.empty())
            return {};
        std::error_code ec;
        const std::filesystem::path inputPath(path);
        if (inputPath.is_absolute())
            return inputPath.lexically_normal();

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (dataDirectory.empty())
            return inputPath.lexically_normal();
        return (dataDirectory / inputPath).lexically_normal();
    }

    std::string ResolveDataRelativeToAbsolute(const std::string& path)
    {
        return ResolveDataRelativePath(path).generic_string();
    }

    bool LoadRaw16(
        const std::string& rawPath,
        int width,
        int height,
        std::vector<std::uint16_t>& outSamples,
        std::string& outError)
    {
        outSamples.clear();
        outError.clear();

        if (width <= 0 || height <= 0)
        {
            outError = "HeightmapImporter::LoadRaw16: width/height must be positive.";
            return false;
        }

        const std::string absolutePath = ResolveDataRelativeToAbsolute(rawPath);
        DataFiles::InputFile inputStream(absolutePath, std::ios::binary);
        if (!inputStream)
        {
            outError = "HeightmapImporter::LoadRaw16: could not open '" + rawPath
                + "' (resolved to '" + absolutePath + "') for reading.";
            return false;
        }

        const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 2u;
        inputStream.seekg(0, std::ios::end);
        const std::streamoff fileSize = inputStream.tellg();
        inputStream.seekg(0, std::ios::beg);
        if (fileSize < 0 || static_cast<size_t>(fileSize) < expectedBytes)
        {
            outError = "HeightmapImporter::LoadRaw16: file is smaller than width*height*2 ("
                + std::to_string(fileSize) + " < " + std::to_string(expectedBytes) + ").";
            return false;
        }

        outSamples.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        inputStream.read(reinterpret_cast<char*>(outSamples.data()), expectedBytes);
        if (inputStream.gcount() != static_cast<std::streamsize>(expectedBytes))
        {
            outError = "HeightmapImporter::LoadRaw16: short read from '" + rawPath + "'.";
            outSamples.clear();
            return false;
        }

        // The .raw format is little-endian uint16 on disk.  The CPU may be
        // little-endian already (every x64 Windows target is), but we still
        // byte-swap defensively if a big-endian sample slips through.  The
        // constant 0x0102 detects endianness at compile time.
        if (static_cast<uint16_t>(0x0102) != 0x0102)
        {
            for (std::uint16_t& sample : outSamples)
            {
                sample = static_cast<std::uint16_t>(
                    (sample << 8) | (sample >> 8));
            }
        }

        return true;
    }

    bool LoadImageToHeightmap(
        const std::string& imagePath,
        std::vector<std::uint16_t>& outSamples,
        int& outWidth,
        int& outHeight,
        std::string& outError)
    {
        outSamples.clear();
        outWidth  = 0;
        outHeight = 0;
        outError.clear();

        if (imagePath.empty())
        {
            outError = "HeightmapImporter::LoadImageToHeightmap: empty path.";
            return false;
        }

        const std::string absolutePath = ResolveDataRelativeToAbsolute(imagePath);

        // CoInitialize is required for the WIC decoder; it's a no-op if
        // COM is already up on this thread.
        const HRESULT comInitResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(comInitResult);

        DirectX::ScratchImage image;
        DirectX::TexMetadata  meta;
        const std::wstring widePath = std::wstring(absolutePath.begin(), absolutePath.end());

        const std::filesystem::path srcFs(absolutePath);
        std::string ext = srcFs.extension().string();
        for (char& c : ext) { c = static_cast<char>(::tolower(static_cast<unsigned char>(c))); }

        HRESULT hr = E_FAIL;
        if      (ext == ".dds")  hr = DirectX::LoadFromDDSFile(widePath.c_str(), DirectX::DDS_FLAGS_NONE, &meta, image);
        else if (ext == ".tga")  hr = DirectX::LoadFromTGAFile(widePath.c_str(), DirectX::TGA_FLAGS_NONE, &meta, image);
        else if (ext == ".hdr")  hr = DirectX::LoadFromHDRFile(widePath.c_str(), &meta, image);
        else                     hr = DirectX::LoadFromWICFile(widePath.c_str(), DirectX::WIC_FLAGS_NONE, &meta, image);

        if (FAILED(hr))
        {
            outError = "HeightmapImporter::LoadImageToHeightmap: failed to decode '"
                + absolutePath + "' (HRESULT " + std::to_string(hr) + ").";
            if (shouldUninitialize) CoUninitialize();
            return false;
        }

        if (meta.width == 0 || meta.height == 0)
        {
            outError = "HeightmapImporter::LoadImageToHeightmap: zero-sized image.";
            if (shouldUninitialize) CoUninitialize();
            return false;
        }

        // Convert to a known channel layout so we can read the first
        // channel as a height value.  R8_UNORM keeps the R channel intact;
        // for HDR sources (32-bit float per channel) we convert through
        // R32_FLOAT and then rescale to 0..65535.  BC-compressed sources
        // (e.g. an existing .dds) are decompressed to RGBA8.
        DirectX::ScratchImage converted;
        if (meta.format == DXGI_FORMAT_R32_FLOAT)
        {
            hr = DirectX::Convert(image.GetImages(), image.GetImageCount(), meta,
                                  DXGI_FORMAT_R32_FLOAT, DirectX::TEX_FILTER_DEFAULT,
                                  DirectX::TEX_THRESHOLD_DEFAULT, converted);
        }
        else if (DirectX::IsCompressed(meta.format))
        {
            DirectX::ScratchImage decompressed;
            hr = DirectX::Decompress(image.GetImages(), image.GetImageCount(), meta,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, decompressed);
            if (SUCCEEDED(hr))
            {
                converted = std::move(decompressed);
                meta = converted.GetMetadata();
            }
        }
        else
        {
            hr = DirectX::Convert(image.GetImages(), image.GetImageCount(), meta,
                                  DXGI_FORMAT_R8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                  DirectX::TEX_THRESHOLD_DEFAULT, converted);
            if (FAILED(hr))
            {
                // Fall back to RGBA8 for sources that don't support direct
                // conversion to single-channel R8 (e.g. RGB565).  The R
                // channel is what we read below, so RGBA8 is functionally
                // equivalent for the heightmap.
                hr = DirectX::Convert(image.GetImages(), image.GetImageCount(), meta,
                                      DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT,
                                      DirectX::TEX_THRESHOLD_DEFAULT, converted);
            }
        }
        if (FAILED(hr))
        {
            outError = "HeightmapImporter::LoadImageToHeightmap: channel conversion failed (HRESULT "
                + std::to_string(hr) + ").";
            if (shouldUninitialize) CoUninitialize();
            return false;
        }

        const DirectX::Image* topMip = converted.GetImage(0, 0, 0);
        if (topMip == nullptr)
        {
            outError = "HeightmapImporter::LoadImageToHeightmap: missing top mip after conversion.";
            if (shouldUninitialize) CoUninitialize();
            return false;
        }

        outWidth  = static_cast<int>(topMip->width);
        outHeight = static_cast<int>(topMip->height);
        outSamples.assign(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight), 0);

        const bool   isFloat    = (topMip->format == DXGI_FORMAT_R32_FLOAT);
        const bool   isSingle8  = (topMip->format == DXGI_FORMAT_R8_UNORM);
        const uint8_t* pixels   = topMip->pixels;
        const size_t  rowPitch  = topMip->rowPitch;

        for (size_t y = 0; y < static_cast<size_t>(outHeight); ++y)
        {
            const uint8_t* row = pixels + y * rowPitch;
            for (size_t x = 0; x < static_cast<size_t>(outWidth); ++x)
            {
                float normalisedHeight = 0.0f;
                if (isFloat)
                {
                    // R32_FLOAT: 4 bytes per sample.
                    const float sample = reinterpret_cast<const float*>(row)[x];
                    normalisedHeight = (sample < 0.0f) ? 0.0f : (sample > 1.0f ? 1.0f : sample);
                }
                else if (isSingle8)
                {
                    normalisedHeight = static_cast<float>(row[x]) / 255.0f;
                }
                else // R8G8B8A8_UNORM: take the red channel.
                {
                    normalisedHeight = static_cast<float>(row[x * 4]) / 255.0f;
                }

                const float scaled = normalisedHeight * 65535.0f + 0.5f;
                outSamples[y * static_cast<size_t>(outWidth) + x] =
                    static_cast<std::uint16_t>((scaled < 0.0f) ? 0.0f :
                                               (scaled > 65535.0f ? 65535.0f : scaled));
            }
        }

        if (shouldUninitialize) CoUninitialize();
        return true;
    }

    bool WriteDdsR16(
        const std::wstring& ddsPath,
        const std::vector<std::uint16_t>& samples,
        int width,
        int height,
        std::string& outError)
    {
        outError.clear();

        if (samples.empty() || width <= 0 || height <= 0)
        {
            outError = "HeightmapImporter::WriteDdsR16: empty input or invalid dimensions.";
            return false;
        }

        if (static_cast<size_t>(width) * static_cast<size_t>(height) != samples.size())
        {
            outError = "HeightmapImporter::WriteDdsR16: sample count does not match width*height.";
            return false;
        }

        // Build a DirectXTex image with the exact layout the engine will
        // sample.  R16_UNORM is a single-channel 16-bit format.  The
        // heightmap .raw is row-major with the top row first, which matches
        // DirectXTex's default image orientation, so no flipping is needed.
        DirectX::TexMetadata metadata{};
        metadata.width      = static_cast<size_t>(width);
        metadata.height     = static_cast<size_t>(height);
        metadata.depth      = 1u;
        metadata.arraySize  = 1u;
        metadata.mipLevels  = 1u;
        metadata.format     = DXGI_FORMAT_R16_UNORM;
        metadata.dimension  = DirectX::TEX_DIMENSION_TEXTURE2D;
        metadata.miscFlags  = 0u;
        metadata.miscFlags2 = 0u;

        DirectX::Image image{};
        image.width     = metadata.width;
        image.height    = metadata.height;
        image.format    = metadata.format;
        image.rowPitch  = static_cast<size_t>(width) * 2u;       // 2 bytes per R16 sample
        image.slicePitch = image.rowPitch * static_cast<size_t>(height);
        image.pixels    = reinterpret_cast<uint8_t*>(const_cast<std::uint16_t*>(samples.data()));

        std::filesystem::create_directories(std::filesystem::path(ddsPath).parent_path());

        const HRESULT hr = DirectX::SaveToDDSFile(
            &image, 1u, metadata, DirectX::DDS_FLAGS_NONE, ddsPath.c_str());

        if (FAILED(hr))
        {
            outError = "HeightmapImporter::WriteDdsR16: SaveToDDSFile failed (HRESULT "
                + std::to_string(hr) + ").";
            return false;
        }

        return true;
    }

    std::string DerivedDdsPath(const std::string& sourcePath)
    {
        if (sourcePath.empty())
            return {};
        const std::filesystem::path absFs(ResolveDataRelativePath(sourcePath));
        return (absFs.parent_path() / (absFs.stem().string() + ".dds")).generic_string();
    }

    bool EncodeHeightmapToDds(
        const std::string& sourcePath,
        const std::vector<std::uint16_t>& samples,
        int width,
        int height,
        std::string& outError)
    {
        outError.clear();

        if (samples.empty() || width <= 0 || height <= 0)
        {
            outError = "HeightmapImporter::EncodeHeightmapToDds: empty samples or invalid dimensions.";
            return false;
        }

        const std::string ddsPath = DerivedDdsPath(sourcePath);
        if (ddsPath.empty())
        {
            outError = "HeightmapImporter::EncodeHeightmapToDds: could not derive DDS path.";
            return false;
        }

        const std::wstring wideDds = std::wstring(ddsPath.begin(), ddsPath.end());
        if (!WriteDdsR16(wideDds, samples, width, height, outError))
        {
            return false;
        }

        return true;
    }

    std::string ConvertRaw16ToDds(
        const std::string& rawPath,
        int width,
        int height,
        std::string& outError)
    {
        outError.clear();

        if (rawPath.empty())
        {
            outError = "HeightmapImporter::ConvertRaw16ToDds: empty source path.";
            return {};
        }

        std::vector<std::uint16_t> samples;
        int resolvedWidth  = width;
        int resolvedHeight = height;

        const std::filesystem::path srcFs(rawPath);
        std::string ext = srcFs.extension().string();
        for (char& c : ext) { c = static_cast<char>(::tolower(static_cast<unsigned char>(c))); }

        if (ext == ".raw")
        {
            if (width <= 0 || height <= 0)
            {
                outError = "HeightmapImporter::ConvertRaw16ToDds: .raw sources require explicit width/height.";
                return {};
            }
            if (!LoadRaw16(rawPath, width, height, samples, outError))
            {
                return {};
            }
        }
        else
        {
            if (!LoadImageToHeightmap(rawPath, samples, resolvedWidth, resolvedHeight, outError))
            {
                return {};
            }
        }

        if (!EncodeHeightmapToDds(rawPath, samples, resolvedWidth, resolvedHeight, outError))
        {
            return {};
        }

        return DerivedDdsPath(rawPath);
    }
}
