// PrincipledBrdf.hlsli
// The Disney principled BRDF (Burley 2012, "Physically-Based Shading at Disney", the
// SIGGRAPH course notes, and the disney.brdf the course shipped for BRDF Explorer).
// Shared by the deferred resolve and the path tracer, so the raster image and the
// reference agree on what a material is.
//
// The base parameters are the engine's own: base colour, metallic, roughness and the
// Specular knob (SurfaceSpecular.hlsli). Disney's specular maps onto that knob exactly -
// both give a dielectric F0 of 0.08 x specular - except that the engine's also scales
// metals, which is kept. On top of those come the model's extra lobes:
//
//   subsurface      flattens the diffuse lobe towards a Hanrahan-Krueger approximation
//   specularTint    tints the dielectric specular towards the base colour's hue
//   anisotropic     stretches the specular lobe along the surface tangent
//   sheen           extra grazing reflection for cloth
//   sheenTint       tints the sheen towards the base colour's hue
//   clearcoat       a second, black-and-white GTR1 specular lobe at a fixed IOR of 1.5
//   clearcoatGloss  0 = satin clearcoat, 1 = gloss
//
// Lobes are returned as BRDF values, without the cosine and without the engine's raster
// convention of leaving 1/pi off the diffuse term; each caller applies its own.

#ifndef PTERO_PRINCIPLED_BRDF_HLSLI
#define PTERO_PRINCIPLED_BRDF_HLSLI

#include "SurfaceSpecular.hlsli"

static const float kPteroPrincipledPi = 3.14159265f;

struct PteroPrincipled
{
    float  subsurface;
    float  specularTint;
    float  sheen;
    float  sheenTint;
    float  clearcoat;
    float  clearcoatGloss;
    float  anisotropic;
    // Unit, perpendicular to the shading normal: the direction the anisotropic highlight
    // stretches along (the texture's U axis). Zero when unknown; only read while
    // anisotropic > 0.
    float3 tangent;
};

// Every extra lobe off: the principled model reduced to its base diffuse and specular.
PteroPrincipled PteroPrincipledNone()
{
    PteroPrincipled p;
    p.subsurface     = 0.0f;
    p.specularTint   = 0.0f;
    p.sheen          = 0.0f;
    p.sheenTint      = 0.5f;
    p.clearcoat      = 0.0f;
    p.clearcoatGloss = 1.0f;
    p.anisotropic    = 0.0f;
    p.tangent        = float3(0.0f, 0.0f, 0.0f);
    return p;
}

// ---------------------------------------------------------------------------
// G-Buffer encoding (SV_Target4 / SV_Target5, RGBA8 UNORM, PrincipledGBuffer.h)
//   target0 = subsurface, specularTint, sheen, sheenTint
//   target1 = clearcoat, clearcoatGloss, anisotropic, tangent angle
// A pixel nobody wrote reads back zero, which is every lobe off.
// ---------------------------------------------------------------------------

// An orthonormal basis around a unit normal that depends on nothing but the normal
// (Duff et al. 2017, "Building an Orthonormal Basis, Revisited"), so the G-Buffer pass
// and the resolve rebuild the same frame and the tangent can travel as one angle in it.
void PteroPrincipledBasis(float3 n, out float3 b1, out float3 b2)
{
    const float s = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float b = n.x * n.y * a;
    b1 = float3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = float3(b, s + n.y * n.y * a, -n.y);
}

// The tangent as an angle in that basis, over [0, 1). Anisotropy is symmetric under
// t -> -t, so half a turn is the whole range and 8 bits resolve it to 0.7 degrees.
float PteroEncodePrincipledTangent(float3 tangent, float3 n)
{
    float3 b1, b2;
    PteroPrincipledBasis(n, b1, b2);
    const float x = dot(tangent, b1);
    const float y = dot(tangent, b2);
    if (x * x + y * y < 1e-8f)
        return 0.0f;
    float angle = atan2(y, x);
    if (angle < 0.0f)
        angle += kPteroPrincipledPi;
    return saturate(angle / kPteroPrincipledPi);
}

float3 PteroDecodePrincipledTangent(float encoded, float3 n)
{
    float3 b1, b2;
    PteroPrincipledBasis(n, b1, b2);
    float s, c;
    sincos(encoded * kPteroPrincipledPi, s, c);
    return b1 * c + b2 * s;
}

void PteroEncodePrincipled(PteroPrincipled p, float3 n, out float4 target0, out float4 target1)
{
    target0 = saturate(float4(p.subsurface, p.specularTint, p.sheen, p.sheenTint));
    target1 = saturate(float4(p.clearcoat, p.clearcoatGloss, p.anisotropic,
                              p.anisotropic > 0.0f ? PteroEncodePrincipledTangent(p.tangent, n) : 0.0f));
}

PteroPrincipled PteroDecodePrincipled(float4 target0, float4 target1, float3 n)
{
    PteroPrincipled p;
    p.subsurface     = target0.r;
    p.specularTint   = target0.g;
    p.sheen          = target0.b;
    p.sheenTint      = target0.a;
    p.clearcoat      = target1.r;
    p.clearcoatGloss = target1.g;
    p.anisotropic    = target1.b;
    p.tangent        = p.anisotropic > 0.0f ? PteroDecodePrincipledTangent(target1.a, n) : float3(0.0f, 0.0f, 0.0f);
    return p;
}

// ---------------------------------------------------------------------------
// Terms
// ---------------------------------------------------------------------------

// (1 - u)^5: Schlick's Fresnel weight.
float PteroSchlickWeight(float u)
{
    const float m = saturate(1.0f - u);
    const float m2 = m * m;
    return m2 * m2 * m;
}

// The base colour's hue at unit luminance - what both tints lerp towards.
float3 PteroPrincipledTintColor(float3 baseColor)
{
    const float luminance = dot(baseColor, float3(0.3f, 0.6f, 0.1f));
    return luminance > 0.0f ? baseColor / luminance : float3(1.0f, 1.0f, 1.0f);
}

// Normal-incidence reflectance with Disney's specular tint. With no tint this is
// PteroComputeF0 exactly, so passes that do not know the tint (SSR, ray-traced
// reflections) only disagree on tinted dielectrics.
float3 PteroComputePrincipledF0(float3 baseColor, float metallic, float specular, float specularTint)
{
    const float3 dielectric = 0.04f * lerp(float3(1.0f, 1.0f, 1.0f), PteroPrincipledTintColor(baseColor), specularTint);
    return saturate(lerp(dielectric, baseColor, metallic) * (specular * 2.0f));
}

// GTR1 (Berry): the clearcoat's long-tailed distribution.
float PteroGtr1(float NdotH, float a)
{
    if (a >= 1.0f)
        return 1.0f / kPteroPrincipledPi;
    const float a2 = a * a;
    const float t = 1.0f + (a2 - 1.0f) * NdotH * NdotH;
    return (a2 - 1.0f) / (kPteroPrincipledPi * log(a2) * t);
}

// GTR2 (GGX / Trowbridge-Reitz) with separate roughness along the tangent (ax) and
// bitangent (ay).
float PteroGtr2Aniso(float NdotH, float HdotX, float HdotY, float ax, float ay)
{
    const float x = HdotX / ax;
    const float y = HdotY / ay;
    const float d = x * x + y * y + NdotH * NdotH;
    return 1.0f / (kPteroPrincipledPi * ax * ay * d * d);
}

// Separable Smith GGX masking divided by 2 N.V, the form the Disney code uses: the
// product for L and V is G / (4 N.L N.V), so the specular is just D F times it.
float PteroSmithGgxAniso(float NdotV, float VdotX, float VdotY, float ax, float ay)
{
    const float x = VdotX * ax;
    const float y = VdotY * ay;
    return 1.0f / (NdotV + sqrt(x * x + y * y + NdotV * NdotV));
}

// Tangent-space roughness from roughness and anisotropy (the notes' aspect mapping,
// which keeps the highlight's area roughly constant as it stretches).
float2 PteroPrincipledAnisoAlpha(float alpha, float anisotropic)
{
    const float aspect = sqrt(1.0f - 0.9f * saturate(anisotropic));
    return max(float2(alpha / aspect, alpha * aspect), 0.001f);
}

struct PteroPrincipledLobes
{
    float3 diffuse;    // Burley diffuse / Hanrahan-Krueger, already weighted by 1 - metallic
    float3 sheen;      // already weighted by 1 - metallic
    float3 specular;   // the GTR2 lobe
    float3 clearcoat;  // the GTR1 lobe
};

// The four lobes for light arriving from L and leaving towards V, as BRDF values (no
// cosine). remapGeometryRoughness applies the notes' (0.5 + roughness / 2)^2 to the
// masking term, which Disney used for analytic lights to tame the shadowing gain of
// very smooth surfaces; image-based or sampled light is meant to go without it.
PteroPrincipledLobes PteroEvalPrincipled(
    float3 N, float3 V, float3 L,
    float3 baseColor, float metallic, float roughness, float specular,
    PteroPrincipled p, bool remapGeometryRoughness)
{
    PteroPrincipledLobes lobes;
    lobes.diffuse   = float3(0.0f, 0.0f, 0.0f);
    lobes.sheen     = float3(0.0f, 0.0f, 0.0f);
    lobes.specular  = float3(0.0f, 0.0f, 0.0f);
    lobes.clearcoat = float3(0.0f, 0.0f, 0.0f);

    const float NdotL = dot(N, L);
    if (NdotL <= 0.0f)
        return lobes;
    const float NdotV = max(dot(N, V), 1e-4f);

    const float3 H = normalize(L + V);
    const float NdotH = saturate(dot(N, H));
    const float LdotH = saturate(dot(L, H));

    const float3 tint = PteroPrincipledTintColor(baseColor);
    const float FL = PteroSchlickWeight(NdotL);
    const float FV = PteroSchlickWeight(NdotV);
    const float FH = PteroSchlickWeight(LdotH);

    // Diffuse: Lambert with Burley's retro-reflection, which brightens rough surfaces at
    // grazing angles and darkens smooth ones there.
    const float Fd90 = 0.5f + 2.0f * LdotH * LdotH * roughness;
    const float Fd = lerp(1.0f, Fd90, FL) * lerp(1.0f, Fd90, FV);

    // Hanrahan-Krueger-inspired flattening; 1.25 preserves albedo.
    const float Fss90 = LdotH * LdotH * roughness;
    const float Fss = lerp(1.0f, Fss90, FL) * lerp(1.0f, Fss90, FV);
    const float ss = 1.25f * (Fss * (1.0f / (NdotL + NdotV) - 0.5f) + 0.5f);

    const float diffuseWeight = 1.0f - metallic;
    lobes.diffuse = baseColor * (lerp(Fd, ss, p.subsurface) * diffuseWeight / kPteroPrincipledPi);
    lobes.sheen = FH * p.sheen * lerp(float3(1.0f, 1.0f, 1.0f), tint, p.sheenTint) * diffuseWeight;

    // Specular: anisotropic GTR2 in the tangent frame. Isotropic surfaces do not care
    // which frame, so any one around N will do when there is no tangent.
    float3 X, Y;
    if (p.anisotropic > 0.0f && dot(p.tangent, p.tangent) > 1e-6f)
    {
        X = normalize(p.tangent - N * dot(N, p.tangent));
        Y = cross(N, X);
    }
    else
    {
        PteroPrincipledBasis(N, X, Y);
    }

    const float2 alpha = PteroPrincipledAnisoAlpha(roughness * roughness, p.anisotropic);
    const float Ds = PteroGtr2Aniso(NdotH, dot(H, X), dot(H, Y), alpha.x, alpha.y);

    const float geometryRoughness = remapGeometryRoughness ? (0.5f + 0.5f * roughness) : roughness;
    const float2 alphaG = PteroPrincipledAnisoAlpha(geometryRoughness * geometryRoughness, p.anisotropic);
    const float Gs = PteroSmithGgxAniso(NdotL, dot(L, X), dot(L, Y), alphaG.x, alphaG.y)
                   * PteroSmithGgxAniso(NdotV, dot(V, X), dot(V, Y), alphaG.x, alphaG.y);

    const float3 F0 = PteroComputePrincipledF0(baseColor, metallic, specular, p.specularTint);
    const float3 Fs = lerp(F0, float3(1.0f, 1.0f, 1.0f), FH);
    lobes.specular = Gs * Fs * Ds;

    // Clearcoat: IOR 1.5 (F0 0.04), fixed masking roughness of 0.25, and the notes' 0.25
    // weight so a full coat reads as a coat rather than a mirror.
    [branch]
    if (p.clearcoat > 0.0f)
    {
        const float Dr = PteroGtr1(NdotH, lerp(0.1f, 0.001f, p.clearcoatGloss));
        const float Fr = lerp(0.04f, 1.0f, FH);
        const float Gr = PteroSmithGgxAniso(NdotL, 0.0f, sqrt(saturate(1.0f - NdotL * NdotL)), 0.25f, 0.25f)
                       * PteroSmithGgxAniso(NdotV, 0.0f, sqrt(saturate(1.0f - NdotV * NdotV)), 0.25f, 0.25f);
        lobes.clearcoat = (0.25f * p.clearcoat * Gr * Fr * Dr).xxx;
    }

    return lobes;
}

#endif // PTERO_PRINCIPLED_BRDF_HLSLI
