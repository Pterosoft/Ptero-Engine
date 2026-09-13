// Ocean_Assemble.hlsl
// -------------------
// Turns the two transformed complex fields into the textures the water shader
// samples: a displacement map (XY choppy + Z height) and a normal/folding map.
//
// The spectrum was built with k = 0 at the texture centre, so the inverse
// transform comes out with its origin shifted.  Multiplying by (-1)^(x+y)
// undoes that shift (the standard fftshift correction).
//
// The Jacobian of the horizontal displacement gives the folding measure used
// for whitecaps: values below 1 mean the surface is compressing, and below 0
// it has folded over — which is exactly where real waves break.

cbuffer AssembleConstants : register(b0)
{
    uint  gSize;
    float gPatchSize;
    float gChoppiness;
    float gFoamBias;
};

Texture2D<float4>   gDisplacementFft : register(t0);  // xy = Dx, zw = Dy
Texture2D<float4>   gHeightSlopeFft  : register(t1);  // xy = height, zw = slopes
RWTexture2D<float4> gDisplacementOut : register(u0);  // xyz = displacement, w = folding
RWTexture2D<float4> gNormalOut       : register(u1);  // xyz = normal, w = folding

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gSize || id.y >= gSize)
        return;

    // Undo the centred-spectrum shift.
    const float sign = (((id.x + id.y) & 1u) == 0u) ? 1.0f : -1.0f;

    const float4 dispFft   = gDisplacementFft[id.xy];
    const float4 heightFft = gHeightSlopeFft[id.xy];

    // FFT(a + i*b) packs signal A in .x and signal B in .y of the result.
    const float dx     = dispFft.x   * sign * gChoppiness;
    const float dy     = dispFft.z   * sign * gChoppiness;
    const float height = heightFft.x * sign;

    const float slopeX = heightFft.z * sign;
    const float slopeY = heightFft.w * sign;

    // Analytic normal from the surface slopes (Z-up).
    const float3 normal = normalize(float3(-slopeX, -slopeY, 1.0f));

    // --- Jacobian of the horizontal displacement, by central differences ---
    const uint2 sizeMask = uint2(gSize - 1u, gSize - 1u);
    const uint2 left  = uint2((id.x - 1u) & sizeMask.x, id.y);
    const uint2 right = uint2((id.x + 1u) & sizeMask.x, id.y);
    const uint2 down  = uint2(id.x, (id.y - 1u) & sizeMask.y);
    const uint2 up    = uint2(id.x, (id.y + 1u) & sizeMask.y);

    const float signLR = (((left.x + left.y) & 1u) == 0u) ? 1.0f : -1.0f;
    const float signRR = (((right.x + right.y) & 1u) == 0u) ? 1.0f : -1.0f;
    const float signDD = (((down.x + down.y) & 1u) == 0u) ? 1.0f : -1.0f;
    const float signUU = (((up.x + up.y) & 1u) == 0u) ? 1.0f : -1.0f;

    const float dxLeft  = gDisplacementFft[left].x  * signLR * gChoppiness;
    const float dxRight = gDisplacementFft[right].x * signRR * gChoppiness;
    const float dyDown  = gDisplacementFft[down].z  * signDD * gChoppiness;
    const float dyUp    = gDisplacementFft[up].z    * signUU * gChoppiness;

    // Cross terms for the full 2x2 determinant.
    const float dxDown  = gDisplacementFft[down].x  * signDD * gChoppiness;
    const float dxUp    = gDisplacementFft[up].x    * signUU * gChoppiness;
    const float dyLeft  = gDisplacementFft[left].z  * signLR * gChoppiness;
    const float dyRight = gDisplacementFft[right].z * signRR * gChoppiness;

    const float texelWorld = gPatchSize / float(gSize);
    const float invTwoDx = 1.0f / (2.0f * max(texelWorld, 1e-5f));

    const float dXdx = (dxRight - dxLeft) * invTwoDx;
    const float dXdy = (dxUp    - dxDown) * invTwoDx;
    const float dYdx = (dyRight - dyLeft) * invTwoDx;
    const float dYdy = (dyUp    - dyDown) * invTwoDx;

    const float jacobian = (1.0f + dXdx) * (1.0f + dYdy) - dXdy * dYdx;
    // Folding measure: 0 on undisturbed water, rising as the surface compresses.
    const float folding = saturate(gFoamBias - jacobian);

    gDisplacementOut[id.xy] = float4(dx, dy, height, folding);
    gNormalOut[id.xy]       = float4(normal, folding);
}
