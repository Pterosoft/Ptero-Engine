// Water.hlsl
// ----------
// Forward refractive ocean/water surface (WaterComponent).  Runs AFTER the
// deferred lighting resolve so the lit opaque scene is available for
// screen-space refraction and reflection.
//
// Surface model
//   * Sum of Gerstner waves with a Phillips-like falloff, wide directional
//     spread, jittered wavelengths and randomised phases so the swell never
//     visibly repeats.
//   * Per-wave steepness is normalised against a fixed budget, so the crests
//     can never fold back on themselves (folding is what produces sawtooth
//     spikes and inverted normals).
//   * Normals come from the analytic surface tangents (cross product), not an
//     additive approximation, so they stay correct at any steepness.
//   * Wave amplitude and ripple detail fade with view distance, which keeps the
//     far field from aliasing into shimmer.
//
// Shading
//   * Screen-space refraction of the scene behind the surface, absorbed toward
//     the deep colour by water-column thickness (Beer-Lambert style).
//   * Subsurface scattering: backlit crests glow turquoise.  This is the single
//     strongest realism cue for water and is what separates it from plastic.
//   * Fresnel (Schlick) blend into a screen-space reflection, falling back to a
//     sky gradient when the ray leaves the screen.
//   * GGX sun glint, and sparse whitecap foam driven by the Jacobian of the
//     horizontal displacement (real whitecaps form where the surface compresses).
//
// World is Z-up: the grid lies on the XY plane and waves displace along Z.
//
// Root signature:
//   b0 VS+PS - camera / transform / sun / screen     t0 PS - lit scene copy
//   b1 VS+PS - water material                        t1 PS - opaque depth
//   s0 linear-clamp, s1 point-clamp

cbuffer WaterFrame : register(b0)
{
    float4x4 gViewProj;
    float4x4 gModel;
    float4x4 gInvViewProj;

    float3   gCameraPos;    float gTime;
    float3   gSunDirection; float gScreenWidth;   // points FROM sun TOWARD scene
    float3   gSunColor;     float gScreenHeight;
    float3   gSkyColor;     int   gDebugMode;
};

// Debug visualisation modes (WaterComponent::DebugMode).
#define WATER_DEBUG_OFF          0
#define WATER_DEBUG_FRESNEL      1
#define WATER_DEBUG_NDOTV        2
#define WATER_DEBUG_NORMALS      3
#define WATER_DEBUG_THICKNESS    4
#define WATER_DEBUG_REFLECTION   5
#define WATER_DEBUG_TRANSMISSION 6
#define WATER_DEBUG_FOAM         7
// Neutral test: drives the Fresnel blend with 0.18 on both sides, so the
// surface MUST render as flat mid-grey.  If it comes out cyan, white or
// gradient-shaded, the fault is outside the water model (exposure applied
// twice, water composited after tonemapping, double sRGB, and so on).
#define WATER_DEBUG_NEUTRAL      8
#define WATER_DEBUG_SUNSPEC      9

cbuffer WaterMaterial : register(b1)
{
    float4 gShallowTint;    // rgb tint applied to the refracted scene
    float4 gExtinction;     // rgb per-metre extinction (Beer-Lambert), a unused
    float4 gFoamColor;      // rgb tint, a = whitecap amount
    // Deep-water scattering *coefficient*, not an albedo.  These must stay very
    // small (~0.005-0.03): they are multiplied by the sky/sun irradiance to give
    // the radiance scattered back out of the volume, and deep water is far
    // darker than the sky above it.  Albedo-sized values here (0.4+) push the
    // whole surface past mid-grey into the tonemapper's shoulder as bright cyan.
    float4 gScatterColor;   // rgb scattering coefficient, a = intensity
    float4 gSssColor;       // rgb subsurface tint, a = intensity (crest glow)

    float  gRoughness;
    float  gMetallic;
    float  gWaveScale;
    float  gBaseAmplitude;

    float  gBaseWavelength;
    float  gBaseSpeed;
    float  gSteepness;
    float  gChoppiness;

    int    gWaveCount;
    float  gDetailTiling;
    float  gDetailStrength;
    float  gFoamThreshold;   // Jacobian below this starts foaming

    float2 gWindDir;
    float  gFresnelPower;
    float  gRefractionStrength;

    float  gAbsorptionDistance;
    float  gShorelineFoam;
    float  gSunSpecPower;
    float  gF0;

    float  gDirectionalSpread;   // radians of fan around the wind direction
    float  gSssPower;
    float  gSssDistortion;
    float  gDetailFadeDistance;

    float  gSsrStride;
    int    gSsrSteps;
    float  gSsrThickness;
    float  gReflectionStrength;

    // Slope-variance filtering.  Ripples that drop below a pixel with distance
    // do not actually vanish; if they are simply faded out the surface becomes a
    // perfect mirror and reflects the sky as a solid white sheet (and shimmers
    // violently in motion).  The lost variance is converted into roughness and a
    // relaxation of the normal toward flat instead.
    float  gDistantRoughness;
    float  gDistantFlatten;
    float  gVolumeDesaturate;
    // World-space distance between mesh vertices.  Waves shorter than a couple
    // of cells cannot be represented by the grid and alias into hard faceting,
    // so the spectrum is band-limited against this (see GridLimit).
    float  gGridSpacing;

    // Cascaded FFT ocean.  When gFftEnabled is non-zero the Gerstner sum is
    // replaced by three tiling FFT patches whose world-space sizes are given
    // here (large swell -> wind waves -> short chop).
    float3 gOceanPatchSizes;
    float  gFftEnabled;

    float  gFftNormalStrength;
    float  gFftDisplacementScale;
    float2 _matPad2;
};

// FFT cascade outputs: xyz = displacement (XY choppy, Z height), w = folding.
Texture2D gOceanDisplacement0 : register(t2);
Texture2D gOceanDisplacement1 : register(t3);
Texture2D gOceanDisplacement2 : register(t4);
Texture2D gOceanNormal0       : register(t5);
Texture2D gOceanNormal1       : register(t6);
Texture2D gOceanNormal2       : register(t7);
SamplerState gLinearWrap      : register(s2);

Texture2D    gRefractionColor : register(t0);
Texture2D    gDepthBuffer     : register(t1);
SamplerState gLinearClamp     : register(s0);
SamplerState gPointClamp      : register(s1);

static const float kGravity = 9.81f;
static const float kPi      = 3.14159265f;
static const float kTwoPi   = 6.28318531f;

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct PSInput
{
    float4 Position      : SV_Position;
    float3 WorldPosition : WORLDPOS;
    float3 WorldNormal   : NORMAL;
    float2 TexCoord      : TEXCOORD;
    float2 FoamHeight    : FOAM;   // x = whitecap factor, y = normalised height
};

float Hash1(float n)
{
    return frac(sin(n * 12.9898f) * 43758.5453f);
}

// Wavelengths shorter than the on-screen sampling rate alias badly, so fade
// each wave out as it becomes small relative to the view distance.
float WaveLod(float wavelength, float viewDistance)
{
    return smoothstep(0.0f, 1.0f, saturate(wavelength * 120.0f / max(viewDistance, 1.0f)));
}

// Band-limit the spectrum to the mesh.  A wave needs several vertices per
// period to be representable; below ~2 cells it aliases against the grid and
// shows up as hard polygonal faceting rather than a wave.  Detail at those
// scales belongs in the shading normal, not in geometry.
float GridLimit(float wavelength)
{
    const float cells = wavelength / max(gGridSpacing, 1e-4f);
    return smoothstep(2.0f, 5.0f, cells);
}

// ---------------------------------------------------------------------------
// Gerstner ocean evaluation.
// Returns the displacement, the exact analytic normal, a whitecap factor from
// the horizontal Jacobian, and a normalised height for subsurface scattering.
// ---------------------------------------------------------------------------
void EvaluateOcean(
    float2 restXY,
    float  viewDistance,
    out float3 displacement,
    out float3 normal,
    out float  whitecap,
    out float  heightNorm)
{
    const int   waveCount = clamp(gWaveCount, 1, 12);
    const float baseAngle = atan2(gWindDir.y, gWindDir.x);
    const float ampScale  = max(gWaveScale, 0.0f);

    // --- Pass 1: total steepness, so the budget below can normalise it ---
    // (no trigonometry here, so this is cheap)
    float steepSum = 1e-5f;
    float ampTotal = 1e-5f;
    {
        float amplitude  = gBaseAmplitude * ampScale;
        float wavelength = gBaseWavelength;
        [loop]
        for (int i = 0; i < waveCount; ++i)
        {
            const float fi = (float)i;
            const float wl = wavelength * (0.85f + 0.3f * Hash1(fi * 3.77f + 5.1f));
            const float k  = kTwoPi / max(wl, 0.05f);
            const float a  = amplitude * WaveLod(wl, viewDistance) * GridLimit(wl);

            steepSum += a * k;
            ampTotal += a;

            // Amplitude falls off nearly in step with wavelength so per-wave
            // steepness stays roughly constant across the spectrum.  A slower
            // amplitude falloff (0.80) leaves the short/mid band far steeper
            // than the swell, which is what makes the surface read as one busy
            // mid-frequency band with no large-scale variation.
            // NOTE: must match the budget pre-pass above exactly.
            amplitude  *= 0.70f;
            wavelength *= 0.68f;
        }
    }

    // Keep the summed steepness under 1.0: at exactly 1.0 the crest becomes a
    // cusp, beyond it the surface folds through itself (spikes + bad normals).
    const float targetSteep = min(gSteepness * max(gChoppiness, 0.0f), 0.85f);
    const float steepBudget = targetSteep / steepSum;

    // --- Pass 2: accumulate displacement, tangents and the Jacobian ---
    displacement = float3(0.0f, 0.0f, 0.0f);
    float3 Tx = float3(1.0f, 0.0f, 0.0f);   // d(surface)/dx
    float3 Ty = float3(0.0f, 1.0f, 0.0f);   // d(surface)/dy

    {
        float amplitude  = gBaseAmplitude * ampScale;
        float wavelength = gBaseWavelength;
        [loop]
        for (int i = 0; i < waveCount; ++i)
        {
            const float fi = (float)i;
            const float r0 = Hash1(fi * 7.13f + 1.7f);
            const float r1 = Hash1(fi * 3.77f + 5.1f);
            const float r2 = Hash1(fi * 11.31f + 2.9f);

            // Real seas are band-structured: the long swell runs close to the
            // wind axis while short wind-chop fans out much wider.  Scaling the
            // spread by the band index reproduces that, and is what stops the
            // whole surface reading as one coherent corduroy pattern.
            // Alternating the sign keeps successive waves on opposite sides.
            const float bandT  = (waveCount > 1) ? (fi / (float)(waveCount - 1)) : 0.0f;
            const float spreadScale = lerp(0.3f, 1.0f, bandT);
            const float side   = (fmod(fi, 2.0f) < 0.5f) ? 1.0f : -1.0f;
            const float spread = side * (0.25f + 0.75f * r0) * gDirectionalSpread * spreadScale;
            const float angle  = baseAngle + spread;
            const float2 D     = float2(cos(angle), sin(angle));

            const float wl = wavelength * (0.85f + 0.3f * r1);
            const float k  = kTwoPi / max(wl, 0.05f);
            const float w  = sqrt(kGravity * k);            // deep-water dispersion
            const float a  = amplitude * WaveLod(wl, viewDistance) * GridLimit(wl);

            // Random per-wave phase: without this every wave crosses zero at the
            // origin simultaneously and the sum looks periodic.
            const float phase = k * dot(D, restXY) - w * gBaseSpeed * gTime + r2 * kTwoPi;
            const float s = sin(phase);
            const float c = cos(phase);

            const float QA  = steepBudget * a;   // horizontal displacement scale
            const float QAk = QA * k;
            const float ak  = a * k;

            displacement.xy += QA * D * c;
            displacement.z  += a * s;

            Tx.x -= QAk * D.x * D.x * s;
            Tx.y -= QAk * D.x * D.y * s;
            Tx.z += ak  * D.x * c;

            Ty.x -= QAk * D.x * D.y * s;
            Ty.y -= QAk * D.y * D.y * s;
            Ty.z += ak  * D.y * c;

            // Amplitude falls off nearly in step with wavelength so per-wave
            // steepness stays roughly constant across the spectrum.  A slower
            // amplitude falloff (0.80) leaves the short/mid band far steeper
            // than the swell, which is what makes the surface read as one busy
            // mid-frequency band with no large-scale variation.
            // NOTE: must match the budget pre-pass above exactly.
            amplitude  *= 0.70f;
            wavelength *= 0.68f;
        }
    }

    // Exact normal: cannot invert regardless of steepness (flat water gives
    // cross((1,0,0),(0,1,0)) = (0,0,1)).
    normal = normalize(cross(Tx, Ty));

    // Whitecaps where the horizontal map compresses.  The Jacobian is 1 on flat
    // water and falls toward 0 as a crest sharpens, so foam ramps in over a
    // fixed band below the threshold rather than keying off absolute height.
    const float jacobian = Tx.x * Ty.y - Tx.y * Ty.x;
    whitecap = 1.0f - smoothstep(gFoamThreshold - 0.25f, gFoamThreshold, jacobian);

    heightNorm = saturate(displacement.z / ampTotal * 0.5f + 0.5f);
}

// Sum the three FFT cascades.  Each patch tiles independently at its own world
// scale, so their periods do not line up and the surface never visibly repeats.
// Cascades whose wavelengths fall below the mesh spacing are faded out for the
// same reason the Gerstner spectrum is band-limited.
void SampleFftCascades(float2 worldXY, float viewDistance, out float3 disp, out float folding)
{
    disp = float3(0.0f, 0.0f, 0.0f);
    folding = 0.0f;

    const float3 patch = max(gOceanPatchSizes, 1e-3f);

    const float w0 = WaveLod(patch.x, viewDistance) * GridLimit(patch.x / 16.0f);
    const float w1 = WaveLod(patch.y, viewDistance) * GridLimit(patch.y / 16.0f);
    const float w2 = WaveLod(patch.z, viewDistance) * GridLimit(patch.z / 16.0f);

    const float4 d0 = gOceanDisplacement0.SampleLevel(gLinearWrap, worldXY / patch.x, 0);
    const float4 d1 = gOceanDisplacement1.SampleLevel(gLinearWrap, worldXY / patch.y, 0);
    const float4 d2 = gOceanDisplacement2.SampleLevel(gLinearWrap, worldXY / patch.z, 0);

    disp = (d0.xyz * w0 + d1.xyz * w1 + d2.xyz * w2) * gFftDisplacementScale;
    folding = saturate(d0.w * w0 + d1.w * w1 + d2.w * w2);
}

float3 SampleFftNormal(float2 worldXY, float viewDistance)
{
    const float3 patch = max(gOceanPatchSizes, 1e-3f);

    const float3 n0 = gOceanNormal0.SampleLevel(gLinearWrap, worldXY / patch.x, 0).xyz;
    const float3 n1 = gOceanNormal1.SampleLevel(gLinearWrap, worldXY / patch.y, 0).xyz;
    const float3 n2 = gOceanNormal2.SampleLevel(gLinearWrap, worldXY / patch.z, 0).xyz;

    // Combine slopes rather than summing normals: adding normal vectors flattens
    // detail, whereas the slopes of independent bands genuinely add.
    float2 slope = n0.xy / max(n0.z, 1e-3f)
                 + n1.xy / max(n1.z, 1e-3f)
                 + n2.xy / max(n2.z, 1e-3f);
    slope *= gFftNormalStrength;

    return normalize(float3(-slope, 1.0f));
}

PSInput VSMain(VSInput input)
{
    PSInput output;

    const float3 restWorld = mul(float4(input.Position, 1.0f), gModel).xyz;
    const float  viewDistance = length(restWorld - gCameraPos);

    float3 disp;
    float3 waveNormal;
    float  whitecap;
    float  heightNorm;

    if (gFftEnabled > 0.5f)
    {
        float folding;
        SampleFftCascades(restWorld.xy, viewDistance, disp, folding);
        // The pixel shader rebuilds the normal from the cascade maps; the
        // vertex normal only needs to be a sane placeholder.
        waveNormal = float3(0.0f, 0.0f, 1.0f);
        whitecap   = folding;
        heightNorm = saturate(disp.z * 0.5f + 0.5f);
    }
    else
    {
        EvaluateOcean(restWorld.xy, viewDistance, disp, waveNormal, whitecap, heightNorm);
    }

    const float3 displacedWorld = restWorld + disp;

    output.Position      = mul(float4(displacedWorld, 1.0f), gViewProj);
    output.WorldPosition = displacedWorld;
    output.WorldNormal   = waveNormal;
    output.TexCoord      = input.TexCoord;
    output.FoamHeight    = float2(whitecap, heightNorm);
    return output;
}

// Domain-warped ripple detail.  Returns a slope (dZ/dx, dZ/dy) rather than a
// normal so it can be added to the macro wave slope directly.
float2 DetailSlope(float2 worldXY, float fade)
{
    float2 g = float2(0.0f, 0.0f);

    if (fade > 0.001f)
    {
        const float2 uv = worldXY * (gDetailTiling * 0.02f);
        const float  t  = gTime;

        // Warp the sampling domain with a slow wave so the ripples stop looking
        // like a fixed grid of sines.
        const float2 warp = float2(
            sin(uv.y * 1.7f + t * 0.35f),
            cos(uv.x * 1.3f - t * 0.28f)) * 0.35f;
        const float2 p = uv + warp;

        float amp  = 1.0f;
        float freq = 1.0f;

        // A few octaves of directional ripples.
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const float fi = (float)i;
            const float ang = 0.7f + fi * 2.399963f;          // golden angle fan
            const float2 d  = float2(cos(ang), sin(ang));
            const float  sp = 1.1f + 0.5f * Hash1(fi + 3.1f);
            g += d * cos(dot(p, d) * 6.2831f * freq + t * sp) * amp;

            amp  *= 0.55f;
            freq *= 1.9f;
        }
    }

    return -g * gDetailStrength * fade;
}

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}

// Single-exit with every `out` written up front: fxc's flow analysis flags
// multi-return functions with out parameters as potentially uninitialised.
bool WorldToScreenUV(float3 worldPos, out float2 uv, out float clipW)
{
    const float4 clip = mul(float4(worldPos, 1.0f), gViewProj);

    uv    = float2(0.0f, 0.0f);
    clipW = clip.w;
    bool onScreen = false;

    if (clip.w > 0.0001f)
    {
        const float2 ndc = clip.xy / clip.w;
        uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        onScreen = (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f);
    }

    return onScreen;
}

// Analytic sky used where the screen-space reflection finds nothing.  Built
// from the engine's Hosek-Wilkie sky/sun colours so it matches the rendered sky.
float3 SkyGradient(float3 dir)
{
    const float up = saturate(dir.z);
    const float lum = dot(gSkyColor, float3(0.2126f, 0.7152f, 0.0722f));
    // Horizon is hazier / desaturated, zenith keeps the sky's own colour.
    const float3 horizon = lerp(gSkyColor, lum.xxx * 1.05f + 0.01f, 0.7f);
    return lerp(horizon, gSkyColor, pow(up, 0.45f));
}

// Screen-space reflection march.  Returns true and the reflected colour when
// the ray hits geometry that is actually in front of the water.
bool TraceReflection(float3 origin, float3 dir, float surfaceDist, out float3 color)
{
    color = float3(0.0f, 0.0f, 0.0f);
    bool hit = false;

    const int steps = clamp(gSsrSteps, 0, 32);
    float stride = max(gSsrStride, 0.05f);
    float3 p = origin;

    [loop]
    for (int i = 0; i < steps; ++i)
    {
        p += dir * stride;
        stride *= 1.35f;                    // geometric march: cheap + long reach

        float2 uv;
        float  clipW;
        if (!WorldToScreenUV(p, uv, clipW))
            break;                           // ray left the screen -> use sky

        const float sceneDepth = gDepthBuffer.SampleLevel(gPointClamp, uv, 0).r;
        if (sceneDepth < 0.9999f)            // skip sky texels, keep marching
        {
            const float3 sceneWorld = ReconstructWorldPosition(uv, sceneDepth);
            const float  sceneDist  = length(sceneWorld - gCameraPos);
            const float  rayDist    = length(p - gCameraPos);

            // Hit when the ray passes behind scene geometry, within a thickness
            // tolerance (looser further out, where the steps are longer).
            if (rayDist > sceneDist && (rayDist - sceneDist) < gSsrThickness * stride)
            {
                // Only accept reflectors that are behind the water surface;
                // anything closer is occluding geometry, not a reflection.
                if (sceneDist > surfaceDist)
                {
                    color = gRefractionColor.SampleLevel(gLinearClamp, uv, 0).rgb;
                    hit = true;
                }
                break;
            }
        }
    }

    return hit;
}

// Full reflection lookup.
//   1. screen-space trace against real geometry,
//   2. otherwise the *actual rendered sky* sampled out of the scene-colour copy
//      by projecting a distant point along the reflected ray.  This matters:
//      the background sky is full Hosek-Wilkie radiance while gSkyColor holds
//      only the normalised tint, so a hand-rolled gradient never matches the
//      real sky and leaves a visible seam at the horizon.
//   3. gradient fallback for rays that leave the screen — those are the steep,
//      upward rays where Fresnel is ~2%, so the approximation is invisible.
float3 EvaluateReflection(float3 pos, float3 R, float surfaceDist)
{
    float3 traced;
    float3 result = SkyGradient(R);   // fallback, overwritten on a better source
    bool resolved = false;

    if (TraceReflection(pos, R, surfaceDist, traced))
    {
        result = traced;
        resolved = true;
    }

    if (!resolved)
    {
        float2 uv;
        float  clipW;
        if (WorldToScreenUV(pos + R * 20000.0f, uv, clipW))
        {
            // Only accept it if that texel really is sky; otherwise the distant
            // projection could land on unrelated foreground geometry.
            if (gDepthBuffer.SampleLevel(gPointClamp, uv, 0).r >= 0.9999f)
                result = gRefractionColor.SampleLevel(gLinearClamp, uv, 0).rgb;
        }
    }

    return result;
}

// GGX specular for the sun glint.
// Angular radius of the sun disc in radians (~0.53 degrees across).  Treating
// the sun as a zero-size point light makes the GGX lobe peak in the tens of
// thousands for water-grade roughness, which blows the whole surface to white.
static const float kSunAngularRadius = 0.00465f;

// GGX sun specular, normalised for the sun's finite angular size (Karis'
// sphere-light approximation: widen alpha, then rescale to conserve energy).
float SpecularSun(float3 N, float3 V, float3 L, float roughness)
{
    float result = 0.0f;

    const float NoL = saturate(dot(N, L));
    if (NoL > 0.0f)
    {
        const float alpha  = max(roughness * roughness, 0.002f);
        // Widening alpha to at least the sun's angular radius turns the
        // pinpoint spike into the broad, realistic glitter path on the water.
        const float alphaP = saturate(alpha + kSunAngularRadius);
        const float energy = (alpha / alphaP) * (alpha / alphaP);

        const float3 H  = normalize(V + L);
        const float NoH = saturate(dot(N, H));
        const float NoV = saturate(dot(N, V));

        const float a2 = alphaP * alphaP;
        const float d  = (NoH * NoH) * (a2 - 1.0f) + 1.0f;
        const float D  = a2 / max(kPi * d * d, 1e-6f);

        const float k  = alphaP * 0.5f;
        const float gV = NoV / (NoV * (1.0f - k) + k);
        const float gL = NoL / (NoL * (1.0f - k) + k);

        result = D * energy * gV * gL / max(4.0f * NoV * NoL, 1e-4f) * NoL;
    }

    return result;
}

float4 PSMain(PSInput input) : SV_Target0
{
    const float2 screenUV = input.Position.xy / float2(gScreenWidth, gScreenHeight);

    const float3 toCamera  = gCameraPos - input.WorldPosition;
    const float  waterDist = length(toCamera);
    const float3 V = toCamera / max(waterDist, 1e-4f);
    const float3 L = normalize(-gSunDirection);

    // --- Surface normal: macro waves + distance-faded ripple detail ---
    // With the FFT ocean the macro normal comes from the cascade slope maps,
    // which carry far more structure than an interpolated vertex normal.
    const float3 macroN = (gFftEnabled > 0.5f)
        ? SampleFftNormal(input.WorldPosition.xy, waterDist)
        : normalize(input.WorldNormal);
    const float distanceFade = smoothstep(
        gDetailFadeDistance * 0.2f, max(gDetailFadeDistance, 1.0f), waterDist);
    const float detailFade = 1.0f - distanceFade;
    const float2 slope = DetailSlope(input.WorldPosition.xy, detailFade);
    float3 N = normalize(float3(macroN.xy + slope, macroN.z));
    // Relax the normal toward flat as the waves fall below a pixel, so the far
    // field resolves into a coherent pale horizon instead of a noisy carpet.
    N = normalize(lerp(N, float3(0.0f, 0.0f, 1.0f), distanceFade * gDistantFlatten));
    // Never let the shading normal tip below the horizon; grazing views of a
    // steep crest would otherwise flip the Fresnel term.
    N = normalize(float3(N.xy, max(N.z, 0.08f)));

    const float NoV = saturate(dot(N, V));

    // --- Opaque scene behind the water: occlusion + column thickness ---
    const float opaqueDepth = gDepthBuffer.SampleLevel(gPointClamp, screenUV, 0).r;
    const float3 opaqueWorld = ReconstructWorldPosition(screenUV, opaqueDepth);
    const float opaqueDist = (opaqueDepth >= 0.9999f) ? 1e8f : length(opaqueWorld - gCameraPos);

    clip(opaqueDist - waterDist + 0.05f);

    const float column = max(opaqueDist - waterDist, 0.0f);

    // --- Screen-space refraction ---
    const float distort = gRefractionStrength * saturate(column * 0.25f);
    float2 refractUV = screenUV + N.xy * distort;

    const float refractDepth = gDepthBuffer.SampleLevel(gPointClamp, refractUV, 0).r;
    const float3 refractWorld = ReconstructWorldPosition(refractUV, refractDepth);
    if (refractDepth < 0.9999f && length(refractWorld - gCameraPos) < waterDist - 0.05f)
        refractUV = screenUV;   // reject foreground bleeding onto the surface

    const float3 bottom = gRefractionColor.SampleLevel(gLinearClamp, refractUV, 0).rgb;

    // --- Volume: Beer-Lambert absorption + in-scattering ---
    // Water is not an opaque blue surface: its colour comes from wavelength-
    // dependent extinction through the column plus light scattered back out of
    // it.  Red is absorbed within a couple of metres, blue-green survives, so
    // shallow water is near-colourless and deep water goes blue.
    // Clamp the path length: beyond ~100 m the transmittance is already zero,
    // and `column` is 1e8 wherever there is no geometry behind the water.
    const float pathLength = min(column, 100.0f) / max(gAbsorptionDistance, 0.01f);
    const float3 transmittance = exp(-max(gExtinction.rgb, 0.0f) * pathLength);
    // Light available to scatter back out of the volume, so the water responds
    // to time of day instead of being a hard-coded colour.
    const float3 waterAmbient = gSkyColor + gSunColor * 0.25f;
    const float3 inScatter = gScatterColor.rgb * gScatterColor.a * waterAmbient
                           * (1.0f - transmittance);
    float3 transmitted = bottom * gShallowTint.rgb * transmittance + inScatter;
    // Pull a little saturation out of the volume: real underwater skylight is
    // not a pure navy albedo, and a fully saturated body reads as blue plastic.
    const float volLum = dot(transmitted, float3(0.2126f, 0.7152f, 0.0722f));
    transmitted = lerp(transmitted, volLum.xxx, saturate(gVolumeDesaturate));

    // --- Subsurface scattering: backlit crests glow ---
    // Light bending through the wave toward the viewer; strongest on raised
    // water when the sun is roughly behind the wave.
    const float3 sssDir = normalize(-L + N * gSssDistortion);
    const float  back   = pow(saturate(dot(V, sssDir)), max(gSssPower, 1.0f));
    const float  lift   = saturate(input.FoamHeight.y * 1.6f - 0.35f);
    const float3 sss    = gSssColor.rgb * gSssColor.a * back * lift * gSunColor;
    transmitted += sss;

    // --- Reflection: screen-space geometry, then the real rendered sky ---
    const float3 R = reflect(-V, N);
    const float3 reflection = EvaluateReflection(input.WorldPosition, R, waterDist)
                            * gReflectionStrength;

    // --- Fresnel (Schlick) for the environment term ---
    const float fresnel = saturate(gF0 + (1.0f - gF0) * pow(1.0f - NoV, max(gFresnelPower, 1.0f)));

    // Energy conserving: (1 - F) * transmitted + F * reflected.
    float3 color = lerp(transmitted, reflection, fresnel);

    // --- Sun glint ---
    // The microfacet Fresnel uses V-dot-H (the angle against the *microfacet*
    // normal), not N-dot-V.  At grazing sun angles H approaches the macro
    // normal, so this still rises to ~60% reflectance the way real water does,
    // while staying at ~2% for an overhead view.
    // Sub-pixel slope variance becomes roughness (Toksvig-style in spirit),
    // which broadens the sun lobe with distance instead of leaving a hard mirror.
    const float rough = clamp(gRoughness + distanceFade * gDistantRoughness, 0.01f, 1.0f);
    const float3 Hv = normalize(V + L);
    const float VoH = saturate(dot(V, Hv));
    const float specF = gF0 + (1.0f - gF0) * pow(1.0f - VoH, 5.0f);
    const float spec = SpecularSun(N, V, L, rough) * gSunSpecPower * specF;
    // Clamp keeps a stray near-mirror pixel from becoming a TAA firefly; the
    // ceiling is still far into HDR so the glitter reads as genuinely bright.
    const float3 sunSpecular = gSunColor * min(spec, 40.0f);
    color += sunSpecular;

    // --- Foam: sparse whitecaps + shoreline ---
    const float shoreFoam = (gShorelineFoam > 0.001f)
        ? (1.0f - smoothstep(0.0f, gShorelineFoam, column))
        : 0.0f;
    const float foam = saturate(input.FoamHeight.x * gFoamColor.a + shoreFoam);
    // Foam is a rough, bright dielectric: mostly diffuse sun + sky light.
    const float3 foamLit = gFoamColor.rgb * (gSkyColor + gSunColor * saturate(dot(N, L)));
    color = lerp(color, foamLit, foam);

    // --- Diagnostics -------------------------------------------------------
    // Isolate one term at a time to tell an optics bug from a wave bug.  Values
    // are written straight into the HDR target, so compare relative gradients
    // rather than absolute brightness.
    if (gDebugMode != WATER_DEBUG_OFF)
    {
        if (gDebugMode == WATER_DEBUG_FRESNEL)
            color = fresnel.xxx;
        else if (gDebugMode == WATER_DEBUG_NDOTV)
            color = NoV.xxx;
        else if (gDebugMode == WATER_DEBUG_NORMALS)
            color = N * 0.5f + 0.5f;
        else if (gDebugMode == WATER_DEBUG_THICKNESS)
            color = saturate(column / max(gAbsorptionDistance, 0.01f)).xxx;
        else if (gDebugMode == WATER_DEBUG_REFLECTION)
            color = reflection;
        else if (gDebugMode == WATER_DEBUG_TRANSMISSION)
            color = transmitted;
        else if (gDebugMode == WATER_DEBUG_FOAM)
            color = foam.xxx;
        else if (gDebugMode == WATER_DEBUG_SUNSPEC)
            color = sunSpecular;
        else if (gDebugMode == WATER_DEBUG_NEUTRAL)
        {
            // Both sides of the energy-conserving blend are mid-grey, so the
            // result is mid-grey for every pixel regardless of Fresnel.
            const float3 neutral = float3(0.18f, 0.18f, 0.18f);
            color = fresnel * neutral + (1.0f - fresnel) * neutral;
        }
    }

    return float4(color, 1.0f);
}
