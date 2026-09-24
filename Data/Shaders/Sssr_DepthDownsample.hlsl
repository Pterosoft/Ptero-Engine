// Sssr_DepthDownsample.hlsl
// FidelityFX SSSR pass 1: builds the min-depth hierarchy the intersection pass traverses,
// with AMD's Single Pass Downsampler (ffx_spd.h). Mip 0 is a straight copy of the scene
// depth so the traversal can read every level through one SRV.
//
// Ported from the SSSR 1.3 sample's DepthDownsample.hlsl. SPD's subgroup path relies on
// Wave intrinsics, so this needs SM6.

Texture2D<float> gDepthBuffer : register(t0);

// 13 levels cover a 4096x4096 depth buffer. Levels the texture does not have are bound to
// null descriptors, which SPD's stores to them hit harmlessly.
// globallycoherent because the last group reads back level 6, which other groups wrote.
globallycoherent RWTexture2D<float> gDownsampledDepth[13] : register(u0);

// Remaining-threadgroup counter; SPD resets it to zero itself at the end of every dispatch.
globallycoherent RWStructuredBuffer<uint> gSpdGlobalAtomic : register(u13);

#define A_GPU
#define A_HLSL
#include "ffx_a.h"

groupshared float gSpdIntermediate[16][16];
groupshared uint  gSpdCounter;

AF4 SpdLoadSourceImage(ASU2 index, AU1 slice) { return gDepthBuffer[index].xxxx; }
// SPD reads its mip 5 back when finishing the tail; our mip index is one higher because
// level 0 is the copy of the source.
AF4 SpdLoad(ASU2 index, AU1 slice) { return gDownsampledDepth[6][index].xxxx; }
void SpdStore(ASU2 pix, AF4 outValue, AU1 index, AU1 slice) { gDownsampledDepth[index + 1][pix] = outValue.x; }
void SpdResetAtomicCounter(AU1 slice) { gSpdGlobalAtomic[0] = 0; }
void SpdIncreaseAtomicCounter(AU1 slice) { InterlockedAdd(gSpdGlobalAtomic[0], 1, gSpdCounter); }
AU1  SpdGetAtomicCounter() { return gSpdCounter; }
AF4  SpdLoadIntermediate(AU1 x, AU1 y) { return gSpdIntermediate[x][y].xxxx; }
void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value) { gSpdIntermediate[x][y] = value.x; }
// Closest surface wins: with non-inverted depth that is the minimum.
AF4  SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3) { return min(min(v0, v1), min(v2, v3)); }

#include "ffx_spd.h"

[numthreads(32, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint2 depthSize;
    gDepthBuffer.GetDimensions(depthSize.x, depthSize.y);

    // Each 32x8 group covers a 64x64 block: two columns and eight rows per thread.
    for (int i = 0; i < 2; ++i)
    {
        for (int j = 0; j < 8; ++j)
        {
            const uint2 idx = uint2(2 * dispatchThreadId.x + i, 8 * dispatchThreadId.y + j);
            if (idx.x < depthSize.x && idx.y < depthSize.y)
            {
                gDownsampledDepth[0][idx] = gDepthBuffer[idx];
            }
        }
    }

    // SPD writes store indices [0, mips), i.e. levels 1..mips here. Passing the number of
    // levels *below* level 0 keeps every store on a real level; SPD itself tops out at 12.
    const uint levelCount = 1u + (uint)floor(log2(float(max(depthSize.x, depthSize.y))));
    const uint generatedMips = min(levelCount - 1u, 12u);
    const uint threadgroupCount = ((depthSize.x + 63) / 64) * ((depthSize.y + 63) / 64);

    SpdDownsample(AU2(groupId.xy), AU1(groupIndex), AU1(generatedMips), AU1(threadgroupCount), 0);
}
