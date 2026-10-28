//- Ptero Engine - Disney principled BRDF (Substance 3D Painter viewport shader)
//- =========================================================================
//-
//- A preview of Data/Shaders/PrincipledBrdf.hlsli inside Painter: the same base
//- parameters (base colour, metallic, roughness and the engine's Specular knob), the same
//- extra lobes (subsurface flatten, specular tint, anisotropic, sheen, sheen tint,
//- clearcoat, clearcoat gloss) and the same masks, read from the channels that the
//- "Ptero Engine" export (python/plugins/ptero_export.py) packs into the engine's maps:
//-
//-   Base color             -> textures.baseColor            (sRGB)
//-   AO (mixed) / Roughness / Metallic
//-                          -> textures.orm  R = AO, G = roughness, B = metallic
//-   Normal (+ height)      -> textures.normal  DirectX green, normalFlipGreen = false
//-   Height                 -> textures.height
//-   Emissive               -> textures.emissive
//-   Opacity                -> textures.opacity
//-   User0 "Clearcoat"      -> textures.principledMask R   (scales the Clearcoat slider)
//-   User1 "Sheen"          -> textures.principledMask G   (scales the Sheen slider)
//-   Anisotropy level       -> textures.principledMask B   (scales the Anisotropic slider)
//-   User2 "Flatten"        -> textures.principledMask A   (scales the Subsurface flatten slider)
//-   User3 "Aniso direction"-> textures.anisotropyDirection  RG, 0.5 = zero, DirectX green
//-
//- Each mask channel that the texture set does not have reads as 1 here, the way the
//- export fills it with white: the slider then applies to the whole surface. A channel
//- that exists but is unpainted reads as 0, exactly as it exports.
//-
//- Lighting. The engine evaluates the lobes against analytic lights; Painter only has its
//- environment map. Here every lobe is integrated against the environment by importance
//- sampling it (cosine-weighted for Burley diffuse and sheen, anisotropic GGX for the
//- specular, GTR1 for the clearcoat), so what is on screen is the engine's BRDF itself
//- rather than Painter's split-sum approximation of a different one. The lobes are
//- the true BRDF values, as the path tracer uses them; the raster resolve's convention of
//- leaving 1/pi off the diffuse family only matters for its punctual lights.

import lib-sampler.glsl
import lib-pbr.glsl
import lib-emissive.glsl
import lib-alpha-test.glsl
import lib-utils.glsl

//: metadata {
//:   "mdl":"mdl::alg::materials::skin_metallic_roughness::skin_metallic_roughness"
//: }

//- Show back faces as there may be holes in front faces.
//: state cull_face off

//: param auto channel_basecolor
uniform SamplerSparse basecolor_tex;
//: param auto channel_roughness
uniform SamplerSparse roughness_tex;
//: param auto channel_metallic
uniform SamplerSparse metallic_tex;
//: param auto channel_anisotropylevel
uniform SamplerSparse anisotropylevel_tex;
//: param auto channel_user0
uniform SamplerSparse clearcoatMask_tex;
//: param auto channel_user1
uniform SamplerSparse sheenMask_tex;
//: param auto channel_user2
uniform SamplerSparse flattenMask_tex;
//: param auto channel_user3
uniform SamplerSparse anisoDirection_tex;

//- Material sliders. Identifiers are read by the export plugin and written to the
//- .material file under the key named in each description.

//: param custom {
//:   "default": 0.5, "min": 0.0, "max": 1.0,
//:   "label": "Specular",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>specularFactor. Scales the normal-incidence reflectance of dielectrics and metals alike; 0.5 is neutral (F0 0.04, metals reflect their base colour).</p></body></html>"
//: }
uniform float ptero_specular;

//: param custom {
//:   "default": 0.0, "min": 0.0, "max": 1.0,
//:   "label": "Specular tint",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>specularTint. Tints the dielectric specular towards the base colour's hue.</p></body></html>"
//: }
uniform float ptero_specularTint;

//: param custom {
//:   "default": 0.0, "min": 0.0, "max": 1.0,
//:   "label": "Subsurface flatten",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>subsurfaceFlatten. Flattens the diffuse lobe towards Disney's Hanrahan-Krueger approximation. Masked by User2.</p></body></html>"
//: }
uniform float ptero_subsurfaceFlatten;

//: param custom {
//:   "default": 0.0, "min": 0.0, "max": 1.0,
//:   "label": "Anisotropic",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>anisotropic. Stretches the highlight along the texture's U axis, or along User3's direction. Masked by Anisotropy level.</p></body></html>"
//: }
uniform float ptero_anisotropic;

//: param custom {
//:   "default": 0.0, "min": 0.0, "max": 1.0,
//:   "label": "Sheen",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>sheen. Grazing reflection for cloth. Masked by User1.</p></body></html>"
//: }
uniform float ptero_sheen;

//: param custom {
//:   "default": 0.5, "min": 0.0, "max": 1.0,
//:   "label": "Sheen tint",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>sheenTint. Tints the sheen towards the base colour's hue.</p></body></html>"
//: }
uniform float ptero_sheenTint;

//: param custom {
//:   "default": 0.0, "min": 0.0, "max": 1.0,
//:   "label": "Clearcoat",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>clearcoat. A second, colourless GTR1 lobe at IOR 1.5. Masked by User0.</p></body></html>"
//: }
uniform float ptero_clearcoat;

//: param custom {
//:   "default": 1.0, "min": 0.0, "max": 1.0,
//:   "label": "Clearcoat gloss",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>clearcoatGloss. 0 = satin, 1 = gloss.</p></body></html>"
//: }
uniform float ptero_clearcoatGloss;

//: param custom {
//:   "default": 1.0, "min": 0.0, "max": 1.0,
//:   "label": "AO strength",
//:   "group": "Ptero Material",
//:   "description": "<html><head/><body><p>ambientOcclusionStrength. Blends the mixed AO (baked mesh AO x AO channel) the way the engine blends the ORM map's red channel.</p></body></html>"
//: }
uniform float ptero_aoStrength;

//: param custom {
//:   "default": false,
//:   "label": "Direct-light masking remap",
//:   "group": "Ptero Preview",
//:   "description": "<html><head/><body><p>Applies Disney's (0.5 + roughness / 2)^2 remap to the specular masking term, as the engine's deferred resolve does for analytic lights. Off matches image-based light and the path tracer. Preview only, not exported.</p></body></html>"
//: }
uniform bool ptero_remapGeometryRoughness;

// ---------------------------------------------------------------------------
// PrincipledBrdf.hlsli, term for term
// ---------------------------------------------------------------------------

float pteroSchlickWeight(float u)
{
  float m = clamp(1.0 - u, 0.0, 1.0);
  float m2 = m * m;
  return m2 * m2 * m;
}

vec3 pteroTintColor(vec3 baseColor)
{
  float luminance = dot(baseColor, vec3(0.3, 0.6, 0.1));
  return luminance > 0.0 ? baseColor / luminance : vec3(1.0);
}

vec3 pteroPrincipledF0(vec3 baseColor, float metallic, float specular, float specularTint)
{
  vec3 dielectric = 0.04 * mix(vec3(1.0), pteroTintColor(baseColor), specularTint);
  return clamp(mix(dielectric, baseColor, metallic) * (specular * 2.0), 0.0, 1.0);
}

float pteroGtr1(float NdotH, float a)
{
  if (a >= 1.0) return M_INV_PI;
  float a2 = a * a;
  float t = 1.0 + (a2 - 1.0) * NdotH * NdotH;
  return (a2 - 1.0) / (M_PI * log(a2) * t);
}

float pteroGtr2Aniso(float NdotH, float HdotX, float HdotY, float ax, float ay)
{
  float x = HdotX / ax;
  float y = HdotY / ay;
  float d = x * x + y * y + NdotH * NdotH;
  return 1.0 / (M_PI * ax * ay * d * d);
}

// Smith GGX masking divided by 2 N.V: the product for L and V is G / (4 N.L N.V).
float pteroSmithGgxAniso(float NdotV, float VdotX, float VdotY, float ax, float ay)
{
  float x = VdotX * ax;
  float y = VdotY * ay;
  return 1.0 / (NdotV + sqrt(x * x + y * y + NdotV * NdotV));
}

vec2 pteroAnisoAlpha(float alpha, float anisotropic)
{
  float aspect = sqrt(1.0 - 0.9 * clamp(anisotropic, 0.0, 1.0));
  return max(vec2(alpha / aspect, alpha * aspect), vec2(0.001));
}

// ---------------------------------------------------------------------------
// Integration against Painter's environment
// ---------------------------------------------------------------------------

vec3 pteroEnv(vec3 dirWS, float pdf)
{
  vec3 dirEnv = worldToEnvSpace(dirWS);
  return envSample(dirEnv, computeLOD(dirEnv, max(pdf, 1e-8)));
}

// Mask channels: a missing channel exports as white, so it leaves the slider whole.
float pteroMask(SamplerSparse smp, SparseCoord coord)
{
  return smp.is_set ? textureSparse(smp, coord).r : 1.0;
}

void shade(V2F inputs)
{
  SparseCoord coord = inputs.sparse_coord;

  vec3  baseColor = getBaseColor(basecolor_tex, coord);
  float roughness = getRoughness(roughness_tex, coord);
  float metallic  = getMetallic(metallic_tex, coord);

  float subsurface     = ptero_subsurfaceFlatten * pteroMask(flattenMask_tex, coord);
  float sheen          = ptero_sheen * pteroMask(sheenMask_tex, coord);
  float clearcoat      = ptero_clearcoat * pteroMask(clearcoatMask_tex, coord);
  float anisotropic    = ptero_anisotropic * pteroMask(anisotropylevel_tex, coord);
  float clearcoatGloss = ptero_clearcoatGloss;

  // The engine applies AO to everything the environment lights, and only to that.
  float ao = mix(1.0, getAO(coord, true, true), ptero_aoStrength);
  float occlusion = ao * getShadowFactor();

  vec3 normal = computeWSNormal(coord, inputs.tangent, inputs.bitangent, inputs.normal);
  LocalVectors vectors = computeLocalFrame(inputs, normal, 0.0);
  vec3 N = vectors.normal;
  vec3 V = vectors.eye;

  emissiveColorOutput(pbrComputeEmissive(emissive_tex, coord));
  alphaKill(coord);

  // Anisotropy runs along the texture's U axis, or the direction map. That map is
  // authored in the engine's DirectX green convention, where +G points down the image;
  // Painter's bitangent points up it.
  vec3 tangent = vectors.tangent;
  if (anisotropic > 0.0 && anisoDirection_tex.is_set)
  {
    vec2 direction = textureSparse(anisoDirection_tex, coord).rg * 2.0 - 1.0;
    if (dot(direction, direction) > 1e-4)
      tangent = vectors.tangent * direction.x - vectors.bitangent * direction.y;
  }
  vec3 X = vectors.tangent;
  vec3 Y = vectors.bitangent;
  if (anisotropic > 0.0 && dot(tangent, tangent) > 1e-6)
  {
    X = normalize(tangent - N * dot(N, tangent));
    Y = cross(N, X);
  }

  float NdotV = max(dot(N, V), 1e-4);
  float FV = pteroSchlickWeight(NdotV);
  vec3 tint = pteroTintColor(baseColor);
  float diffuseWeight = 1.0 - metallic;
  vec3 sheenColor = sheen * mix(vec3(1.0), tint, ptero_sheenTint) * diffuseWeight;
  vec3 F0 = pteroPrincipledF0(baseColor, metallic, ptero_specular, ptero_specularTint);

  vec2 alpha = pteroAnisoAlpha(roughness * roughness, anisotropic);
  float geometryRoughness = ptero_remapGeometryRoughness ? (0.5 + 0.5 * roughness) : roughness;
  vec2 alphaG = pteroAnisoAlpha(geometryRoughness * geometryRoughness, anisotropic);
  float GsV = pteroSmithGgxAniso(NdotV, dot(V, X), dot(V, Y), alphaG.x, alphaG.y);

  float coatAlpha = mix(0.1, 0.001, clearcoatGloss);
  float GrV = pteroSmithGgxAniso(NdotV, 0.0, sqrt(clamp(1.0 - NdotV * NdotV, 0.0, 1.0)), 0.25, 0.25);

  vec3 diffuseRadiance  = vec3(0.0);
  vec3 sheenRadiance    = vec3(0.0);
  vec3 specularRadiance = vec3(0.0);
  vec3 coatRadiance     = vec3(0.0);

  for (int i = 0; i < nbSamples; ++i)
  {
    vec2 Xi = fibonacci2DDitheredTemporal(i, nbSamples);
    float phi = M_2PI * Xi.x;
    float u = min(Xi.y, 0.9999);

    // Diffuse and sheen: cosine-weighted, so f * N.L / pdf is f * pi.
    {
      float r = sqrt(u);
      vec3 L = X * (r * cos(phi)) + Y * (r * sin(phi)) + N * sqrt(1.0 - u);
      float NdotL = dot(N, L);
      if (NdotL > 0.0)
      {
        vec3 H = normalize(L + V);
        float LdotH = clamp(dot(L, H), 0.0, 1.0);
        float FL = pteroSchlickWeight(NdotL);
        float FH = pteroSchlickWeight(LdotH);

        float Fd90 = 0.5 + 2.0 * LdotH * LdotH * roughness;
        float Fd = mix(1.0, Fd90, FL) * mix(1.0, Fd90, FV);
        float Fss90 = LdotH * LdotH * roughness;
        float Fss = mix(1.0, Fss90, FL) * mix(1.0, Fss90, FV);
        float ss = 1.25 * (Fss * (1.0 / (NdotL + NdotV) - 0.5) + 0.5);

        vec3 radiance = pteroEnv(L, NdotL * M_INV_PI);
        diffuseRadiance += radiance * mix(Fd, ss, subsurface);
        sheenRadiance   += radiance * (FH * M_PI);
      }
    }

    // Specular: sample the anisotropic GGX normal distribution (pdf_h = D N.H).
    {
      float slope = sqrt(u / (1.0 - u));
      vec3 Hl = normalize(vec3(alpha.x * slope * cos(phi), alpha.y * slope * sin(phi), 1.0));
      vec3 H = X * Hl.x + Y * Hl.y + N * Hl.z;
      vec3 L = reflect(-V, H);
      float NdotL = dot(N, L);
      if (NdotL > 0.0)
      {
        float NdotH = max(Hl.z, 1e-6);
        float VdotH = max(dot(V, H), 1e-6);
        float D = pteroGtr2Aniso(NdotH, dot(H, X), dot(H, Y), alpha.x, alpha.y);
        float Gs = GsV * pteroSmithGgxAniso(NdotL, dot(L, X), dot(L, Y), alphaG.x, alphaG.y);
        vec3 Fs = mix(F0, vec3(1.0), pteroSchlickWeight(VdotH));
        float fade = horizonFading(dot(vectors.vertexNormal, L), horizonFade);
        // f N.L / pdf_L with pdf_L = D N.H / (4 V.H); D cancels.
        specularRadiance += fade * pteroEnv(L, D * NdotH / (4.0 * VdotH))
                          * Fs * (Gs * 4.0 * VdotH * NdotL / NdotH);
      }
    }

    // Clearcoat: sample GTR1 (pdf_h = D N.H).
    if (clearcoat > 0.0)
    {
      float a2 = coatAlpha * coatAlpha;
      float cosTheta = sqrt(max(0.0, (1.0 - pow(a2, 1.0 - u)) / (1.0 - a2)));
      float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
      vec3 H = X * (sinTheta * cos(phi)) + Y * (sinTheta * sin(phi)) + N * cosTheta;
      vec3 L = reflect(-V, H);
      float NdotL = dot(N, L);
      if (NdotL > 0.0)
      {
        float NdotH = max(cosTheta, 1e-6);
        float VdotH = max(dot(V, H), 1e-6);
        float D = pteroGtr1(NdotH, coatAlpha);
        float Gr = GrV * pteroSmithGgxAniso(NdotL, 0.0, sqrt(clamp(1.0 - NdotL * NdotL, 0.0, 1.0)), 0.25, 0.25);
        float Fr = mix(0.04, 1.0, pteroSchlickWeight(VdotH));
        float fade = horizonFading(dot(vectors.vertexNormal, L), horizonFade);
        coatRadiance += fade * pteroEnv(L, D * NdotH / (4.0 * VdotH))
                      * (0.25 * clearcoat * Gr * Fr * 4.0 * VdotH * NdotL / NdotH);
      }
    }
  }

  float invCount = 1.0 / float(nbSamples);
  diffuseRadiance  *= baseColor * diffuseWeight * invCount;
  sheenRadiance    *= sheenColor * invCount;
  specularRadiance *= invCount;
  coatRadiance     *= invCount;

  // Everything is final radiance already, so albedo is left at one.
  albedoOutput(vec3(1.0));
  diffuseShadingOutput(occlusion * diffuseRadiance);
  specularShadingOutput(occlusion * (specularRadiance + sheenRadiance + coatRadiance));
}
