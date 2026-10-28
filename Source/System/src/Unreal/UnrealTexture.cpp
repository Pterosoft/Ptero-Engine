#include "UnrealTexture.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include "..\SDKs\DirectXTex\DirectXTex\DirectXTex.h"
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>

namespace Ptero::Unreal
{
    namespace
    {
        struct SourceFormatDesc
        {
            const char* Name;
            int BytesPerPixel;
            int BytesPerChannel;
        };

        constexpr SourceFormatDesc kSourceFormats[] = {
            { "TSF_G8", 1, 1 },
            { "TSF_BGRA8", 4, 1 },
            { "TSF_BGRE8", 4, 1 },
            { "TSF_RGBA16", 8, 2 },
            { "TSF_RGBA16F", 8, 2 },
            { "TSF_RGBA8_DEPRECATED", 4, 1 },
            { "TSF_RGBE8_DEPRECATED", 4, 1 },
            { "TSF_G16", 2, 2 },
            { "TSF_RGBA32F", 16, 4 },
            { "TSF_R16F", 2, 2 },
            { "TSF_R32F", 4, 4 },
        };

        const SourceFormatDesc* FindSourceFormat(const std::string& name)
        {
            for (const SourceFormatDesc& desc : kSourceFormats)
            {
                if (name == desc.Name)
                {
                    return &desc;
                }
            }
            return nullptr;
        }

        // Inverse of TSCF_UEDELTA (UE 5.8+). The encoder splits the image into tiles whose
        // rows are at most 4096 bytes wide and roughly 32K pixels in area, and inside each
        // tile stores every row as the difference to the row above; a tile's first row is
        // stored as is. Only the tile heights matter for decoding, since the prediction is
        // purely vertical. 8-bit channels are plain byte differences; 16-bit channels are
        // word differences biased by 0x8080. Derived from and verified byte-exact against
        // assets saved by UE 5.8 at many sizes and formats.
        bool UndoUnrealDelta(std::uint8_t* pixels, const int width, const int height, const SourceFormatDesc& format, std::string& error)
        {
            const std::size_t rowBytes = static_cast<std::size_t>(width) * format.BytesPerPixel;
            if (format.BytesPerChannel > 2)
            {
                error = std::string("UE Delta compression of ") + format.Name + " textures is not supported";
                return false;
            }

            const std::int64_t tilesAcross = (std::max<std::int64_t>)(1, static_cast<std::int64_t>((rowBytes + 4095) / 4096));
            const std::int64_t tileWidth = (width + tilesAcross - 1) / tilesAcross;
            const std::int64_t targetRows = (32768 + tileWidth - 1) / tileWidth;
            const std::int64_t tilesDown = (std::max<std::int64_t>)(1, height / targetRows);
            const std::int64_t rowsPerTile = (height + tilesDown - 1) / tilesDown;

            for (std::int64_t y = 0; y < height; ++y)
            {
                if (y % rowsPerTile == 0)
                {
                    continue;
                }

                std::uint8_t* row = pixels + static_cast<std::size_t>(y) * rowBytes;
                const std::uint8_t* above = row - rowBytes;
                if (format.BytesPerChannel == 1)
                {
                    for (std::size_t index = 0; index < rowBytes; ++index)
                    {
                        row[index] = static_cast<std::uint8_t>(row[index] + above[index]);
                    }
                }
                else
                {
                    for (std::size_t index = 0; index + 1 < rowBytes; index += 2)
                    {
                        std::uint16_t delta = 0;
                        std::uint16_t previous = 0;
                        std::memcpy(&delta, row + index, 2);
                        std::memcpy(&previous, above + index, 2);
                        const std::uint16_t value = static_cast<std::uint16_t>(delta + previous - 0x8080u);
                        std::memcpy(row + index, &value, 2);
                    }
                }
            }
            return true;
        }

        // Same decode as FColor::FromRGBE.
        float DecodeRgbeChannel(const std::uint8_t mantissa, const std::uint8_t exponent)
        {
            return exponent == 0 ? 0.0f : static_cast<float>(mantissa) / 255.0f * std::ldexp(1.0f, static_cast<int>(exponent) - 128);
        }

        // Converts raw source pixels into one of the three image layouts the texture
        // importer consumes.
        bool BuildImage(const std::uint8_t* pixels, const int width, const int height, const SourceFormatDesc& format,
            const bool srgb, DirectX::ScratchImage& image, std::string& error)
        {
            const std::string name = format.Name;
            const std::size_t pixelCount = static_cast<std::size_t>(width) * height;

            if (name == "TSF_G8" || name == "TSF_BGRA8" || name == "TSF_RGBA8_DEPRECATED")
            {
                const DXGI_FORMAT dxgi = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
                if (FAILED(image.Initialize2D(dxgi, width, height, 1, 1)))
                {
                    error = "out of memory";
                    return false;
                }
                std::uint8_t* out = image.GetImage(0, 0, 0)->pixels;
                for (std::size_t index = 0; index < pixelCount; ++index)
                {
                    std::uint8_t* dst = out + index * 4;
                    if (name == "TSF_G8")
                    {
                        dst[0] = dst[1] = dst[2] = pixels[index];
                        dst[3] = 255;
                    }
                    else if (name == "TSF_BGRA8")
                    {
                        std::memcpy(dst, pixels + index * 4, 4);
                    }
                    else
                    {
                        const std::uint8_t* src = pixels + index * 4;
                        dst[0] = src[2];
                        dst[1] = src[1];
                        dst[2] = src[0];
                        dst[3] = src[3];
                    }
                }
                return true;
            }

            if (name == "TSF_G16" || name == "TSF_RGBA16")
            {
                if (FAILED(image.Initialize2D(DXGI_FORMAT_R16G16B16A16_UNORM, width, height, 1, 1)))
                {
                    error = "out of memory";
                    return false;
                }
                std::uint16_t* out = reinterpret_cast<std::uint16_t*>(image.GetImage(0, 0, 0)->pixels);
                for (std::size_t index = 0; index < pixelCount; ++index)
                {
                    if (name == "TSF_G16")
                    {
                        std::uint16_t value = 0;
                        std::memcpy(&value, pixels + index * 2, 2);
                        out[index * 4 + 0] = out[index * 4 + 1] = out[index * 4 + 2] = value;
                        out[index * 4 + 3] = 0xFFFF;
                    }
                    else
                    {
                        std::memcpy(out + index * 4, pixels + index * 8, 8);
                    }
                }
                return true;
            }

            if (FAILED(image.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1)))
            {
                error = "out of memory";
                return false;
            }
            float* out = reinterpret_cast<float*>(image.GetImage(0, 0, 0)->pixels);
            for (std::size_t index = 0; index < pixelCount; ++index)
            {
                float* dst = out + index * 4;
                if (name == "TSF_BGRE8" || name == "TSF_RGBE8_DEPRECATED")
                {
                    const std::uint8_t* src = pixels + index * 4;
                    const bool bgr = name == "TSF_BGRE8";
                    dst[0] = DecodeRgbeChannel(bgr ? src[2] : src[0], src[3]);
                    dst[1] = DecodeRgbeChannel(src[1], src[3]);
                    dst[2] = DecodeRgbeChannel(bgr ? src[0] : src[2], src[3]);
                    dst[3] = 1.0f;
                }
                else if (name == "TSF_RGBA16F")
                {
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        std::uint16_t half = 0;
                        std::memcpy(&half, pixels + index * 8 + channel * 2, 2);
                        dst[channel] = DirectX::PackedVector::XMConvertHalfToFloat(half);
                    }
                }
                else if (name == "TSF_R16F")
                {
                    std::uint16_t half = 0;
                    std::memcpy(&half, pixels + index * 2, 2);
                    dst[0] = dst[1] = dst[2] = DirectX::PackedVector::XMConvertHalfToFloat(half);
                    dst[3] = 1.0f;
                }
                else if (name == "TSF_RGBA32F")
                {
                    std::memcpy(dst, pixels + index * 16, 16);
                }
                else if (name == "TSF_R32F")
                {
                    std::memcpy(dst, pixels + index * 4, 4);
                    dst[1] = dst[2] = dst[0];
                    dst[3] = 1.0f;
                }
                else
                {
                    error = "unsupported texture source format " + name;
                    return false;
                }
            }
            return true;
        }

        // PNG / JPEG source art is a complete image file; WIC decodes it and the result is
        // brought into the same layouts as raw sources.
        //
        // UE compresses a TSF_BGRA8 source by handing its BGRA bytes to the PNG encoder as if
        // they were RGBA, so such a PNG holds blue where it says red (verified against a TGA
        // exported from the editor). swapRedBlue undoes that.
        bool DecodeImageFile(const std::vector<std::uint8_t>& bytes, const bool srgb, const bool swapRedBlue,
            DirectX::ScratchImage& image, std::string& error)
        {
            const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            DirectX::ScratchImage decoded;
            DirectX::TexMetadata metadata{};
            const HRESULT hr = DirectX::LoadFromWICMemory(bytes.data(), bytes.size(), DirectX::WIC_FLAGS_IGNORE_SRGB, &metadata, decoded);
            if (SUCCEEDED(comInit))
            {
                CoUninitialize();
            }
            if (FAILED(hr))
            {
                error = "could not decode the embedded source image";
                return false;
            }

            // Convert between UNORM formats only, so no transfer curve is applied; the sRGB tag
            // goes on afterwards. The stored values are already sRGB-encoded.
            const bool wide = DirectX::BitsPerColor(metadata.format) > 8;
            const DXGI_FORMAT target = wide ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            if (metadata.format == target)
            {
                image = std::move(decoded);
            }
            else
            {
                // Grayscale must be replicated into RGB rather than landing only in red.
                const bool gray = metadata.format == DXGI_FORMAT_R8_UNORM || metadata.format == DXGI_FORMAT_R16_UNORM;
                const DirectX::TEX_FILTER_FLAGS filter = gray ? DirectX::TEX_FILTER_RGB_COPY_RED : DirectX::TEX_FILTER_DEFAULT;
                if (FAILED(DirectX::Convert(*decoded.GetImage(0, 0, 0), target, filter, DirectX::TEX_THRESHOLD_DEFAULT, image)))
                {
                    error = "could not convert the embedded source image";
                    return false;
                }
            }

            if (swapRedBlue && !wide)
            {
                const DirectX::Image* pixels = image.GetImage(0, 0, 0);
                for (std::size_t y = 0; y < pixels->height; ++y)
                {
                    std::uint8_t* row = pixels->pixels + y * pixels->rowPitch;
                    for (std::size_t x = 0; x < pixels->width; ++x)
                    {
                        std::swap(row[x * 4 + 0], row[x * 4 + 2]);
                    }
                }
            }

            if (srgb && !wide)
            {
                image.OverrideFormat(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
            }
            return true;
        }
    }

    namespace
    {
        // Texture2D, or a TextureCube whose source is a single long-lat (equirectangular)
        // image, which is how UE stores imported .hdr / .exr skies.
        const ObjectExport* FindTextureExport(const Package& package)
        {
            if (const ObjectExport* texture = package.FindExportByClass("Texture2D"))
            {
                return texture;
            }
            return package.FindExportByClass("TextureCube");
        }
    }

    bool ReadTextureInfo(const Package& package, TextureInfo& info, std::string& error)
    {
        const ObjectExport* textureExport = FindTextureExport(package);
        if (textureExport == nullptr)
        {
            error = package.FindExportByClass("Texture2DArray") != nullptr
                ? "texture arrays are not supported"
                : "the package does not contain a Texture2D";
            return false;
        }

        info.Name = textureExport->ObjectName;
        ByteReader reader = package.ExportPropertyReader(*textureExport);
        const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);

        if (const PropertyTag* srgb = FindProperty(tags, "SRGB"))
        {
            info.SRGB = srgb->BoolValue;
        }
        if (const PropertyTag* settings = FindProperty(tags, "CompressionSettings"))
        {
            info.CompressionSettings = EnumValueName(PropertyEnum(reader, *settings, package));
        }
        if (info.CompressionSettings.empty())
        {
            info.CompressionSettings = "TC_Default";
        }

        const PropertyTag* sourceTag = FindProperty(tags, "Source");
        if (sourceTag == nullptr)
        {
            error = "the texture has no source art (it may be cooked)";
            return false;
        }

        const std::vector<PropertyTag> source = PropertyStruct(reader, *sourceTag, package);
        if (const PropertyTag* tag = FindProperty(source, "SizeX")) info.Width = static_cast<int>(PropertyInt(reader, *tag));
        if (const PropertyTag* tag = FindProperty(source, "SizeY")) info.Height = static_cast<int>(PropertyInt(reader, *tag));
        if (const PropertyTag* tag = FindProperty(source, "Format")) info.SourceFormat = EnumValueName(PropertyEnum(reader, *tag, package));
        if (const PropertyTag* tag = FindProperty(source, "CompressionFormat")) info.SourceCompression = EnumValueName(PropertyEnum(reader, *tag, package));
        if (info.SourceCompression.empty())
        {
            const PropertyTag* png = FindProperty(source, "bPNGCompressed");
            info.SourceCompression = (png != nullptr && png->BoolValue) ? "TSCF_PNG" : "TSCF_None";
        }

        if (!reader.Ok() || info.Width <= 0 || info.Height <= 0 || info.SourceFormat.empty())
        {
            error = "could not read the texture's source description";
            return false;
        }

        if (package.ExportClassName(*textureExport) == "TextureCube")
        {
            const PropertyTag* longLat = FindProperty(source, "bLongLatCubemap");
            const PropertyTag* slices = FindProperty(source, "NumSlices");
            const bool single = (longLat != nullptr && longLat->BoolValue) || (slices != nullptr && PropertyInt(reader, *slices) == 1);
            if (!single)
            {
                error = "six-face cube maps are not supported (long-lat ones are)";
                return false;
            }
        }
        return true;
    }

    bool ReadTextureSource(const Package& package, TextureInfo& info, DirectX::ScratchImage& image, std::string& error)
    {
        if (!ReadTextureInfo(package, info, error))
        {
            return false;
        }

        const SourceFormatDesc* format = FindSourceFormat(info.SourceFormat);
        if (format == nullptr)
        {
            error = "unsupported texture source format " + info.SourceFormat;
            return false;
        }

        const ObjectExport* textureExport = FindTextureExport(package);
        EditorBulkDataRef bulkData;
        if (!FindEditorBulkData(package, *textureExport, bulkData))
        {
            error = "could not locate the texture's source payload";
            return false;
        }

        std::vector<std::uint8_t> payload;
        if (!ReadEditorBulkData(package, bulkData, payload, error))
        {
            return false;
        }

        const bool srgb = info.SRGB && !info.IsNormalMap();

        if (info.SourceCompression == "TSCF_PNG" || info.SourceCompression == "TSCF_JPEG")
        {
            const bool swapRedBlue = info.SourceCompression == "TSCF_PNG" && info.SourceFormat == "TSF_BGRA8";
            return DecodeImageFile(payload, srgb, swapRedBlue, image, error);
        }
        if (info.SourceCompression != "TSCF_None" && info.SourceCompression != "TSCF_UEDELTA")
        {
            error = "texture source compression " + info.SourceCompression + " is not supported";
            return false;
        }

        // Mip 0 of the first block/slice/layer comes first in the payload.
        const std::size_t mipBytes = static_cast<std::size_t>(info.Width) * info.Height * format->BytesPerPixel;
        if (payload.size() < mipBytes)
        {
            error = "texture payload is smaller than its first mip";
            return false;
        }

        if (info.SourceCompression == "TSCF_UEDELTA" &&
            !UndoUnrealDelta(payload.data(), info.Width, info.Height, *format, error))
        {
            return false;
        }

        return BuildImage(payload.data(), info.Width, info.Height, *format, srgb, image, error);
    }
}
