#include "pch.h"
#include "EngineCVars.h"

#include "System/CVar.h"
#include "DX12SceneRenderer.h"
#include "System/PteroLog.h"

namespace
{
    // Settings structs hold a few counts as UINT rather than int. They are the
    // same size and representation, and a cvar only ever writes a value it has
    // already range-checked, so aliasing the pointer is safe and avoids a
    // parallel int mirror that could drift out of step with the panel.
    int* AsIntPointer(unsigned int* value)
    {
        return reinterpret_cast<int*>(value);
    }

    void RegisterColorGrade(const char* prefix, AgxColorGradeControl& control, const char* what)
    {
        const std::string base = prefix;
        CVar::RegisterFloat((base + ".total").c_str(), &control.Total, what, -2.0f, 2.0f);
        CVar::RegisterFloat((base + ".red").c_str(), &control.Red, what, -2.0f, 2.0f);
        CVar::RegisterFloat((base + ".green").c_str(), &control.Green, what, -2.0f, 2.0f);
        CVar::RegisterFloat((base + ".blue").c_str(), &control.Blue, what, -2.0f, 2.0f);
        CVar::RegisterFloat((base + ".yellow").c_str(), &control.Yellow, what, -2.0f, 2.0f);
    }

    void RegisterColorGradeRegion(const char* prefix, AgxColorGradeRegion& region, const char* regionName)
    {
        const std::string base = prefix;
        const std::string contrast = "contrast, " + std::string(regionName);
        const std::string gamma = "gamma, " + std::string(regionName);
        const std::string gain = "gain, " + std::string(regionName);
        const std::string saturation = "saturation, " + std::string(regionName);
        const std::string vibrance = "vibrance, " + std::string(regionName);
        RegisterColorGrade((base + ".contrast").c_str(), region.Contrast, contrast.c_str());
        RegisterColorGrade((base + ".gamma").c_str(), region.Gamma, gamma.c_str());
        RegisterColorGrade((base + ".gain").c_str(), region.Gain, gain.c_str());
        RegisterColorGrade((base + ".saturation").c_str(), region.Saturation, saturation.c_str());
        RegisterColorGrade((base + ".vibrance").c_str(), region.Vibrance, vibrance.c_str());
    }
}

void RegisterEngineCVars(DX12SceneRenderer& renderer)
{
    // ---------------------------------------------------------------- renderer
    DX12SceneRenderer* rendererPointer = &renderer;

    CVar::RegisterFloat("r.viewdistance", &renderer.GetViewDistanceMetersRef(),
        "Camera far plane in metres.", 500.0f, 100000.0f,
        [rendererPointer] { rendererPointer->SetViewDistanceMeters(rendererPointer->GetViewDistanceMeters()); });
    CVar::RegisterBool("r.grid", &renderer.GetGridEnabledRef(),
        "Draws the viewport ground grid.");
    CVar::RegisterBool("r.wireframe", &renderer.GetWireframeEnabledRef(),
        "Draws scene geometry as wireframe.",
        [rendererPointer] { rendererPointer->InvalidateEntityPipeline(); });

    // ---------------------------------------------------------------------- GI
    CVar::RegisterEnum("gi.mode", reinterpret_cast<int*>(&renderer.GetGlobalIlluminationMode()),
        "Which global-illumination system runs.", { "disabled", "rtgi", "radiancecascades" }, 0);

    RtGISettings& rtgi = renderer.GetRtgiSettings();
    CVar::RegisterBool ("rtgi.enabled", &rtgi.Enabled, "Ray-traced global illumination.");
    CVar::RegisterBool ("rtgi.nrd", &rtgi.UseNrdDenoiser, "Denoise with NVIDIA RELAX instead of the built-in temporal filter.");
    CVar::RegisterInt  ("rtgi.raysperpixel", &rtgi.RaysPerPixel, "GI rays traced per pixel per frame.", 1, 32);
    CVar::RegisterInt  ("rtgi.maxbounces", &rtgi.MaxBounces, "Indirect bounces per path.", 1, 8);
    CVar::RegisterBool ("rtgi.nee", &rtgi.NextEventEstimation, "Next-event estimation: sample lights directly at each hit.");
    CVar::RegisterFloat("rtgi.radianceclamp", &rtgi.RadianceClamp, "Ceiling on a single sample's radiance; lower kills fireflies and dims bright bounces.", 0.0f, 1000.0f);
    CVar::RegisterFloat("rtgi.accumulationblend", &rtgi.AccumulationBlend, "Weight of the current frame in the non-NRD accumulator.", 0.0f, 1.0f);
    CVar::RegisterFloat("rtgi.gi.intensity", &rtgi.GiIntensity, "Multiplier on the final indirect contribution.", 0.0f, 10.0f);
    CVar::RegisterFloat("rtgi.colorleak", &rtgi.ColorLeakIntensity, "How much surface colour bleeds into the bounce.", 0.0f, 10.0f);
    CVar::RegisterBool ("rtgi.specular.enabled", &rtgi.SpecularEnabled, "Ray-traced specular (glossy reflections) on top of diffuse GI.");
    CVar::RegisterFloat("rtgi.specular.roughnessthreshold", &rtgi.SpecularRoughnessThreshold, "Surfaces rougher than this get no ray-traced specular.", 0.0f, 1.0f);
    CVar::RegisterFloat("rtgi.specular.intensity", &rtgi.SpecularIntensity, "Multiplier on ray-traced specular.", 0.0f, 10.0f);
    CVar::RegisterBool ("rtgi.temporalreuse", &rtgi.TemporalReuseEnabled, "ReSTIR temporal reservoir reuse. Unused while the NRD denoiser is on.");
    CVar::RegisterInt  ("rtgi.maxhistorylength", &rtgi.MaxHistoryLength, "Frames a temporal reservoir may accumulate.", 1, 200);
    CVar::RegisterBool ("rtgi.spatialreuse", &rtgi.SpatialReuseEnabled, "ReSTIR spatial reservoir reuse. Unused while the NRD denoiser is on.");
    CVar::RegisterInt  ("rtgi.spatialsamples", &rtgi.SpatialSamples, "Neighbours consulted during spatial reuse.", 1, 32);
    CVar::RegisterFloat("rtgi.spatialradius", &rtgi.SpatialRadius, "Spatial reuse search radius in pixels.", 1.0f, 200.0f);
    CVar::RegisterFloat("rtgi.depththreshold", &rtgi.DepthThreshold, "Relative depth difference above which a reused sample is rejected.", 0.0f, 1.0f);
    CVar::RegisterFloat("rtgi.normalthreshold", &rtgi.NormalThreshold, "Normal dot-product below which a reused sample is rejected.", 0.0f, 1.0f);
    CVar::RegisterFloat("rtgi.nrd.maxaccumulationtime", &rtgi.NrdMaxAccumulationTime, "RELAX history length, in seconds.", 0.0f, 5.0f);
    CVar::RegisterFloat("rtgi.nrd.disocclusionthreshold", &rtgi.NrdDisocclusionThreshold, "RELAX disocclusion sensitivity; higher keeps more history through motion.", 0.0f, 1.0f);
    CVar::RegisterInt  ("rtgi.nrd.atrousiterations", &rtgi.NrdAtrousIterations, "RELAX a-trous blur passes.", 0, 8);
    CVar::RegisterFloat("rtgi.nrd.sharpen", &rtgi.NrdSharpenAmount, "RELAX output sharpening.", 0.0f, 2.0f);
    CVar::RegisterEnum ("rtgi.debugview", &rtgi.DebugView,
        "Replaces the image with one RTGI intermediate.",
        { "off", "radiance", "luminance", "reservoir", "deterministic_probe", "flat_white",
          "world_normal", "world_position", "hit_distance", "ray_direction" }, 0);

    RadianceCascadesSettings& cascades = renderer.GetRadianceCascadesSettings();
    CVar::RegisterBool ("rc.enabled", &cascades.Enabled, "Radiance-cascade global illumination.");
    CVar::RegisterInt  ("rc.cascadecount", AsIntPointer(&cascades.CascadeCount), "Number of cascades.", 1, 8);
    CVar::RegisterInt  ("rc.probespacingbase", AsIntPointer(&cascades.ProbeSpacingBase), "Probe spacing of cascade 0, in pixels.", 2, 128);
    CVar::RegisterInt  ("rc.raysperprobe", AsIntPointer(&cascades.RaysPerProbe), "Rays cast per probe per cascade.", 1, 256);
    CVar::RegisterFloat("rc.raylengthbase", &cascades.RayLengthBase, "Ray length of cascade 0, in metres.", 0.01f, 100.0f);
    CVar::RegisterFloat("rc.raylengthscale", &cascades.RayLengthScale, "Ray-length multiplier between cascades.", 1.0f, 8.0f);
    CVar::RegisterFloat("rc.intervallengthscale", &cascades.IntervalLengthScale, "Scales every cascade's ray interval.", 0.1f, 8.0f);
    CVar::RegisterFloat("rc.hysteresis", &cascades.Hysteresis, "Temporal blend of probe radiance; higher is more stable and more laggy.", 0.0f, 1.0f);
    CVar::RegisterFloat("rc.gi.intensity", &cascades.GiIntensity, "Multiplier on the cascade indirect contribution.", 0.0f, 10.0f);
    CVar::RegisterFloat("rc.colorbleeding", &cascades.ColorBleedingStrength, "How much surface colour tints the bounce.", 0.0f, 10.0f);
    CVar::RegisterInt  ("rc.sparse.tablecapacity", AsIntPointer(&cascades.SparseProbeTableCapacity), "Sparse probe hash-table size.", 256, 1048576);
    CVar::RegisterInt  ("rc.sparse.cellsize", AsIntPointer(&cascades.SparseProbeCellSize), "Sparse probe cell size.", 1, 256);
    CVar::RegisterInt  ("rc.sparse.searchsteps", AsIntPointer(&cascades.SparseProbeSearchSteps), "Probe-lookup probe count before giving up.", 1, 256);
    CVar::RegisterFloat("rc.sparse.reusestrength", &cascades.SparseProbeReuseStrength, "How strongly a nearby cached probe is reused.", 0.0f, 1.0f);
    CVar::RegisterFloat("rc.raybias", &cascades.RayBias, "Ray origin offset along the normal, in metres.", 0.0f, 1.0f);
    CVar::RegisterFloat("rc.spatialfilter", &cascades.SpatialFilterStrength, "Strength of the cascade spatial filter.", 0.0f, 4.0f);
    CVar::RegisterFloat("rc.historyclampscale", &cascades.HistoryClampScale, "Neighbourhood clamp width applied to probe history.", 0.0f, 4.0f);
    CVar::RegisterFloat("rc.historydepthsensitivity", &cascades.HistoryDepthSensitivity, "Depth sensitivity of the history reprojection test.", 0.0f, 1024.0f);
    CVar::RegisterFloat("rc.historynormalthreshold", &cascades.HistoryNormalThreshold, "Normal dot-product below which probe history is dropped.", 0.0f, 1.0f);
    CVar::RegisterInt  ("rc.debugview", &cascades.DebugView, "Cascade debug visualisation.", 0, 8);

    RadianceProbeSettings& probes = renderer.GetProbeSettings();
    CVar::RegisterBool ("probes.enabled", &probes.Enabled, "Irradiance probe grid.");
    CVar::RegisterInt  ("probes.gridx", &probes.GridX, "Probes along X.", 1, 128);
    CVar::RegisterInt  ("probes.gridy", &probes.GridY, "Probes along Y.", 1, 128);
    CVar::RegisterInt  ("probes.gridz", &probes.GridZ, "Probes along Z.", 1, 128);
    CVar::RegisterFloat("probes.spacing", &probes.Spacing, "Distance between probes, in metres.", 0.1f, 100.0f);
    CVar::RegisterFloat("probes.originx", &probes.OriginX, "Grid origin X.", -10000.0f, 10000.0f);
    CVar::RegisterFloat("probes.originy", &probes.OriginY, "Grid origin Y.", -10000.0f, 10000.0f);
    CVar::RegisterFloat("probes.originz", &probes.OriginZ, "Grid origin Z.", -10000.0f, 10000.0f);
    CVar::RegisterBool ("probes.followcamera", &probes.FollowCamera, "Recentres the grid on the camera.");
    CVar::RegisterInt  ("probes.raysperprobe", &probes.RaysPerProbe, "Rays cast per probe per update.", 1, 1024);
    CVar::RegisterFloat("probes.updateblend", &probes.UpdateBlend, "Weight of a fresh probe update against its history.", 0.0f, 1.0f);
    CVar::RegisterBool ("probes.debug.show", &probes.DebugShowProbes, "Draws a sphere at every probe.");
    CVar::RegisterInt  ("probes.debug.lightingmode", &probes.DebugLightingMode, "What the debug spheres display.", 0, 4);
    CVar::RegisterFloat("probes.debug.sphereradius", &probes.DebugSphereRadius, "Radius of the debug spheres, in metres.", 0.01f, 5.0f);

    // --------------------------------------------------------- ambient occlusion
    RtAOSettings& rtao = renderer.GetRtaoSettings();
    CVar::RegisterBool ("rtao.enabled", &rtao.Enabled, "Ray-traced ambient occlusion.");
    CVar::RegisterInt  ("rtao.raysperpixel", &rtao.RaysPerPixel, "Occlusion rays per pixel.", 1, 128);
    CVar::RegisterFloat("rtao.maxraylength", &rtao.MaxRayLength, "Longest occlusion ray, in metres.", 0.01f, 100.0f);
    CVar::RegisterFloat("rtao.raybias", &rtao.RayBias, "Ray origin offset along the normal.", 0.0f, 1.0f);
    CVar::RegisterFloat("rtao.power", &rtao.AOPower, "Exponent applied to the occlusion term.", 0.1f, 8.0f);
    CVar::RegisterFloat("rtao.intensity", &rtao.Intensity, "Multiplier on the occlusion term.", 0.0f, 4.0f);
    CVar::RegisterInt  ("rtao.debugview", &rtao.DebugView, "RTAO debug visualisation.", 0, 4);

    GtaoSettings& gtao = renderer.GetGtaoSettings();
    CVar::RegisterBool ("gtao.enabled", &gtao.Enabled, "Ground-truth ambient occlusion (screen space).");
    CVar::RegisterInt  ("gtao.quality", &gtao.QualityLevel, "0 low .. 3 ultra.", 0, 3);
    CVar::RegisterFloat("gtao.radius", &gtao.Radius, "World-space sampling radius, in metres.", 0.01f, 20.0f);
    CVar::RegisterInt  ("gtao.denoisepasses", &gtao.DenoisePasses, "Denoise passes over the AO term.", 0, 4);
    CVar::RegisterFloat("gtao.finalvaluepower", &gtao.FinalValuePower, "Exponent applied to the AO term.", 0.1f, 8.0f);
    CVar::RegisterFloat("gtao.falloffrange", &gtao.FalloffRange, "Fraction of the radius over which occlusion fades out.", 0.0f, 1.0f);
    CVar::RegisterFloat("gtao.sampledistributionpower", &gtao.SampleDistributionPower, "Biases samples towards the centre as it rises.", 0.1f, 8.0f);
    CVar::RegisterFloat("gtao.thinoccludercompensation", &gtao.ThinOccluderCompensation, "Reduces over-darkening behind thin geometry.", 0.0f, 1.0f);
    CVar::RegisterFloat("gtao.radiusmultiplier", &gtao.RadiusMultiplier, "Scales the effective radius.", 0.1f, 8.0f);
    CVar::RegisterFloat("gtao.depthmipsamplingoffset", &gtao.DepthMIPSamplingOffset, "Depth mip bias used while sampling.", 0.0f, 8.0f);
    CVar::RegisterFloat("gtao.intensity", &gtao.Intensity, "Multiplier on the occlusion term.", 0.0f, 4.0f);
    CVar::RegisterInt  ("gtao.debugview", &gtao.DebugView, "GTAO debug visualisation.", 0, 4);

    // ------------------------------------------------------------- reflections
    SsrSettings& ssr = renderer.GetSsrSettings();
    CVar::RegisterBool ("ssr.enabled", &ssr.Enabled, "Screen-space reflections.");
    CVar::RegisterFloat("ssr.intensity", &ssr.Intensity, "Multiplier on the reflection.", 0.0f, 4.0f);
    CVar::RegisterInt  ("ssr.maxsteps", &ssr.MaxSteps, "March steps before a ray is abandoned.", 1, 512);
    CVar::RegisterFloat("ssr.stepsize", &ssr.StepSize, "Length of the first march step, in metres.", 0.001f, 10.0f);
    CVar::RegisterFloat("ssr.stepgrowth", &ssr.StepGrowth, "Multiplier applied to each successive step.", 1.0f, 2.0f);
    CVar::RegisterFloat("ssr.thickness", &ssr.Thickness, "Assumed depth-buffer thickness when testing a hit.", 0.001f, 10.0f);
    CVar::RegisterFloat("ssr.maxroughness", &ssr.MaxRoughness, "Surfaces rougher than this get no screen-space reflection.", 0.0f, 1.0f);
    CVar::RegisterInt  ("ssr.refinesteps", &ssr.RefineSteps, "Binary-search steps used to refine a hit.", 0, 32);
    CVar::RegisterFloat("ssr.maxdistance", &ssr.MaxDistance, "Longest reflection ray, in metres.", 1.0f, 1000.0f);
    CVar::RegisterFloat("ssr.edgefadestart", &ssr.EdgeFadeStart, "Screen fraction at which reflections start fading at the border.", 0.0f, 1.0f);
    CVar::RegisterInt  ("ssr.debugview", &ssr.DebugView, "SSR debug visualisation.", 0, 4);

    // ----------------------------------------------------------- anti-aliasing
    TaaSettings& taa = renderer.GetTaaSettings();
    CVar::RegisterBool ("taa.enabled", &taa.Enabled, "Temporal anti-aliasing.");
    CVar::RegisterFloat("taa.blendfactor", &taa.BlendFactor, "Weight of the current frame; lower accumulates longer.", 0.0f, 1.0f);
    CVar::RegisterFloat("taa.jitterscale", &taa.JitterScale, "Sub-pixel jitter magnitude in pixels; 0 disables jitter.", 0.0f, 4.0f);

    SMAASettings& smaa = renderer.GetSmaaSettings();
    CVar::RegisterBool ("smaa.enabled", &smaa.Enabled, "SMAA spatial anti-aliasing.");
    CVar::RegisterFloat("smaa.edgethreshold", &smaa.EdgeThreshold, "Luma difference that counts as an edge; lower finds more edges.", 0.0f, 1.0f);
    CVar::RegisterFloat("smaa.maxsearchsteps", &smaa.MaxSearchSteps, "Horizontal/vertical edge search length.", 0.0f, 112.0f);
    CVar::RegisterFloat("smaa.maxsearchstepsdiag", &smaa.MaxSearchStepsDiag, "Diagonal edge search length.", 0.0f, 20.0f);
    CVar::RegisterFloat("smaa.cornerrounding", &smaa.CornerRounding, "How much sharp corners are rounded, as a percentage.", 0.0f, 100.0f);
    CVar::RegisterEnum ("smaa.debugview", &smaa.DebugView, "SMAA debug visualisation.", { "off", "edges", "blendweights" }, 0);

    MsaaSettings& msaa = renderer.GetMsaaSettings();
    CVar::RegisterBool("msaa.enabled", &msaa.Enabled, "Multi-sample the G-Buffer. Costs SampleCount times the G-Buffer bandwidth.");
    CVar::RegisterInt ("msaa.samplecount", AsIntPointer(&msaa.SampleCount), "Samples per pixel: 2, 4 or 8.", 2, 8,
        [&msaa] { msaa.Validate(); });
    CVar::RegisterInt ("msaa.quality", AsIntPointer(&msaa.Quality), "Hardware quality level; 0 is the default for the sample count.", 0, 32);

    DlssSettings& dlss = renderer.GetDlssSettings();
    CVar::RegisterBool("dlss.enabled", &dlss.Enabled, "NVIDIA DLSS upscaling.");
    CVar::RegisterEnum("dlss.mode", &dlss.Mode, "DLSS quality preset.",
        { "off", "maxperformance", "balanced", "maxquality", "ultraperformance", "ultraquality", "dlaa" }, 0);

    SharpenSettings& sharpen = renderer.GetSharpenSettings();
    CVar::RegisterBool ("sharpen.image.enabled", &sharpen.ImageSharpeningEnabled, "Post-process image sharpening.");
    CVar::RegisterFloat("sharpen.image.strength", &sharpen.ImageSharpeningStrength, "Image sharpening strength.", 0.0f, 1.5f,
        [&sharpen] { sharpen.Validate(); });
    CVar::RegisterBool ("sharpen.texture.enabled", &sharpen.TextureSharpeningEnabled, "Applies a negative mip bias when sampling textures.");
    CVar::RegisterFloat("sharpen.texture.miplodbias", &sharpen.TextureMipLODBias, "Mip LOD bias; more negative is sharper and noisier.", -3.0f, 0.0f,
        [&sharpen] { sharpen.Validate(); });

    // ------------------------------------------------------------ post-process
    BloomSettings& bloom = renderer.GetBloomSettings();
    CVar::RegisterBool ("bloom.enabled", &bloom.Enabled, "Bloom.");
    CVar::RegisterFloat("bloom.intensity", &bloom.Intensity, "How much bloom is added back to the image.", 0.0f, 2.0f);
    CVar::RegisterFloat("bloom.threshold", &bloom.Threshold, "Luminance above which a pixel contributes to bloom.", 0.0f, 20.0f);
    CVar::RegisterFloat("bloom.knee", &bloom.Knee, "Softness of the threshold.", 0.0f, 1.0f);
    CVar::RegisterFloat("bloom.radius", &bloom.Radius, "Upsample filter radius.", 0.0f, 8.0f);
    CVar::RegisterInt  ("bloom.miplevels", &bloom.MipLevels, "Mip levels in the bloom chain.", 1, 12);

    ChromaticAberrationSettings& chroma = renderer.GetChromaticAberrationSettings();
    CVar::RegisterBool ("chroma.enabled", &chroma.Enabled, "Chromatic aberration.");
    CVar::RegisterFloat("chroma.strength", &chroma.Strength, "Channel separation in pixels at the frame edge.", 0.0f, 20.0f);
    CVar::RegisterFloat("chroma.falloff", &chroma.Falloff, "How sharply the effect grows towards the edge.", 0.0f, 8.0f);
    CVar::RegisterInt  ("chroma.samplecount", &chroma.SampleCount, "Samples taken along the separation.", 1, 32);
    CVar::RegisterFloat("chroma.centerx", &chroma.CenterX, "Distortion centre, X, in normalised screen space.", 0.0f, 1.0f);
    CVar::RegisterFloat("chroma.centery", &chroma.CenterY, "Distortion centre, Y, in normalised screen space.", 0.0f, 1.0f);

    AgxTonemapSettings& agx = renderer.GetAgxSettings();
    CVar::RegisterBool ("agx.enabled", &agx.Enabled, "AgX tonemapping.");
    CVar::RegisterFloat("agx.ev100", &agx.Ev100, "Photographic exposure in EV100; higher is darker, one unit per stop.", -16.0f, 16.0f);
    CVar::RegisterFloat("agx.exposure", &agx.Exposure, "Exposure trim on top of agx.ev100, in stops.", -10.0f, 10.0f);
    CVar::RegisterFloat("agx.ev100min", &agx.Ev100Min, "Lower clamp on agx.ev100.", -16.0f, 16.0f);
    CVar::RegisterFloat("agx.ev100max", &agx.Ev100Max, "Upper clamp on agx.ev100.", -16.0f, 16.0f);
    CVar::RegisterEnum ("agx.exposuremode", reinterpret_cast<int*>(&agx.ExposureMode),
        "Manual uses agx.ev100; auto meters the scene with a luminance histogram.", { "manual", "auto" }, 0);
    CVar::RegisterFloat("agx.autoexposure.speedup", &agx.AutoExposureSpeedUp,
        "Stops per second when adapting to a brighter scene.", 0.0f, 20.0f);
    CVar::RegisterFloat("agx.autoexposure.speeddown", &agx.AutoExposureSpeedDown,
        "Stops per second when adapting to a darker scene.", 0.0f, 20.0f);
    CVar::RegisterFloat("agx.autoexposure.lowpercent", &agx.AutoExposureLowPercent,
        "Fraction of the darkest metered weight to discard.", 0.0f, 1.0f);
    CVar::RegisterFloat("agx.autoexposure.highpercent", &agx.AutoExposureHighPercent,
        "Cumulative point the metering stops averaging at.", 0.0f, 1.0f);
    CVar::RegisterFloat("agx.autoexposure.greypoint", &agx.AutoExposureGreyPoint,
        "Scene luminance the metered average is exposed onto.", 0.01f, 1.0f);
    CVar::RegisterFloat("agx.autoexposure.meteringmask", &agx.AutoExposureMeteringMask,
        "0 meters the whole frame evenly, 1 weights the centre heavily.", 0.0f, 1.0f);
    CVar::RegisterFloat("agx.autoexposure.histogramlogmin", &agx.AutoExposureHistogramLogMin,
        "Darkest luminance the histogram resolves, in log2.", -20.0f, 0.0f);
    CVar::RegisterFloat("agx.autoexposure.histogramlogmax", &agx.AutoExposureHistogramLogMax,
        "Brightest luminance the histogram resolves, in log2.", 0.0f, 20.0f);
    CVar::RegisterFloat("agx.toestrength", &agx.ToeStrength, "Shadow roll-off of the AgX curve.", 0.0f, 4.0f);
    CVar::RegisterFloat("agx.shoulderstrength", &agx.ShoulderStrength, "Highlight roll-off of the AgX curve.", 0.0f, 4.0f);
    RegisterColorGradeRegion("agx.global", agx.Global, "whole image");
    RegisterColorGradeRegion("agx.shadows", agx.Shadows, "shadows only");
    RegisterColorGradeRegion("agx.midtones", agx.Midtones, "midtones only");
    RegisterColorGradeRegion("agx.highlights", agx.Highlights, "highlights only");

    // ------------------------------------------------------------- atmospherics
    TimeOfDaySettings& tod = renderer.GetTimeOfDaySettings();
    CVar::RegisterBool ("tod.enabled", &tod.Enabled, "Sun, sky ambient, procedural sky and clouds as one system.");
    CVar::RegisterFloat("tod.time", &tod.TimeOfDay, "Time of day in hours; noon is 12.", 0.0f, 24.0f);
    CVar::RegisterFloat("tod.turbidity", &tod.Turbidity, "Atmospheric turbidity: 2 very clear, 10 heavy haze.", 1.0f, 10.0f);
    CVar::RegisterFloat("tod.groundalbedo", &tod.GroundAlbedo, "Ground albedo; tints the sky near the horizon.", 0.0f, 1.0f);
    CVar::RegisterFloat("tod.sunintensitylux", &tod.SunIntensityLux, "Sun intensity in lux; real noon sun is about 100000.", 0.0f, 400000.0f);
    CVar::RegisterFloat("tod.skyintensitylux", &tod.SkyIntensityLux, "Sky ambient intensity in lux.", 0.0f, 200000.0f);
    CVar::RegisterBool ("tod.overridesuncolor", &tod.OverrideSunColor, "Use the manual sun colour instead of the Hosek-Wilkie one.");
    CVar::RegisterBool ("tod.overrideskycolor", &tod.OverrideSkyColor, "Use the manual sky colour instead of the Hosek-Wilkie one.");
    CVar::RegisterFloat("tod.suncolor.r", &tod.SunColorR, "Manual sun colour, red (linear).", 0.0f, 10.0f);
    CVar::RegisterFloat("tod.suncolor.g", &tod.SunColorG, "Manual sun colour, green (linear).", 0.0f, 10.0f);
    CVar::RegisterFloat("tod.suncolor.b", &tod.SunColorB, "Manual sun colour, blue (linear).", 0.0f, 10.0f);
    CVar::RegisterFloat("tod.skycolor.r", &tod.SkyColorR, "Manual sky colour, red (linear).", 0.0f, 10.0f);
    CVar::RegisterFloat("tod.skycolor.g", &tod.SkyColorG, "Manual sky colour, green (linear).", 0.0f, 10.0f);
    CVar::RegisterFloat("tod.skycolor.b", &tod.SkyColorB, "Manual sky colour, blue (linear).", 0.0f, 10.0f);

    WindSettings& wind = renderer.GetWindSettings();
    CVar::RegisterBool ("wind.enabled", &wind.Enabled, "Scene-wide wind, shared by rain and vegetation.");
    CVar::RegisterFloat("wind.direction", &wind.DirectionRadians, "Heading in radians, counter-clockwise from world +X.", -6.2832f, 6.2832f);
    CVar::RegisterFloat("wind.strength", &wind.Strength, "Sustained wind speed, m/s. 2 light, 8 branches moving, 15 trees swaying.", 0.0f, 60.0f);
    CVar::RegisterFloat("wind.gustamplitude", &wind.GustAmplitude, "Peak extra speed contributed by gusts, m/s.", 0.0f, 60.0f);
    CVar::RegisterFloat("wind.gustfrequency", &wind.GustFrequency, "Gust pulse rate in Hz.", 0.0f, 10.0f);
    CVar::RegisterFloat("wind.gustwavelength", &wind.GustWavelength, "Size of one gust cell in metres; keep it larger than the visible area.", 1.0f, 1000.0f);
    CVar::RegisterFloat("wind.vegetationbendscale", &wind.VegetationBendScale, "Global multiplier on vegetation bending.", 0.0f, 4.0f);
    CVar::RegisterFloat("wind.flutterfrequency", &wind.FlutterFrequency, "Leaf flutter rate in Hz.", 0.0f, 20.0f);

    VolumetricFogSettings& fog = renderer.GetVolumetricFogSettings();
    CVar::RegisterBool ("fog.enabled", &fog.Enabled, "Froxel volumetric fog.");
    CVar::RegisterInt  ("fog.froxeltilesize", &fog.FroxelTileSize, "Froxel width in pixels; smaller is sharper and costlier.", 2, 32);
    CVar::RegisterInt  ("fog.depthslices", &fog.DepthSlices, "Froxel slices along the view ray.", 8, 256);
    CVar::RegisterFloat("fog.startdistance", &fog.StartDistance, "Nearest fogged distance, in metres.", 0.0f, 1000.0f);
    CVar::RegisterFloat("fog.maxdistance", &fog.MaxDistance, "Furthest fogged distance, in metres.", 1.0f, 10000.0f);
    CVar::RegisterFloat("fog.density", &fog.Density, "Extinction per metre.", 0.0f, 2.0f);
    CVar::RegisterFloat("fog.anisotropy", &fog.Anisotropy, "Henyey-Greenstein g: positive scatters forward.", -0.99f, 0.99f);
    CVar::RegisterFloat("fog.baseheight", &fog.BaseHeight, "Height at which the density falloff starts, in metres.", -1000.0f, 10000.0f);
    CVar::RegisterFloat("fog.heightfalloff", &fog.HeightFalloff, "Exponential density falloff with height; 0 is uniform.", 0.0f, 2.0f);
    CVar::RegisterFloat("fog.color.r", &fog.ColorR, "Fog scattering colour, red.", 0.0f, 10.0f);
    CVar::RegisterFloat("fog.color.g", &fog.ColorG, "Fog scattering colour, green.", 0.0f, 10.0f);
    CVar::RegisterFloat("fog.color.b", &fog.ColorB, "Fog scattering colour, blue.", 0.0f, 10.0f);
    CVar::RegisterFloat("fog.emissive.r", &fog.EmissiveColorR, "Fog self-emission, red.", 0.0f, 10.0f);
    CVar::RegisterFloat("fog.emissive.g", &fog.EmissiveColorG, "Fog self-emission, green.", 0.0f, 10.0f);
    CVar::RegisterFloat("fog.emissive.b", &fog.EmissiveColorB, "Fog self-emission, blue.", 0.0f, 10.0f);
    CVar::RegisterFloat("fog.emissive.intensity", &fog.EmissiveIntensity, "Multiplier on fog self-emission.", 0.0f, 100.0f);
    CVar::RegisterInt  ("fog.debugview", &fog.DebugView, "Volumetric fog debug visualisation.", 0, 4);

    VolumetricCloudSettings& clouds = renderer.GetVolumetricCloudSettings();
    CVar::RegisterBool ("clouds.enabled", &clouds.Enabled, "Ray-marched volumetric clouds.");
    CVar::RegisterFloat("clouds.planetradiuskm", &clouds.PlanetRadiusKm, "Planet radius in kilometres; sets the curvature of the layer.", 100.0f, 20000.0f);
    CVar::RegisterFloat("clouds.layerbottom", &clouds.LayerBottomMeters, "Cloud base altitude, in metres.", 0.0f, 20000.0f);
    CVar::RegisterFloat("clouds.layerthickness", &clouds.LayerThicknessMeters, "Cloud layer thickness, in metres.", 1.0f, 20000.0f);
    CVar::RegisterFloat("clouds.coverage", &clouds.Coverage, "Fraction of sky covered.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.cloudtype", &clouds.CloudType, "0 stratus, 1 cumulonimbus.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.density", &clouds.Density, "Cloud density multiplier.", 0.0f, 4.0f);
    CVar::RegisterFloat("clouds.basenoisescale", &clouds.BaseNoiseScaleMeters, "Base shape noise wavelength, in metres.", 100.0f, 200000.0f);
    CVar::RegisterFloat("clouds.detailnoisescale", &clouds.DetailNoiseScaleMeters, "Detail erosion noise wavelength, in metres.", 10.0f, 50000.0f);
    CVar::RegisterFloat("clouds.detailstrength", &clouds.DetailStrength, "How hard the detail noise erodes the base shape.", 0.0f, 2.0f);
    CVar::RegisterFloat("clouds.curlstrength", &clouds.CurlStrength, "Curl-noise warp applied to the detail.", 0.0f, 4.0f);
    CVar::RegisterFloat("clouds.anvilbias", &clouds.AnvilBias, "Spreads cloud tops outward into anvils.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.weatherscale", &clouds.WeatherScaleMeters, "Weather-map wavelength, in metres.", 1000.0f, 500000.0f);
    CVar::RegisterFloat("clouds.cloudtopoffset", &clouds.CloudTopOffsetMeters, "Wind shear applied towards the cloud top, in metres.", 0.0f, 5000.0f);
    CVar::RegisterInt  ("clouds.weatherseed", &clouds.WeatherSeed, "Seed for the procedural weather map.", 0, 1000000);
    CVar::RegisterFloat("clouds.weathercellsize", &clouds.WeatherCellSize, "Weather-map cell size multiplier.", 0.01f, 10.0f);
    CVar::RegisterFloat("clouds.weathercoveragebias", &clouds.WeatherCoverageBias, "Added to the weather map's coverage.", -1.0f, 1.0f);
    CVar::RegisterFloat("clouds.weathertypebias", &clouds.WeatherTypeBias, "Added to the weather map's cloud type.", -1.0f, 1.0f);
    CVar::RegisterFloat("clouds.winddirection", &clouds.WindDirectionDegrees, "Cloud wind heading, in degrees.", -360.0f, 360.0f);
    CVar::RegisterFloat("clouds.windspeed", &clouds.WindSpeed, "Cloud wind speed, m/s.", 0.0f, 200.0f);
    CVar::RegisterFloat("clouds.windskew", &clouds.WindSkew, "How much the wind leans the column with altitude.", 0.0f, 2.0f);
    CVar::RegisterFloat("clouds.detailwindspeedscale", &clouds.DetailWindSpeedScale, "Detail noise wind speed, relative to the base.", 0.0f, 10.0f);
    CVar::RegisterFloat("clouds.scattering.r", &clouds.ScatteringColorR, "Cloud scattering albedo, red.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.scattering.g", &clouds.ScatteringColorG, "Cloud scattering albedo, green.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.scattering.b", &clouds.ScatteringColorB, "Cloud scattering albedo, blue.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.extinctionscale", &clouds.ExtinctionScale, "Extinction per unit density.", 0.0f, 2.0f);
    CVar::RegisterFloat("clouds.phaseg0", &clouds.PhaseG0, "Forward-scattering lobe.", -0.99f, 0.99f);
    CVar::RegisterFloat("clouds.phaseg1", &clouds.PhaseG1, "Back-scattering lobe.", -0.99f, 0.99f);
    CVar::RegisterFloat("clouds.phaseblend", &clouds.PhaseBlend, "Blend between the two phase lobes.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.powderstrength", &clouds.PowderStrength, "Powder (dark-edge) approximation strength.", 0.0f, 2.0f);
    CVar::RegisterInt  ("clouds.multiscatteroctaves", &clouds.MultiScatterOctaves, "Multiple-scattering octaves.", 1, 8);
    CVar::RegisterFloat("clouds.ms.scatterfalloff", &clouds.MsScatterFalloff, "Scatter falloff per multiple-scattering octave.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.ms.extinctionfalloff", &clouds.MsExtinctionFalloff, "Extinction falloff per octave.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.ms.phasefalloff", &clouds.MsPhaseFalloff, "Phase falloff per octave.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.sunintensityscale", &clouds.SunIntensityScale, "Multiplier on sunlight reaching the clouds.", 0.0f, 10.0f);
    CVar::RegisterFloat("clouds.ambientintensityscale", &clouds.AmbientIntensityScale, "Multiplier on sky ambient reaching the clouds.", 0.0f, 10.0f);
    CVar::RegisterFloat("clouds.groundbouncescale", &clouds.GroundBounceScale, "Light bounced from the ground into the cloud base.", 0.0f, 10.0f);
    CVar::RegisterInt  ("clouds.maxsteps", &clouds.MaxSteps, "Primary ray-march steps.", 8, 512);
    CVar::RegisterInt  ("clouds.lightsteps", &clouds.LightSteps, "Steps along the shadow ray towards the sun.", 1, 32);
    CVar::RegisterFloat("clouds.lightmarchdistance", &clouds.LightMarchDistanceMeters, "Length of the shadow ray, in metres.", 10.0f, 20000.0f);
    CVar::RegisterFloat("clouds.maxtracedistance", &clouds.MaxTraceDistanceMeters, "Longest primary ray, in metres.", 1000.0f, 500000.0f);
    CVar::RegisterFloat("clouds.distancefadestart", &clouds.DistanceFadeStartMeters, "Where clouds start fading out, in metres.", 1000.0f, 500000.0f);
    CVar::RegisterFloat("clouds.detailfadedistance", &clouds.DetailFadeDistanceMeters, "Where detail noise stops being evaluated, in metres.", 100.0f, 200000.0f);
    CVar::RegisterFloat("clouds.shadowconespread", &clouds.ShadowConeSpread, "Cone spread of the shadow ray.", 0.0f, 1.0f);
    CVar::RegisterFloat("clouds.shadowstepgrowth", &clouds.ShadowStepGrowth, "Step growth along the shadow ray.", 1.0f, 4.0f);
    CVar::RegisterInt  ("clouds.resolutiondivisor", &clouds.ResolutionDivisor, "Renders clouds at 1/N resolution.", 1, 8);
    CVar::RegisterBool ("clouds.temporalupsampling", &clouds.TemporalUpsampling, "Reprojects previous frames to fill the low-resolution trace.");
    CVar::RegisterFloat("clouds.temporalblend", &clouds.TemporalBlend, "Weight of cloud history; higher is smoother and laggier.", 0.0f, 1.0f);
    CVar::RegisterInt  ("clouds.debugview", &clouds.DebugView, "Cloud debug visualisation.", 0, 8);

    // ----------------------------------------------------------------- shadows
    PointShadowSettings& pointShadows = renderer.GetPointShadowSettings();
    CVar::RegisterInt  ("shadow.point.mapsize", &pointShadows.MapSize, "Cube face resolution. Changing this rebuilds the shadow atlas.", 128, 4096);
    CVar::RegisterFloat("shadow.point.bias", &pointShadows.Bias, "Constant depth bias.", 0.0f, 0.1f);
    CVar::RegisterFloat("shadow.point.slopescaleddepthbias", &pointShadows.SlopeScaledDepthBias, "Depth bias proportional to the surface slope.", 0.0f, 16.0f);
    CVar::RegisterFloat("shadow.point.normaloffset", &pointShadows.NormalOffset, "Offsets the lookup along the normal, in texels.", 0.0f, 16.0f);
    CVar::RegisterFloat("shadow.point.seamblenddistance", &pointShadows.SeamBlendDistance, "Blend width across cube-face seams.", 0.0f, 1.0f);
    CVar::RegisterInt  ("shadow.point.filterradius", &pointShadows.FilterRadius, "PCF radius in texels.", 0, 8);
    CVar::RegisterInt  ("shadow.point.debugview", &pointShadows.DebugView, "Point-shadow debug visualisation.", 0, 4);

    PTERO_LOG_INFO("CVar", "%llu cvars registered. Type 'list' in the console to see them.",
                   static_cast<unsigned long long>(CVar::Count()));
}
