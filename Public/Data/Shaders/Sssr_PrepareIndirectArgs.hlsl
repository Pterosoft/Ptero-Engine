// Sssr_PrepareIndirectArgs.hlsl
// FidelityFX SSSR: turns the counters ClassifyTiles accumulated into ExecuteIndirect
// dispatch arguments, and zeroes the append counters for next frame. Ported from the
// SSSR 1.3 sample's PrepareIndirectArgs.hlsl. The argument layout (see Sssr_Common.hlsli)
// additionally splits large dispatches into rows and records the counts, because the
// sample's 1D dispatch overflows the 65535-group limit at 4K.
//
// Part of the SSSR pass set, which is compiled for SM6 because its other passes use Wave
// intrinsics.

#include "Sssr_Common.hlsli"

RWStructuredBuffer<uint> gRayCounter   : register(u0);   // [0] ray append, [1] tile append
RWStructuredBuffer<uint> gIndirectArgs : register(u1);

void WriteDispatch(uint offset, uint groupCount)
{
    const uint rows = (groupCount + kPteroSssrMaxGroupsX - 1u) / kPteroSssrMaxGroupsX;
    gIndirectArgs[offset + 0] = rows > 1u ? kPteroSssrMaxGroupsX : groupCount;
    gIndirectArgs[offset + 1] = max(rows, 1u);
    gIndirectArgs[offset + 2] = 1u;
}

[numthreads(1, 1, 1)]
void CSMain()
{
    const uint rayCount  = gRayCounter[0];
    const uint tileCount = gRayCounter[1];

    WriteDispatch(0u, (rayCount + 63u) / 64u);   // 64 rays per intersection group
    WriteDispatch(3u, tileCount);                // one denoiser group per 8x8 tile
    gIndirectArgs[kPteroSssrArgsRayCount]  = rayCount;
    gIndirectArgs[kPteroSssrArgsTileCount] = tileCount;

    gRayCounter[0] = 0u;
    gRayCounter[1] = 0u;
}
