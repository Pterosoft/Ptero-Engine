// Particle_Draw.hlsl
// Vertex + pixel shaders for drawing one particle system's sprites.
//
// Six procedural vertices per particle - no vertex or index buffer - with the
// quad built in the vertex shader from the particle's world position, its size
// and rotation curves, and the facing mode. The pixel shader samples the sprite
// (optionally a flipbook atlas), applies the colour-over-life gradient and the
// emissive term, and fades the sprite where it approaches scene geometry.
//
// Targets vs_5_0 / ps_5_0 so the startup shader cache produces exactly the
// bytecode this renderer asks for at run time instead of compiling twice.

struct Particle
{
    float3 Position;
    float  Age;
    float3 Velocity;
    float  Life;
    float4 Seed;
    float  Rotation;
    float  RotationRate;
    float  SizeScale;
    float  FrameOffset;
};

struct ParticleLight
{
    float3 Position;
    float  Radius;
    float3 Color;
    float  InvRadiusSq;
};

#define PARTICLE_MAX_LIGHTS 8

// Facing modes - must match ParticleFacingMode in Components.h.
#define FACING_BILLBOARD  0u
#define FACING_STRETCHED  1u
#define FACING_HORIZONTAL 2u
#define FACING_VERTICAL   3u

StructuredBuffer<Particle> Particles   : register(t0);
Texture2D                  SpriteTex   : register(t1);
Texture2D<float>           SceneDepth  : register(t2);

SamplerState LinearClampSampler : register(s0);

cbuffer ParticleDrawConstants : register(b0)
{
    float4x4 ViewProj;

    float3 CameraPosition;
    float  ParticleCount;

    float3 CameraRight;
    float  StartSize;

    float3 CameraUp;
    float  EndSize;

    float3 CameraForward;
    float  StretchFactor;

    float4 ColorStart;
    float4 ColorMid;
    float4 ColorEnd;

    float3 EmissiveColor;
    float  ColorMidPoint;

    float4 BaseColorTint;

    float  EmissiveIntensity;
    float  FlipbookColumns;
    float  FlipbookRows;
    float  FlipbookFps;

    float  FlipbookBlendFrames;
    float  SoftFadeDistance;
    float  CameraFadeDistance;
    float  UseSoftParticles;

    float  NearPlane;
    float  FarPlane;
    float  ScreenWidth;
    float  ScreenHeight;

    float  LightingInfluence;
    float  SphericalNormal;
    float  CullDistance;
    uint   FacingMode;

    float3 SunDirection;   // world-space direction FROM the sun TOWARD the scene
    float  HasSpriteTexture;

    float3 SunColor;
    float  NumLights;

    float3 SkyColor;
    float  BlendModeIsPremultiplied;

    // Padded as four scalars rather than float+float3: a float3 may not straddle
    // a 16-byte boundary, so the latter would silently occupy two registers and
    // shift every light below it out of step with the C++ struct.
    float  AlphaFromLuminance;
    float  ManualDepthTest;
    float  Pad0;
    float  Pad1;

    ParticleLight Lights[PARTICLE_MAX_LIGHTS];
};

struct VSOutput
{
    float4 Position    : SV_Position;
    float2 Uv          : TEXCOORD0;
    float4 Color       : TEXCOORD1;
    float3 WorldPos    : TEXCOORD2;
    float  ViewDepth   : TEXCOORD3;
    float2 FrameBlend  : TEXCOORD4;  // x = frame index, y = blend to next frame
    float  Fade        : TEXCOORD5;  // distance and camera-proximity fade
};

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSq = dot(value, value);
    return (lengthSq > 1.0e-8f) ? (value * rsqrt(lengthSq)) : fallback;
}

// Unit quad corners for a two-triangle strip expressed as a triangle list.
float2 GetQuadCorner(uint vertexId)
{
    switch (vertexId)
    {
    case 0u: return float2(-1.0f, -1.0f);
    case 1u: return float2( 1.0f, -1.0f);
    case 2u: return float2(-1.0f,  1.0f);
    case 3u: return float2(-1.0f,  1.0f);
    case 4u: return float2( 1.0f, -1.0f);
    default: return float2( 1.0f,  1.0f);
    }
}

// Two-segment gradient. Splitting at ColorMidPoint rather than using a fixed
// midpoint is what lets a flame hold its white-hot core for the first fifth of
// its life and spend the rest cooling.
float4 EvaluateLifeColor(float normalizedAge)
{
    if (normalizedAge < ColorMidPoint)
    {
        return lerp(ColorStart, ColorMid, normalizedAge / max(ColorMidPoint, 1e-4f));
    }

    const float t = (normalizedAge - ColorMidPoint) / max(1.0f - ColorMidPoint, 1e-4f);
    return lerp(ColorMid, ColorEnd, saturate(t));
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    VSOutput output;

    const Particle p = Particles[instanceId];
    const float2 corner = GetQuadCorner(vertexId);

    // Dead slots are collapsed to a degenerate quad at the camera position.
    // Culling them here costs one branch and saves the rasteriser the work;
    // the alternative - an indirect draw over a compacted list - is not worth
    // the extra pass at the particle counts a level actually places.
    if (p.Life <= 0.0f || p.Age >= p.Life)
    {
        output.Position = float4(0.0f, 0.0f, -1.0f, 1.0f);
        output.Uv = float2(0.0f, 0.0f);
        output.Color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.WorldPos = CameraPosition;
        output.ViewDepth = 0.0f;
        output.FrameBlend = float2(0.0f, 0.0f);
        output.Fade = 0.0f;
        return output;
    }

    const float normalizedAge = saturate(p.Age / max(p.Life, 1e-4f));

    const float size = lerp(StartSize, EndSize, normalizedAge) * p.SizeScale;

    const float3 toCamera = CameraPosition - p.Position;
    const float distanceToCamera = length(toCamera);

    // Orientation basis for the quad.
    float3 axisX;
    float3 axisY;

    if (FacingMode == FACING_HORIZONTAL)
    {
        axisX = float3(1.0f, 0.0f, 0.0f);
        axisY = float3(0.0f, 1.0f, 0.0f);
    }
    else if (FacingMode == FACING_VERTICAL)
    {
        // Upright card that yaws to face the camera: the world up axis stays
        // vertical no matter where the camera is, which is what keeps a column
        // of flame from tipping over as the player looks down at it.
        const float3 flatToCamera = SafeNormalize(float3(toCamera.xy, 0.0f), float3(0.0f, 1.0f, 0.0f));
        axisX = SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), flatToCamera), float3(1.0f, 0.0f, 0.0f));
        axisY = float3(0.0f, 0.0f, 1.0f);
    }
    else
    {
        axisX = CameraRight;
        axisY = CameraUp;
    }

    // Sprite spin. Stretched particles take their orientation from the velocity
    // instead, so the spin is skipped for them.
    if (FacingMode != FACING_STRETCHED)
    {
        const float sinRotation = sin(p.Rotation);
        const float cosRotation = cos(p.Rotation);
        const float3 rotatedX = axisX * cosRotation + axisY * sinRotation;
        const float3 rotatedY = axisY * cosRotation - axisX * sinRotation;
        axisX = rotatedX;
        axisY = rotatedY;
    }

    float3 offset;
    if (FacingMode == FACING_STRETCHED)
    {
        const float speed = length(p.Velocity);
        const float3 viewDirection = SafeNormalize(toCamera, -CameraForward);
        const float3 motionAxis = SafeNormalize(p.Velocity, CameraUp);
        const float3 sideAxis = SafeNormalize(cross(viewDirection, motionAxis), CameraRight);

        // The quad grows along the velocity only, so a slow ember stays round
        // and a fast spark becomes a streak without changing its width.
        const float stretchedLength = size * (1.0f + speed * StretchFactor);
        offset = motionAxis * (corner.y * stretchedLength * 0.5f)
               + sideAxis * (corner.x * size * 0.5f);
    }
    else
    {
        offset = axisX * (corner.x * size * 0.5f) + axisY * (corner.y * size * 0.5f);
    }

    const float3 worldPosition = p.Position + offset;

    output.Position = mul(float4(worldPosition, 1.0f), ViewProj);
    output.WorldPos = worldPosition;
    output.ViewDepth = output.Position.w;
    output.Uv = corner * 0.5f + 0.5f;
    output.Color = EvaluateLifeColor(normalizedAge) * BaseColorTint;

    // Flipbook frame. With no explicit frame rate the whole atlas is spread
    // across the particle's life, which is what a hand-authored flame sheet
    // expects; an explicit rate loops instead and can start on a random frame.
    const float frameCount = max(FlipbookColumns * FlipbookRows, 1.0f);
    float framePosition;
    if (FlipbookFps > 0.0f)
    {
        framePosition = p.Age * FlipbookFps + p.FrameOffset * frameCount;
    }
    else
    {
        framePosition = normalizedAge * frameCount;
    }
    output.FrameBlend = float2(framePosition, FlipbookBlendFrames);

    // Fade out at the far cull distance so particles leave the frame smoothly,
    // and fade as the camera gets close enough to be inside the sprite.
    float fade = 1.0f - saturate((distanceToCamera - CullDistance * 0.85f) / max(CullDistance * 0.15f, 1e-3f));
    if (CameraFadeDistance > 0.0f)
    {
        fade *= saturate(distanceToCamera / CameraFadeDistance);
    }
    output.Fade = fade;

    return output;
}

// ─── Pixel ───────────────────────────────────────────────────────────────────

// Maps one flipbook frame index onto the atlas, reading left to right then top
// to bottom. The UV is scaled to the sub-rect and inset by half a texel's worth
// of the frame so bilinear filtering cannot bleed in the neighbouring frame.
float2 FlipbookUv(float2 uv, float frameIndex)
{
    const float columns = max(FlipbookColumns, 1.0f);
    const float rows = max(FlipbookRows, 1.0f);
    const float frameCount = columns * rows;

    const float wrapped = frameIndex - floor(frameIndex / frameCount) * frameCount;
    const float index = floor(wrapped);

    const float column = index - floor(index / columns) * columns;
    const float row = floor(index / columns);

    const float2 frameSize = float2(1.0f / columns, 1.0f / rows);
    const float2 inset = frameSize * 0.001f;
    return (float2(column, row) + inset) * frameSize + saturate(uv) * (frameSize - inset * 2.0f);
}

float4 SampleSprite(float2 uv, float2 frameBlend)
{
    if (HasSpriteTexture < 0.5f)
    {
        // No texture assigned: fall back to a soft radial blob so an emitter is
        // still visible (and tunable) before its material has been authored.
        const float radius = length(uv * 2.0f - 1.0f);
        const float falloff = saturate(1.0f - radius);
        return float4(1.0f, 1.0f, 1.0f, falloff * falloff);
    }

    const float frameCount = max(FlipbookColumns * FlipbookRows, 1.0f);
    if (frameCount <= 1.0f)
    {
        return SpriteTex.Sample(LinearClampSampler, uv);
    }

    const float4 current = SpriteTex.Sample(LinearClampSampler, FlipbookUv(uv, frameBlend.x));
    if (frameBlend.y < 0.5f)
    {
        return current;
    }

    const float4 next = SpriteTex.Sample(LinearClampSampler, FlipbookUv(uv, frameBlend.x + 1.0f));
    return lerp(current, next, frac(frameBlend.x));
}

// Depth buffer value -> view-space distance, for a standard (non-reversed)
// D3D perspective projection.
float LinearizeDepth(float deviceDepth)
{
    const float denominator = FarPlane - deviceDepth * (FarPlane - NearPlane);
    return (NearPlane * FarPlane) / max(denominator, 1e-6f);
}

float3 EvaluateSceneLighting(float3 worldPosition, float3 toCamera, float2 uv)
{
    // A sprite has no real normal. Blending the camera-facing direction toward
    // a hemisphere derived from the UV gives thick smoke enough shape for the
    // light to wrap around it, while thin wisps keep the flat card look.
    const float2 centered = uv * 2.0f - 1.0f;
    const float radiusSq = saturate(dot(centered, centered));
    const float3 sphereNormal = normalize(float3(centered, sqrt(max(1.0f - radiusSq, 1e-4f))));

    const float3 viewNormal = SafeNormalize(toCamera, float3(0.0f, 0.0f, 1.0f));
    const float3 right = SafeNormalize(cross(float3(0.0f, 0.0f, 1.0f), viewNormal), float3(1.0f, 0.0f, 0.0f));
    const float3 up = cross(viewNormal, right);
    const float3 worldSphereNormal = normalize(
        right * sphereNormal.x + up * sphereNormal.y + viewNormal * sphereNormal.z);

    const float3 normal = normalize(lerp(viewNormal, worldSphereNormal, saturate(SphericalNormal)));

    // Half-lambert: a particle is a participating medium, so light that reaches
    // the back of it still shows through. A hard N.L would make smoke read as a
    // collection of solid discs.
    const float sunWrap = saturate(dot(normal, -SunDirection) * 0.5f + 0.5f);
    float3 lighting = SkyColor + SunColor * sunWrap;

    const int lightCount = min(int(NumLights), PARTICLE_MAX_LIGHTS);
    [loop]
    for (int i = 0; i < lightCount; ++i)
    {
        const float3 toLight = Lights[i].Position - worldPosition;
        const float distanceSq = dot(toLight, toLight);
        const float normalized = saturate(distanceSq * Lights[i].InvRadiusSq);
        if (normalized >= 1.0f)
            continue;

        const float3 lightDirection = toLight * rsqrt(max(distanceSq, 1e-6f));
        const float wrap = saturate(dot(normal, lightDirection) * 0.5f + 0.5f);
        const float falloff = (1.0f - normalized) * (1.0f - normalized);
        lighting += Lights[i].Color * wrap * falloff;
    }

    return lighting;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float4 sprite = SampleSprite(input.Uv, input.FrameBlend);

    // An additive sheet authored as colour on black carries its opacity in its
    // brightness, not in an alpha channel it may not even have. Reading the
    // luminance is what makes such a sheet render at all.
    if (AlphaFromLuminance > 0.5f)
    {
        sprite.a = saturate(dot(sprite.rgb, float3(0.2126f, 0.7152f, 0.0722f)));
    }

    float alpha = sprite.a * input.Color.a * input.Fade;
    if (alpha <= 0.001f)
    {
        discard;
    }

    // Occlusion and the soft fade both read the same depth sample, so they
    // share one fetch.
    //
    // ManualDepthTest replaces the hardware depth test under MSAA, where this
    // pass runs with no depth-stencil view bound at all (see ParticleRenderer's
    // ParticleDrawContext::ManualDepthTest). Without it, particles would draw
    // straight through walls.
    if (UseSoftParticles > 0.5f || ManualDepthTest > 0.5f)
    {
        const float2 screenUv = input.Position.xy / float2(max(ScreenWidth, 1.0f), max(ScreenHeight, 1.0f));
        const float sceneDepth = SceneDepth.SampleLevel(LinearClampSampler, screenUv, 0.0f);

        // A depth of 1 is the cleared far plane - nothing was drawn there, so
        // there is nothing to be occluded by and nothing to fade against.
        if (sceneDepth < 0.9999f)
        {
            const float sceneViewDepth = LinearizeDepth(sceneDepth);
            const float difference = sceneViewDepth - input.ViewDepth;

            if (ManualDepthTest > 0.5f && difference <= 0.0f)
            {
                discard;
            }

            if (UseSoftParticles > 0.5f)
            {
                alpha *= saturate(difference / max(SoftFadeDistance, 1e-4f));
            }
        }
    }

    if (alpha <= 0.001f)
    {
        discard;
    }

    const float3 toCamera = CameraPosition - input.WorldPos;

    float3 color = sprite.rgb * input.Color.rgb;

    // The emissive term is the whole effect for fire: it is what puts the
    // sprite into HDR so bloom picks it up, and it is the same quantity the
    // emitter turns into light for the GI, so the two cannot drift apart.
    float3 emissive = EmissiveColor * EmissiveIntensity * sprite.rgb * input.Color.rgb;

    if (LightingInfluence > 0.0f)
    {
        const float3 lighting = EvaluateSceneLighting(input.WorldPos, toCamera, input.Uv);
        color = lerp(color, color * lighting, saturate(LightingInfluence));
    }

    color += emissive;

    // Premultiplied output folds the coverage into the colour; the blend state
    // then uses ONE / INV_SRC_ALPHA rather than SRC_ALPHA / INV_SRC_ALPHA.
    if (BlendModeIsPremultiplied > 0.5f)
    {
        color *= alpha;
    }

    return float4(color, alpha);
}
