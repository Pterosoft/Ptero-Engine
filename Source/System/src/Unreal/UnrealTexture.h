#pragma once

#include "UnrealPackage.h"

#include <string>

namespace DirectX
{
    class ScratchImage;
}

namespace Ptero::Unreal
{
    struct TextureInfo
    {
        std::string Name;
        int Width = 0;
        int Height = 0;
        std::string SourceFormat;        // TSF_BGRA8, TSF_G8, ...
        std::string SourceCompression;   // TSCF_None, TSCF_PNG, TSCF_UEDELTA, ...
        std::string CompressionSettings; // TC_Default, TC_Normalmap, TC_Masks, ...
        bool SRGB = true;

        bool IsNormalMap() const { return CompressionSettings == "TC_Normalmap"; }
        bool IsLinear() const
        {
            return !SRGB || CompressionSettings == "TC_Masks" || CompressionSettings == "TC_Grayscale" ||
                CompressionSettings == "TC_Alpha" || CompressionSettings == "TC_Displacementmap" ||
                CompressionSettings == "TC_VectorDisplacementmap" || CompressionSettings == "TC_DistanceFieldFont";
        }
        bool IsHdr() const
        {
            return CompressionSettings == "TC_HDR" || CompressionSettings == "TC_HDR_Compressed" ||
                CompressionSettings == "TC_HalfFloat" || CompressionSettings == "TC_SingleFloat" ||
                CompressionSettings == "TC_HDR_F32";
        }
    };

    // Reads the metadata of the package's Texture2D without touching the pixel payload.
    bool ReadTextureInfo(const Package& package, TextureInfo& info, std::string& error);

    // Decodes the texture's source art (mip 0 of the first block/slice/layer). The image is
    // B8G8R8A8 for 8-bit sources (grayscale is expanded to gray RGB), R16G16B16A16 for 16-bit
    // ones and R32G32B32A32_FLOAT for HDR.
    bool ReadTextureSource(const Package& package, TextureInfo& info, DirectX::ScratchImage& image, std::string& error);
}
