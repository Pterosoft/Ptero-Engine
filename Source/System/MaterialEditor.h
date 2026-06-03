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
    bool IsDoubleSided = false;
    bool UseAlphaCutout = false;
    bool UseTransparentBlend = false;
    bool IsDecalMaterial = false;
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
