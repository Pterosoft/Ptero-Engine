// DeferredLighting.hlsl
// Fullscreen lighting-resolve pass for the deferred renderer.
// Reads the three G-Buffer textures (albedo, normal, material) plus the
// depth buffer, reconstructs world-space position, then evaluates the
// Hosek-Wilkie sun/sky directional light, PCF shadows, and all dynamic
// point lights to produce the final HDR colour.

// -------------------------------------------------------------------------
// Constant buffers
// -------------------------------------------------------------------------

// Per-frame camera and inverse-projection data used to reconstruct world
// position from the G-Buffer depth value.
cbuffer CameraConstants : register(b0)
{
    float4x4 gInvViewProj;  // inverse of viewProjection (row-major pre-transposed)
    float3   gCameraPos;    // world-space camera position
    float    _CamPad;
};

// Scene lighting – mirrors the layout in the former forward MeshEntity.hlsl.
#define MAX_POINT_LIGHTS 16

struct PointLightData
{
    float3 Position;    // world-space position
    float  Radius;
    float3 Color;       // pre-multiplied HDR colour (intensity baked in)
    float  InvRadiusSq; // 1 / (Radius^2)
    float  FalloffExponent;
    float  SourceRadius;
    float  CastShadows;
    float  ShadowIndex;
};

cbuffer SceneLighting : register(b1)
{
    float3 gSunDirection;   // world-space direction FROM sun TOWARD scene (normalised)
    float  _LightPad0;
    float3 gSunColor;
    float  _LightPad1;
    float3 gSkyAmbient;
    float  _LightPad2;
    int    gNumPointLights;
    float  gGiIntensity;    // 0 = GI disabled; >0 blends GI contribution
    float  gAoIntensity;    // 0 = RTAO disabled; >0 overrides G-Buffer AO
    int    gAoDebugView;    // 0 = normal lit output; >0 = standalone AO debug
    int    gVolumetricFogEnabled;
    float  gFogStartDistance;
    float  gFogMaxDistance;
    int    gFogDebugView;
    float  gSpecularIntensity; // 0 = specular reflections disabled; >0 blends ray-traced specular
    PointLightData gPointLights[MAX_POINT_LIGHTS];
    int    gRtgiDebugView;
    int    gPointShadowDebugView;
    int    gPointShadowFilterRadius;
    float  gPointShadowSeamBlendDistance;
    float  gPointShadowNormalOffset;
};

// Shadow constants – same layout as ShadowData in the old MeshEntity.hlsl.
cbuffer ShadowData : register(b2)
{
    float4x4 gLightViewProj;
    float    gShadowMapSize;
    float    gShadowBias;
    float    gPointShadowMapSize;
    float    gPointShadowBias;
    float4x4 gPointShadowFaceViewProj[24];
};

cbuffer ProbeConstants : register(b3)
{
    uint   gProbeGridX;
    uint   gProbeGridY;
    uint   gProbeGridZ;
    float  gProbeSpacing;
    float3 gProbeOrigin;
    float  _ProbePad0;
};

struct ProbeSH
{
    float4 c[7];
};

// -------------------------------------------------------------------------
// Textures / samplers
// -------------------------------------------------------------------------
Texture2D    gGBufferAlbedo   : register(t0); // RT0: albedo (RGB) + unused (A)
Texture2D    gGBufferNormal   : register(t1); // RT1: oct normal (RG) + copied depth (B)
Texture2D    gGBufferMaterial : register(t2); // RT2: roughness/metallic/AO
Texture2D    gDepthBuffer     : register(t3); // scene depth (R32_FLOAT or D32_FLOAT read via SRV)
Texture2D    gShadowMap       : register(t4); // sun shadow map
Texture2D    gGiAccumulation  : register(t5); // DXR GI accumulation buffer (RG11B10 HDR)
Texture2D    gRtaoTexture     : register(t6); // DXR ambient occlusion (R16F)
Texture3D    gVolumetricFog   : register(t7); // accumulated froxel fog (rgb=scattering, a=transmittance)
Texture2D    gSpecularReflect : register(t8); // DXR specular reflections (RGBA16F)
Texture2DArray gPointShadowMaps : register(t9);
StructuredBuffer<ProbeSH> gRadianceProbes : register(t10);

SamplerState             gPointSampler   : register(s0); // point-clamp for G-buffer reads
SamplerComparisonState   gShadowSampler  : register(s1); // PCF comparison sampler
SamplerState             gLinearSampler  : register(s2); // linear-clamp for volumetric fog filtering

// -------------------------------------------------------------------------
// Vertex / pixel structs
// -------------------------------------------------------------------------
struct VSInput
{
    uint VertexId : SV_VertexID;
};

struct PSInput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

// -------------------------------------------------------------------------
// Fullscreen triangle vertex shader
// Generates a single triangle that covers the entire viewport from three
// vertex IDs without needing a vertex buffer.
// -------------------------------------------------------------------------
PSInput VSMain(VSInput input)
{
    PSInput output;
    // Correct fullscreen triangle: NDC Y=+1 is top, UV Y=0 is top – must match.
    // Vertex 0 (id=0): NDC=(-1, 1), UV=(0,0) – top-left
    // Vertex 1 (id=1): NDC=(-1,-3), UV=(0,2) – off-screen bottom
    // Vertex 2 (id=2): NDC=( 3, 1), UV=(2,0) – off-screen right
    float2 ndc;
    ndc.x = (input.VertexId == 2) ?  3.0f : -1.0f;
    ndc.y = (input.VertexId == 1) ? -3.0f :  1.0f; // Y=+1 at top so UV and G-buffer align
    output.Position = float4(ndc, 0.0f, 1.0f);
    output.TexCoord.x = (input.VertexId == 2) ? 2.0f : 0.0f;
    output.TexCoord.y = (input.VertexId == 1) ? 2.0f : 0.0f;
    return output;
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

// Reconstruct world-space position from a depth buffer sample.
// gInvViewProj is stored as Transpose(inv(viewProj)) so HLSL mul(row, M) works correctly.
float3 ReconstructWorldPosition(float2 uv, float depth)
{
    // UV (0,0) = top-left matches NDC (+x right, +y up).
    // u in [0,1] -> NDC x in [-1,+1]; v in [0,1] -> NDC y in [+1,-1] (flip Y)
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

float3 DecodeOctNormal(float2 encoded)
{
    float2 oct = encoded * 2.0f - 1.0f;
    float3 n = float3(oct, 1.0f - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0f)
    {
        n.xy = (1.0f - abs(n.yx)) * (float2(n.xy >= 0.0f) * 2.0f - 1.0f);
    }
    return normalize(n);
}

float3 ComputeFogUv(float2 uv, float viewDistance)
{
    float nearZ = max(gFogStartDistance, 0.001f);
    float farZ = max(gFogMaxDistance, nearZ + 0.001f);
    float fogZ = saturate(log(max(viewDistance, nearZ) / nearZ) / log(farZ / nearZ));
    return float3(uv, fogZ);
}

// 3x3 PCF shadow factor; 1 = fully lit, 0 = fully shadowed.
float SampleShadowPCF(float3 worldPos)
{
    float4 lightClip  = mul(float4(worldPos, 1.0f), gLightViewProj);
    float3 projCoords = lightClip.xyz / lightClip.w;

    float2 uv;
    uv.x =  projCoords.x * 0.5f + 0.5f;
    uv.y = -projCoords.y * 0.5f + 0.5f;

    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f ||
        projCoords.z < 0.0f || projCoords.z > 1.0f)
        return 1.0f;

    const float depth     = projCoords.z - gShadowBias;
    const float texelSize = 1.0f / gShadowMapSize;

    float shadow = 0.0f;
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            shadow += gShadowMap.SampleCmpLevelZero(gShadowSampler, uv + offset, depth);
        }
    }
    return shadow / 9.0f;
}

float SamplePointShadow(int lightIndex, float3 worldPos, float3 surfaceNormal)
{
    if (lightIndex < 0 || lightIndex >= MAX_POINT_LIGHTS)
        return 1.0f;
    if (gPointLights[lightIndex].CastShadows < 0.5f)
        return 1.0f;

    int shadowIndex = (int)gPointLights[lightIndex].ShadowIndex;
    if (shadowIndex < 0)
        return 1.0f;

    float3 toPoint = worldPos - gPointLights[lightIndex].Position;
    float distanceToLight = length(toPoint);
    if (distanceToLight <= 0.0001f)
        return 1.0f;

    float3 sampleDir = toPoint / distanceToLight;

    const float mapSize = max(gPointShadowMapSize, 1.0f);
    const float texelSize = 1.0f / mapSize;
    const float normalOffsetDistance = max(gPointShadowNormalOffset, 0.0f) * texelSize * max(gPointLights[lightIndex].Radius, 1e-4f);
    float3 offsetWorldPos = worldPos + normalize(surfaceNormal) * normalOffsetDistance;

    toPoint = offsetWorldPos - gPointLights[lightIndex].Position;
    distanceToLight = length(toPoint);
    if (distanceToLight <= 0.0001f)
        return 1.0f;
    sampleDir = toPoint / distanceToLight;

    int faceIndex = -1;
    int secondFaceIndex = -1;
    float2 uv = 0.0f.xx;
    float2 secondUv = 0.0f.xx;
    float bestEdgeDistance = -1.0f;
    float secondEdgeDistance = -1.0f;
    const int faceBaseIndex = shadowIndex * 6;
    [unroll]
    for (int candidateFace = 0; candidateFace < 6; ++candidateFace)
    {
        float4 clipPos = mul(float4(offsetWorldPos, 1.0f), gPointShadowFaceViewProj[faceBaseIndex + candidateFace]);
        if (abs(clipPos.w) <= 1e-5f)
            continue;

        float3 ndc = clipPos.xyz / clipPos.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f)
            continue;

        float2 candidateUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        if (all(candidateUv >= 0.0f.xx) && all(candidateUv <= 1.0f.xx))
        {
            float2 edgeDistance2D = min(candidateUv, 1.0f.xx - candidateUv);
            float edgeDistance = min(edgeDistance2D.x, edgeDistance2D.y);
            if (edgeDistance > bestEdgeDistance)
            {
                secondFaceIndex = faceIndex;
                secondUv = uv;
                secondEdgeDistance = bestEdgeDistance;
                faceIndex = candidateFace;
                uv = candidateUv;
                bestEdgeDistance = edgeDistance;
            }
            else if (edgeDistance > secondEdgeDistance)
            {
                secondFaceIndex = candidateFace;
                secondUv = candidateUv;
                secondEdgeDistance = edgeDistance;
            }
        }
    }

    if (faceIndex < 0)
        return 1.0f;

    const float lightRadius = max(gPointLights[lightIndex].Radius, 1e-4f);
    float currentDepth = distanceToLight / lightRadius;
    const float3 lightDir = -sampleDir;
    const float normalAlignment = saturate(dot(normalize(surfaceNormal), normalize(lightDir)));
    const float slopeBias = (1.0f - normalAlignment) * texelSize * 2.0f;
    const float depthBias = (gPointShadowBias / lightRadius) + slopeBias;

    const int filterRadius = clamp(gPointShadowFilterRadius, 0, 4);

    float primaryVisibility = 0.0f;
    float primaryWeight = 0.0f;
    [loop]
    for (int y = -filterRadius; y <= filterRadius; ++y)
    {
        [loop]
        for (int x = -filterRadius; x <= filterRadius; ++x)
        {
            float2 sampleUv = saturate(uv + float2(x, y) * texelSize);
            int2 samplePixel = int2(sampleUv * (mapSize - 1.0f));
            float storedDepth = gPointShadowMaps.Load(int4(samplePixel, shadowIndex * 6 + faceIndex, 0)).r;
            primaryVisibility += ((currentDepth - depthBias) <= storedDepth) ? 1.0f : 0.0f;
            primaryWeight += 1.0f;
        }
    }
    primaryVisibility /= max(primaryWeight, 1.0f);

    if (secondFaceIndex < 0 || gPointShadowSeamBlendDistance <= 0.0f)
        return primaryVisibility;

    float edgeDistance = min(uv.x, min(uv.y, min(1.0f - uv.x, 1.0f - uv.y)));
    float seamBlend = saturate(1.0f - (edgeDistance / gPointShadowSeamBlendDistance));
    if (seamBlend <= 0.0f)
        return primaryVisibility;

    float secondaryVisibility = 0.0f;
    float secondaryWeight = 0.0f;
    [loop]
    for (int y = -filterRadius; y <= filterRadius; ++y)
    {
        [loop]
        for (int x = -filterRadius; x <= filterRadius; ++x)
        {
            float2 sampleUv = saturate(secondUv + float2(x, y) * texelSize);
            int2 samplePixel = int2(sampleUv * (mapSize - 1.0f));
            float storedDepth = gPointShadowMaps.Load(int4(samplePixel, shadowIndex * 6 + secondFaceIndex, 0)).r;
            secondaryVisibility += ((currentDepth - depthBias) <= storedDepth) ? 1.0f : 0.0f;
            secondaryWeight += 1.0f;
        }
    }
    secondaryVisibility /= max(secondaryWeight, 1.0f);

    return lerp(primaryVisibility, secondaryVisibility, seamBlend);
}

float3 EvaluateProbeSH(ProbeSH sh, float3 dir)
{
    float x = dir.x;
    float y = dir.y;
    float z = dir.z;

    float basis0 = 0.282095f * 3.14159265f;
    float basis1 = 0.488603f * y * 2.09439510f;
    float basis2 = 0.488603f * z * 2.09439510f;
    float basis3 = 0.488603f * x * 2.09439510f;
    float basis4 = 1.092548f * x * y * 0.78539816f;
    float basis5 = 1.092548f * y * z * 0.78539816f;
    float basis6 = 0.315392f * (3.0f * z * z - 1.0f) * 0.78539816f;
    float basis7 = 1.092548f * x * z * 0.78539816f;
    float basis8 = 0.546274f * (x * x - y * y) * 0.78539816f;

    float3 result = float3(0, 0, 0);
    result.r = sh.c[0].x * basis0 + sh.c[0].w * basis1 + sh.c[1].z * basis2 + sh.c[2].y * basis3
             + sh.c[3].x * basis4 + sh.c[3].w * basis5 + sh.c[4].z * basis6 + sh.c[5].y * basis7
             + sh.c[6].x * basis8;
    result.g = sh.c[0].y * basis0 + sh.c[1].x * basis1 + sh.c[1].w * basis2 + sh.c[2].z * basis3
             + sh.c[3].y * basis4 + sh.c[4].x * basis5 + sh.c[4].w * basis6 + sh.c[5].z * basis7
             + sh.c[6].y * basis8;
    result.b = sh.c[0].z * basis0 + sh.c[1].y * basis1 + sh.c[2].x * basis2 + sh.c[2].w * basis3
             + sh.c[3].z * basis4 + sh.c[4].y * basis5 + sh.c[5].x * basis6 + sh.c[5].w * basis7
             + sh.c[6].z * basis8;
    return max(result, 0.0f);
}

uint FlattenProbeCoord(uint3 coord)
{
    return coord.x + coord.y * gProbeGridX + coord.z * gProbeGridX * gProbeGridY;
}

float3 SampleRadianceProbeIrradiance(float3 worldPos, float3 normal)
{
    if (gProbeGridX == 0 || gProbeGridY == 0 || gProbeGridZ == 0 || gProbeSpacing <= 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    float3 probeCoordF = (worldPos - gProbeOrigin) / gProbeSpacing;
    float3 clampedCoordF = clamp(probeCoordF, 0.0f.xxx, float3(gProbeGridX - 1, gProbeGridY - 1, gProbeGridZ - 1));
    float3 baseCoordF = floor(clampedCoordF);
    float3 fracCoord = saturate(clampedCoordF - baseCoordF);

    uint3 baseCoord = uint3(baseCoordF);
    uint3 nextCoord = min(baseCoord + 1u, uint3(gProbeGridX - 1, gProbeGridY - 1, gProbeGridZ - 1));

    float3 c000 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(baseCoord.x, baseCoord.y, baseCoord.z))], normal);
    float3 c100 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(nextCoord.x, baseCoord.y, baseCoord.z))], normal);
    float3 c010 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(baseCoord.x, nextCoord.y, baseCoord.z))], normal);
    float3 c110 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(nextCoord.x, nextCoord.y, baseCoord.z))], normal);
    float3 c001 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(baseCoord.x, baseCoord.y, nextCoord.z))], normal);
    float3 c101 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(nextCoord.x, baseCoord.y, nextCoord.z))], normal);
    float3 c011 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(baseCoord.x, nextCoord.y, nextCoord.z))], normal);
    float3 c111 = EvaluateProbeSH(gRadianceProbes[FlattenProbeCoord(uint3(nextCoord.x, nextCoord.y, nextCoord.z))], normal);

    float3 cx00 = lerp(c000, c100, fracCoord.x);
    float3 cx10 = lerp(c010, c110, fracCoord.x);
    float3 cx01 = lerp(c001, c101, fracCoord.x);
    float3 cx11 = lerp(c011, c111, fracCoord.x);
    float3 cxy0 = lerp(cx00, cx10, fracCoord.y);
    float3 cxy1 = lerp(cx01, cx11, fracCoord.y);
    return lerp(cxy0, cxy1, fracCoord.z);
}

// -------------------------------------------------------------------------
// Cook-Torrance BRDF
// Evaluates diffuse + specular for a single light direction L_in.
// lightColor  – pre-weighted colour (light colour × NdotL × shadow / falloff)
//               NOTE: NdotL is NOT pre-applied here; we compute it inside.
// V           – unit view direction (surface → camera)
// N           – unit surface normal
// albedo      – base colour
// metallic, roughness – PBR material parameters
// -------------------------------------------------------------------------
float3 EvalBRDF(float3 L_in, float3 lightRadiance,
                float3 V, float3 N,
                float3 albedo, float metallic, float roughness)
{
    float NdotL = saturate(dot(N, L_in));
    if (NdotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

    float3 H     = normalize(V + L_in);
    float  NdotV = max(dot(N, V), 0.0001f);
    float  NdotH = saturate(dot(N, H));
    float  HdotV = saturate(dot(H, V));

    // GGX NDF
    float a  = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    float D = a2 / (3.14159265f * denom * denom);

    // Smith-GGX geometry (k remapped for direct lighting)
    float k   = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
    float G_V = NdotV / (NdotV * (1.0f - k) + k);
    float G_L = NdotL / (NdotL * (1.0f - k) + k);
    float G   = G_V * G_L;

    // Schlick Fresnel
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 F  = F0 + (1.0f - F0) * pow(1.0f - HdotV, 5.0f);

    // Specular term (Cook-Torrance)
    float3 specular = (D * G * F) / max(4.0f * NdotV * NdotL, 0.001f);

    // Lambertian diffuse; metals have no diffuse.
    // No 1/pi division: real-time convention keeps albedo in [0,1] as the diffuse colour
    // without the physics-correct pi normalisation that would darken surfaces ~3x.
    float3 kD = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);
    float3 diffuseTerm = kD * albedo;

    return (diffuseTerm + specular) * lightRadiance * NdotL;
}

// -------------------------------------------------------------------------
// Pixel shader
// -------------------------------------------------------------------------
float4 PSMain(PSInput input) : SV_Target
{
    float2 uv = input.TexCoord;
    uint2 pixel = uint2(input.Position.xy);

    // Read G-Buffer.
    float4 albedoSample   = gGBufferAlbedo.Load(int3(pixel, 0));
    float4 normalSample   = gGBufferNormal.Load(int3(pixel, 0));
    float4 materialSample = gGBufferMaterial.Load(int3(pixel, 0));
    float  rawDepth       = gDepthBuffer.Load(int3(pixel, 0)).r;

    // Sky / skybox pixels have depth == 1.0 after clear; skip lighting for those.
    [branch]
    if (rawDepth >= 1.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // Decode G-Buffer values.
    float3 albedo    = albedoSample.rgb;
    float3 N         = DecodeOctNormal(normalSample.xy);
    float  roughness = max(materialSample.r, 0.04f); // G-buffer R
    float  metallic  = materialSample.g;             // G-buffer G
    // Override the baked G-Buffer AO with the ray-traced AO when available.
    float  ao        = materialSample.b;             // G-buffer B
    float  rtao      = ao;
    if (gAoIntensity > 0.0f)
    {
        rtao = gRtaoTexture.Load(int3(pixel, 0)).r;
        ao = lerp(ao, rtao, gAoIntensity);
    }

    if (gAoDebugView > 0)
    {
        float debugAo = (gAoIntensity > 0.0f) ? rtao : ao;
        return float4(debugAo.xxx, 1.0f);
    }

    // Reconstruct world-space position and view direction.
    float3 worldPos = ReconstructWorldPosition(uv, rawDepth);
    float3 V        = normalize(gCameraPos - worldPos);

    // ---- Sun directional light with PCF shadow ----
    float  shadowFactor = SampleShadowPCF(worldPos);
    float3 L_sun        = normalize(-gSunDirection);
    float3 sunContrib   = EvalBRDF(L_sun, gSunColor * shadowFactor, V, N, albedo, metallic, roughness);

    // ---- Sky ambient (hemisphere diffuse + rough-specular approximation) ----
    // Only upward-facing surfaces see the sky hemisphere.
    // We use the PCF shadow factor as a sky-visibility proxy: if the sun cannot
    // reach a surface, neither can open sky (good approximation for enclosed scenes).
    // A small minimum (0.05) provides a subtle fill so fully-shadowed areas are not
    // pitch-black even without GI enabled.
    float  skyWeight   = max(N.z, 0.0f);
    float  skyVis      = saturate(shadowFactor * 0.95f + 0.05f); // 5% in full shadow, 100% in sun
    float3 F0          = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 ambientDiff = gSkyAmbient * skyWeight * skyVis * (1.0f - metallic) * albedo;
    float3 F_amb       = F0 + (max(float3(1,1,1) * (1.0f - roughness), F0) - F0)
                             * pow(1.0f - saturate(dot(N, V)), 5.0f);
    float3 ambientSpec = gSkyAmbient * skyWeight * skyVis * F_amb * (1.0f / (roughness * roughness + 1.0f));
    float3 ambient     = (ambientDiff + ambientSpec) * ao;

    // ---- Point lights ----
    float3 pointSum = float3(0.0f, 0.0f, 0.0f);
    float pointShadowDebug = 1.0f;
    [unroll]
    for (int i = 0; i < MAX_POINT_LIGHTS; ++i)
    {
        float active = (i < gNumPointLights) ? 1.0f : 0.0f;

        float3 toLight    = gPointLights[i].Position - worldPos;
        float  distSq     = dot(toLight, toLight);
        float3 L_pt       = normalize(toLight);

        float  dist = sqrt(max(distSq, 1e-6f));
        float  normalizedDistance = saturate(dist / max(gPointLights[i].Radius, 1e-4f));
        float  rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
        rangeMask *= rangeMask;
        float  falloffExponent = max(gPointLights[i].FalloffExponent, 0.001f);
        float  distanceFalloff = pow(max(dist, 1e-3f), -falloffExponent);
        float  falloff = rangeMask * distanceFalloff;

        float3 lightPos = gPointLights[i].Position;
        if (gPointLights[i].SourceRadius > 0.0001f)
        {
            float3 toCenter = worldPos - gPointLights[i].Position;
            float  toCenterLen = length(toCenter);
            if (toCenterLen > 0.0001f)
            {
                lightPos += (toCenter / toCenterLen) * min(gPointLights[i].SourceRadius, gPointLights[i].Radius * 0.5f);
                toLight = lightPos - worldPos;
                distSq = dot(toLight, toLight);
                dist = sqrt(max(distSq, 1e-6f));
                normalizedDistance = saturate(dist / max(gPointLights[i].Radius, 1e-4f));
                rangeMask = saturate(1.0f - normalizedDistance * normalizedDistance);
                rangeMask *= rangeMask;
                distanceFalloff = pow(max(dist, 1e-3f), -falloffExponent);
                falloff = rangeMask * distanceFalloff;
                L_pt = normalize(toLight);
            }
        }

        float pointShadow = SamplePointShadow(i, worldPos, N);
        pointShadowDebug = min(pointShadowDebug, pointShadow);
        pointSum += EvalBRDF(L_pt, gPointLights[i].Color * falloff * active * pointShadow, V, N, albedo, metallic, roughness);
    }

    if (gPointShadowDebugView > 0)
    {
        return float4(pointShadowDebug.xxx, 1.0f);
    }

    // ---- Final composite ----
    float3 lit = sunContrib + ambient + pointSum;

    // Add DXR global illumination.
    // The RTGI / NRD path now stores demodulated diffuse irradiance so the denoiser
    // does not blur texture detail; remodulate by the visible surface albedo here.
    if (gGiIntensity > 0.0f)
    {
        float3 giDiffuseIrradiance = float3(0.0f, 0.0f, 0.0f);
        if (gProbeGridX > 0 && gProbeGridY > 0 && gProbeGridZ > 0)
        {
            giDiffuseIrradiance = SampleRadianceProbeIrradiance(worldPos, N);
            float probeLuma = dot(giDiffuseIrradiance, float3(0.2126f, 0.7152f, 0.0722f));
            giDiffuseIrradiance = lerp(probeLuma.xxx, giDiffuseIrradiance, 1.35f) * 2.0f;
        }
        else
            giDiffuseIrradiance = gGiAccumulation.Load(int3(pixel, 0)).rgb;

        // Guard against NaN/Inf from the GI texture corrupting the final colour.
        if (!any(isnan(giDiffuseIrradiance)) && !any(isinf(giDiffuseIrradiance)))
            lit += giDiffuseIrradiance * albedo * (1.0f - metallic) * gGiIntensity * ao;
    }

    // Add DXR specular reflections.
    // The specular buffer stores pre-weighted GGX reflected radiance.
    if (gSpecularIntensity > 0.0f)
    {
        float3 specRefl = gSpecularReflect.Load(int3(pixel, 0)).rgb;
        if (!any(isnan(specRefl)) && !any(isinf(specRefl)))
        {
            if (gRtgiDebugView == 3)
                return float4(specRefl, 1.0f);
            lit += specRefl * gSpecularIntensity;
        }
    }

    if (gVolumetricFogEnabled != 0)
    {
        float viewDistance = distance(gCameraPos, worldPos);
        float fogViewDistance = max(viewDistance - 0.05f, gFogStartDistance);
        float3 fogUv = ComputeFogUv(uv, fogViewDistance);
        float4 fogSample = gVolumetricFog.SampleLevel(gLinearSampler, fogUv, 0.0f);

        if (any(isnan(fogSample)) || any(isinf(fogSample)))
        {
            fogSample = float4(0.0f, 0.0f, 0.0f, 1.0f);
        }

        fogSample.rgb = max(fogSample.rgb, 0.0f.xxx);
        fogSample.a = saturate(fogSample.a);
        if (fogSample.a < 0.001f && dot(fogSample.rgb, fogSample.rgb) < 1e-8f)
        {
            fogSample.a = 1.0f;
        }

        if (gFogDebugView == 1)
        {
            return float4(fogSample.rgb, 1.0f);
        }
        if (gFogDebugView == 2)
        {
            return float4(fogSample.aaa, 1.0f);
        }

        lit = (lit * fogSample.a) + fogSample.rgb;
    }

    return float4(lit, 1.0f);
}
