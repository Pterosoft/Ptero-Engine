// AgxTonemap.hlsl  – AgX-based tone-mapping compute shader.
//
// Implements the AgX display transform described by Troy Sobotka:
//   https://iolite-engine.com/blog_posts/minimal_agx_implementation
//
// The shader applies:
//   1. Exposure: EV100 (higher = darker), clamped to [Ev100Min, Ev100Max], plus
//      an EV trim. This is the only thing that decides overall brightness.
//   2. AgX log-space inset over AgX's own fixed 16.5-stop window (maps linear
//      scene-referred light into a perceptual signal with shoulder and toe).
//   3. Parametric sigmoid contrast curve.
//   4. Saturation adjustment in the output-referred space.
//
// Root signature (AgxTonemapper.cpp):
//   b0 – AgxConstantBuffer
//   t0 – input scene colour (SRV)
//   u0 – tonemapped output (UAV)

// ---------------------------------------------------------------------------
// Constant buffer
// ---------------------------------------------------------------------------
struct GradeControl
{
    float Total;
    float Red;
    float Green;
    float Blue;
    float Yellow;
    float _Pad0;
    float _Pad1;
    float _Pad2;
};

struct GradeRegion
{
    GradeControl Contrast;
    GradeControl Gamma;
    GradeControl Gain;
    GradeControl Saturation;
    GradeControl Vibrance;
};

cbuffer AgxConstantBuffer : register(b0)
{
    float Exposure;        // EV stops of trim; positive = brighter
    float Ev100Min;        // lower clamp on Ev100
    float Ev100Max;        // upper clamp on Ev100
    float ToeStrength;     // shadow shaping; 1.0 = default
    float ShoulderStrength;// highlight shaping; 1.0 = default
    float Ev100;           // photographic exposure value; higher = darker
    uint  UseAutoExposure; // 1 = take Ev100 from gAutoExposure instead
    float _Pad2;

    GradeRegion GlobalGrade;
    GradeRegion ShadowsGrade;
    GradeRegion MidtonesGrade;
    GradeRegion HighlightsGrade;

    uint  FrameWidth;
    uint  FrameHeight;
    float _Pad3;
    float _Pad4;
};

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
Texture2D<float4>   gInput  : register(t0);
RWTexture2D<float4> gOutput : register(u0);

// The adapted exposure, maintained entirely on the GPU by the two
// AutoExposure passes. Reading it here rather than feeding it back through
// this pass's constant buffer is what keeps that buffer static frame to frame.
ByteAddressBuffer   gAutoExposure : register(t1);

// ---------------------------------------------------------------------------
// AgX helpers
// ---------------------------------------------------------------------------

float3 AgxDefaultContrastApprox(float3 x)
{
    // 6th-degree polynomial fit to the AgX default contrast curve.
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return + 15.5f     * x4 * x2
           - 40.14f    * x4 * x
           + 31.96f    * x4
           - 6.868f    * x2 * x
           + 0.4298f   * x2
           + 0.1191f   * x
           - 0.00232f;
}

float SafeScalar(float value)
{
    return (isnan(value) || isinf(value)) ? 0.0f : value;
}

float3 SanitizeColor(float3 color)
{
    color = float3(SafeScalar(color.r), SafeScalar(color.g), SafeScalar(color.b));
    return clamp(color, 0.0f.xxx, 65504.0f.xxx);
}

// AgX's log encoding window. These are a fixed property of the display
// transform, not a per-scene control: the whole point of AgX is that every
// scene is viewed through the same 16.5-stop window, and you move the *scene*
// into that window with exposure. Driving these two bounds from the UI - which
// is what this shader used to do - is what let a collapsed Min/Max pair shrink
// the window to a per-channel step function and posterise the frame.
static const float kAgxMinEv = -12.47393f;
static const float kAgxMaxEv = 4.026069f;

// EV100 -> linear exposure scale (Lagarde & de Rousiers, "Moving Frostbite to
// PBR"): the incident luminance that should map to white is 1.2 * 2^EV100, so
// the scene is divided by it. One EV unit is one photographic stop and a higher
// EV100 gives a darker image, exactly like stopping a camera down.
//
// Caveat on the absolute numbers: this renderer's scene-linear values are not
// cd/m2. Lights are normalised against an 800 lm reference (kRefLumens in
// DX12SceneRenderer.cpp), so "1.0" means one reference bulb, not one nit. The
// EV100 slider is therefore photographic in *behaviour* - one unit per stop,
// higher is darker - but its zero point is this engine's, not a light meter's.
float Ev100ToExposureScale(float ev100)
{
    return 1.0f / max(1.2f * exp2(ev100), 1e-6f);
}

float3 ApplySaturation(float3 color, float saturation)
{
    const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
    const float luma = dot(color, lumaWeights);
    return lerp(luma.xxx, color, saturation);
}

float3 ApplyContrast(float3 color, float3 contrast)
{
    return saturate((color - 0.5f) * contrast + 0.5f);
}

float3 ApplyToeShoulder(float3 color, float toe, float shoulder)
{
    const float safeToe = max(toe, 0.001f);
    const float safeShoulder = max(shoulder, 0.001f);
    const float pivot = 0.5f;

    float3 below = pivot * pow(saturate(color / pivot), 1.0f / safeToe);
    float3 above = 1.0f - (1.0f - pivot) * pow(saturate((1.0f - color) / (1.0f - pivot)), safeShoulder);

    color = lerp(below, above, step(pivot.xxx, color));
    return saturate(color);
}

float3 GradeToRgby(GradeControl control)
{
    return control.Total.xxx + float3(control.Red + control.Yellow, control.Green + control.Yellow, control.Blue);
}

void ComputeToneWeights(float luma, out float shadows, out float midtones, out float highlights)
{
    shadows = 1.0f - smoothstep(0.20f, 0.50f, luma);
    highlights = smoothstep(0.55f, 0.85f, luma);
    midtones = saturate(1.0f - shadows - highlights);

    const float sum = max(shadows + midtones + highlights, 1e-5f);
    shadows /= sum;
    midtones /= sum;
    highlights /= sum;
}

float3 ResolveGradeAmount(GradeControl globalControl, GradeControl shadowsControl, GradeControl midtonesControl, GradeControl highlightsControl, float shadowsWeight, float midtonesWeight, float highlightsWeight)
{
    return GradeToRgby(globalControl)
        + GradeToRgby(shadowsControl) * shadowsWeight
        + GradeToRgby(midtonesControl) * midtonesWeight
        + GradeToRgby(highlightsControl) * highlightsWeight;
}

float3 ResolveGradeMultiplier(GradeControl globalControl, GradeControl shadowsControl, GradeControl midtonesControl, GradeControl highlightsControl, float shadowsWeight, float midtonesWeight, float highlightsWeight)
{
    return max(1.0f.xxx + ResolveGradeAmount(globalControl, shadowsControl, midtonesControl, highlightsControl, shadowsWeight, midtonesWeight, highlightsWeight), 0.001f.xxx);
}

float3 ApplyVibrance(float3 color, float3 vibrance)
{
    const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
    const float luma = dot(color, lumaWeights);
    const float maxChannel = max(max(color.r, color.g), color.b);
    const float minChannel = min(min(color.r, color.g), color.b);
    const float colorfulness = maxChannel - minChannel;
    const float3 amount = 1.0f.xxx + vibrance * (1.0f - colorfulness);
    return lerp(luma.xxx, color, max(amount, 0.0f.xxx));
}

float3 ApplyColorGrading(float3 color)
{
    color = SanitizeColor(color);

    const float3 lumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
    const float luma = dot(color, lumaWeights);

    float shadowsWeight = 0.0f;
    float midtonesWeight = 0.0f;
    float highlightsWeight = 0.0f;
    ComputeToneWeights(luma, shadowsWeight, midtonesWeight, highlightsWeight);

    const float3 contrast = ResolveGradeMultiplier(GlobalGrade.Contrast, ShadowsGrade.Contrast, MidtonesGrade.Contrast, HighlightsGrade.Contrast, shadowsWeight, midtonesWeight, highlightsWeight);
    const float3 gamma = ResolveGradeMultiplier(GlobalGrade.Gamma, ShadowsGrade.Gamma, MidtonesGrade.Gamma, HighlightsGrade.Gamma, shadowsWeight, midtonesWeight, highlightsWeight);
    const float3 gain = ResolveGradeMultiplier(GlobalGrade.Gain, ShadowsGrade.Gain, MidtonesGrade.Gain, HighlightsGrade.Gain, shadowsWeight, midtonesWeight, highlightsWeight);
    const float3 saturation = ResolveGradeMultiplier(GlobalGrade.Saturation, ShadowsGrade.Saturation, MidtonesGrade.Saturation, HighlightsGrade.Saturation, shadowsWeight, midtonesWeight, highlightsWeight);
    const float3 vibrance = ResolveGradeAmount(GlobalGrade.Vibrance, ShadowsGrade.Vibrance, MidtonesGrade.Vibrance, HighlightsGrade.Vibrance, shadowsWeight, midtonesWeight, highlightsWeight);

    color = ApplyContrast(color, contrast);
    color = pow(max(color, 0.0f.xxx), 1.0f.xxx / max(gamma, 0.001f.xxx));
    color *= gain;
    color = ApplySaturation(color, saturation);
    color = ApplyVibrance(color, vibrance);
    return saturate(SanitizeColor(color));
}

float3 AgxEncode(float3 color)
{
    // AgX input matrix (converts sRGB / Rec.709 primaries to an internal working space).
    static const float3x3 agxInputMatrix = float3x3(
         0.842479062253094f,   0.0784335999999992f, 0.0792237451477643f,
         0.0423282422610123f,  0.878468636469772f,  0.0791661274605434f,
         0.0423756549057051f,  0.0784336f,          0.879142973793104f
    );

    color = SanitizeColor(color);
    color = mul(agxInputMatrix, color);
    color = SanitizeColor(color);

    // Encode over AgX's fixed window. Exposure has already placed the scene
    // inside it, so there is nothing scene-dependent left to do here.
    const float3 logColor = log2(max(color, 1e-10f.xxx));
    color = saturate((logColor - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv));

    // Apply the default contrast sigmoid.
    return AgxDefaultContrastApprox(color);
}

float3 AgxEotf(float3 color)
{
    static const float3x3 agxOutputMatrix = float3x3(
         1.19687900512017f,   -0.0980208811401368f, -0.0990297440797205f,
        -0.0528968517574562f,  1.15190312990417f,   -0.0989611768448433f,
        -0.0529716355144438f, -0.0980434501171241f,  1.15107367264116f
    );

    color = mul(agxOutputMatrix, color);
    color = SanitizeColor(color);

    // Convert from the AgX encoded display signal to display-linear light.
    return SanitizeColor(pow(max(color, 0.0f.xxx), 2.2f.xxx));
}

// ---------------------------------------------------------------------------
// Main compute entry point
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= FrameWidth || dispatchId.y >= FrameHeight)
        return;

    float4 inputColor = gInput.Load(int3(dispatchId.xy, 0));

    float3 color = SanitizeColor(inputColor.rgb);

    // 1. Exposure. In manual mode Ev100 is used as authored; in automatic mode
    //    the histogram meter supplies it, already clamped and time-smoothed.
    //    Either way it is held inside [Ev100Min, Ev100Max]. The Exposure slider
    //    is a trim on top, in stops, positive = brighter - which is also how
    //    the automatic exposure is biased, applied after the clamp.
    float ev100 = Ev100;
    if (UseAutoExposure != 0)
    {
        const float metered = asfloat(gAutoExposure.Load(0));
        ev100 = (isnan(metered) || isinf(metered)) ? Ev100 : metered;
    }
    ev100 = clamp(ev100, min(Ev100Min, Ev100Max), max(Ev100Min, Ev100Max));
    color *= Ev100ToExposureScale(ev100) * exp2(Exposure);
    color = SanitizeColor(color);

    // 2. AgX base transform into encoded display space.
    color = AgxEncode(color);

    // 3. Tonal shaping in the encoded domain.
    color = ApplyToeShoulder(color, ToeStrength, ShoulderStrength);

    // 4. Convert AgX encoded signal to display-linear light for the SDR swap chain.
    color = AgxEotf(color);

    // 5. Apply color grading in display-linear space.
    color = ApplyColorGrading(color);

    gOutput[dispatchId.xy] = float4(SanitizeColor(color), 1.0f);
}
