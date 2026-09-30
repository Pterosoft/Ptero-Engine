#pragma once

// What the passes that sample the virtual shadow map need to know about it
// (VirtualShadowMapRenderer). Must match PteroVsmConstants in
// Data/Shaders/VirtualShadowMap.hlsli.

#include <DirectXMath.h>
#include <cstdint>

struct VsmGpuConstants
{
    // --- Sun clipmap ---
    DirectX::XMFLOAT3 Anchor{};  float FirstTexelSize = 1.0f;
    DirectX::XMFLOAT3 AxisX{};   float PixelFootprint = 0.001f;
    DirectX::XMFLOAT3 AxisY{};   float LodBias = 0.0f;
    DirectX::XMFLOAT3 AxisZ{};   float DepthNear = 0.0f;
    float         DepthRangeInv = 1.0f;
    std::uint32_t LevelCount = 0;
    std::uint32_t PoolPagesX = 1;
    std::uint32_t Enabled = 0;          // the sun is shadowed by the map
    float         NormalOffset = 0.0f;
    float         ConstantBias = 0.0f;
    std::int32_t  DebugView = 0;
    std::uint32_t Active = 0;           // the map is in use at all: the pool is bound
    DirectX::XMINT4 WindowOrigin[16]{};

    // --- Local lights: one virtual cube per slot ---
    std::uint32_t LocalEnabled = 0;
    float         LocalLodBias = 0.0f;
    std::uint32_t LocalPageTableBase = 0;
    float         LocalNear = 0.05f;
    DirectX::XMFLOAT4 LocalLights[16]{};      // xyz position, w radius (0 = slot unused)
    DirectX::XMFLOAT4 LocalDirections[16]{};  // xyz emission axis, w cosine below which it emits nothing
};
static_assert(sizeof(VsmGpuConstants) == 880, "Must match PteroVsmConstants");
