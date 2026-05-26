#pragma once

#include <DirectXMath.h>
#include <cstddef>

struct Vertex
{
    DirectX::XMFLOAT3 Position{};
    DirectX::XMFLOAT3 Normal{};
    DirectX::XMFLOAT2 TexCoord{};
    DirectX::XMFLOAT4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

struct alignas(256) PassConstants
{
    DirectX::XMMATRIX ViewProj;

    // A single XMMATRIX is 64 bytes, so pad the struct to one full 256-byte CB slot.
    std::byte Padding[192]{};
};

static_assert(sizeof(DirectX::XMMATRIX) == 64, "PassConstants padding assumes a 64-byte XMMATRIX.");
static_assert(sizeof(PassConstants) == 256, "PassConstants must be exactly 256 bytes.");
