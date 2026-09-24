#pragma once

#include "..\System\include\System\Mesh.h"
#include "..\SDKs\nlohmann\json.hpp"
#include "LightStyles.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Euler angle handling for the whole engine. Every system that turns a
// TransformComponent into a matrix goes through here so they cannot drift apart.
namespace PteroTransform
{
    // Ptero's world is Z-up: the camera's up vector is +Z and terrain height runs along Z.
    // Euler angles are therefore composed X, then Y, then Z, which leaves the heading -
    // the rotation about the world up axis - applied last.
    //
    // XMMatrixRotationRollPitchYaw cannot be used for this. Its order (roll Z, pitch X,
    // yaw Y) is built for a Y-up world, and in a Z-up one it makes Rotation.y and
    // Rotation.z collapse into the same rotation whenever Rotation.x reaches +/-90
    // degrees - exactly the angle that stands a Y-up authored asset upright. Composing in
    // X, Y, Z order moves that degeneracy to Rotation.y = +/-90, which upright props never
    // reach.
    inline DirectX::XMMATRIX ComposeRotation(const DirectX::XMFLOAT3& eulerRadians)
    {
        return DirectX::XMMatrixRotationX(eulerRadians.x)
             * DirectX::XMMatrixRotationY(eulerRadians.y)
             * DirectX::XMMatrixRotationZ(eulerRadians.z);
    }

    // Scene format 1 stored Euler angles meant for XMMatrixRotationRollPitchYaw. Re-express
    // such a triple in the order ComposeRotation uses, so a level authored before the change
    // keeps the exact orientation it was saved with.
    inline DirectX::XMFLOAT3 ConvertLegacyRotation(const DirectX::XMFLOAT3& legacyEulerRadians)
    {
        // The legacy order is Rz(roll) * Rx(pitch) * Ry(yaw), with pitch = .x, yaw = .y and
        // roll = .z. Only six entries of that product are needed. They are evaluated in
        // double rather than read back out of an XMMATRIX: a triple that lands close to the
        // singularity feeds atan2 two very small arguments, and float rounding there is
        // enough to shift an entity by a measurable fraction of a degree.
        const double pitch = legacyEulerRadians.x;
        const double yaw   = legacyEulerRadians.y;
        const double roll  = legacyEulerRadians.z;
        const double sinPitch = std::sin(pitch), cosPitch = std::cos(pitch);
        const double sinYaw   = std::sin(yaw),   cosYaw   = std::cos(yaw);
        const double sinRoll  = std::sin(roll),  cosRoll  = std::cos(roll);

        const double m00 =  cosRoll * cosYaw + sinRoll * sinPitch * sinYaw;
        const double m01 =  sinRoll * cosPitch;
        const double m02 = -cosRoll * sinYaw + sinRoll * sinPitch * cosYaw;
        const double m10 = -sinRoll * cosYaw + cosRoll * sinPitch * sinYaw;
        const double m11 =  cosRoll * cosPitch;
        const double m12 =  sinRoll * sinYaw + cosRoll * sinPitch * cosYaw;
        const double m22 =  cosPitch * cosYaw;

        // The target order Rx(x) * Ry(y) * Rz(z) gives m02 = -sin y, m12 = sin x cos y,
        // m22 = cos x cos y, m01 = cos y sin z and m00 = cos y cos z.
        const double sinNewY = std::clamp(-m02, -1.0, 1.0);
        const double cosNewY = std::sqrt((std::max)(0.0, 1.0 - sinNewY * sinNewY));

        DirectX::XMFLOAT3 converted{};
        converted.y = static_cast<float>(std::asin(sinNewY));
        if (cosNewY > 1.0e-9)
        {
            converted.x = static_cast<float>(std::atan2(m12, m22));
            converted.z = static_cast<float>(std::atan2(m01, m00));
        }
        else
        {
            // Degenerate: Y sits at +/-90, so X and Z describe the same rotation here.
            // Fold the whole thing into Z and leave X at zero.
            converted.x = 0.0f;
            converted.z = static_cast<float>(std::atan2(-m10, m11));
        }
        return converted;
    }
}

struct TransformComponent
{
    static constexpr float MeshWorldScale = 0.1f;

    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Rotation{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Scale{ 1.0f, 1.0f, 1.0f };

    DirectX::XMMATRIX GetTransform() const
    {
        // Compose the entity transform from the editor-facing position, Euler rotation, and scale values.
        // Imported meshes render 10x too large in this renderer, so apply a fixed global mesh scale.
        const DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(
            Scale.x * MeshWorldScale,
            Scale.y * MeshWorldScale,
            Scale.z * MeshWorldScale);
        const DirectX::XMMATRIX rotationMatrix = PteroTransform::ComposeRotation(Rotation);
        const DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(Position.x, Position.y, Position.z);
        return scaleMatrix * rotationMatrix * translationMatrix;
    }
};

struct MeshComponent
{
    std::shared_ptr<Mesh> MeshAsset;
    // Relative path (from Data/) of the imported .fbx used to load MeshAsset.
    std::string MeshPath;
    // Relative path (from Data/) of the assigned .json material or multi-material file.
    std::string MaterialPath;
    // Multiplies the camera-distance heuristic used to pick lower LODs.
    // Values above 1 switch to cheaper LODs sooner; values below 1 keep detail longer.
    float LodUsageScale = 1.0f;
    // -1 = automatic, otherwise force a specific LOD level for debugging.
    int DebugForcedLod = -1;
};

struct NameComponent
{
    std::string Name = "Entity";
};

// Shape of a light's emitter. All three share one component, one GPU record and
// one code path through the deferred shading, the ray-traced GI and the
// volumetric fog - a spot is a point light with a cone mask, and a rect is a
// point light whose position is resolved per-pixel to the nearest point on the
// rectangle. Keeping them unified is what lets light styles, shadows and the
// GI contribution work identically for all three.
//
// Spot and Rect take their orientation from the entity's rotation and emit
// along the entity's local -Z, so an unrotated light points straight down -
// a ceiling lamp with no setup. This matches the decal convention.
enum class LightType : int
{
    Point = 0,
    Spot  = 1,
    Rect  = 2,
};

// Point light component: a positional light that radiates equally in all directions.
// All properties here are artist-facing and saved directly to the level file.
//
// Despite the name - kept because the level format and a good deal of engine
// code refer to it - this carries spot and rectangular area lights too, via
// the Type field below.
struct PointLightComponent
{
    // Which emitter shape this light uses. The fields each type reads are
    // grouped under their own headings further down.
    LightType Type = LightType::Point;

    // Luminous flux (total light output) in lumens.
    // Reference: ~800 lm ≈ a 60W incandescent bulb, ~1600 lm ≈ 100W equivalent.
    float IntensityLumens = 800.0f;

    // Radius of the light's influence sphere in world units.
    // Surfaces beyond this distance receive no contribution from this light.
    float Radius = 5.0f;

    // Linear RGB light color (HDR – values above 1 are valid).
    float ColorR = 1.0f;
    float ColorG = 1.0f;
    float ColorB = 1.0f;

    // Whether this light contributes to shadow maps.
    bool CastShadows = true;

    // Whether this light contributes scattering to volumetric fog.
    bool AffectVolumetricFog = true;

    // Whether this light contributes to indirect lighting - the ray-traced GI
    // bounce, the radiance probes and the radiance cascades.
    //
    // Off is what a cinematic light wants: a key or rim light placed to shape a
    // subject is not a real light in the world, and letting it bounce colour
    // off the surrounding walls gives the shot away. Turning this off leaves
    // the direct contribution exactly as authored and removes only the bounce.
    bool AffectGlobalIllumination = true;

    // Scales this light's indirect contribution without touching its direct
    // one. Between the two extremes of the toggle above: useful for a practical
    // that is a little too eager in the bounce, or for pushing a bounce past
    // what the direct light alone would give.
    float GiContribution = 1.0f;

    // Physical source radius in world units; non-zero gives area-light soft shadows.
    float SourceRadius = 0.0f;

    // Attenuation falloff exponent. 2 = physically correct inverse-square,
    // lower values give softer/more stylised falloff.
    float FalloffExponent = 2.0f;

    // When non-zero, overrides the RGB color with a blackbody temperature (Kelvin).
    // Set UseTemperature = false to use the RGB color directly.
    bool UseTemperature = false;
    float TemperatureKelvin = 6500.0f;

    // --- Spot light (LightType::Spot) --------------------------------------
    // Full cone angles in degrees, not half-angles: the inner cone is the
    // hotspot that receives the light's full intensity, and the falloff runs
    // from there out to the outer cone. Equal values give a hard-edged cone.
    float SpotInnerConeDegrees = 25.0f;
    float SpotOuterConeDegrees = 40.0f;

    // --- Rect light (LightType::Rect) --------------------------------------
    // Size of the emitting rectangle in metres, lying in the entity's local XY
    // plane and emitting along local -Z. This is the shape a window, a strip
    // light or a softbox actually is, and it is why a rect light wraps around
    // a surface instead of pinching to a highlight the way a point light does.
    float RectWidth = 1.0f;
    float RectHeight = 1.0f;

    // Emit from both faces. Off is correct for a window or a panel mounted on a
    // wall; on suits a floating strip that should light in both directions.
    bool RectTwoSided = false;

    // --- Light style -------------------------------------------------------
    // An animated brightness curve applied on top of IntensityLumens.  The
    // multiplier reaches every consumer of this light - deferred shading, RTGI,
    // volumetric fog - because it is folded into the light colour once, at the
    // point the per-frame GPU light array is built.  See LightStyles.h.
    LightStyleId Style = LightStyleId::None;

    // Pattern steps per second for the stepped styles, or the noise rate for
    // Fire and Torch.  Defaults to the style's own natural rate when a style is
    // first chosen in the editor.
    float StyleSpeed = 10.0f;

    // 0 leaves the light constant, 1 applies the style in full.  Above 1
    // exaggerates it, which is occasionally what a hero fire wants.
    float StyleAmplitude = 1.0f;

    // Seconds of offset into the curve.  Two torches sharing a style and speed
    // flicker in lockstep unless they are given different phases.
    float StylePhaseOffset = 0.0f;

    // Only used by LightStyleId::Custom.  Letters 'a' (black) to 'z', where
    // 'm' is the light's authored brightness.
    std::string CustomStylePattern = "mmnmmommommnonmmonqnmmo";
};

struct AudioEmitterComponent
{
    std::string EventPath;
    bool AutoPlay = true;
    int RuntimeEmitterHandle = -1;
    bool RuntimeAutoPlayStarted = false;
    std::string RuntimeRegisteredEventPath;
};

// DecalComponent: projects a material onto surfaces within an oriented box volume.
// The decal faces -Z in local space; rotate the entity to control projection direction.
struct DecalComponent
{
    // Relative path (from Data/) of the .json material assigned to this decal.
    // The material must have "isDecalMaterial": true to be eligible.
    std::string MaterialPath;

    // Half-extents of the projection box in world units along each local axis.
    float SizeX = 1.0f;
    float SizeY = 1.0f;
    float SizeZ = 1.0f;
};

// RainComponent: places a camera-relative rain particle simulation at this entity's location.
// All fields here are artist-facing and saved directly to the level file.
struct RainComponent
{
    // Physics
    float WindX           = 0.5f;
    float WindY           = 0.0f;
    float WindZ           = 0.2f;
    float Gravity         = 9.8f;

    // Bounding box half-extents (XYZ) around the camera.
    float BoxExtentX      = 20.0f;
    float BoxExtentY      = 15.0f;
    float BoxExtentZ      = 20.0f;

    // Visual
    float Intensity       = 1.0f;
    float StreakLength     = 0.18f;
    float ColorR           = 0.65f;
    float ColorG           = 0.75f;
    float ColorB           = 0.85f;
    float ColorA           = 0.35f;
    float WetnessIntensity = 0.7f;

    bool  Enabled          = true;
};

// Where new particles are born inside the emitter's local space.  The emitter
// faces +Z (the world up axis), so a Cone emitter with no rotation throws
// particles upward - which is what a fire wants with no setup at all.
enum class ParticleEmitterShape : int
{
    Point  = 0,
    Sphere = 1,   // filled sphere of ShapeRadius
    Box    = 2,   // filled box of ShapeExtents
    Cone   = 3,   // disc of ShapeRadius spraying into ConeAngleDegrees
    Disc   = 4,   // ring/disc of ShapeRadius in the local XY plane
    Edge   = 5,   // line along local X of length 2 * ShapeRadius
};

// How a particle quad is oriented.  Fire and smoke want Billboard; sparks and
// embers read much better stretched along their velocity.
enum class ParticleFacingMode : int
{
    Billboard          = 0,   // always faces the camera
    VelocityStretched  = 1,   // faces the camera, stretched along velocity
    Horizontal         = 2,   // lies flat in the world XY plane
    Vertical           = 3,   // upright, yawing to face the camera
};

// A single particle system placed in the level.  The simulation itself runs on
// the GPU (see ParticleRenderer / Particle_Update.hlsl); everything here is the
// artist-facing description of it and is saved straight into the level file.
//
// The *look* - texture, blend mode, emissive colour, soft-particle depth fade -
// lives in the assigned particle material, so several emitters can share one
// authored flame material and differ only in shape and rate.
struct ParticleSystemComponent
{
    bool Enabled = true;

    // Data-relative .json material.  Should have "isParticleMaterial": true;
    // its base colour texture is the sprite (or flipbook atlas) and its
    // emissive colour drives both the on-screen glow and the GI contribution.
    std::string MaterialPath = "Materials/Fire.json";

    // --- Emission ----------------------------------------------------------
    // Particles spawned per second.  The buffer is sized from
    // SpawnRate * (Lifetime * (1 + LifetimeVariance)), capped at MaxParticles,
    // so raising the rate does not silently start recycling live particles.
    float SpawnRate = 120.0f;

    // Seconds a particle lives, and the +/- fraction of random variation.
    float Lifetime = 1.6f;
    float LifetimeVariance = 0.35f;

    // Hard ceiling on simultaneous particles for this emitter.
    int MaxParticles = 4096;

    // Fill the system with mid-life particles on the first frame rather than
    // letting it build up from empty.  A fire that fades in over two seconds
    // every time the level loads looks like a bug.
    bool Prewarm = true;

    // Emit continuously, or emit BurstCount particles once per BurstInterval.
    bool  Burst = false;
    int   BurstCount = 32;
    float BurstInterval = 1.0f;

    // --- Shape -------------------------------------------------------------
    ParticleEmitterShape Shape = ParticleEmitterShape::Cone;
    float ShapeRadius = 0.28f;
    float ConeAngleDegrees = 18.0f;
    DirectX::XMFLOAT3 ShapeExtents{ 0.5f, 0.5f, 0.5f };

    // Bias spawn positions toward the shell of the shape.  0 fills the volume,
    // 1 puts every particle on the surface.  A ring of flame around a log is a
    // Disc at 1; a body of fire is a Cone at 0.
    float ShapeShellBias = 0.0f;

    // --- Motion ------------------------------------------------------------
    float InitialSpeed = 1.35f;
    float SpeedVariance = 0.45f;

    // Constant world-space acceleration.  Fire wants positive Z (hot gas
    // rising); smoke wants a little less; debris wants -9.8.
    DirectX::XMFLOAT3 Acceleration{ 0.0f, 0.0f, 2.4f };

    // Velocity damping per second, as a fraction retained.  Higher drag makes
    // particles settle into the flow instead of coasting.
    float Drag = 0.6f;

    // How strongly the scene-wide wind pushes this system, 0 = immune.
    float WindInfluence = 0.35f;

    // Curl-style turbulence.  Strength is in metres per second squared;
    // Frequency is in cycles per metre; Speed scrolls the field over time.
    // This is what turns a cone of sprites into something that licks and curls.
    float TurbulenceStrength = 1.15f;
    float TurbulenceFrequency = 0.85f;
    float TurbulenceSpeed = 0.55f;

    // Spin about the emitter's local Z axis, in radians per second at unit
    // radius.  A small value gives a flame a slow twist as it rises.
    float VortexStrength = 0.0f;

    // --- Size --------------------------------------------------------------
    // Metres.  Interpolated over the particle's life.
    float StartSize = 0.34f;
    float EndSize = 0.9f;
    float SizeVariance = 0.3f;

    // --- Rotation ----------------------------------------------------------
    float StartRotationDegrees = 0.0f;
    float RandomStartRotation = 1.0f;      // 0..1 fraction of a full turn
    float RotationSpeedDegrees = 25.0f;
    float RotationSpeedVariance = 1.0f;    // 0..1; 1 randomises the direction too

    // --- Colour over life --------------------------------------------------
    // Three keys are enough to describe a flame: the white-hot core, the body
    // colour, and what it fades to as it cools.  Alpha is the opacity curve.
    // These multiply the material's base colour and sprite texture.
    DirectX::XMFLOAT4 ColorStart{ 1.0f, 0.82f, 0.35f, 1.0f };
    DirectX::XMFLOAT4 ColorMid  { 1.0f, 0.38f, 0.08f, 0.85f };
    DirectX::XMFLOAT4 ColorEnd  { 0.25f, 0.06f, 0.02f, 0.0f };
    float ColorMidPoint = 0.35f;   // 0..1 position of ColorMid along the life

    // Multiplies the material's emissive colour before it is written to screen
    // and before it is turned into light for the GI.  Fire is emissive: this is
    // the main brightness control for the effect.
    float EmissiveIntensity = 4.0f;

    // --- Flipbook ----------------------------------------------------------
    // 1x1 means the sprite texture is used whole.  Anything larger treats the
    // base colour texture as an atlas read left-to-right, top-to-bottom.
    int FlipbookColumns = 1;
    int FlipbookRows = 1;
    // Frames per second.  0 spreads the whole flipbook across the particle's
    // lifetime instead, which is usually what a hand-authored flame atlas wants.
    float FlipbookFps = 0.0f;
    // Cross-fade between adjacent frames instead of cutting.  Costs a second
    // texture fetch and hides a low frame count.
    bool FlipbookBlendFrames = true;
    // Start each particle on a random frame so a cluster does not animate in
    // unison.  Only meaningful when FlipbookFps > 0.
    bool FlipbookRandomStartFrame = true;

    // --- Rendering ---------------------------------------------------------
    ParticleFacingMode Facing = ParticleFacingMode::Billboard;

    // Metres of stretch per metre/second of speed, for VelocityStretched.
    float StretchFactor = 0.12f;

    // Fade the sprite out as it approaches scene geometry, so a flame meets the
    // floor in a soft gradient instead of a hard intersection line.  The
    // distance is in metres of view-space depth.
    bool  SoftParticles = true;
    float SoftFadeDistance = 0.35f;

    // Metres past which the system stops drawing (but keeps simulating for one
    // more second, so walking back toward it does not reveal an empty emitter).
    float CullDistance = 120.0f;

    // --- Light and GI contribution -----------------------------------------
    // A particle system is not in the ray tracing acceleration structure, so
    // RTGI cannot see the sprites themselves.  Instead the system registers an
    // analytic light standing in for the emissive volume, which then feeds the
    // deferred shading, the RTGI point-light set and the volumetric fog in one
    // go.  For a fire that is the physically meaningful contribution anyway:
    // the flame lights the room far more than it is lit by it.
    bool EmitLight = true;

    // Luminous flux of the proxy light, in lumens.
    float LightIntensityLumens = 1400.0f;

    // Influence radius in metres.
    float LightRadius = 7.0f;

    // Take the proxy light's colour from the particle colour gradient (the
    // usual choice - the light then follows any recolouring of the fire) or
    // use LightColor* directly.
    bool  UseParticleColorForLight = true;
    float LightColorR = 1.0f;
    float LightColorG = 0.55f;
    float LightColorB = 0.18f;

    // Metres above the emitter origin to place the proxy light.  A fire's
    // apparent light source sits inside the flame, not at its base.
    float LightHeightOffset = 0.45f;

    // Extra multiplier on how much this system contributes to indirect light
    // only.  1 keeps the bounce consistent with the direct contribution;
    // lowering it tames a fire that is washing out a small room's GI.
    float GiContribution = 1.0f;

    // Let the proxy light cast ray-traced shadows.  Worth it for a hero fire,
    // wasteful for a row of candles.
    bool LightCastShadows = false;

    // Feed the proxy light into the volumetric fog, giving the fire a visible
    // glow in smoke or mist.
    bool LightAffectVolumetricFog = true;

    // Animated brightness curve for the proxy light.  Defaults to Fire, which
    // is the whole point: the room's lighting and its GI flicker together with
    // the flame instead of sitting at a dead constant.
    LightStyleId LightStyle = LightStyleId::Fire;
    float LightStyleSpeed = 1.0f;
    float LightStyleAmplitude = 0.85f;
    float LightStylePhaseOffset = 0.0f;
    std::string LightCustomStylePattern = "mmnmmommommnonmmonqnmmo";

    // Apply the same style curve to the sprites' emissive brightness, so the
    // flame itself brightens and dims with the light it casts.
    bool StyleDrivesParticleEmissive = true;
};

// Hard ceiling on particles across all systems in a level, so a mistyped rate
// on one emitter cannot exhaust VRAM.
inline constexpr int kParticleMaxPerSystem = 65536;
inline constexpr int kParticleMaxSystems   = 16;

// TerrainComponent: a heightmap-driven patch of ground that lives at the
// entity's transform.  The heightmap data itself is a single-channel 16-bit
// unsigned grid stored in the matching `HeightmapRawPath` .raw file (little
// endian, width*height*2 bytes).  The renderer writes a sibling .dds
// (`HeightmapDdsPath`) the first time the terrain is built so the rest of the
// engine can GPU-load it through the standard TextureManager.
// One paintable terrain surface layer (CryEngine-style).  Each layer is a
// tiled diffuse texture blended against the others by the per-sample splat
// weights baked into the terrain mesh's vertex colour.  A terrain supports up
// to kTerrainMaxLayers (4) simultaneous layers so the four weights fit in the
// RGBA vertex-colour channel.
struct TerrainPaintLayer
{
    // Data-relative .dds diffuse texture for this layer.  Empty = solid tint.
    std::string DiffuseTexturePath;

    // How many times the texture repeats across the whole patch.  Higher =
    // finer detail.  Tuned per layer so grass and rock can tile differently.
    float TileScale = 16.0f;

    // Multiplied with the sampled texture (also acts as the colour when no
    // texture is assigned).
    float TintR = 1.0f;
    float TintG = 1.0f;
    float TintB = 1.0f;
    float TintA = 1.0f;
};

// Hard cap on simultaneous paint layers (weights are packed into the RGBA
// vertex-colour channel, so four is the natural maximum).
inline constexpr int kTerrainMaxLayers = 4;

struct TerrainComponent
{
    // The original .raw heightmap (16-bit LE, 1 channel, 0..65535).  Relative
    // to Data/ so the level file stays portable.
    std::string HeightmapRawPath;

    // Cached/derived DDS path (R16_UNORM) written by the importer.  May be
    // empty until the first import runs.
    std::string HeightmapDdsPath;

    // Optional Data-relative material JSON used for the terrain surface.
    // The terrain renderer currently consumes baseColor, baseColorTint,
    // metallicFactor, roughnessFactor, and ambientOcclusionStrength.
    std::string MaterialPath;

    // Heightmap grid resolution (samples, not metres).
    int Width  = 0;
    int Height = 0;

    // World-space size of the patch in metres (square: X and Y are equal).
    float WorldSize = 1024.0f;

    // Vertical scale applied to the normalised 0..1 heightmap (metres).
    float HeightScale = 256.0f;

    // Vertical offset added after the scale (useful for placing a terrain
    // patch at a non-zero elevation).
    float HeightOffset = 0.0f;

    // Convenience: editor-side brush state.  These fields are saved with the
    // level so the artist picks up exactly where they left off.
    enum class BrushType : int
    {
        Raise  = 0,
        Lower  = 1,
        Flatten= 2,
        Smooth = 3,
        Paint  = 4,   // paint the active material layer (does not edit height)
    };
    BrushType Brush = BrushType::Raise;
    float BrushRadius  = 8.0f;     // world units
    float BrushStrength= 0.5f;     // metres per second of brush (raise/lower)
    float FlattenHeight= 0.0f;     // target world-space height for Flatten
    int   BrushSmoothingPasses = 1;// how many neighbour-avg iterations Smooth runs

    // --- Material painting ---------------------------------------------------
    // Up to kTerrainMaxLayers surface layers blended by a per-sample splat map.
    std::vector<TerrainPaintLayer> PaintLayers;

    // Which layer the Paint brush currently writes into.
    int ActivePaintLayer = 0;

    // Sibling file (Data-relative) holding the RGBA8 per-sample layer weights.
    // Derived from the heightmap path the first time the terrain is painted.
    std::string SplatMapPath;
};

// WaterComponent: a flat, animated water surface (ocean / lake / pond) that
// lives on the entity's XY plane at the entity's Z height.  The renderer
// generates a tessellated grid of SizeX x SizeY world units, displaces it with
// Gerstner + sine waves and a height map in the vertex shader, and shades it in
// a forward pass after lighting (Data/Shaders/Water.hlsl, a port of
// tuxalin/water-shader).
//
// The *area* (SizeX/SizeY) and tessellation (Resolution) are edited on the
// component; the *look* (colours, waves, reflection, foam, textures) comes from
// the assigned water material JSON (see Data/Materials/Ocean.json and Water.json).
struct WaterComponent
{
    // World-space size of the water area, in metres, along the entity's local
    // X and Y axes.  The surface is centred on the entity's position.
    float SizeX = 200.0f;
    float SizeY = 200.0f;

    // Grid tessellation (vertices per axis).  Higher = smoother waves at the
    // cost of more triangles.  Clamped to [2, 512] by the renderer.
    int Resolution = 384;

    // Data-relative .json water material ("water" block).  Empty = the
    // shader's built-in defaults.
    std::string MaterialPath = "Materials/Ocean.json";

    // Multiplier on the material's wave amplitude factor, so the same material
    // can be made calmer or rougher per placement without authoring a new one.
    float WaveScale = 1.0f;

    // Diagnostic view (see Water.hlsl WATER_DEBUG_*): 0 = off, 1 = normals,
    // 2 = Fresnel, 3 = water depth, 4 = refraction, 5 = reflection,
    // 6 = specular, 7 = foam.
    int DebugMode = 0;
};

// How a vegetation layer reacts to wind and to the runtime interaction map.
// Trees get the full hierarchical model (trunk sway + branch sway + leaf
// flutter) but ignore interactors, because a player brushing past a trunk
// should not move it.  Grass gets a cheap single-hinge blade bend that also
// reads the interaction map, which is where trampling and trails come from.
enum class VegetationBendModel : int
{
    None  = 0,
    Tree  = 1,
    Grass = 2,
};

// One scatter rule inside a vegetation area: an asset plus everything that
// decides where copies of it may land and how they move.  A single area
// usually holds several of these (canopy trees, undergrowth, grass) so one
// volume can describe a whole biome.
struct VegetationLayer
{
    std::string Name = "Layer";

    // Relative path (from Data/) of the imported .fbx used for this layer.
    std::string MeshPath;
    // Relative path (from Data/) of the assigned .json material.  Foliage
    // materials should set "alphaCutoff" so the leaf cards clip correctly.
    std::string MaterialPath;

    bool Enabled = true;

    // --- Density ----------------------------------------------------------
    // Instances per square metre of the area's projected (XY) footprint.  The
    // scatter grid cell is 1/sqrt(Density) across, so resizing the area keeps
    // the same visual density instead of rescaling the whole distribution.
    float Density = 0.5f;

    // --- Per-instance variation -------------------------------------------
    float MinScale = 0.8f;
    float MaxScale = 1.2f;

    // Random yaw about world up (Z).  Almost always wanted; disable only for
    // assets that must face a fixed direction.
    bool  RandomYaw = true;

    // Extra random tilt away from the resolved up axis, in degrees.  A few
    // degrees breaks up the "planted in a grid" look on flat ground.
    float MaxTiltDegrees = 0.0f;

    // --- Surface filtering -------------------------------------------------
    // 0 keeps instances upright along world +Z, 1 aligns them fully to the
    // surface normal.  Trees want 0 (they grow up, not out of a slope), grass
    // around 0.3, rocks and debris 1.
    float AlignToNormal = 0.0f;

    // Reject any surface steeper than this, measured in degrees from
    // horizontal.  This is what keeps trees off cliff faces.
    float MaxSlopeDegrees = 40.0f;

    // World-space Z band the layer is allowed to occupy, in metres.  Useful
    // for tree lines and shorelines.
    float MinAltitude = -100000.0f;
    float MaxAltitude =  100000.0f;

    // Push instances this far into the surface along the hit normal so they
    // never appear to float over uneven ground.
    float SinkOffset = 0.02f;

    // --- Terrain paint-layer mask ------------------------------------------
    // When >= 0, only spawn where the terrain's splat weight for that layer
    // exceeds TerrainLayerThreshold.  Indices match TerrainComponent::
    // PaintLayers (0..kTerrainMaxLayers-1).  -1 disables the mask.  This is
    // what makes grass follow the painted grass layer for free.
    int   TerrainLayerMask = -1;
    float TerrainLayerThreshold = 0.5f;

    // --- Spacing -----------------------------------------------------------
    // Discard an instance if another instance of the same layer already sits
    // within this radius (metres).  0 disables self-thinning.
    float CollisionRadius = 0.0f;

    // --- Rendering ---------------------------------------------------------
    // Distance in metres past which the layer stops drawing entirely.
    float CullDistance = 150.0f;

    // Fraction of CullDistance over which instances fade out, 0..1.  Fading
    // rather than popping matters a lot for grass.
    float FadeFraction = 0.15f;

    // Data-relative .dds card drawn instead of the mesh at the farthest LOD.
    // Empty means the layer simply culls at CullDistance with no billboard
    // stage, which is the right choice for grass -- a billboard of a grass
    // tuft costs the same as the tuft.
    //
    // The texture is authored per layer today; a generated imposter atlas
    // would fill this same slot without changing anything downstream.
    std::string BillboardTexturePath;

    // Multiplies the billboard card's size, which is otherwise derived from
    // the mesh's own bounds so it matches the geometry it replaces.
    float BillboardScale = 1.0f;

    bool  CastShadows = true;

    // Feed this layer's LOD0 instances to the ray tracing acceleration
    // structure.  Alpha-tested foliage is expensive to trace, so this should
    // stay off for grass and on only for hero trees.
    bool  ContributeToRayTracing = false;

    // --- Wind and interaction ----------------------------------------------
    VegetationBendModel BendModel = VegetationBendModel::Tree;

    // Overall responsiveness to the scene wind.  0 = rigid.
    float WindInfluence = 1.0f;

    // Higher values resist the main bend, so a mature trunk moves less than a
    // sapling driven by the same wind.
    float Stiffness = 1.0f;

    // Amplitude of the high-frequency leaf flutter term.
    float FlutterAmount = 1.0f;

    // How strongly this layer reacts to the runtime interaction map.  Only
    // meaningful for BendModel::Grass.
    float InteractionInfluence = 1.0f;
};

// Shape of the volume an artist places to describe where vegetation goes.
enum class VegetationAreaShape : int
{
    Box     = 0,
    Sphere  = 1,
    Polygon = 2,   // local XY footprint extruded along Z
};

// A single artist edit layered on top of the procedural scatter.  Storing
// these sparsely is what lets the level file keep rules rather than a baked
// list of transforms while still allowing "delete that one tree clipping the
// house".  InstanceId is the stable hash of (layer, cell) the scatter assigns,
// so an override survives a density change as long as that cell still exists.
struct VegetationInstanceOverride
{
    std::uint64_t InstanceId = 0;

    // Suppress this instance entirely.
    bool Removed = false;

    // When set, Position/RotationZ/Scale replace the generated transform.
    bool HasTransform = false;
    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
    float RotationZ = 0.0f;
    float Scale     = 1.0f;
};

// Upper bound on layers per area, and a hard ceiling on generated instances so
// a mistyped density cannot allocate an unbounded buffer.
inline constexpr int kVegetationMaxLayers    = 8;
inline constexpr int kVegetationMaxInstances = 500000;

// VegetationAreaComponent: a volume that procedurally populates itself with
// vegetation instead of being painted instance-by-instance with a brush.  The
// artist edits the volume and the per-layer rules; the scatter is regenerated
// deterministically from Seed, so the level file stays small and the result is
// reproducible.  Instances snap down onto terrain and/or static geometry.
struct VegetationAreaComponent
{
    VegetationAreaShape Shape = VegetationAreaShape::Box;

    // Half-extents in metres for Box and the radius for Sphere (ExtentX).
    // For Polygon, ExtentZ is the vertical half-height of the extrusion.
    float ExtentX = 32.0f;
    float ExtentY = 32.0f;
    float ExtentZ = 32.0f;

    // Footprint for Shape::Polygon, in the entity's local XY plane, metres.
    // Wound either way; containment uses an even-odd crossing test.
    std::vector<DirectX::XMFLOAT2> PolygonPoints;

    std::vector<VegetationLayer> Layers;

    // Changing this reshuffles the entire distribution without touching any
    // other rule — the fastest way to escape an arrangement that looks wrong.
    std::uint32_t Seed = 1337u;

    // Which surfaces instances are allowed to land on.  With both enabled the
    // scatter takes whichever hit is highest under the candidate point.
    bool SnapToTerrain  = true;
    bool SnapToGeometry = true;

    // How far down from the top of the volume to search for a surface, in
    // metres.  Candidates that find nothing within this distance are dropped.
    float SnapMaxDistance = 500.0f;

    // When true this volume removes vegetation rather than adding it, so it
    // can carve roads, clearings and building footprints out of other areas.
    bool IsExclusionVolume = false;

    // Sparse artist edits applied after the procedural pass.
    std::vector<VegetationInstanceOverride> Overrides;

    // --- Editor-only state, saved so the artist resumes where they left off --
    bool ShowBounds  = true;
    int  ActiveLayer = 0;
};

struct Entity
{
    // Persistent identity, saved with the level. Names repeat and list positions shift
    // under editing, so this is what a node graph's entity reference holds. 0 means "not
    // assigned yet"; EnsureEntityIds (EntityIds.h) fills those in and splits duplicates.
    std::uint64_t Id = 0;
    std::string Name = "Entity";
    TransformComponent Transform;
    std::optional<MeshComponent> Mesh;
    std::optional<PointLightComponent> PointLight;
    std::optional<AudioEmitterComponent> AudioEmitter;
    std::optional<DecalComponent> Decal;
    std::optional<RainComponent> Rain;
    std::optional<ParticleSystemComponent> ParticleSystem;
    std::optional<TerrainComponent> Terrain;
    std::optional<WaterComponent> Water;
    std::optional<VegetationAreaComponent> VegetationArea;

    bool HasMeshComponent() const
    {
        return Mesh.has_value();
    }

    MeshComponent& AddMeshComponent()
    {
        if (!Mesh.has_value())
        {
            Mesh.emplace();
        }

        return *Mesh;
    }

    bool HasNameComponent() const
    {
        return !Name.empty();
    }

    NameComponent GetNameComponent() const
    {
        return NameComponent{ Name };
    }

    void AddNameComponent(const NameComponent& nameComponent)
    {
        // Mirror the serialized NameComponent back into the editor-facing entity name used by the current tools.
        Name = nameComponent.Name;
    }

    bool HasTransformComponent() const
    {
        return true;
    }

    const TransformComponent& GetTransformComponent() const
    {
        return Transform;
    }

    TransformComponent& AddTransformComponent(const TransformComponent& transformComponent)
    {
        Transform = transformComponent;
        return Transform;
    }

    bool HasPointLightComponent() const
    {
        return PointLight.has_value();
    }

    PointLightComponent& AddPointLightComponent()
    {
        if (!PointLight.has_value())
        {
            PointLight.emplace();
        }

        return *PointLight;
    }

    bool HasAudioEmitterComponent() const
    {
        return AudioEmitter.has_value();
    }

    AudioEmitterComponent& AddAudioEmitterComponent()
    {
        if (!AudioEmitter.has_value())
        {
            AudioEmitter.emplace();
        }

        return *AudioEmitter;
    }

    bool HasDecalComponent() const
    {
        return Decal.has_value();
    }

    DecalComponent& AddDecalComponent()
    {
        if (!Decal.has_value())
        {
            Decal.emplace();
        }

        return *Decal;
    }

    bool HasRainComponent() const
    {
        return Rain.has_value();
    }

    RainComponent& AddRainComponent()
    {
        if (!Rain.has_value())
        {
            Rain.emplace();
        }

        return *Rain;
    }

    bool HasParticleSystemComponent() const
    {
        return ParticleSystem.has_value();
    }

    ParticleSystemComponent& AddParticleSystemComponent()
    {
        if (!ParticleSystem.has_value())
        {
            ParticleSystem.emplace();
        }

        return *ParticleSystem;
    }

    bool HasTerrainComponent() const
    {
        return Terrain.has_value();
    }

    TerrainComponent& AddTerrainComponent()
    {
        if (!Terrain.has_value())
        {
            Terrain.emplace();
        }

        return *Terrain;
    }

    bool HasWaterComponent() const
    {
        return Water.has_value();
    }

    WaterComponent& AddWaterComponent()
    {
        if (!Water.has_value())
        {
            Water.emplace();
        }

        return *Water;
    }

    bool HasVegetationAreaComponent() const
    {
        return VegetationArea.has_value();
    }

    VegetationAreaComponent& AddVegetationAreaComponent()
    {
        if (!VegetationArea.has_value())
        {
            VegetationArea.emplace();
        }

        return *VegetationArea;
    }
};

namespace DirectX
{
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(XMFLOAT2, x, y)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(XMFLOAT3, x, y, z)
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(XMFLOAT4, x, y, z, w)
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TransformComponent, Position, Rotation, Scale)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NameComponent, Name)

// MeshComponent serialization only persists the file paths; the runtime asset handle is transient.
inline void to_json(nlohmann::json& j, const MeshComponent& mc)
{
    j = nlohmann::json{
        { "MeshPath", mc.MeshPath },
        { "MaterialPath", mc.MaterialPath },
        { "LodUsageScale", mc.LodUsageScale },
        { "DebugForcedLod", mc.DebugForcedLod }
    };
}

inline void from_json(const nlohmann::json& j, MeshComponent& mc)
{
    mc.MeshPath    = j.value("MeshPath",    std::string{});
    mc.MaterialPath = j.value("MaterialPath", std::string{});
    mc.LodUsageScale = j.value("LodUsageScale", 1.0f);
    mc.DebugForcedLod = j.value("DebugForcedLod", -1);
}

// PointLightComponent – all fields are plain scalars so a single macro handles both directions.
inline void to_json(nlohmann::json& j, const PointLightComponent& pl)
{
    j = nlohmann::json{
        { "Type",              static_cast<int>(pl.Type) },
        { "SpotInnerConeDegrees", pl.SpotInnerConeDegrees },
        { "SpotOuterConeDegrees", pl.SpotOuterConeDegrees },
        { "RectWidth",         pl.RectWidth         },
        { "RectHeight",        pl.RectHeight        },
        { "RectTwoSided",      pl.RectTwoSided      },
        { "IntensityLumens",   pl.IntensityLumens   },
        { "Radius",            pl.Radius            },
        { "ColorR",            pl.ColorR            },
        { "ColorG",            pl.ColorG            },
        { "ColorB",            pl.ColorB            },
        { "CastShadows",       pl.CastShadows       },
        { "AffectVolumetricFog", pl.AffectVolumetricFog },
        { "AffectGlobalIllumination", pl.AffectGlobalIllumination },
        { "GiContribution",    pl.GiContribution    },
        { "SourceRadius",      pl.SourceRadius      },
        { "FalloffExponent",   pl.FalloffExponent   },
        { "UseTemperature",    pl.UseTemperature    },
        { "TemperatureKelvin", pl.TemperatureKelvin },
        { "Style",             static_cast<int>(pl.Style) },
        { "StyleSpeed",        pl.StyleSpeed        },
        { "StyleAmplitude",    pl.StyleAmplitude    },
        { "StylePhaseOffset",  pl.StylePhaseOffset  },
        { "CustomStylePattern", pl.CustomStylePattern }
    };
}

inline void from_json(const nlohmann::json& j, PointLightComponent& pl)
{
    // Levels authored before spot and rect lights existed have no Type, and
    // default to the point light they were saved as.
    const int lightTypeIndex = j.value("Type", static_cast<int>(LightType::Point));
    pl.Type = (lightTypeIndex >= static_cast<int>(LightType::Point) &&
               lightTypeIndex <= static_cast<int>(LightType::Rect))
        ? static_cast<LightType>(lightTypeIndex)
        : LightType::Point;

    pl.SpotInnerConeDegrees = j.value("SpotInnerConeDegrees", 25.0f);
    pl.SpotOuterConeDegrees = j.value("SpotOuterConeDegrees", 40.0f);
    pl.RectWidth         = j.value("RectWidth",         1.0f);
    pl.RectHeight        = j.value("RectHeight",        1.0f);
    pl.RectTwoSided      = j.value("RectTwoSided",      false);

    pl.IntensityLumens   = j.value("IntensityLumens",   800.0f);
    pl.Radius            = j.value("Radius",            5.0f);
    pl.ColorR            = j.value("ColorR",            1.0f);
    pl.ColorG            = j.value("ColorG",            1.0f);
    pl.ColorB            = j.value("ColorB",            1.0f);
    pl.CastShadows       = j.value("CastShadows",       true);
    pl.AffectVolumetricFog = j.value("AffectVolumetricFog", true);
    pl.AffectGlobalIllumination = j.value("AffectGlobalIllumination", true);
    pl.GiContribution    = j.value("GiContribution",    1.0f);
    pl.SourceRadius      = j.value("SourceRadius",      0.0f);
    pl.FalloffExponent   = j.value("FalloffExponent",   2.0f);
    pl.UseTemperature    = j.value("UseTemperature",    false);
    pl.TemperatureKelvin = j.value("TemperatureKelvin", 6500.0f);

    const int styleIndex = j.value("Style", static_cast<int>(LightStyleId::None));
    pl.Style = (styleIndex >= 0 && styleIndex < kLightStyleCount)
        ? static_cast<LightStyleId>(styleIndex)
        : LightStyleId::None;
    pl.StyleSpeed        = j.value("StyleSpeed",        10.0f);
    pl.StyleAmplitude    = j.value("StyleAmplitude",    1.0f);
    pl.StylePhaseOffset  = j.value("StylePhaseOffset",  0.0f);
    pl.CustomStylePattern = j.value("CustomStylePattern", std::string{ "mmnmmommommnonmmonqnmmo" });

    if (pl.StyleSpeed < 0.0f)     pl.StyleSpeed = 0.0f;
    if (pl.StyleAmplitude < 0.0f) pl.StyleAmplitude = 0.0f;

    // The cone falloff divides by (inner - outer), so an inner cone wider than
    // the outer one would invert the gradient rather than simply looking wrong.
    pl.SpotOuterConeDegrees = std::clamp(pl.SpotOuterConeDegrees, 0.1f, 179.0f);
    pl.SpotInnerConeDegrees = std::clamp(pl.SpotInnerConeDegrees, 0.0f, pl.SpotOuterConeDegrees);
    if (pl.RectWidth  < 0.001f) pl.RectWidth  = 0.001f;
    if (pl.RectHeight < 0.001f) pl.RectHeight = 0.001f;
    if (pl.GiContribution < 0.0f) pl.GiContribution = 0.0f;
}

inline void to_json(nlohmann::json& j, const AudioEmitterComponent& ae)
{
    j = nlohmann::json{
        { "EventPath", ae.EventPath },
        { "AutoPlay", ae.AutoPlay }
    };
}

inline void from_json(const nlohmann::json& j, AudioEmitterComponent& ae)
{
    ae.EventPath = j.value("EventPath", std::string{});
    ae.AutoPlay = j.value("AutoPlay", true);
}

inline void to_json(nlohmann::json& j, const DecalComponent& dc)
{
    j = nlohmann::json{
        { "MaterialPath", dc.MaterialPath },
        { "SizeX",        dc.SizeX        },
        { "SizeY",        dc.SizeY        },
        { "SizeZ",        dc.SizeZ        }
    };
}

inline void from_json(const nlohmann::json& j, DecalComponent& dc)
{
    dc.MaterialPath = j.value("MaterialPath", std::string{});
    dc.SizeX        = j.value("SizeX",        1.0f);
    dc.SizeY        = j.value("SizeY",        1.0f);
    dc.SizeZ        = j.value("SizeZ",        1.0f);
}

inline void to_json(nlohmann::json& j, const RainComponent& rc)
{
    j = nlohmann::json{
        { "WindX",            rc.WindX            },
        { "WindY",            rc.WindY            },
        { "WindZ",            rc.WindZ            },
        { "Gravity",          rc.Gravity          },
        { "BoxExtentX",       rc.BoxExtentX       },
        { "BoxExtentY",       rc.BoxExtentY       },
        { "BoxExtentZ",       rc.BoxExtentZ       },
        { "Intensity",        rc.Intensity        },
        { "StreakLength",     rc.StreakLength      },
        { "ColorR",           rc.ColorR           },
        { "ColorG",           rc.ColorG           },
        { "ColorB",           rc.ColorB           },
        { "ColorA",           rc.ColorA           },
        { "WetnessIntensity", rc.WetnessIntensity },
        { "Enabled",          rc.Enabled          }
    };
}

inline void from_json(const nlohmann::json& j, RainComponent& rc)
{
    rc.WindX            = j.value("WindX",            0.5f);
    rc.WindY            = j.value("WindY",            0.0f);
    rc.WindZ            = j.value("WindZ",            0.2f);
    rc.Gravity          = j.value("Gravity",          9.8f);
    rc.BoxExtentX       = j.value("BoxExtentX",       20.0f);
    rc.BoxExtentY       = j.value("BoxExtentY",       15.0f);
    rc.BoxExtentZ       = j.value("BoxExtentZ",       20.0f);
    rc.Intensity        = j.value("Intensity",        1.0f);
    rc.StreakLength      = j.value("StreakLength",     0.18f);
    rc.ColorR           = j.value("ColorR",           0.65f);
    rc.ColorG           = j.value("ColorG",           0.75f);
    rc.ColorB           = j.value("ColorB",           0.85f);
    rc.ColorA           = j.value("ColorA",           0.35f);
    rc.WetnessIntensity = j.value("WetnessIntensity", 0.7f);
    rc.Enabled          = j.value("Enabled",          true);
}

inline void to_json(nlohmann::json& j, const ParticleSystemComponent& ps)
{
    j = nlohmann::json{
        { "Enabled",                  ps.Enabled                  },
        { "MaterialPath",             ps.MaterialPath             },
        { "SpawnRate",                ps.SpawnRate                },
        { "Lifetime",                 ps.Lifetime                 },
        { "LifetimeVariance",         ps.LifetimeVariance         },
        { "MaxParticles",             ps.MaxParticles             },
        { "Prewarm",                  ps.Prewarm                  },
        { "Burst",                    ps.Burst                    },
        { "BurstCount",               ps.BurstCount               },
        { "BurstInterval",            ps.BurstInterval            },
        { "Shape",                    static_cast<int>(ps.Shape)  },
        { "ShapeRadius",              ps.ShapeRadius              },
        { "ConeAngleDegrees",         ps.ConeAngleDegrees         },
        { "ShapeExtents",             ps.ShapeExtents             },
        { "ShapeShellBias",           ps.ShapeShellBias           },
        { "InitialSpeed",             ps.InitialSpeed             },
        { "SpeedVariance",            ps.SpeedVariance            },
        { "Acceleration",             ps.Acceleration             },
        { "Drag",                     ps.Drag                     },
        { "WindInfluence",            ps.WindInfluence            },
        { "TurbulenceStrength",       ps.TurbulenceStrength       },
        { "TurbulenceFrequency",      ps.TurbulenceFrequency      },
        { "TurbulenceSpeed",          ps.TurbulenceSpeed          },
        { "VortexStrength",           ps.VortexStrength           },
        { "StartSize",                ps.StartSize                },
        { "EndSize",                  ps.EndSize                  },
        { "SizeVariance",             ps.SizeVariance             },
        { "StartRotationDegrees",     ps.StartRotationDegrees     },
        { "RandomStartRotation",      ps.RandomStartRotation      },
        { "RotationSpeedDegrees",     ps.RotationSpeedDegrees     },
        { "RotationSpeedVariance",    ps.RotationSpeedVariance    },
        { "ColorStart",               ps.ColorStart               },
        { "ColorMid",                 ps.ColorMid                 },
        { "ColorEnd",                 ps.ColorEnd                 },
        { "ColorMidPoint",            ps.ColorMidPoint            },
        { "EmissiveIntensity",        ps.EmissiveIntensity        },
        { "FlipbookColumns",          ps.FlipbookColumns          },
        { "FlipbookRows",             ps.FlipbookRows             },
        { "FlipbookFps",              ps.FlipbookFps              },
        { "FlipbookBlendFrames",      ps.FlipbookBlendFrames      },
        { "FlipbookRandomStartFrame", ps.FlipbookRandomStartFrame },
        { "Facing",                   static_cast<int>(ps.Facing) },
        { "StretchFactor",            ps.StretchFactor            },
        { "SoftParticles",            ps.SoftParticles            },
        { "SoftFadeDistance",         ps.SoftFadeDistance         },
        { "CullDistance",             ps.CullDistance             },
        { "EmitLight",                ps.EmitLight                },
        { "LightIntensityLumens",     ps.LightIntensityLumens     },
        { "LightRadius",              ps.LightRadius              },
        { "UseParticleColorForLight", ps.UseParticleColorForLight },
        { "LightColorR",              ps.LightColorR              },
        { "LightColorG",              ps.LightColorG              },
        { "LightColorB",              ps.LightColorB              },
        { "LightHeightOffset",        ps.LightHeightOffset        },
        { "GiContribution",           ps.GiContribution           },
        { "LightCastShadows",         ps.LightCastShadows         },
        { "LightAffectVolumetricFog", ps.LightAffectVolumetricFog },
        { "LightStyle",               static_cast<int>(ps.LightStyle) },
        { "LightStyleSpeed",          ps.LightStyleSpeed          },
        { "LightStyleAmplitude",      ps.LightStyleAmplitude      },
        { "LightStylePhaseOffset",    ps.LightStylePhaseOffset    },
        { "LightCustomStylePattern",  ps.LightCustomStylePattern  },
        { "StyleDrivesParticleEmissive", ps.StyleDrivesParticleEmissive }
    };
}

inline void from_json(const nlohmann::json& j, ParticleSystemComponent& ps)
{
    ps.Enabled                  = j.value("Enabled",                  true);
    ps.MaterialPath             = j.value("MaterialPath",             std::string{ "Materials/Fire.json" });
    ps.SpawnRate                = j.value("SpawnRate",                120.0f);
    ps.Lifetime                 = j.value("Lifetime",                 1.6f);
    ps.LifetimeVariance         = j.value("LifetimeVariance",         0.35f);
    ps.MaxParticles             = j.value("MaxParticles",             4096);
    ps.Prewarm                  = j.value("Prewarm",                  true);
    ps.Burst                    = j.value("Burst",                    false);
    ps.BurstCount               = j.value("BurstCount",               32);
    ps.BurstInterval            = j.value("BurstInterval",            1.0f);
    ps.Shape                    = static_cast<ParticleEmitterShape>(
                                      j.value("Shape", static_cast<int>(ParticleEmitterShape::Cone)));
    ps.ShapeRadius              = j.value("ShapeRadius",              0.28f);
    ps.ConeAngleDegrees         = j.value("ConeAngleDegrees",         18.0f);
    ps.ShapeExtents             = j.value("ShapeExtents",             DirectX::XMFLOAT3{ 0.5f, 0.5f, 0.5f });
    ps.ShapeShellBias           = j.value("ShapeShellBias",           0.0f);
    ps.InitialSpeed             = j.value("InitialSpeed",             1.35f);
    ps.SpeedVariance            = j.value("SpeedVariance",            0.45f);
    ps.Acceleration             = j.value("Acceleration",             DirectX::XMFLOAT3{ 0.0f, 0.0f, 2.4f });
    ps.Drag                     = j.value("Drag",                     0.6f);
    ps.WindInfluence            = j.value("WindInfluence",            0.35f);
    ps.TurbulenceStrength       = j.value("TurbulenceStrength",       1.15f);
    ps.TurbulenceFrequency      = j.value("TurbulenceFrequency",      0.85f);
    ps.TurbulenceSpeed          = j.value("TurbulenceSpeed",          0.55f);
    ps.VortexStrength           = j.value("VortexStrength",           0.0f);
    ps.StartSize                = j.value("StartSize",                0.34f);
    ps.EndSize                  = j.value("EndSize",                  0.9f);
    ps.SizeVariance             = j.value("SizeVariance",             0.3f);
    ps.StartRotationDegrees     = j.value("StartRotationDegrees",     0.0f);
    ps.RandomStartRotation      = j.value("RandomStartRotation",      1.0f);
    ps.RotationSpeedDegrees     = j.value("RotationSpeedDegrees",     25.0f);
    ps.RotationSpeedVariance    = j.value("RotationSpeedVariance",    1.0f);
    ps.ColorStart               = j.value("ColorStart",               DirectX::XMFLOAT4{ 1.0f, 0.82f, 0.35f, 1.0f });
    ps.ColorMid                 = j.value("ColorMid",                 DirectX::XMFLOAT4{ 1.0f, 0.38f, 0.08f, 0.85f });
    ps.ColorEnd                 = j.value("ColorEnd",                 DirectX::XMFLOAT4{ 0.25f, 0.06f, 0.02f, 0.0f });
    ps.ColorMidPoint            = j.value("ColorMidPoint",            0.35f);
    ps.EmissiveIntensity        = j.value("EmissiveIntensity",        4.0f);
    ps.FlipbookColumns          = j.value("FlipbookColumns",          1);
    ps.FlipbookRows             = j.value("FlipbookRows",             1);
    ps.FlipbookFps              = j.value("FlipbookFps",              0.0f);
    ps.FlipbookBlendFrames      = j.value("FlipbookBlendFrames",      true);
    ps.FlipbookRandomStartFrame = j.value("FlipbookRandomStartFrame", true);
    ps.Facing                   = static_cast<ParticleFacingMode>(
                                      j.value("Facing", static_cast<int>(ParticleFacingMode::Billboard)));
    ps.StretchFactor            = j.value("StretchFactor",            0.12f);
    ps.SoftParticles            = j.value("SoftParticles",            true);
    ps.SoftFadeDistance         = j.value("SoftFadeDistance",         0.35f);
    ps.CullDistance             = j.value("CullDistance",             120.0f);
    ps.EmitLight                = j.value("EmitLight",                true);
    ps.LightIntensityLumens     = j.value("LightIntensityLumens",     1400.0f);
    ps.LightRadius              = j.value("LightRadius",              7.0f);
    ps.UseParticleColorForLight = j.value("UseParticleColorForLight", true);
    ps.LightColorR              = j.value("LightColorR",              1.0f);
    ps.LightColorG              = j.value("LightColorG",              0.55f);
    ps.LightColorB              = j.value("LightColorB",              0.18f);
    ps.LightHeightOffset        = j.value("LightHeightOffset",        0.45f);
    ps.GiContribution           = j.value("GiContribution",           1.0f);
    ps.LightCastShadows         = j.value("LightCastShadows",         false);
    ps.LightAffectVolumetricFog = j.value("LightAffectVolumetricFog", true);

    const int lightStyleIndex = j.value("LightStyle", static_cast<int>(LightStyleId::Fire));
    ps.LightStyle = (lightStyleIndex >= 0 && lightStyleIndex < kLightStyleCount)
        ? static_cast<LightStyleId>(lightStyleIndex)
        : LightStyleId::Fire;
    ps.LightStyleSpeed          = j.value("LightStyleSpeed",          1.0f);
    ps.LightStyleAmplitude      = j.value("LightStyleAmplitude",      0.85f);
    ps.LightStylePhaseOffset    = j.value("LightStylePhaseOffset",    0.0f);
    ps.LightCustomStylePattern  = j.value("LightCustomStylePattern",  std::string{ "mmnmmommommnonmmonqnmmo" });
    ps.StyleDrivesParticleEmissive = j.value("StyleDrivesParticleEmissive", true);

    // Clamp everything the simulation divides by or sizes a buffer from, so a
    // hand-edited level file cannot produce a degenerate or unbounded system.
    if (ps.SpawnRate < 0.0f)          ps.SpawnRate = 0.0f;
    if (ps.Lifetime < 0.01f)          ps.Lifetime = 0.01f;
    if (ps.LifetimeVariance < 0.0f)   ps.LifetimeVariance = 0.0f;
    if (ps.LifetimeVariance > 0.95f)  ps.LifetimeVariance = 0.95f;
    if (ps.MaxParticles < 1)          ps.MaxParticles = 1;
    if (ps.MaxParticles > kParticleMaxPerSystem) ps.MaxParticles = kParticleMaxPerSystem;
    if (ps.BurstCount < 0)            ps.BurstCount = 0;
    if (ps.BurstInterval < 0.01f)     ps.BurstInterval = 0.01f;
    if (ps.ShapeRadius < 0.0f)        ps.ShapeRadius = 0.0f;
    if (ps.ConeAngleDegrees < 0.0f)   ps.ConeAngleDegrees = 0.0f;
    if (ps.ConeAngleDegrees > 180.0f) ps.ConeAngleDegrees = 180.0f;
    if (ps.ShapeShellBias < 0.0f)     ps.ShapeShellBias = 0.0f;
    if (ps.ShapeShellBias > 1.0f)     ps.ShapeShellBias = 1.0f;
    if (ps.ColorMidPoint < 0.01f)     ps.ColorMidPoint = 0.01f;
    if (ps.ColorMidPoint > 0.99f)     ps.ColorMidPoint = 0.99f;
    if (ps.FlipbookColumns < 1)       ps.FlipbookColumns = 1;
    if (ps.FlipbookRows < 1)          ps.FlipbookRows = 1;
    if (ps.FlipbookFps < 0.0f)        ps.FlipbookFps = 0.0f;
    if (ps.SoftFadeDistance < 0.001f) ps.SoftFadeDistance = 0.001f;
    if (ps.CullDistance < 1.0f)       ps.CullDistance = 1.0f;
    if (ps.LightRadius < 0.001f)      ps.LightRadius = 0.001f;
    if (ps.LightStyleSpeed < 0.0f)    ps.LightStyleSpeed = 0.0f;
    if (ps.LightStyleAmplitude < 0.0f) ps.LightStyleAmplitude = 0.0f;
    if (ps.GiContribution < 0.0f)     ps.GiContribution = 0.0f;
    if (ps.StartSize < 0.0f)          ps.StartSize = 0.0f;
    if (ps.EndSize < 0.0f)            ps.EndSize = 0.0f;
}

inline void to_json(nlohmann::json& j, const TerrainPaintLayer& layer)
{
    j = nlohmann::json{
        { "DiffuseTexturePath", layer.DiffuseTexturePath },
        { "TileScale",          layer.TileScale          },
        { "TintR",              layer.TintR              },
        { "TintG",              layer.TintG              },
        { "TintB",              layer.TintB              },
        { "TintA",              layer.TintA              }
    };
}

inline void from_json(const nlohmann::json& j, TerrainPaintLayer& layer)
{
    layer.DiffuseTexturePath = j.value("DiffuseTexturePath", std::string{});
    layer.TileScale          = j.value("TileScale", 16.0f);
    layer.TintR              = j.value("TintR", 1.0f);
    layer.TintG              = j.value("TintG", 1.0f);
    layer.TintB              = j.value("TintB", 1.0f);
    layer.TintA              = j.value("TintA", 1.0f);
}

inline void to_json(nlohmann::json& j, const TerrainComponent& tc)
{
    j = nlohmann::json{
        { "HeightmapRawPath",        tc.HeightmapRawPath        },
        { "HeightmapDdsPath",        tc.HeightmapDdsPath        },
        { "MaterialPath",            tc.MaterialPath            },
        { "Width",                   tc.Width                   },
        { "Height",                  tc.Height                  },
        { "WorldSize",               tc.WorldSize               },
        { "HeightScale",             tc.HeightScale             },
        { "HeightOffset",            tc.HeightOffset            },
        { "BrushType",               static_cast<int>(tc.Brush) },
        { "BrushRadius",             tc.BrushRadius             },
        { "BrushStrength",           tc.BrushStrength           },
        { "FlattenHeight",           tc.FlattenHeight           },
        { "BrushSmoothingPasses",    tc.BrushSmoothingPasses    },
        { "PaintLayers",             tc.PaintLayers             },
        { "ActivePaintLayer",        tc.ActivePaintLayer        },
        { "SplatMapPath",            tc.SplatMapPath            }
    };
}

inline void from_json(const nlohmann::json& j, TerrainComponent& tc)
{
    tc.HeightmapRawPath     = j.value("HeightmapRawPath",  std::string{});
    tc.HeightmapDdsPath     = j.value("HeightmapDdsPath",  std::string{});
    tc.MaterialPath         = j.value("MaterialPath",      std::string{});
    tc.Width                = j.value("Width",             0);
    tc.Height               = j.value("Height",            0);
    tc.WorldSize            = j.value("WorldSize",         1024.0f);
    tc.HeightScale          = j.value("HeightScale",       256.0f);
    tc.HeightOffset         = j.value("HeightOffset",      0.0f);
    tc.Brush                = static_cast<TerrainComponent::BrushType>(
                                 j.value("BrushType", static_cast<int>(TerrainComponent::BrushType::Raise)));
    tc.BrushRadius          = j.value("BrushRadius",       8.0f);
    tc.BrushStrength        = j.value("BrushStrength",     0.5f);
    tc.FlattenHeight        = j.value("FlattenHeight",     0.0f);
    tc.BrushSmoothingPasses = j.value("BrushSmoothingPasses", 1);
    tc.PaintLayers          = j.value("PaintLayers", std::vector<TerrainPaintLayer>{});
    tc.ActivePaintLayer     = j.value("ActivePaintLayer", 0);
    tc.SplatMapPath         = j.value("SplatMapPath", std::string{});

    if (tc.PaintLayers.size() > static_cast<size_t>(kTerrainMaxLayers))
        tc.PaintLayers.resize(kTerrainMaxLayers);
    if (tc.ActivePaintLayer < 0)
        tc.ActivePaintLayer = 0;
}

inline void to_json(nlohmann::json& j, const WaterComponent& wc)
{
    j = nlohmann::json{
        { "SizeX",        wc.SizeX        },
        { "SizeY",        wc.SizeY        },
        { "Resolution",   wc.Resolution   },
        { "MaterialPath", wc.MaterialPath },
        { "WaveScale",    wc.WaveScale    },
        { "DebugMode",    wc.DebugMode    }
    };
}

inline void from_json(const nlohmann::json& j, WaterComponent& wc)
{
    wc.SizeX        = j.value("SizeX",        200.0f);
    wc.SizeY        = j.value("SizeY",        200.0f);
    wc.Resolution   = j.value("Resolution",   384);
    wc.MaterialPath = j.value("MaterialPath", std::string{ "Materials/Ocean.json" });
    wc.WaveScale    = j.value("WaveScale",    1.0f);
    wc.DebugMode    = j.value("DebugMode",    0);

    if (wc.Resolution < 2)   wc.Resolution = 2;
    if (wc.Resolution > 512) wc.Resolution = 512;
    if (wc.DebugMode < 0 || wc.DebugMode > 7) wc.DebugMode = 0;
}

inline void to_json(nlohmann::json& j, const VegetationLayer& vl)
{
    j = nlohmann::json{
        { "Name",                   vl.Name                            },
        { "MeshPath",               vl.MeshPath                        },
        { "MaterialPath",           vl.MaterialPath                    },
        { "Enabled",                vl.Enabled                         },
        { "Density",                vl.Density                         },
        { "MinScale",               vl.MinScale                        },
        { "MaxScale",               vl.MaxScale                        },
        { "RandomYaw",              vl.RandomYaw                       },
        { "MaxTiltDegrees",         vl.MaxTiltDegrees                  },
        { "AlignToNormal",          vl.AlignToNormal                   },
        { "MaxSlopeDegrees",        vl.MaxSlopeDegrees                 },
        { "MinAltitude",            vl.MinAltitude                     },
        { "MaxAltitude",            vl.MaxAltitude                     },
        { "SinkOffset",             vl.SinkOffset                      },
        { "TerrainLayerMask",       vl.TerrainLayerMask                },
        { "TerrainLayerThreshold",  vl.TerrainLayerThreshold           },
        { "CollisionRadius",        vl.CollisionRadius                 },
        { "CullDistance",           vl.CullDistance                    },
        { "FadeFraction",           vl.FadeFraction                    },
        { "BillboardTexturePath",   vl.BillboardTexturePath            },
        { "BillboardScale",         vl.BillboardScale                  },
        { "CastShadows",            vl.CastShadows                     },
        { "ContributeToRayTracing", vl.ContributeToRayTracing          },
        { "BendModel",              static_cast<int>(vl.BendModel)     },
        { "WindInfluence",          vl.WindInfluence                   },
        { "Stiffness",              vl.Stiffness                       },
        { "FlutterAmount",          vl.FlutterAmount                   },
        { "InteractionInfluence",   vl.InteractionInfluence            }
    };
}

inline void from_json(const nlohmann::json& j, VegetationLayer& vl)
{
    vl.Name                   = j.value("Name",                  std::string{ "Layer" });
    vl.MeshPath               = j.value("MeshPath",              std::string{});
    vl.MaterialPath           = j.value("MaterialPath",          std::string{});
    vl.Enabled                = j.value("Enabled",               true);
    vl.Density                = j.value("Density",               0.5f);
    vl.MinScale               = j.value("MinScale",              0.8f);
    vl.MaxScale               = j.value("MaxScale",              1.2f);
    vl.RandomYaw              = j.value("RandomYaw",             true);
    vl.MaxTiltDegrees         = j.value("MaxTiltDegrees",        0.0f);
    vl.AlignToNormal          = j.value("AlignToNormal",         0.0f);
    vl.MaxSlopeDegrees        = j.value("MaxSlopeDegrees",       40.0f);
    vl.MinAltitude            = j.value("MinAltitude",           -100000.0f);
    vl.MaxAltitude            = j.value("MaxAltitude",            100000.0f);
    vl.SinkOffset             = j.value("SinkOffset",            0.02f);
    vl.TerrainLayerMask       = j.value("TerrainLayerMask",      -1);
    vl.TerrainLayerThreshold  = j.value("TerrainLayerThreshold", 0.5f);
    vl.CollisionRadius        = j.value("CollisionRadius",       0.0f);
    vl.CullDistance           = j.value("CullDistance",          150.0f);
    vl.FadeFraction           = j.value("FadeFraction",          0.15f);
    vl.BillboardTexturePath   = j.value("BillboardTexturePath",  std::string{});
    vl.BillboardScale         = j.value("BillboardScale",        1.0f);
    vl.CastShadows            = j.value("CastShadows",           true);
    vl.ContributeToRayTracing = j.value("ContributeToRayTracing", false);
    vl.BendModel              = static_cast<VegetationBendModel>(
                                    j.value("BendModel", static_cast<int>(VegetationBendModel::Tree)));
    vl.WindInfluence          = j.value("WindInfluence",         1.0f);
    vl.Stiffness              = j.value("Stiffness",             1.0f);
    vl.FlutterAmount          = j.value("FlutterAmount",         1.0f);
    vl.InteractionInfluence   = j.value("InteractionInfluence",  1.0f);

    // Clamp the fields the scatter and cull passes divide by or index with, so
    // a hand-edited level file cannot produce a degenerate distribution.
    if (vl.Density < 0.0f)      vl.Density = 0.0f;
    if (vl.MinScale < 0.001f)   vl.MinScale = 0.001f;
    if (vl.MaxScale < vl.MinScale) vl.MaxScale = vl.MinScale;
    if (vl.CullDistance < 1.0f) vl.CullDistance = 1.0f;
    if (vl.FadeFraction < 0.0f) vl.FadeFraction = 0.0f;
    if (vl.FadeFraction > 1.0f) vl.FadeFraction = 1.0f;
    if (vl.TerrainLayerMask >= kTerrainMaxLayers) vl.TerrainLayerMask = kTerrainMaxLayers - 1;
    if (vl.TerrainLayerMask < 0) vl.TerrainLayerMask = -1;
}

inline void to_json(nlohmann::json& j, const VegetationInstanceOverride& ov)
{
    j = nlohmann::json{
        { "InstanceId",   ov.InstanceId   },
        { "Removed",      ov.Removed      },
        { "HasTransform", ov.HasTransform },
        { "Position",     ov.Position     },
        { "RotationZ",    ov.RotationZ    },
        { "Scale",        ov.Scale        }
    };
}

inline void from_json(const nlohmann::json& j, VegetationInstanceOverride& ov)
{
    ov.InstanceId   = j.value("InstanceId",   static_cast<std::uint64_t>(0));
    ov.Removed      = j.value("Removed",      false);
    ov.HasTransform = j.value("HasTransform", false);
    ov.Position     = j.value("Position",     DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f });
    ov.RotationZ    = j.value("RotationZ",    0.0f);
    ov.Scale        = j.value("Scale",        1.0f);
}

inline void to_json(nlohmann::json& j, const VegetationAreaComponent& va)
{
    j = nlohmann::json{
        { "Shape",             static_cast<int>(va.Shape) },
        { "ExtentX",           va.ExtentX                 },
        { "ExtentY",           va.ExtentY                 },
        { "ExtentZ",           va.ExtentZ                 },
        { "PolygonPoints",     va.PolygonPoints           },
        { "Layers",            va.Layers                  },
        { "Seed",              va.Seed                    },
        { "SnapToTerrain",     va.SnapToTerrain           },
        { "SnapToGeometry",    va.SnapToGeometry          },
        { "SnapMaxDistance",   va.SnapMaxDistance         },
        { "IsExclusionVolume", va.IsExclusionVolume       },
        { "Overrides",         va.Overrides               },
        { "ShowBounds",        va.ShowBounds              },
        { "ActiveLayer",       va.ActiveLayer             }
    };
}

inline void from_json(const nlohmann::json& j, VegetationAreaComponent& va)
{
    va.Shape             = static_cast<VegetationAreaShape>(
                               j.value("Shape", static_cast<int>(VegetationAreaShape::Box)));
    va.ExtentX           = j.value("ExtentX",           32.0f);
    va.ExtentY           = j.value("ExtentY",           32.0f);
    va.ExtentZ           = j.value("ExtentZ",           32.0f);
    va.PolygonPoints     = j.value("PolygonPoints",     std::vector<DirectX::XMFLOAT2>{});
    va.Layers            = j.value("Layers",            std::vector<VegetationLayer>{});
    va.Seed              = j.value("Seed",              1337u);
    va.SnapToTerrain     = j.value("SnapToTerrain",     true);
    va.SnapToGeometry    = j.value("SnapToGeometry",    true);
    va.SnapMaxDistance   = j.value("SnapMaxDistance",   500.0f);
    va.IsExclusionVolume = j.value("IsExclusionVolume", false);
    va.Overrides         = j.value("Overrides",         std::vector<VegetationInstanceOverride>{});
    va.ShowBounds        = j.value("ShowBounds",        true);
    va.ActiveLayer       = j.value("ActiveLayer",       0);

    if (va.Layers.size() > static_cast<size_t>(kVegetationMaxLayers))
        va.Layers.resize(kVegetationMaxLayers);
    if (va.ActiveLayer < 0)
        va.ActiveLayer = 0;
    if (va.ActiveLayer >= static_cast<int>(va.Layers.size()))
        va.ActiveLayer = va.Layers.empty() ? 0 : static_cast<int>(va.Layers.size()) - 1;

    // Half-extents feed straight into the scatter's cell count; a zero or
    // negative extent would produce an empty or inverted grid.
    if (va.ExtentX < 0.01f) va.ExtentX = 0.01f;
    if (va.ExtentY < 0.01f) va.ExtentY = 0.01f;
    if (va.ExtentZ < 0.01f) va.ExtentZ = 0.01f;
    if (va.SnapMaxDistance < 0.01f) va.SnapMaxDistance = 0.01f;
}
