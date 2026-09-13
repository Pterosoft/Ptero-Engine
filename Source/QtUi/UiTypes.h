#pragma once
#include <cstdint>

// A renderer descriptor is an engine resource, independent of the widget toolkit.
using UiTextureID = std::uint64_t;
constexpr UiTextureID UiTextureID_Invalid = 0;
using UiU32 = std::uint32_t;
struct UiVec2
{
    float x = 0, y = 0;
    constexpr UiVec2() = default;
    constexpr UiVec2(float x_, float y_) : x(x_), y(y_)
    {
    }
};
struct UiVec4
{
    float x = 0, y = 0, z = 0, w = 0;
    constexpr UiVec4() = default;
    constexpr UiVec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_)
    {
    }
};
#define UI_COL32(R, G, B, A) ((UiU32)(R) | ((UiU32)(G) << 8) | ((UiU32)(B) << 16) | ((UiU32)(A) << 24))
