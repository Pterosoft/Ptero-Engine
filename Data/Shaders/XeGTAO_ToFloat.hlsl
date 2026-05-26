// XeGTAO_ToFloat.hlsl  –  cs_6_0
// Final pass: convert packed R8_UINT AO term to R8_UNORM float texture for
// binding to the deferred lighting pass.

#include "XeGTAO.h"

Texture2D<uint>    t_PackedAO  : register(t0);
RWTexture2D<float> u_OutputAO  : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    uint width, height;
    t_PackedAO.GetDimensions(width, height);
    if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
        return;

    uint packed = t_PackedAO[dispatchThreadID];
    float visibility = float(packed & 0xff) / 255.0f;
    u_OutputAO[dispatchThreadID] = visibility;
}
