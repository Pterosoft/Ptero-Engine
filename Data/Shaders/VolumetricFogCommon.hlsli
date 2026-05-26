cbuffer FogConstants : register(b0)
{
    uint     gFrameWidth;
    uint     gFrameHeight;
    uint     gFroxelWidth;
    uint     gFroxelHeight;

    uint     gDepthSlices;
    uint     gDebugView;
    float    gNearPlane;
    float    gFarPlane;

    float    gStartDistance;
    float    gMaxDistance;
    float    gDensity;
    float    gAnisotropy;

    float    gBaseHeight;
    float    gHeightFalloff;
    float2   _FogPad0;

    float3   gFogColor;
    float    _FogPad1;

    float3   gEmissiveColor;
    float    gEmissiveIntensity;

    float3   gCameraPos;
    float    _FogPad2;

    float3   gSunDir;
    float    _FogPad3;

    float3   gSunColor;
    float    _FogPad4;

    float3   gSkyColor;
    float    _FogPad5;

    uint     gNumPointLights;
    float3   _FogPad6;

    struct FogPointLight
    {
        float3 Position;
        float  Radius;
        float3 Color;
        float  InvRadiusSq;
    };

    FogPointLight gPointLights[4];

    float4x4 gViewProjInv;
    float4x4 gCurrViewProj;
};

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gViewProjInv);
    return worldPos.xyz / max(worldPos.w, 1e-6f);
}

float SliceToViewDepth(float sliceIndex)
{
    float z = saturate((sliceIndex + 0.5f) / max((float)gDepthSlices, 1.0f));
    float nearZ = max(gStartDistance, 0.001f);
    float farZ = max(gMaxDistance, nearZ + 0.001f);
    return nearZ * pow(farZ / nearZ, z);
}

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = max(1.0f + g2 - 2.0f * g * cosTheta, 1e-4f);
    return (1.0f - g2) / (12.56637061f * denom * sqrt(denom));
}

float ComputeHeightDensity(float worldZ)
{
    if (gHeightFalloff <= 0.0f)
        return 1.0f;
    return exp(-max(0.0f, worldZ - gBaseHeight) * gHeightFalloff);
}
