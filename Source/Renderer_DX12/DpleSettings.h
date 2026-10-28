#pragma once

// DpleSettings
// Deterministic Photoreal Lighting Enhancer - every tunable DPLE has, edited in its own
// window (Windows > DPLE...) and persisted with the scene. Ported from the Unreal plugin of
// the same name; see DpleRenderer.h for what the passes do and where they sit in the frame.
//
// Distances are in world units (metres). The Unreal plugin authored them in centimetres;
// the defaults below are the same physical sizes.
//
// DPLE is not a GI solver and not a neural upscaler. It re-presents lighting the renderer
// has already resolved: it adds no new bounce light, and its ceiling is exactly the quality
// of its hand-tuned heuristics.

// Debug visualisations. The numbering matches the Unreal plugin's r.DPLE.DebugView, minus
// the views that do not exist here (UE's shading model ID became Ptero's material class).
enum class DpleDebugView : int
{
    None = 0,
    AmbientVisibility = 1,   // combined ambient visibility after material scaling and the cap
    ContactShadows = 2,      // contact shadow visibility on its own
    IndirectFraction = 3,    // estimated indirect share: red = indirect, green = direct
    MaterialClass = 4,       // grey default, red skin (SSS), green foliage, blue wet, black passthrough
    AORadii = 5,             // micro / contact / broad visibility in R / G / B
    GBufferMapping = 6,      // G-Buffer UV: both ramps must reach the screen edge without flattening
    SpecularShare = 7,       // how much of each pixel the micro-specular section may touch
    MicroSpecularGain = 8,   // green brightened, red darkened, flat grey = doing nothing
    Count
};

// Named starting points, so there is always something known-good to fall back to.
enum class DplePreset : int
{
    Subtle = 0,       // ~half strength; contact grounding only
    Balanced = 1,     // the documented starting values (= factory defaults)
    Strong = 2,       // deeper occlusion, detail enhancement on, full-resolution working buffers
    Performance = 3,  // Balanced look, bought back through resolution rather than strength
    Count
};

struct DpleSettings
{
    // Master switch. Off by default: turning it on changes the look of every surface, so
    // a level opts in rather than every existing level changing on load.
    bool Enabled = false;

    // AO, contact shadows, temporal and denoise run at output resolution divided by this
    // (1-4). The main performance lever. It divides the *output* resolution, so with an
    // upscaler at 50% the default of 2 already puts these passes at render resolution.
    int WorkingResolutionDivisor = 2;

    // ---- Multiscale ambient occlusion ------------------------------------------------
    bool EnableAmbientOcclusion = true;
    float MicroAORadius = 0.06f;        // pore/weave scale
    float MicroAOIntensity = 0.18f;
    float ContactAORadius = 0.25f;      // object contact scale
    float ContactAOIntensity = 0.30f;
    float BroadAORadius = 1.20f;        // room/alcove scale
    float BroadAOIntensity = 0.12f;
    // Hard ceiling on how much the three radii combined may darken the indirect term.
    // Without it three individually reasonable radii stack into crushed black creases.
    float MaxCombinedAO = 0.45f;
    // Below 1 softens contact darkening, above 1 deepens it.
    float AOPower = 1.0f;
    // Depth-proportional normal bias that keeps flat surfaces from self-occluding.
    float AOBias = 0.02f;

    // ---- Contact shadows (dominant light = the sun) ----------------------------------
    bool EnableContactShadows = true;
    float ContactShadowLength = 0.30f;
    int ContactShadowSteps = 10;
    // How thick a depth-buffer occluder is assumed to be. Too small and thin geometry
    // stops casting; too large and everything behind a silhouette gets a false shadow.
    float ContactShadowThickness = 0.02f;
    float ContactShadowIntensity = 0.75f;

    // ---- Material response -----------------------------------------------------------
    // Ptero's G-Buffer has no shading model ID. What it does carry: a subsurface profile
    // slot (skin), a foliage flag written by the vegetation shaders, and the glass repack.
    bool EnableMaterialResponse = true;
    float SkinAOScale = 0.45f;          // AO scale on surfaces with a subsurface profile
    float SkinWarmth = 0.25f;           // how far occluded skin shifts toward red
    float FoliageAOScale = 0.60f;       // contact/broad AO scale on vegetation leaves
    float FoliageSaturation = 0.15f;    // saturation lift in occluded foliage
    // Wet surfaces have no tag of their own, so they are inferred: low roughness on a
    // non-metal. A polished marble floor reads as wet to it. 0 disables the inference.
    float WetRoughnessThreshold = 0.18f;
    float WetResponseStrength = 0.20f;

    // ---- Micro-specular response -----------------------------------------------------
    // The only section that touches the specular half of the response.
    bool EnableMicroSpecular = true;
    // Occludes the specular share by the AO term, narrowed by roughness and view angle.
    float SpecularOcclusionStrength = 1.0f;
    // How far the albedo gradient may tilt the shading normal before the sun's GGX lobe
    // is re-evaluated against it. Needs the sun: with time of day off it does nothing.
    float MicroSpecularStrength = 1.0f;
    // Gradient tap radius in G-Buffer texels. At 1 the signal reads as speckle.
    float MicroSpecularDetailScale = 1.5f;
    // Roughness at which grain-scale detail starts fading out, reaching zero at fully
    // rough. Weathered timber sits at 0.6-0.85, so keep this above that.
    float MicroSpecularRoughnessMax = 0.85f;

    // ---- Indirect estimation ---------------------------------------------------------
    // AO applies to the estimated indirect share of each pixel only, never to the whole
    // composite - darkening direct sunlight is what makes post-process AO look like dirt.
    float IndirectFractionMin = 0.15f;
    float IndirectFractionMax = 0.85f;
    float DirectLightWeight = 1.0f;

    // ---- Temporal / denoise ----------------------------------------------------------
    bool EnableTemporalReconstruction = true;
    float HistoryWeightStable = 0.92f;
    float HistoryWeightMoving = 0.72f;
    float DisocclusionDepthTolerance = 0.05f;   // relative depth mismatch that drops history
    float NeighborhoodClampScale = 1.25f;       // clamp box width in standard deviations
    bool EnableSpatialDenoise = true;
    int SpatialDenoiseRadius = 2;               // taps per side; raise if foliage sparkles

    // ---- Detail enhancement ----------------------------------------------------------
    // Off by default, deliberately: this re-amplifies exactly the band TAA and the
    // upscalers spent their budget suppressing. Validate on a slow and a fast pan.
    bool EnableDetailEnhancement = false;
    // 0.10 is the tested value; the useful range is roughly 0.05-0.25. At 1.0 every
    // pixel sits in the luminance clamp, which is a precise description of grain.
    float FineDetailStrength = 0.10f;
    float StructureStrength = 0.18f;
    float MaxLuminanceChange = 0.15f;
    // Fades detail out while the camera turns (full suppression at 60 deg/s). Camera
    // global rather than per pixel: per-pixel velocity splits the frame along depth.
    float DetailMotionSuppression = 0.0f;

    // Not saved with the scene, like every other pass's debug view.
    int DebugView = 0;

    // ---- fallbacks -------------------------------------------------------------------

    // Every preset starts from the factory values and changes only what it means to
    // change, so a field added later gets a sane value in all four.
    static DpleSettings MakePreset(DplePreset preset)
    {
        DpleSettings result;
        result.Enabled = true;
        switch (preset)
        {
        case DplePreset::Subtle:
            result.MicroAOIntensity = 0.10f;
            result.ContactAOIntensity = 0.18f;
            result.BroadAOIntensity = 0.08f;
            result.MaxCombinedAO = 0.28f;
            result.ContactShadowIntensity = 0.50f;
            result.ContactShadowLength = 0.20f;
            result.MicroSpecularStrength = 0.50f;
            result.EnableDetailEnhancement = false;
            break;
        case DplePreset::Strong:
            result.MicroAOIntensity = 0.26f;
            result.ContactAOIntensity = 0.40f;
            result.BroadAOIntensity = 0.18f;
            result.MaxCombinedAO = 0.60f;
            result.ContactShadowIntensity = 0.90f;
            result.ContactShadowLength = 0.40f;
            result.ContactShadowSteps = 14;
            result.MicroSpecularStrength = 2.00f;
            result.MicroSpecularRoughnessMax = 0.90f;
            result.MicroSpecularDetailScale = 2.00f;
            result.EnableDetailEnhancement = true;
            result.FineDetailStrength = 0.10f;
            result.StructureStrength = 0.18f;
            break;
        case DplePreset::Performance:
            result.ContactShadowSteps = 8;
            result.ContactShadowLength = 0.25f;
            result.EnableDetailEnhancement = false;
            break;
        case DplePreset::Balanced:
        default:
            break;
        }
        result.WorkingResolutionDivisor = GetPresetWorkingResolutionDivisor(preset);
        return result;
    }

    static int GetPresetWorkingResolutionDivisor(DplePreset preset)
    {
        switch (preset)
        {
        case DplePreset::Strong:      return 1;
        case DplePreset::Performance: return 4;
        default:                      return 2;
        }
    }

    // Per-section restore to the factory values. Tuning happens one section at a time,
    // so undoing it should too.
    void ResetAmbientOcclusion()
    {
        const DpleSettings f;
        EnableAmbientOcclusion = f.EnableAmbientOcclusion;
        MicroAORadius = f.MicroAORadius;       MicroAOIntensity = f.MicroAOIntensity;
        ContactAORadius = f.ContactAORadius;   ContactAOIntensity = f.ContactAOIntensity;
        BroadAORadius = f.BroadAORadius;       BroadAOIntensity = f.BroadAOIntensity;
        MaxCombinedAO = f.MaxCombinedAO;       AOPower = f.AOPower;       AOBias = f.AOBias;
    }

    void ResetContactShadows()
    {
        const DpleSettings f;
        EnableContactShadows = f.EnableContactShadows;
        ContactShadowLength = f.ContactShadowLength;
        ContactShadowSteps = f.ContactShadowSteps;
        ContactShadowThickness = f.ContactShadowThickness;
        ContactShadowIntensity = f.ContactShadowIntensity;
    }

    void ResetMaterialResponse()
    {
        const DpleSettings f;
        EnableMaterialResponse = f.EnableMaterialResponse;
        SkinAOScale = f.SkinAOScale;           SkinWarmth = f.SkinWarmth;
        FoliageAOScale = f.FoliageAOScale;     FoliageSaturation = f.FoliageSaturation;
        WetRoughnessThreshold = f.WetRoughnessThreshold;
        WetResponseStrength = f.WetResponseStrength;
    }

    void ResetMicroSpecular()
    {
        const DpleSettings f;
        EnableMicroSpecular = f.EnableMicroSpecular;
        SpecularOcclusionStrength = f.SpecularOcclusionStrength;
        MicroSpecularStrength = f.MicroSpecularStrength;
        MicroSpecularDetailScale = f.MicroSpecularDetailScale;
        MicroSpecularRoughnessMax = f.MicroSpecularRoughnessMax;
    }

    void ResetIndirectEstimation()
    {
        const DpleSettings f;
        IndirectFractionMin = f.IndirectFractionMin;
        IndirectFractionMax = f.IndirectFractionMax;
        DirectLightWeight = f.DirectLightWeight;
    }

    void ResetTemporal()
    {
        const DpleSettings f;
        EnableTemporalReconstruction = f.EnableTemporalReconstruction;
        HistoryWeightStable = f.HistoryWeightStable;
        HistoryWeightMoving = f.HistoryWeightMoving;
        DisocclusionDepthTolerance = f.DisocclusionDepthTolerance;
        NeighborhoodClampScale = f.NeighborhoodClampScale;
        EnableSpatialDenoise = f.EnableSpatialDenoise;
        SpatialDenoiseRadius = f.SpatialDenoiseRadius;
    }

    void ResetDetailEnhancement()
    {
        const DpleSettings f;
        EnableDetailEnhancement = f.EnableDetailEnhancement;
        FineDetailStrength = f.FineDetailStrength;
        StructureStrength = f.StructureStrength;
        MaxLuminanceChange = f.MaxLuminanceChange;
        DetailMotionSuppression = f.DetailMotionSuppression;
    }
};
