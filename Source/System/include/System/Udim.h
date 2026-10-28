#pragma once

#include <cmath>
#include <cstdint>
#include <string>

// UDIM texture sets.
//
// A material texture path may contain the token <UDIM> (as written by Substance Painter,
// Mari, etc.), e.g. "Geometry/Mastaba/Textures/M_Walls_Base_color_<UDIM>.dds". The mesh
// keeps the UVs of every UDIM tile as authored (u in [0,10), v in [0,N)), and the
// renderer draws each tile's triangles with the token replaced by that tile's number.
// Tile numbering follows the usual convention: 1001 + u + 10 * v, with u/v being the
// integer part of the authored (unflipped) UV.
namespace Udim
{
    inline constexpr const char* kToken = "<UDIM>";
    inline constexpr std::uint32_t kFirstTile = 1001;
    inline constexpr std::uint32_t kLastTile  = 1100;

    inline bool HasToken(const std::string& path)
    {
        return path.find(kToken) != std::string::npos;
    }

    // Replaces every <UDIM> token with the tile number. Paths without the token are returned unchanged.
    inline std::string Resolve(std::string path, std::uint32_t tile)
    {
        const std::string tileText = std::to_string(tile);
        for (std::size_t pos = path.find(kToken); pos != std::string::npos; pos = path.find(kToken, pos + tileText.size()))
        {
            path.replace(pos, std::char_traits<char>::length(kToken), tileText);
        }
        return path;
    }

    // Tile number for an engine texture coordinate. The engine stores v flipped
    // (v' = 1 - v, see FbxCompiler), so the authored row is floor(1 - v').
    inline std::uint32_t TileFromTexCoord(float u, float vFlipped)
    {
        const int col = static_cast<int>(std::floor(u));
        const int row = static_cast<int>(std::floor(1.0f - vFlipped));
        if (col < 0 || col > 9 || row < 0 || row > 9)
            return 0; // outside the UDIM range
        return kFirstTile + static_cast<std::uint32_t>(col + 10 * row);
    }
}
