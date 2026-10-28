// LensFlare_Draw.hlsl
//
// Raster half of the lens flare (see LensFlare_Common.hlsli):
//   VSGhost / PSGhost           - every ghost's traced ray grid as a mesh, additively,
//                                 into a half-resolution target
//   VSFullscreen / PSComposite  - input image + the upsampled ghosts -> output
//   VSStarburst / PSStarburst   - the aperture diffraction starburst on each light

#include "LensFlare_Common.hlsli"

Texture2D<float4>                    gInputColor       : register(t0);
StructuredBuffer<LensFlareGridEntry> gGrid             : register(t2);
StructuredBuffer<float4>             gBounds           : register(t3);
StructuredBuffer<float>              gVisibility       : register(t4);
Texture2D<float4>                    gGhostTarget      : register(t5);
Texture2D<float4>                    gStarburstTexture : register(t6);
SamplerState                         gLinearClamp      : register(s0);

// Keeps an unlucky caustic inside what a 16-bit float target can hold.
static const float kMaxRadiance = 30000.0f;

static const uint2 kQuadCorners[6] =
{
    uint2(0, 0), uint2(1, 0), uint2(1, 1),
    uint2(1, 1), uint2(0, 1), uint2(0, 0),
};

// ---------------------------------------------------------------------------------
// Ghosts
// ---------------------------------------------------------------------------------

struct GhostVertex
{
    float4 Position  : SV_Position;
    float3 Radiance  : RADIANCE;
    float2 Aperture  : APERTURE;
    float  RelRadius : REL_RADIUS;
};

GhostVertex VSGhost(uint vertexId : SV_VertexID, uint instance : SV_InstanceID)
{
    const uint gridSize = gCounts.x;
    const uint quadsPerRow = gridSize - 1;
    const uint channelCount = gCounts.z;
    const uint ghostCount = gCounts.y;

    const uint quad = vertexId / 6;
    const uint2 cell = uint2(quad % quadsPerRow, quad / quadsPerRow);
    const uint2 corner = kQuadCorners[vertexId % 6];

    const uint channel = instance % channelCount;
    const uint lightGhost = instance / channelCount;
    const LensFlareLight light = gLights[lightGhost / ghostCount];

    const uint base = instance * gridSize * gridSize;
    const LensFlareGridEntry e00 = gGrid[base + cell.y * gridSize + cell.x];
    const LensFlareGridEntry e10 = gGrid[base + cell.y * gridSize + cell.x + 1];
    const LensFlareGridEntry e11 = gGrid[base + (cell.y + 1) * gridSize + cell.x + 1];
    const LensFlareGridEntry e01 = gGrid[base + (cell.y + 1) * gridSize + cell.x];
    const LensFlareGridEntry mine = gGrid[base + (cell.y + corner.y) * gridSize + cell.x + corner.x];

    const float visibility = gVisibility[(uint)light.Extra.x];
    const bool valid = e00.Intensity >= 0.0f && e10.Intensity >= 0.0f
                    && e11.Intensity >= 0.0f && e01.Intensity >= 0.0f
                    && visibility > 1.0e-4f;

    // Energy conservation: the light entering through this cell of the entrance plane
    // lands on the quad the four rays span. A share dA / A_pupil of what the lens gathers,
    // spread over the quad's solid angle, is the ghost's radiance there.
    const float4 bounds = gBounds[lightGhost];
    const float2 pupilCell = (bounds.zw - bounds.xy) / quadsPerRow;
    const float2 d1 = e11.Ndc - e00.Ndc;
    const float2 d2 = e01.Ndc - e10.Ndc;
    const float quadArea = 0.5f * abs(d1.x * d2.y - d1.y * d2.x);
    // Never denser than one ghost-target pixel: a caustic fold has zero area.
    const float minArea = 4.0f / (gTarget.x * gTarget.y);
    const float solidAngle = max(quadArea, minArea) * gTanHalf.x * gTanHalf.y;
    const float density = (pupilCell.x * pupilCell.y) / gLens.z / solidAngle;

    GhostVertex output;
    output.Radiance = min(
        mine.Intensity * density * light.Color.rgb * gChannel[channel].rgb * visibility * gStarburst.w,
        kMaxRadiance.xxx);
    output.Aperture = mine.Aperture;
    output.RelRadius = mine.RelRadius;
    // A lost quad collapses to a point, which rasterises nothing.
    output.Position = valid ? float4(mine.Ndc, 0.0f, 1.0f) : float4(-4.0f, -4.0f, 0.0f, 1.0f);
    return output;
}

float4 PSGhost(GhostVertex input) : SV_Target
{
    // The iris and the lens rims clip the ghost per pixel, with a one-pixel soft edge.
    const float iris = LensFlareApertureDistance(input.Aperture);
    const float irisMask = saturate((1.0f - iris) / max(fwidth(iris), 1.0e-4f) + 0.5f);
    const float rimMask = saturate((1.0f - input.RelRadius) / max(fwidth(input.RelRadius), 1.0e-4f) + 0.5f);
    return float4(input.Radiance * irisMask * rimMask, 0.0f);
}

// ---------------------------------------------------------------------------------
// Composite
// ---------------------------------------------------------------------------------

float4 VSFullscreen(uint vertexId : SV_VertexID) : SV_Position
{
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float4 PSComposite(float4 position : SV_Position) : SV_Target
{
    const float4 scene = gInputColor.Load(int3(position.xy, 0));
    const float3 ghosts = gGhostTarget.SampleLevel(gLinearClamp, position.xy / gTarget.zw, 0.0f).rgb;
    return float4(scene.rgb + ghosts, scene.a);
}

// ---------------------------------------------------------------------------------
// Starburst
// ---------------------------------------------------------------------------------

struct StarburstVertex
{
    float4 Position : SV_Position;
    float2 Uv       : TEXCOORD0;
    float3 Radiance : RADIANCE;
};

StarburstVertex VSStarburst(uint vertexId : SV_VertexID, uint instance : SV_InstanceID)
{
    const LensFlareLight light = gLights[instance];
    const float visibility = gVisibility[(uint)light.Extra.x];

    const float2 corner = float2(kQuadCorners[vertexId % 6]);
    const float2 offset = corner * 2.0f - 1.0f;
    const float2 centre = float2(light.Screen.x * 2.0f - 1.0f, 1.0f - light.Screen.y * 2.0f);

    StarburstVertex output;
    output.Position = float4(centre + offset * float2(gStarburst.x * gStarburst.z, gStarburst.x), 0.0f, 1.0f);
    output.Uv = corner;
    output.Radiance = min(light.Color.rgb * visibility * gStarburst.y, kMaxRadiance.xxx);
    if (visibility <= 1.0e-4f)
        output.Position = float4(-4.0f, -4.0f, 0.0f, 1.0f);
    return output;
}

float4 PSStarburst(StarburstVertex input) : SV_Target
{
    return float4(gStarburstTexture.SampleLevel(gLinearClamp, input.Uv, 0.0f).rgb * input.Radiance, 0.0f);
}
