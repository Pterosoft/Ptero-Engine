// RtGI_NrdPrepare.hlsl  –  cs_6_5
// Prepares the four NRD RELAX_DIFFUSE input textures from the G-Buffer and
// the raw (noisy) RayGen output.
//
// Outputs:
//   u_DiffRadianceHitDist  (RGBA16F)  –  RELAX_FrontEnd_PackRadianceAndHitDist()
//   u_NormalRoughness      (R10G10B10A2_UINT) –  NRD_FrontEnd_PackNormalAndRoughness() encoded as 10/10/10/2 integer components
//                                               (NRD later reads the same bits as R10G10B10A2_UNORM)
//   u_ViewZ                (R32F)     –  linear view-space Z (positive into screen)
//   u_MotionVectors        (RG16F)    –  screen-space motion  ( prev_uv - curr_uv )
//
// Root signature is the same 9-param layout as the other RTGI passes but we
// use more slots:
//   param 0 : CBV  b0  RtGIConstants
//   param 1 : SRV  t0  albedo G-buffer     (Texture2D RGBA8)
//   param 2 : SRV  t1  normalDepth G-buffer (Texture2D RGBA16F: xy=oct-normal, z=hw-depth)
//   param 3 : SRV  t2  material G-buffer    (Texture2D RGBA8: r=roughness)
//   param 4 : SRV  t3  GI radiance texture  (Texture2D RGBA16F from RayGen)
//   param 5 : UAV  u0  DiffRadianceHitDist  (RWTexture2D RGBA16F)
//   param 6 : UAV  u1  NormalRoughness      (RWTexture2D RGBA8)
//   param 7 : UAV  u2  ViewZ                (RWTexture2D R32F)
//   param 8 : UAV  u3  MotionVectors        (RWTexture2D RG16F)
//
// NOTE: all of u0/u1/u2/u3 use separate descriptor table params because the
// existing root signature assigns one descriptor per table slot. We reuse
// params 3..8 (SRV and UAV tables) but bind different descriptors.

#include "RtGI_Common.hlsli"

// ------ NRD HLSL front-end definitions (packing helpers only, no dispatch layout) ------
// We only need RELAX_FrontEnd_PackRadianceAndHitDist and
// NRD_FrontEnd_PackNormalAndRoughness from NRD.hlsli, so include it.
#define NRD_COMPILER_DXC
#include "NRD/NRD.hlsli"

// ─── Inputs ──────────────────────────────────────────────────────────────────
Texture2D<float4>  t_Albedo          : register(t0);   // RGB albedo (unused here but bound)
Texture2D<float4>  t_NormalDepth     : register(t1);   // xy: oct-normal [0..1], z: hw depth
Texture2D<float4>  t_Material        : register(t2);   // r=roughness, g=metallic, b=ao
Texture2D<float4>  t_GiRadiance      : register(t3);   // raw GI radiance (xyz) from RayGen

// ─── Outputs ─────────────────────────────────────────────────────────────────
RWTexture2D<float4> u_DiffRadianceHitDist : register(u0);  // RGBA16F
RWTexture2D<uint4>  u_NormalRoughness     : register(u1);  // R10G10B10A2_UINT view of the typeless normal/roughness texture
RWTexture2D<float>  u_ViewZ               : register(u2);  // R32F
RWTexture2D<float2> u_MotionVectors       : register(u3);  // RG16F

// ─────────────────────────────────────────────────────────────────────────────
// Linearize hardware depth (DX12 D32_FLOAT) to linear view-space Z.
// g_ViewProjInv reconstructs world from NDC; we use the projection parameters
// stored in the constant buffer instead for efficiency.
//
// For a DX12 reverse-Z (depth closer = 1.0) projection:
//   viewZ = near / depth    (valid only with infinite reverse-Z)
// For a standard (depth closer = 0.0) projection:
//   viewZ = (near * far) / (far - depth * (far - near))
//
// We derive viewZ directly from the reconstructed world position using the
// current non-jittered world-to-view matrix uploaded by the renderer.
// ─────────────────────────────────────────────────────────────────────────────
float ComputeViewZ(uint2 pixel, float hwDepth)
{
    // Reconstruct world position (shared helper from RtGI_Common.hlsli).
    float3 worldPos = ReconstructWorldPos(pixel, hwDepth);

    // The renderer uploads matrices transposed for row-vector multiplication,
    // so mul(float4(worldPos, 1), g_WorldToView) yields the current view-space position.
    float3 viewPos = mul(float4(worldPos, 1.0f), g_WorldToView).xyz;

    // NRD expects positive distance from the camera into the screen.
    return max(1e-6f, abs(viewPos.z));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixel = DTid.xy;
    if (pixel.x >= g_FrameWidth || pixel.y >= g_FrameHeight)
        return;

    const float4 normalDepthSample = t_NormalDepth.Load(int3(pixel, 0));
    const float  hwDepth           = normalDepthSample.z;
    const bool   isSky             = (hwDepth <= 0.0f || hwDepth >= 1.0f);

    // ── ViewZ ────────────────────────────────────────────────────────────────
    // NRD expects a large positive value for sky pixels.
    const float viewZ = isSky ? 1e6f : ComputeViewZ(pixel, hwDepth);
    u_ViewZ[pixel] = viewZ;

    // ── Normal + Roughness ───────────────────────────────────────────────────
    // Decode the oct-encoded world-space normal from the G-Buffer.
    float2 octN = normalDepthSample.xy * 2.0f - 1.0f;
    float  octZ = 1.0f - abs(octN.x) - abs(octN.y);
    float2 wrapped = (1.0f - abs(octN.yx)) * (float2(octN.xy >= 0.0f) * 2.0f - 1.0f);
    float3 worldNormal = normalize(float3(octZ < 0.0f ? wrapped : octN.xy, octZ));

    if (isSky)
        worldNormal = float3(0, 0, 1);

    // NRD_NORMAL_ENCODING 2 = R10G10B10A2_UNORM.
    // Typed UAV stores are NOT supported for R10G10B10A2_UNORM on DX12, so we write the
    // same bits through a R10G10B10A2_UINT UAV view of the typeless texture.
    // Feed the real surface roughness from the material G-buffer so RELAX can perform
    // materially-aware edge stopping instead of assuming every surface is fully rough.
    float roughness = saturate(t_Material.Load(int3(pixel, 0)).r);
    float4 packedNR = NRD_FrontEnd_PackNormalAndRoughness(worldNormal, roughness, 0);
    uint   r10 = (uint)(saturate(packedNR.x) * 1023.0f + 0.5f) & 0x3FFu;
    uint   g10 = (uint)(saturate(packedNR.y) * 1023.0f + 0.5f) & 0x3FFu;
    uint   b10 = (uint)(saturate(packedNR.z) * 1023.0f + 0.5f) & 0x3FFu;
    uint   a2  = (uint)(saturate(packedNR.w) *    3.0f + 0.5f) & 0x3u;
    u_NormalRoughness[pixel] = uint4(r10, g10, b10, a2);

    // ── Diffuse Radiance + Hit Distance ──────────────────────────────────────
    float4 giSample   = t_GiRadiance.Load(int3(pixel, 0));
    float3 giRadiance = giSample.rgb;

    // RayGen now stores the average ray hit distance in alpha. Feeding RELAX a
    // real hit distance greatly improves edge preservation and reduces motion artifacts.
    const float hitDist = isSky ? 0.0f : max(giSample.a, 1e-4f);

    // RELAX_FrontEnd_PackRadianceAndHitDist: packs (radiance, hitDist) into RGBA16F.
    float4 packedDiff = RELAX_FrontEnd_PackRadianceAndHitDist(
        isSky ? float3(0, 0, 0) : giRadiance, hitDist, /*sanitize=*/true);
    u_DiffRadianceHitDist[pixel] = packedDiff;

    // ── Screen-space Motion Vectors ──────────────────────────────────────────
    // Motion = previous_UV - current_UV (NRD convention).
    // Important: this must be NON-JITTERED motion. Camera jitter is provided to NRD
    // separately via CommonSettings.cameraJitter/cameraJitterPrev.
    float2 motionVec = float2(0, 0);
    if (!isSky)
    {
        float3 worldPos = ReconstructWorldPos(pixel, hwDepth);
        float2 prevUV   = ReprojectUV(worldPos);
        float2 currUV   = ProjectCurrentUV(worldPos);
        motionVec = prevUV - currUV;
    }
    u_MotionVectors[pixel] = motionVec;
}
