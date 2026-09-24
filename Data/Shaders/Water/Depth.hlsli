//
// Description : Water color based on water depth and color extinction
//
// based on Rendering Water as a Post-process Effect by Wojciech Toman
//
// Ported from tuxalin/water-shader (shaders/hlsl/water/depth.cginc).
//

#ifndef WATER_DEPTH_HLSLI
#define WATER_DEPTH_HLSLI

// waterTransparency - x = water clarity, y = water visibility along eye vector
// waterDepthValues - x = water depth in world space, y = view/accumulated water depth in world space
float3 DepthRefraction(float2 waterTransparency, float2 waterDepthValues, float shoreRange, float3 horizontalExtinction,
    float3 refractionColor, float3 shoreColor, float3 surfaceColor, float3 depthColor)
{
    float waterClarity = waterTransparency.x;
    float visibility = waterTransparency.y;
    float waterDepth = waterDepthValues.x;
    float viewWaterDepth = waterDepthValues.y;

    float accDepth = viewWaterDepth * waterClarity; // accumulated water depth
    float accDepthExp = saturate(accDepth / (2.5 * visibility));
    accDepthExp *= (1.0 - accDepthExp) * accDepthExp * accDepthExp + 1; // out cubic

    surfaceColor = lerp(shoreColor, surfaceColor, saturate(waterDepth / shoreRange));
    float3 waterColor = lerp(surfaceColor, depthColor, saturate(waterDepth / horizontalExtinction));

    refractionColor = lerp(refractionColor, surfaceColor * waterColor, saturate(accDepth / visibility));
    refractionColor = lerp(refractionColor, depthColor, accDepthExp);
    refractionColor = lerp(refractionColor, depthColor * waterColor, saturate(waterDepth / horizontalExtinction));
    return refractionColor;
}

#endif // WATER_DEPTH_HLSLI
