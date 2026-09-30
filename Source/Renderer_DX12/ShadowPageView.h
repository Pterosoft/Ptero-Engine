#pragma once

// One page of a paged shadow map to render: the page's own projection and the tile of
// the depth target it lands in. VirtualShadowMapRenderer hands a list of these to each
// caster renderer, which draws whatever overlaps each page into its tile.

#include <DirectXMath.h>
#include <d3d12.h>

#include <cstdint>

struct ShadowPageView
{
    DirectX::XMFLOAT4X4 ViewProjection{};            // row-major, not transposed (culling)
    DirectX::XMFLOAT4X4 ViewProjectionTransposed{};  // as the depth shaders take it
    D3D12_VIEWPORT      Viewport{};
    D3D12_RECT          Scissor{};
    // How geometry detail is picked for the page, so a cached page does not depend on
    // where the camera was when it was drawn:
    //   Orthographic - one texel is TexelWorldSize everywhere (the sun).
    //   Perspective  - one texel subtends LodTexelAngle radians from LodOrigin (a local
    //                  light's cube face).
    enum class LodMode : std::uint32_t { Orthographic, Perspective };
    LodMode             Lod = LodMode::Orthographic;
    float               TexelWorldSize = 0.0f;
    DirectX::XMFLOAT3   LodOrigin{};
    float               LodTexelAngle = 0.0f;
};
