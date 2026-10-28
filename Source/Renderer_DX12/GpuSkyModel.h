#pragma once

#include "HosekWilkieSky.h"

#include <DirectXMath.h>

// GpuSkyModel
// The sky's radiance model for shaders - the Hosek-Wilkie daylight and moonlit skies
// and the night gradient under them - as the sky pass draws it, minus its discs and
// stars. Embedded at the end of each GI pass's constants, so rays that escape the
// scene bounce the sky that is on screen rather than a flat colour. Mirrors
// PteroSkyModel in Data/Shaders/SkyModel.hlsli, where PteroSkyRadiance evaluates it.
// The ground under the horizon (HosekWilkieResult::GroundRadiance) rides in the
// spare w of SunScale, MoonScale and NightZenith (r, g, b), which keeps every GI
// pass's constant layout as it was.
struct GpuSkyModel
{
    DirectX::XMFLOAT4 SunModel[9];
    DirectX::XMFLOAT4 SunScale;
    DirectX::XMFLOAT4 MoonModel[9];
    DirectX::XMFLOAT4 MoonScale;
    DirectX::XMFLOAT4 SunDirection;
    DirectX::XMFLOAT4 MoonDirection;
    DirectX::XMFLOAT4 NightZenith;
    DirectX::XMFLOAT4 NightHorizon;
};
static_assert(sizeof(GpuSkyModel) == 24 * 16, "GpuSkyModel must match PteroSkyModel in SkyModel.hlsli");

// The night gradient's horizon, from its zenith: a slightly paler blue. Shared with
// the sky pass so the drawn and the bounced night skies match.
inline DirectX::XMFLOAT3 NightSkyHorizon(const DirectX::XMFLOAT3& zenith)
{
    return DirectX::XMFLOAT3(zenith.x * 1.15f, zenith.y * 1.15f, zenith.z * 1.05f);
}

// `scale` multiplies every radiance - the GI pre-exposure for the GI passes.
inline GpuSkyModel MakeGpuSkyModel(const HosekWilkieResult& hosek, float scale)
{
    GpuSkyModel sky{};
    for (int k = 0; k < 9; ++k)
    {
        sky.SunModel[k] = DirectX::XMFLOAT4(hosek.SkyModel[k][0], hosek.SkyModel[k][1], hosek.SkyModel[k][2], 0.0f);
        sky.MoonModel[k] = DirectX::XMFLOAT4(hosek.MoonSkyModel[k][0], hosek.MoonSkyModel[k][1], hosek.MoonSkyModel[k][2], 0.0f);
    }
    sky.SunScale = DirectX::XMFLOAT4(hosek.DaySkyScale[0] * scale, hosek.DaySkyScale[1] * scale, hosek.DaySkyScale[2] * scale,
                                     hosek.GroundRadiance[0] * scale);
    sky.MoonScale = DirectX::XMFLOAT4(hosek.MoonSkyScale[0] * scale, hosek.MoonSkyScale[1] * scale, hosek.MoonSkyScale[2] * scale,
                                      hosek.GroundRadiance[1] * scale);
    // The Hosek vectors point from the body into the scene.
    sky.SunDirection = DirectX::XMFLOAT4(-hosek.SolarDirX, -hosek.SolarDirY, -hosek.SolarDirZ, 0.0f);
    sky.MoonDirection = DirectX::XMFLOAT4(-hosek.MoonDirX, -hosek.MoonDirY, -hosek.MoonDirZ, 0.0f);
    const DirectX::XMFLOAT3 zenith(hosek.NightSkyR * scale, hosek.NightSkyG * scale, hosek.NightSkyB * scale);
    const DirectX::XMFLOAT3 horizon = NightSkyHorizon(zenith);
    sky.NightZenith = DirectX::XMFLOAT4(zenith.x, zenith.y, zenith.z, hosek.GroundRadiance[2] * scale);
    sky.NightHorizon = DirectX::XMFLOAT4(horizon.x, horizon.y, horizon.z, 0.0f);
    return sky;
}
