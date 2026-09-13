#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

enum class MaterialTextureSlot
{
    BaseColor,
    Normal,
    Metallic,
    Roughness,
    MetallicRoughness,
    AmbientOcclusion,
    Emissive,
    Height,
    Opacity
};

// How a particle sprite is composited into the scene colour target.
// Fire is Additive; smoke and steam are AlphaBlend; a flipbook exported with
// premultiplied alpha (the usual output of offline fluid sims) is Premultiplied.
enum class ParticleBlendMode
{
    Additive      = 0,
    AlphaBlend    = 1,
    Premultiplied = 2,
};

struct MaterialTextureSet
{
    std::string BaseColorTexturePath;
    std::string NormalTexturePath;
    std::string MetallicTexturePath;
    std::string RoughnessTexturePath;
    std::string MetallicRoughnessTexturePath;
    std::string AmbientOcclusionTexturePath;
    std::string EmissiveTexturePath;
    std::string HeightTexturePath;
    std::string OpacityTexturePath;

    std::string& GetTexturePath(MaterialTextureSlot textureSlot);
    const std::string& GetTexturePath(MaterialTextureSlot textureSlot) const;
};

struct MaterialDefinition
{
    std::string Name = "NewMaterial";
    std::array<float, 4> BaseColorTint{ 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, 3> EmissiveColor{ 0.0f, 0.0f, 0.0f };
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    float NormalScale = 1.0f;
    float AmbientOcclusionStrength = 1.0f;
    float HeightScale = 0.05f;
    float Opacity = 1.0f;
    float AlphaCutoff = 0.5f;
    float GlassIor = 1.5f;
    float GlassThickness = 0.08f;
    float GlassDispersion = 0.35f;
    float GlassCausticStrength = 0.0f;

    // UV transform applied to every texture this material samples. Rotation spins the
    // source UVs about (0.5, 0.5), tiling then repeats them, offset slides the result.
    std::array<float, 2> UvTiling{ 1.0f, 1.0f };
    std::array<float, 2> UvOffset{ 0.0f, 0.0f };
    float UvRotationDegrees = 0.0f;

    // Parallax occlusion mapping. Needs a Height texture; HeightScale above is the depth
    // of the height volume, measured in tiled UV units.
    bool UseParallaxOcclusion = false;
    int ParallaxMinSteps = 8;
    int ParallaxMaxSteps = 32;
    // Metres. Past this the parallax offset fades out so distant surfaces stop paying for
    // a ray march they cannot resolve. 0 keeps it at full strength everywhere.
    float ParallaxFadeDistance = 30.0f;

    bool IsDoubleSided = false;
    bool UseAlphaCutout = false;
    bool UseTransparentBlend = false;
    bool UseGlassRendering = false;
    bool UseThinGlass = true;
    bool IsDecalMaterial = false;

    // --- Particle materials -------------------------------------------------
    // A particle material is consumed by the particle renderer rather than the
    // G-Buffer pass: it is drawn as a camera-facing sprite, it never writes
    // depth or normals, and it is lit entirely by its own emissive term. The
    // fields below only mean anything when IsParticleMaterial is set; the
    // shared fields above still apply (baseColor is the sprite texture,
    // baseColorTint multiplies it, emissiveColor is the glow).
    bool IsParticleMaterial = false;

    ParticleBlendMode ParticleBlend = ParticleBlendMode::Additive;

    // Multiplies emissiveColor. Separated from the colour so brightness can be
    // pushed into HDR without the artist having to type values above 1 into a
    // colour picker that clamps its swatch at 1.
    float ParticleEmissiveIntensity = 1.0f;

    // How much of the sprite's lit (non-emissive) appearance comes from the
    // scene's lights. Fire is 0 - it makes its own light. Smoke wants ~1 so it
    // darkens away from the flame and catches the sun.
    float ParticleLightingInfluence = 0.0f;

    // Scene lighting received by the sprite is spread over this much of a
    // sphere's normal range. 0 shades the sprite as a flat card facing the
    // camera; 1 shades it as if it were a sphere, which reads far better for
    // thick smoke. Only matters when ParticleLightingInfluence > 0.
    float ParticleSphericalNormal = 0.6f;

    // Attenuates the sprite as the camera approaches it, so flying through a
    // fire does not end in a full-screen orange flash. Metres.
    float ParticleCameraFadeDistance = 0.6f;

    // Flipbook atlas layout baked into the material, so one authored flame
    // texture carries its own frame count and every emitter that uses the
    // material inherits it. A system whose own columns/rows are left at 1x1
    // falls back to these.
    int ParticleFlipbookColumns = 1;
    int ParticleFlipbookRows = 1;

    // Take the sprite's opacity from the brightness of its RGB instead of from
    // its alpha channel.
    //
    // Additive effect sheets - flames, sparks, explosions - are almost always
    // authored as colour on black, where the black *is* the transparency and
    // the alpha channel is either absent or junk. Such a sheet renders as
    // nothing at all when sampled the usual way. This reads the opacity out of
    // the luminance, which is the quantity those sheets actually encode, and it
    // is also what makes an exported flipbook usable without an alpha repair
    // pass in an image editor first.
    bool ParticleAlphaFromLuminance = false;

    MaterialTextureSet Textures;
};

// A multi-material groups several sub-materials under one parent asset.
// Each sub-material's index (0-based) matches the FBX material ID / sub-mesh materialId
// so the correct sub-material is automatically selected for each triangle range.
struct MultiMaterialDefinition
{
    std::string Name = "NewMultiMaterial";
    // One entry per FBX material slot; index == materialId from the cooked mesh.
    std::vector<MaterialDefinition> SubMaterials;
};

class MaterialEditor final
{
public:
    MaterialEditor();

    // ---- Single-material API (unchanged behaviour) ----
    void NewMaterial(const std::string& materialName = "NewMaterial");
    bool LoadMaterialFromFile(const std::filesystem::path& materialFilePath);
    bool SaveMaterialToFile(const std::filesystem::path& materialFilePath);
    bool SaveCurrentMaterial();
    bool OpenMaterialWithDialog(HWND ownerWindowHandle);
    bool SaveMaterialWithDialog(HWND ownerWindowHandle);
    bool BrowseForTexture(HWND ownerWindowHandle, MaterialTextureSlot textureSlot);

    const MaterialDefinition& GetCurrentMaterial() const;
    MaterialDefinition& GetCurrentMaterial();
    const std::filesystem::path& GetCurrentMaterialPath() const;
    const std::string& GetLastErrorMessage() const;

    std::filesystem::path GetMaterialLibraryDirectory() const;
    std::vector<std::filesystem::path> FindAvailableMaterials() const;

    // ---- Multi-material API ----

    // Start a new, empty multi-material with no sub-materials.
    void NewMultiMaterial(const std::string& multiMaterialName = "NewMultiMaterial");

    // Add a blank sub-material at the next available material-id slot.
    void AddSubMaterial(const std::string& subMaterialName = "");

    // Remove the sub-material at the given index (re-indexes higher slots down by one).
    bool RemoveSubMaterial(int subMaterialIndex);

    // Load / save the whole multi-material to/from a JSON file.
    bool LoadMultiMaterialFromFile(const std::filesystem::path& filePath);
    bool SaveMultiMaterialToFile(const std::filesystem::path& filePath);
    bool SaveCurrentMultiMaterial();
    bool OpenMultiMaterialWithDialog(HWND ownerWindowHandle);
    bool SaveMultiMaterialWithDialog(HWND ownerWindowHandle);

    // Browse for a texture for a specific sub-material slot.
    bool BrowseForSubMaterialTexture(HWND ownerWindowHandle, int subMaterialIndex, MaterialTextureSlot textureSlot);

    const MultiMaterialDefinition& GetCurrentMultiMaterial() const;
    MultiMaterialDefinition& GetCurrentMultiMaterial();
    const std::filesystem::path& GetCurrentMultiMaterialPath() const;

    std::vector<std::filesystem::path> FindAvailableMultiMaterials() const;

private:
    std::filesystem::path BuildDefaultMaterialPath() const;
    std::filesystem::path BuildDefaultMultiMaterialPath() const;

    MaterialDefinition mCurrentMaterial;
    std::filesystem::path mCurrentMaterialPath;
    MultiMaterialDefinition mCurrentMultiMaterial;
    std::filesystem::path mCurrentMultiMaterialPath;
    std::string mLastErrorMessage;
};
