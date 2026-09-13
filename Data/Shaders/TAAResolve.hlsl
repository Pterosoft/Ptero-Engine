// TAAResolve.hlsl  – Temporal Anti-Aliasing resolve compute shader.
//
// Blends the current frame colour with the reprojected history sample to
// produce a temporally-stable output.  Neighbourhood AABB clamping is used
// to keep the history valid and avoid ghosting on fast-moving objects.
//
// Root signature layout (see TAARenderer.cpp):
//   b0 – TaaConstantBuffer
//   t0 – current frame (SceneColorFormat, SRV)
//   t1 – history buffer from the previous frame (SRV)
//   u0 – resolved output UAV

// ----- constant buffer -------------------------------------------------
cbuffer TaaConstantBuffer : register(b0)
{
    float BlendFactor;       // weight of current frame colour (0.05 – 0.3 is typical)
    uint  FrameWidth;
    uint  FrameHeight;
    float _Pad0;
}

// ----- resources -------------------------------------------------------
Texture2D<float4>   gCurrentFrame : register(t0);
Texture2D<float4>   gHistory      : register(t1);
RWTexture2D<float4> gOutput       : register(u0);

// ----- helpers ---------------------------------------------------------
float3 RGBToYCoCg(float3 rgb)
{
    // Luma/chrominance transform – AABB clamping in YCoCg space is more robust
    // than clamping directly in RGB.
    float Y  =  0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b;
    float Co =  0.5f  * rgb.r                 - 0.5f  * rgb.b;
    float Cg = -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b;
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycc)
{
    float Y  = ycc.x;
    float Co = ycc.y;
    float Cg = ycc.z;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float3 SanitizeColor(float3 color)
{
    if (any(isnan(color)) || any(isinf(color)))
        return float3(0.0f, 0.0f, 0.0f);
    return max(color, 0.0f.xxx);
}

// Sample the 3x3 neighbourhood and return the min/max AABB in YCoCg space.
void SampleNeighbourhoodAABB(uint2 center, out float3 aabbMin, out float3 aabbMax)
{
    aabbMin = float3( 1e9,  1e9,  1e9);
    aabbMax = float3(-1e9, -1e9, -1e9);

    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 sampleCoord = (int2)center + int2(dx, dy);
            sampleCoord = clamp(sampleCoord, int2(0, 0), int2((int)FrameWidth - 1, (int)FrameHeight - 1));
            float3 s = RGBToYCoCg(gCurrentFrame[sampleCoord].rgb);
            aabbMin = min(aabbMin, s);
            aabbMax = max(aabbMax, s);
        }
    }
}

// ----- main compute entry ---------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    uint2 coord = dispatchId.xy;
    if (coord.x >= FrameWidth || coord.y >= FrameHeight)
        return;

    // Current frame sample in YCoCg.
    float3 current = RGBToYCoCg(gCurrentFrame[coord].rgb);

    // History sample (same screen position – no reprojection for a stationary scene;
    // sufficient for a forward-rendered editor viewport).
    float3 history = RGBToYCoCg(gHistory[coord].rgb);

    // Clamp the history sample into the neighbourhood AABB to remove ghosting.
    float3 aabbMin, aabbMax;
    SampleNeighbourhoodAABB(coord, aabbMin, aabbMax);
    history = clamp(history, aabbMin, aabbMax);

    // Exponential moving average: low BlendFactor = more history weight.
    float3 resolved = lerp(history, current, BlendFactor);

    // Convert back to RGB.
    float3 resolvedRGB = YCoCgToRGB(resolved);
    resolvedRGB = SanitizeColor(resolvedRGB);

    gOutput[coord] = float4(resolvedRGB, 1.0f);
}
