// TAAResolve.hlsl - Temporal anti-aliasing and temporal upsampling (TAAU).
//
// A port of Unreal Engine 5's "Gen 4" temporal AA (Engine/Shaders/Private/
// TemporalAA.usf), for its two main configurations:
//
//   TAA_UPSAMPLE 0 - TAA_PASS_CONFIG Main            (input and output are one size)
//   TAA_UPSAMPLE 1 - TAA_PASS_CONFIG MainUpsampling  (TAAU: input at render size,
//                                                    output and history at display size)
//
// and its four qualities (r.TemporalAA.Quality): 0 Low, 1 Medium, 2 High,
// 3 Medium-High. Everything that decides how the image looks is kept as Unreal
// has it - the 3x3 jitter-aware reconstruction filter (or the upsampling filter
// with adaptive kernel size), Karis HDR weighting, YCoCg neighbourhood clamping
// (min/max, or sample distance for High TAAU), 5-tap Catmull-Rom history
// sampling, nearest-depth velocity dilation, dynamic anti-ghosting, the
// velocity-driven blend factor and the anti-flicker floor.
//
// What differs is only the plumbing around the engine:
//   - Depth is standard Z (0 near, 1 far) instead of Unreal's reversed Z. It is
//     flipped on load so the "larger is nearer" comparisons stay as written.
//   - Unreal's velocity buffer only holds moving objects. This engine's motion
//     vectors hold camera + object motion for every mesh and 0 where nothing was
//     drawn, so a pixel counts as "dynamic" where its motion vector disagrees
//     with the motion the camera alone gives it from depth.
//   - Exposure is the tonemapper's EV100 (manual, or metered last frame), in
//     place of Unreal's eye adaptation; the scene has no pre-exposure.
//   - The responsive-AA stencil bit does not exist here.
//
// Root signature (see TAARenderer.cpp):
//   b0       - TaaConstants
//   t0..t4   - scene colour, scene depth, motion vectors, history, exposure buffer
//   u0       - output (becomes next frame's history)
//   s0       - linear clamp sampler

#ifndef TAA_UPSAMPLE
    #define TAA_UPSAMPLE 0
#endif

#ifndef TAA_QUALITY
    #define TAA_QUALITY 2
#endif

#define TAA_QUALITY_LOW         0
#define TAA_QUALITY_MEDIUM      1
#define TAA_QUALITY_HIGH        2
#define TAA_QUALITY_MEDIUM_HIGH 3

#define HISTORY_CLAMPING_BOX_MIN_MAX         0
#define HISTORY_CLAMPING_BOX_SAMPLE_DISTANCE 2

//------------------------------------------------------- CONFIG (TemporalAA.usf)

#if TAA_UPSAMPLE == 0 // TAA_PASS_CONFIG == 0, Main
    #define AA_BICUBIC 1
    #define AA_CROSS 2
    #define AA_DYNAMIC 1
    #define AA_MANUALLY_CLAMP_HISTORY_UV 1
    #define AA_TONE 1
    #define AA_YCOCG 1

    #if TAA_QUALITY == TAA_QUALITY_LOW
        #define AA_FILTERED 0
        #define AA_DYNAMIC_ANTIGHOST 0
    #elif TAA_QUALITY == TAA_QUALITY_MEDIUM
        #define AA_FILTERED 1
        #define AA_DYNAMIC_ANTIGHOST 0
    #else // HIGH, MEDIUM_HIGH
        #define AA_FILTERED 1
        #define AA_DYNAMIC_ANTIGHOST 1
    #endif

#else // TAA_PASS_CONFIG == 1, MainUpsampling
    #define AA_BICUBIC 1
    #define AA_CROSS 1
    #define AA_DYNAMIC 1
    #define AA_MANUALLY_CLAMP_HISTORY_UV 1
    #define AA_TONE 1
    #define AA_YCOCG 1
    #define AA_UPSAMPLE 1
    #define AA_UPSAMPLE_ADAPTIVE_FILTERING 1

    #if TAA_QUALITY == TAA_QUALITY_LOW
        #define AA_FILTERED 0
        #define AA_SAMPLES 6
    #elif TAA_QUALITY == TAA_QUALITY_MEDIUM
        #define AA_FILTERED 1
        #define AA_SAMPLES 6
    #elif TAA_QUALITY == TAA_QUALITY_HIGH
        #define AA_HISTORY_CLAMPING_BOX (HISTORY_CLAMPING_BOX_SAMPLE_DISTANCE)
        #define AA_FILTERED 1
        #define AA_DYNAMIC_ANTIGHOST 1
        #define AA_SAMPLES 9
    #else // MEDIUM_HIGH
        #define AA_FILTERED 1
        #define AA_SAMPLES 6
        #define AA_DYNAMIC_ANTIGHOST 1
    #endif
#endif

#ifndef AA_SAMPLES
    #define AA_SAMPLES 5
#endif
#ifndef AA_DYNAMIC_ANTIGHOST
    #define AA_DYNAMIC_ANTIGHOST 0
#endif
#ifndef AA_UPSAMPLE
    #define AA_UPSAMPLE 0
#endif
#ifndef AA_UPSAMPLE_ADAPTIVE_FILTERING
    #define AA_UPSAMPLE_ADAPTIVE_FILTERING 0
#endif
#ifndef AA_HISTORY_CLAMPING_BOX
    #define AA_HISTORY_CLAMPING_BOX (HISTORY_CLAMPING_BOX_MIN_MAX)
#endif

// Downsampling (screen percentage above 100%) is not offered.
#define AA_DOWNSAMPLE 0

#define THREADGROUP_SIZEX 8
#define THREADGROUP_SIZEY 8

//------------------------------------------------------- PARAMETERS

cbuffer TaaConstants : register(b0)
{
    // Inverse(current view-projection) * previous view-projection, both without
    // jitter, transposed for mul(v, M). Maps this frame's NDC to last frame's clip.
    float4x4 ClipToPrevClip;

    float4 InputSceneColorSize;          // render size, 1 / render size
    int2   InputMinPixelCoord;
    int2   InputMaxPixelCoord;

    float4 OutputViewportSize;           // output size, 1 / output size
    float4 HistoryBufferSize;            // history size, 1 / history size
    float4 HistoryBufferUVMinMax;
    float4 ScreenPosToHistoryBufferUV;

    float2 ScreenPosAbsMax;
    float2 TemporalJitterPixels;         // render pixels, +Y down

    float4 InputViewSize;                // render size, 1 / render size
    float2 InputViewMin;
    float  ScreenPercentage;             // render / output, per axis
    float  UpscaleFactor;                // output / render, per axis

    float4 SampleWeights[3];             // 9 used
    float4 PlusWeights[2];               // 5 used

    float  CurrentFrameWeight;
    int    bCameraCut;
    uint   bVelocityAvailable;
    float  HistoryPreExposureCorrection;

    // Exposure, matching AgxTonemap.hlsl so the HDR weighting sees the image the
    // way the player will.
    float  Ev100;
    float  Ev100Min;
    float  Ev100Max;
    float  ExposureTrim;
    uint   UseAutoExposure;
    float  DynamicThresholdPixels;
    float2 _Pad0;
};

Texture2D<float4>   InputSceneColor   : register(t0);
Texture2D<float>    SceneDepthTexture : register(t1);
Texture2D<float2>   VelocityTexture   : register(t2);
Texture2D<float4>   HistoryBuffer     : register(t3);
ByteAddressBuffer   AutoExposure      : register(t4);
RWTexture2D<float4> OutComputeTex     : register(u0);
SamplerState        LinearClampSampler : register(s0);

#define GET_SAMPLE_WEIGHT(i) (SampleWeights[(i) / 4][(i) % 4])
#define GET_PLUS_WEIGHT(i)   (PlusWeights[(i) / 4][(i) % 4])

static const float MaxHalfFloat = 65504.0f;

//------------------------------------------------------- CONSTANTS

static const int2 kOffsets3x3[9] =
{
    int2(-1, -1),
    int2( 0, -1),
    int2( 1, -1),
    int2(-1,  0),
    int2( 0,  0), // K
    int2( 1,  0),
    int2(-1,  1),
    int2( 0,  1),
    int2( 1,  1),
};

static const uint kSquareIndexes3x3[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint kPlusIndexes3x3[5] = { 1, 3, 4, 5, 7 };
static const uint kNeighborsCount = 9;

//------------------------------------------------------- HELPERS

float Pow2(float x) { return x * x; }
float4 Pow2(float4 x) { return x * x; }
float max3(float a, float b, float c) { return max(a, max(b, c)); }
float4 min3(float4 a, float4 b, float4 c) { return min(a, min(b, c)); }
float4 max3(float4 a, float4 b, float4 c) { return max(a, max(b, c)); }

int2 SignFastInt(float2 v)
{
    return int2(v.x >= 0.0 ? 1 : -1, v.y >= 0.0 ? 1 : -1);
}

float2 ViewportUVToScreenPos(float2 ViewportUV)
{
    return float2(2.0 * ViewportUV.x - 1.0, 1.0 - 2.0 * ViewportUV.y);
}

// Engine depth (0 near, 1 far) to Unreal's convention (1 near, 0 far), which
// the nearest-depth search below is written for.
float LoadDeviceZ(int2 Coord)
{
    Coord = clamp(Coord, InputMinPixelCoord, InputMaxPixelCoord);
    return 1.0 - SceneDepthTexture.Load(int3(Coord, 0));
}

float Ev100ToExposureScale(float ev100)
{
    return 1.0f / max(1.2f * exp2(ev100), 1e-6f);
}

float EyeAdaptationLookup()
{
    float ev100 = Ev100;
    if (UseAutoExposure != 0)
    {
        const float metered = asfloat(AutoExposure.Load(0));
        ev100 = (isnan(metered) || isinf(metered)) ? Ev100 : metered;
    }
    ev100 = clamp(ev100, min(Ev100Min, Ev100Max), max(Ev100Min, Ev100Max));
    return Ev100ToExposureScale(ev100) * exp2(ExposureTrim);
}

//------------------------------------------------------- VELOCITY

// Motion the camera alone gives a point at ScreenPos with Unreal-convention
// depth DeviceZ, as ScreenPos - PrevScreenPos.
float2 ComputeCameraBackVector(float2 ScreenPos, float DeviceZ)
{
    float4 ThisClip = float4(ScreenPos, 1.0 - DeviceZ, 1);
    float4 PrevClip = mul(ThisClip, ClipToPrevClip);
    float2 PrevScreen = PrevClip.xy / PrevClip.w;
    return ScreenPos - PrevScreen;
}

// The engine stores (current - previous) in texture UV, +Y down. Unreal's back
// vector is the same difference in screen position, +Y up, twice the range.
float2 DecodeVelocity(float2 EncodedVelocity)
{
    return float2(2.0 * EncodedVelocity.x, -2.0 * EncodedVelocity.y);
}

float2 LoadEncodedVelocity(int2 Coord)
{
    if (bVelocityAvailable == 0)
        return float2(0, 0);
    Coord = clamp(Coord, InputMinPixelCoord, InputMaxPixelCoord);
    return VelocityTexture.Load(int3(Coord, 0));
}

// Whether a motion vector was written there at all. Empty pixels are cleared to 0.
bool IsVelocityWritten(float2 EncodedVelocity)
{
    return any(EncodedVelocity != 0.0);
}

// Unreal's "dynamic" test: the pixel belongs to something that moves on its own.
//
// The motion vectors are rasterised without jitter while depth is jittered, so
// along a silhouette a pixel's vector can belong to the surface beside the one
// its depth describes. Camera-only motion at a fixed screen position runs along
// a line as depth varies, so the vector is compared against that line between
// the nearest and farthest depth of its plus neighbourhood rather than against
// one depth: whatever surface the vector came from, a static one lands on it.
bool IsDynamicPixel(int2 Coord, out float2 Encoded)
{
    Encoded = LoadEncodedVelocity(Coord);
    if (!IsVelocityWritten(Encoded))
        return false;

    float NearZ = LoadDeviceZ(Coord);
    float FarZ = NearZ;
    [unroll]
    for (uint i = 0; i < 5; i++)
    {
        const float Z = LoadDeviceZ(Coord + kOffsets3x3[kPlusIndexes3x3[i]]);
        NearZ = max(NearZ, Z);
        FarZ = min(FarZ, Z);
    }

    const float2 ToPixels = 0.5 * InputSceneColorSize.xy;
    const float2 ScreenPos = ViewportUVToScreenPos((float2(Coord) + 0.5) * InputSceneColorSize.zw);
    const float2 A = ComputeCameraBackVector(ScreenPos, NearZ) * ToPixels;
    const float2 B = ComputeCameraBackVector(ScreenPos, FarZ) * ToPixels;
    const float2 P = DecodeVelocity(Encoded) * ToPixels;

    const float2 AB = B - A;
    const float T = saturate(dot(P - A, AB) / max(dot(AB, AB), 1e-8));
    const float Distance = length(P - (A + T * AB));
    return Distance > DynamicThresholdPixels + 0.02 * max(length(A), length(B));
}

bool IsDynamicPixel(int2 Coord)
{
    float2 Encoded;
    return IsDynamicPixel(Coord, Encoded);
}

//------------------------------------------------------- COLOUR SPACE

float3 RGBToYCoCg(float3 RGB)
{
    float Y  = dot(RGB, float3( 1, 2,  1));
    float Co = dot(RGB, float3( 2, 0, -2));
    float Cg = dot(RGB, float3(-1, 2, -1));
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 YCoCg)
{
    float Y  = YCoCg.x * 0.25;
    float Co = YCoCg.y * 0.25;
    float Cg = YCoCg.z * 0.25;
    return float3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
}

float Luma4(float3 Color)
{
    return (Color.g * 2.0) + (Color.r + Color.b);
}

float HdrWeight4(float3 Color, float Exposure)
{
    return rcp(Luma4(Color) * Exposure + 4.0);
}

float HdrWeightY(float Color, float Exposure)
{
    return rcp(Color * Exposure + 4.0);
}

float2 WeightedLerpFactors(float WeightA, float WeightB, float Blend)
{
    float BlendA = (1.0 - Blend) * WeightA;
    float BlendB = Blend * WeightB;
    float RcpBlend = rcp(BlendA + BlendB);
    BlendA *= RcpBlend;
    BlendB *= RcpBlend;
    return float2(BlendA, BlendB);
}

float4 TransformSceneColor(float4 RawLinearSceneColorRGBA)
{
#if AA_YCOCG
    return float4(RGBToYCoCg(RawLinearSceneColorRGBA.rgb), RawLinearSceneColorRGBA.a);
#else
    return RawLinearSceneColorRGBA;
#endif
}

float4 TransformBackToRawLinearSceneColor(float4 SceneColor)
{
#if AA_YCOCG
    return float4(YCoCgToRGB(SceneColor.xyz), SceneColor.a);
#else
    return SceneColor;
#endif
}

float GetSceneColorLuma4(float4 SceneColor)
{
#if AA_YCOCG
    return SceneColor.x;
#else
    return Luma4(SceneColor.rgb);
#endif
}

//------------------------------------------------------- INTERMEDIARY STRUCTURES

struct FTAAInputParameters
{
    float2 ViewportUV;
    float2 ScreenPos;
    float2 NearestBufferUV;
#if AA_UPSAMPLE
    float2 NearestTopLeftBufferUV;
#endif
    float bIsResponsiveAAPixel;
    float FrameExposureScale;
    float4 CachedNeighbors[kNeighborsCount];
};

struct FTAAIntermediaryResult
{
    float4 Filtered;
    float FilteredTemporalWeight;
    float InvFilterScaleFactor;
};

struct FTAASceneColorSample
{
    float4 Color;
    float HdrWeight;
};

float GetSceneColorHdrWeight(in FTAAInputParameters InputParams, float4 SceneColor)
{
#if AA_YCOCG
    return HdrWeightY(SceneColor.x, InputParams.FrameExposureScale);
#else
    return HdrWeight4(SceneColor.rgb, InputParams.FrameExposureScale);
#endif
}

int2 NearestInputPixel(in FTAAInputParameters InputParams)
{
    return int2(InputParams.NearestBufferUV * InputSceneColorSize.xy);
}

// AA_SAMPLE_CACHE_METHOD_VGPR_3X3: the 3x3 neighbourhood around the nearest
// input pixel is transformed once and kept in registers.
void PrecacheInputSceneColor(inout FTAAInputParameters InputParams)
{
    [unroll]
    for (uint i = 0; i < kNeighborsCount; i++)
    {
        int2 Coord = NearestInputPixel(InputParams) + kOffsets3x3[i];
        Coord = clamp(Coord, InputMinPixelCoord, InputMaxPixelCoord);
        float4 Raw = InputSceneColor[Coord];
        // A NaN or Inf in the scene would otherwise spread through the history.
        Raw = (any(isnan(Raw)) || any(isinf(Raw))) ? float4(0, 0, 0, Raw.a) : Raw;
        InputParams.CachedNeighbors[i] = TransformSceneColor(float4(Raw.rgb, 0.0));
    }
}

FTAASceneColorSample SampleCachedSceneColorTexture(in FTAAInputParameters InputParams, int2 PixelOffset)
{
    uint NeighborsId = uint(4 + PixelOffset.x + PixelOffset.y * 3);
    FTAASceneColorSample Sample;
    Sample.Color = InputParams.CachedNeighbors[NeighborsId];
    Sample.HdrWeight = GetSceneColorHdrWeight(InputParams, Sample.Color);
    return Sample;
}

float SampleCachedSceneDepthTexture(in FTAAInputParameters InputParams, int2 PixelOffset)
{
    return LoadDeviceZ(NearestInputPixel(InputParams) + PixelOffset);
}

//------------------------------------------------------- TEMPORAL UPSAMPLING

#if AA_UPSAMPLE

float ComputeSampleWeigth(in FTAAIntermediaryResult IntermediaryResult, float2 PixelDelta)
{
    float u2 = UpscaleFactor * UpscaleFactor;

    // The point of InvFilterScaleFactor is to blur current frame scene color when upscaling.
    if (!AA_DOWNSAMPLE)
    {
        u2 *= (IntermediaryResult.InvFilterScaleFactor * IntermediaryResult.InvFilterScaleFactor);
    }

    // 1 - 1.9 * x^2 + 0.9 * x^4
    float x2 = saturate(u2 * dot(PixelDelta, PixelDelta));
    return (0.905 * x2 - 1.9) * x2 + 1;
}

float ComputePixelWeigth(in FTAAIntermediaryResult IntermediaryResult, float2 PixelDelta)
{
    float u2 = UpscaleFactor * UpscaleFactor;

    if (!AA_DOWNSAMPLE)
    {
        u2 *= (IntermediaryResult.InvFilterScaleFactor * IntermediaryResult.InvFilterScaleFactor);
    }

    float x2 = saturate(u2 * dot(PixelDelta, PixelDelta));
    float r = (0.905 * x2 - 1.9) * x2 + 1;

    if (!AA_DOWNSAMPLE)
    {
        // Multiply pixel weight ^ 2 by upscale factor because have only a probability = screen percentage ^ 2 to return 1.
        // Only do it for upsampling to not converge slower than if doing screen percentage 100%.
        return u2 * r;
    }
    return r;
}

#endif // AA_UPSAMPLE

//------------------------------------------------------- TAA MAJOR FUNCTIONS

void FilterCurrentFrameInputSamples(
    in FTAAInputParameters InputParams,
    inout FTAAIntermediaryResult IntermediaryResult)
{
#if !AA_FILTERED
    {
        IntermediaryResult.Filtered = SampleCachedSceneColorTexture(InputParams, int2(0, 0)).Color;
        return;
    }
#else
    float4 Filtered;
    {
    #if AA_UPSAMPLE
        // Pixel coordinate of the center of output pixel O in the input viewport.
        float2 PPCo = InputParams.ViewportUV * InputViewSize.xy + TemporalJitterPixels;
        // Pixel coordinate of the center of the nearest input pixel K.
        float2 PPCk = floor(PPCo) + 0.5;
        // Vector in pixel between pixel K -> O.
        float2 dKO = PPCo - PPCk;
    #endif

    #if AA_SAMPLES == 9
        const uint SampleIndexes[9] = kSquareIndexes3x3;
    #else
        const uint SampleIndexes[5] = kPlusIndexes3x3;
    #endif

        float NeighborsFinalWeight = 0;
        float4 NeighborsColor = 0;

        [unroll]
        for (uint i = 0; i < AA_SAMPLES; i++)
        {
            // Get the sample offset from the nearest input pixel.
            int2 SampleOffset;

        #if AA_UPSAMPLE && AA_SAMPLES == 6
            if (i == 5)
            {
                SampleOffset = SignFastInt(dKO);
            }
            else
        #endif
            {
                const uint SampleIndex = SampleIndexes[i];
                SampleOffset = kOffsets3x3[SampleIndex];
            }
            float2 fSampleOffset = float2(SampleOffset);

            // Finds out the spatial weight of this input sample.
        #if AA_UPSAMPLE
            float2 dPP = fSampleOffset - dKO;
            float SampleSpatialWeight = ComputeSampleWeigth(IntermediaryResult, dPP);
        #elif AA_SAMPLES == 9
            float SampleSpatialWeight = GET_SAMPLE_WEIGHT(i);
        #else
            float SampleSpatialWeight = GET_PLUS_WEIGHT(i);
        #endif

            FTAASceneColorSample Sample = SampleCachedSceneColorTexture(InputParams, SampleOffset);

        #if AA_TONE
            float SampleHdrWeight = Sample.HdrWeight;
        #else
            float SampleHdrWeight = 1;
        #endif

            float SampleFinalWeight = SampleSpatialWeight * SampleHdrWeight;

            NeighborsColor       += SampleFinalWeight * Sample.Color;
            NeighborsFinalWeight += SampleFinalWeight;
        }

    #if AA_TONE || AA_UPSAMPLE
        // Reweight because SampleFinalWeight does not that have total sum = 1.
        Filtered = NeighborsColor * rcp(NeighborsFinalWeight);
    #else
        Filtered = NeighborsColor;
    #endif

    #if AA_UPSAMPLE
        // Compute the temporal weight of the output pixel.
        IntermediaryResult.FilteredTemporalWeight = ComputePixelWeigth(IntermediaryResult, dKO);
    #endif
    }

    IntermediaryResult.Filtered = Filtered;
#endif
}

void ComputeNeighborhoodBoundingbox(
    in FTAAInputParameters InputParams,
    in FTAAIntermediaryResult IntermediaryResult,
    out float4 OutNeighborMin,
    out float4 OutNeighborMax)
{
    float4 Neighbors[kNeighborsCount];
    [unroll]
    for (uint i = 0; i < kNeighborsCount; i++)
    {
        Neighbors[i] = SampleCachedSceneColorTexture(InputParams, kOffsets3x3[i]).Color;
    }

    float4 NeighborMin;
    float4 NeighborMax;

#if AA_HISTORY_CLAMPING_BOX == HISTORY_CLAMPING_BOX_SAMPLE_DISTANCE
    // Do color clamping only within a radius.
    {
        float2 PPCo = InputParams.ViewportUV * InputViewSize.xy + TemporalJitterPixels;
        float2 PPCk = floor(PPCo) + 0.5;
        float2 dKO = PPCo - PPCk;

        // Sample 4 is is always going to be considered anyway.
        NeighborMin = Neighbors[4];
        NeighborMax = Neighbors[4];

        // Reduce distance threshold as upsacale factor increase to reduce ghosting.
        float DistthresholdLerp = UpscaleFactor - 1;
        float DistThreshold = lerp(1.51, 1.3, DistthresholdLerp);

    #if AA_SAMPLES == 9
        const uint Indexes[9] = kSquareIndexes3x3;
    #else
        const uint Indexes[5] = kPlusIndexes3x3;
    #endif

        [unroll]
        for (uint i = 0; i < AA_SAMPLES; i++)
        {
            uint NeightborId = Indexes[i];
            if (NeightborId != 4)
            {
                float2 dPP = float2(kOffsets3x3[NeightborId]) - dKO;

                [flatten]
                if (dot(dPP, dPP) < (DistThreshold * DistThreshold))
                {
                    NeighborMin = min(NeighborMin, Neighbors[NeightborId]);
                    NeighborMax = max(NeighborMax, Neighbors[NeightborId]);
                }
            }
        }
    }
#else // HISTORY_CLAMPING_BOX_MIN_MAX
    {
        NeighborMin = min3(Neighbors[1], Neighbors[3], Neighbors[4]);
        NeighborMin = min3(NeighborMin,  Neighbors[5], Neighbors[7]);

        NeighborMax = max3(Neighbors[1], Neighbors[3], Neighbors[4]);
        NeighborMax = max3(NeighborMax,  Neighbors[5], Neighbors[7]);

    #if AA_SAMPLES == 6
        {
            float2 PPCo = InputParams.ViewportUV * InputViewSize.xy + TemporalJitterPixels;
            float2 PPCk = floor(PPCo) + 0.5;
            float2 dKO = PPCo - PPCk;

            int2 FifthNeighborOffset = SignFastInt(dKO);
            float4 FifthNeighbor = SampleCachedSceneColorTexture(InputParams, FifthNeighborOffset).Color;

            NeighborMin = min(NeighborMin, FifthNeighbor);
            NeighborMax = max(NeighborMax, FifthNeighbor);
        }
    #elif AA_SAMPLES == 9
        {
            NeighborMin = min3(NeighborMin, Neighbors[0], Neighbors[2]);
            NeighborMin = min3(NeighborMin, Neighbors[6], Neighbors[8]);

            NeighborMax = max3(NeighborMax, Neighbors[0], Neighbors[2]);
            NeighborMax = max3(NeighborMax, Neighbors[6], Neighbors[8]);
        }
    #endif
    }
#endif

    OutNeighborMin = NeighborMin;
    OutNeighborMax = NeighborMax;
}

// Sample the history using Catmull-Rom (5 bilinear taps) to reduce blur on motion.
float4 SampleHistory(in float2 HistoryScreenPosition)
{
    float4 RawHistory = 0;

    float2 HistoryBufferUV = HistoryScreenPosition * ScreenPosToHistoryBufferUV.xy + ScreenPosToHistoryBufferUV.zw;

    // Clamp HistoryBufferUV so that we don't have to do it entirely for each samples.
#if AA_MANUALLY_CLAMP_HISTORY_UV
    HistoryBufferUV = clamp(HistoryBufferUV, HistoryBufferUVMinMax.xy, HistoryBufferUVMinMax.zw);
#endif

    // GetBicubic2DCatmullRomSamples (TextureSampling.ush)
    const float2 Size = HistoryBufferSize.xy;
    const float2 InvSize = HistoryBufferSize.zw;
    float2 PixelCoord = HistoryBufferUV * Size;

    float2 iuv = floor(PixelCoord - 0.5) + 1.0;
    float2 f = PixelCoord - iuv;
    float2 f2 = f * f;
    float2 f_2 = f + f;

    float2 uv = (1.25 - f2) * f + 0.5;
    float2 t0 = (0.25 * f2 - 0.0625) * (1.125 - 0.5 * f2.yx);
    float2 w0 = t0 - f_2 * t0;
    float2 w3 = t0 + f_2 * t0;

    float2 cuv = (iuv - 0.5) * InvSize;
    uv *= InvSize;

    float2 SampleUV[5];
    SampleUV[0] = cuv + float2(-InvSize.x, uv.y);
    SampleUV[1] = cuv + float2(2.0 * InvSize.x, uv.y);
    SampleUV[2] = cuv + uv;
    SampleUV[3] = cuv + float2(uv.x, -InvSize.y);
    SampleUV[4] = cuv + float2(uv.x, 2.0 * InvSize.y);

    float SampleWeight[5];
    SampleWeight[0] = w0.x;
    SampleWeight[1] = w3.x;
    SampleWeight[2] = 1.0 - w0.x - w3.x - w0.y - w3.y;
    SampleWeight[3] = w0.y;
    SampleWeight[4] = w3.y;

    static const int2 UVDir[5] = { int2(-1, 0), int2(1, 0), int2(0, 0), int2(0, -1), int2(0, 1) };

    [unroll]
    for (uint i = 0; i < 5; i++)
    {
        float2 UV = SampleUV[i];

        // Clamp SampleUV within HistoryBufferUVMinMax to avoid sampling outside the view.
    #if AA_MANUALLY_CLAMP_HISTORY_UV
        if (UVDir[i].x < 0)
            UV.x = max(UV.x, HistoryBufferUVMinMax.x);
        else if (UVDir[i].x > 0)
            UV.x = min(UV.x, HistoryBufferUVMinMax.z);

        if (UVDir[i].y < 0)
            UV.y = max(UV.y, HistoryBufferUVMinMax.y);
        else if (UVDir[i].y > 0)
            UV.y = min(UV.y, HistoryBufferUVMinMax.w);
    #endif

        RawHistory += HistoryBuffer.SampleLevel(LinearClampSampler, UV, 0) * SampleWeight[i];
    }

    RawHistory.rgb *= HistoryPreExposureCorrection;
    return TransformSceneColor(RawHistory);
}

float4 ClampHistory(float4 History, float4 NeighborMin, float4 NeighborMax)
{
    return clamp(History, NeighborMin, NeighborMax);
}

//------------------------------------------------------- TAA MAIN FUNCTION

float4 TemporalAASample(float2 ViewportUV, float FrameExposureScale)
{
    // SETUP
    FTAAInputParameters InputParams;
    InputParams.FrameExposureScale = FrameExposureScale;
    InputParams.ViewportUV = ViewportUV;
    InputParams.ScreenPos = ViewportUVToScreenPos(ViewportUV);
    // Input and output share one viewport, so without upsampling the nearest
    // input pixel is the output pixel itself.
    InputParams.NearestBufferUV = ViewportUV;
    InputParams.bIsResponsiveAAPixel = 0.0;

#if AA_UPSAMPLE
    {
        // Pixel coordinate of the center of output pixel O in the input viewport.
        float2 PPCo = ViewportUV * InputViewSize.xy + TemporalJitterPixels;
        // Pixel coordinate of the center of the nearest input pixel K.
        float2 PPCk = floor(PPCo) + 0.5;
        // Pixel coordinate of the center of the nearest top left input pixel T.
        float2 PPCt = floor(PPCo - 0.5) + 0.5;

        InputParams.NearestBufferUV = InputSceneColorSize.zw * (InputViewMin + PPCk);
        InputParams.NearestTopLeftBufferUV = InputSceneColorSize.zw * (InputViewMin + PPCt);
    }
#endif

    FTAAIntermediaryResult IntermediaryResult;
    IntermediaryResult.Filtered = 0;
    IntermediaryResult.FilteredTemporalWeight = 1;
    IntermediaryResult.InvFilterScaleFactor = 1;

    // FIND MOTION OF PIXEL AND NEAREST IN NEIGHBORHOOD
    float3 PosN; // Position of this pixel, possibly later nearest pixel in neighborhood.
    PosN.xy = InputParams.ScreenPos;
    PosN.z = SampleCachedSceneDepthTexture(InputParams, int2(0, 0));

    // Pixel offset of minimum depth.
    int2 VelocityOffset = int2(0, 0);
#if AA_CROSS
    {
        // For motion vector, use camera/dynamic motion from min depth pixel in pattern around pixel.
        // This enables better quality outline on foreground against different motion background.
        // Larger 2 pixel distance "x" works best (because AA dilates surface).
        float4 Depths;
        Depths.x = SampleCachedSceneDepthTexture(InputParams, int2(-AA_CROSS, -AA_CROSS));
        Depths.y = SampleCachedSceneDepthTexture(InputParams, int2( AA_CROSS, -AA_CROSS));
        Depths.z = SampleCachedSceneDepthTexture(InputParams, int2(-AA_CROSS,  AA_CROSS));
        Depths.w = SampleCachedSceneDepthTexture(InputParams, int2( AA_CROSS,  AA_CROSS));

        int2 DepthOffset = int2(AA_CROSS, AA_CROSS);
        int DepthOffsetXx = AA_CROSS;
        // Nearest depth is the largest depth (depth surface 0=far, 1=near).
        if (Depths.x > Depths.y)
        {
            DepthOffsetXx = -AA_CROSS;
        }
        if (Depths.z > Depths.w)
        {
            DepthOffset.x = -AA_CROSS;
        }
        float DepthsXY = max(Depths.x, Depths.y);
        float DepthsZW = max(Depths.z, Depths.w);
        if (DepthsXY > DepthsZW)
        {
            DepthOffset.y = -AA_CROSS;
            DepthOffset.x = DepthOffsetXx;
        }
        float DepthsXYZW = max(DepthsXY, DepthsZW);
        if (DepthsXYZW > PosN.z)
        {
            VelocityOffset = DepthOffset;
            PosN.z = DepthsXYZW;
        }
    }
#endif

    // Camera motion for pixel or nearest pixel (in ScreenPos space).
    bool OffScreen = false;
    float Velocity = 0;
    float2 HistoryScreenPosition = InputParams.ScreenPos;
    {
        float2 BackN = ComputeCameraBackVector(PosN.xy, PosN.z);
        float2 BackTemp = BackN * OutputViewportSize.xy;

    #if AA_DYNAMIC
        {
            // As in Unreal, the velocity buffer is only taken for things that move
            // on their own; everything else is reprojected from depth above. That
            // also keeps static meshes out of reach of the motion vector pass,
            // which draws without a depth test.
            float2 EncodedVelocity;
            bool DynamicN = IsDynamicPixel(NearestInputPixel(InputParams) + VelocityOffset, EncodedVelocity);
            if (DynamicN)
            {
                BackN = DecodeVelocity(EncodedVelocity);
            }
            BackTemp = BackN * OutputViewportSize.xy;
        }
    #endif

        Velocity = sqrt(dot(BackTemp, BackTemp));
        // BackN is in units of 2pixels/viewportWidthInPixels.
        // This converts back projection vector to [-1 to 1] offset in viewport.
        HistoryScreenPosition = InputParams.ScreenPos - BackN;

        // Detect if HistoryBufferUV would be outside of the viewport.
        OffScreen = max(abs(HistoryScreenPosition.x), abs(HistoryScreenPosition.y)) >= 1.0;
    }

    // Precache input scene color.
    PrecacheInputSceneColor(InputParams);

    // Filter input.
#if AA_UPSAMPLE_ADAPTIVE_FILTERING == 0
    FilterCurrentFrameInputSamples(InputParams, IntermediaryResult);
#endif

    // Compute neighborhood bounding box.
    float4 NeighborMin;
    float4 NeighborMax;
    ComputeNeighborhoodBoundingbox(InputParams, IntermediaryResult, NeighborMin, NeighborMax);

    // Sample history.
    float4 History = SampleHistory(HistoryScreenPosition);

    // Whether the feedback needs to be reset.
    bool IgnoreHistory = OffScreen || bCameraCut != 0;

    // DYNAMIC ANTI GHOSTING
#if AA_DYNAMIC_ANTIGHOST && AA_DYNAMIC
    bool Dynamic4;
    {
        // TODO: try a 2x2 for AA_UPSAMPLE
        const int2 K = NearestInputPixel(InputParams);
        bool Dynamic1 = IsDynamicPixel(K + int2( 0, -1));
        bool Dynamic3 = IsDynamicPixel(K + int2(-1,  0));
        Dynamic4 = IsDynamicPixel(K);
        bool Dynamic5 = IsDynamicPixel(K + int2( 1,  0));
        bool Dynamic7 = IsDynamicPixel(K + int2( 0,  1));

        bool Dynamic = Dynamic1 || Dynamic3 || Dynamic4 || Dynamic5 || Dynamic7;
        IgnoreHistory = IgnoreHistory || (!Dynamic && History.a > 0);
    }
#endif

    // Save off luma of history before the clamp.
    float LumaHistory = GetSceneColorLuma4(History);

    // Clamp history.
    History = ClampHistory(History, NeighborMin, NeighborMax);

    // Filter input after color clamping.
#if AA_UPSAMPLE_ADAPTIVE_FILTERING == 1
    {
        if (IgnoreHistory)
        {
            // Set the input filter infinitely large when we know need to rely on it.
            IntermediaryResult.InvFilterScaleFactor = 0;
        }

        // Blur input according to input pixel velocity to reduce blocky filtering cause by narrow filter on low screen percentage.
        // Multiplied by upscale factor because Velocity is in output resolution based pixel velocity.
        IntermediaryResult.InvFilterScaleFactor -= (Velocity * UpscaleFactor) * 0.1;

        // Set a minimal filtering scale to screen percentage to not unecessarily blur the input more than the screen percentage.
        IntermediaryResult.InvFilterScaleFactor = max(IntermediaryResult.InvFilterScaleFactor, ScreenPercentage);

        FilterCurrentFrameInputSamples(InputParams, IntermediaryResult);
    }
#endif

    // COMPUTE BLEND AMOUNT
    float BlendFinal;
    {
        float LumaFiltered = GetSceneColorLuma4(IntermediaryResult.Filtered);

        BlendFinal = IntermediaryResult.FilteredTemporalWeight * CurrentFrameWeight;

        BlendFinal = lerp(BlendFinal, 0.2, saturate(Velocity / 40));

        // Make sure to have at least some small contribution
        BlendFinal = max(BlendFinal, saturate(0.01 * LumaHistory / abs(LumaFiltered - LumaHistory)));

        // Responsive forces 1/4 of new frame.
        BlendFinal = InputParams.bIsResponsiveAAPixel ? (1.0 / 4.0) : BlendFinal;

        if (bCameraCut != 0)
        {
            BlendFinal = 1.0;
        }
    }

    // Offscreen feedback resets.
    if (IgnoreHistory)
    {
        History = IntermediaryResult.Filtered;
        History.a = 0.0;
    }

    // DO FINAL BLEND BETWEEN HISTORY AND FILTERED COLOR
    // Luma weighted blend
    float FilterWeight = GetSceneColorHdrWeight(InputParams, IntermediaryResult.Filtered.xxxx);
    float HistoryWeight = GetSceneColorHdrWeight(InputParams, History.xxxx);

    float4 OutputPayload;
    {
        float2 Weights = WeightedLerpFactors(HistoryWeight, FilterWeight, BlendFinal);
        OutputPayload = History * Weights.x + IntermediaryResult.Filtered * Weights.y;
    }

    OutputPayload = TransformBackToRawLinearSceneColor(OutputPayload);

    // Transform NaNs to black, transform negative colors to black.
    OutputPayload = -min(-OutputPayload, 0.0);

#if AA_DYNAMIC_ANTIGHOST && AA_DYNAMIC
    OutputPayload.a = Dynamic4 ? 1 : 0;
#else
    // Zero out to remove any prior computation of alpha
    OutputPayload.a = 0;
#endif

    return OutputPayload;
}

//------------------------------------------------------- ENTRY POINT

[numthreads(THREADGROUP_SIZEX, THREADGROUP_SIZEY, 1)]
void CSMain(uint2 DispatchThreadId : SV_DispatchThreadID)
{
    if (any(float2(DispatchThreadId) >= OutputViewportSize.xy))
        return;

    float2 ViewportUV = (float2(DispatchThreadId) + 0.5f) * OutputViewportSize.zw;

    float FrameExposureScale = EyeAdaptationLookup();
    float4 OutColor = TemporalAASample(ViewportUV, FrameExposureScale);

    OutComputeTex[DispatchThreadId] = min(MaxHalfFloat.xxxx, OutColor);
}
