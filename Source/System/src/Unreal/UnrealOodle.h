#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Oodle is Epic's proprietary compressor; UE 5 editor packages store texture source art and
// mesh descriptions Oodle-compressed. Ptero does not ship Oodle. Instead it loads the
// oo2core DLL from the Unreal Engine installation already on the user's machine (every UE
// install carries one for its .NET tools), or from a path given in PTERO_OODLE_DLL.
namespace Ptero::Unreal::Oodle
{
    bool IsAvailable();
    // Human-readable reason the decompressor could not be loaded (empty when available).
    std::string UnavailableReason();
    // Path of the DLL in use (empty when unavailable).
    std::string LoadedFrom();

    bool Decompress(const std::uint8_t* source, std::size_t sourceSize, std::uint8_t* destination, std::size_t rawSize);
}
