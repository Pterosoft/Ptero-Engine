#include "pch.h"

#include "MaterialEditor.h"
#include "include\System\SystemAssetApi.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <commdlg.h>
#include <fstream>
#include <system_error>

using json = nlohmann::json;

namespace
{
    std::filesystem::path FindProjectDataDirectory()
    {
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD characterCount = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (characterCount == 0 || characterCount == std::size(executablePath))
        {
            return {};
        }

        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        while (!currentPath.empty())
        {
            const std::filesystem::path dataDirectory = currentPath / "Data";
            if (std::filesystem::exists(dataDirectory) && std::filesystem::is_directory(dataDirectory))
            {
                return std::filesystem::weakly_canonical(dataDirectory);
            }

            const std::filesystem::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
            {
                break;
            }

            currentPath = parentPath;
        }

        return {};
    }

    std::string NormalizeRelativeDataPath(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path normalizedPath = relativePath.lexically_normal();
        return (normalizedPath.empty() || normalizedPath == ".") ? std::string{} : normalizedPath.generic_string();
    }

    std::string GetTextureSlotDisplayName(const MaterialTextureSlot textureSlot)
    {
        switch (textureSlot)
        {
        case MaterialTextureSlot::BaseColor:
            return "BaseColor";
        case MaterialTextureSlot::Normal:
            return "Normal";
        case MaterialTextureSlot::Metallic:
            return "Metallic";
        case MaterialTextureSlot::Roughness:
            return "Roughness";
        case MaterialTextureSlot::MetallicRoughness:
            return "MetallicRoughness";
        case MaterialTextureSlot::AmbientOcclusion:
            return "AmbientOcclusion";
        case MaterialTextureSlot::Emissive:
            return "Emissive";
        case MaterialTextureSlot::Height:
            return "Height";
        case MaterialTextureSlot::Opacity:
            return "Opacity";
        default:
            return "Unknown";
        }
    }

    json SerializeMaterialDefinition(const MaterialDefinition& materialDefinition)
    {
        return json{
            { "name", materialDefinition.Name },
            { "baseColorTint", materialDefinition.BaseColorTint },
            { "emissiveColor", materialDefinition.EmissiveColor },
            { "metallicFactor", materialDefinition.MetallicFactor },
            { "roughnessFactor", materialDefinition.RoughnessFactor },
            { "specularFactor", materialDefinition.SpecularFactor },
            { "normalScale", materialDefinition.NormalScale },
            { "ambientOcclusionStrength", materialDefinition.AmbientOcclusionStrength },
            { "heightScale", materialDefinition.HeightScale },
            { "heightReference", materialDefinition.HeightReference },
            { "opacity", materialDefinition.Opacity },
            { "alphaCutoff", materialDefinition.AlphaCutoff },
            { "glassIor", materialDefinition.GlassIor },
            { "glassThickness", materialDefinition.GlassThickness },
            { "glassDispersion", materialDefinition.GlassDispersion },
            { "glassCausticStrength", materialDefinition.GlassCausticStrength },
            { "uvTiling", materialDefinition.UvTiling },
            { "uvOffset", materialDefinition.UvOffset },
            { "uvRotationDegrees", materialDefinition.UvRotationDegrees },
            { "useParallaxOcclusion", materialDefinition.UseParallaxOcclusion },
            { "parallaxMinSteps", materialDefinition.ParallaxMinSteps },
            { "parallaxMaxSteps", materialDefinition.ParallaxMaxSteps },
            { "parallaxFadeDistance", materialDefinition.ParallaxFadeDistance },
            { "doubleSided", materialDefinition.IsDoubleSided },
            { "useAlphaCutout", materialDefinition.UseAlphaCutout },
            { "useTransparentBlend", materialDefinition.UseTransparentBlend },
            { "useGlassRendering", materialDefinition.UseGlassRendering },
            { "useThinGlass", materialDefinition.UseThinGlass },
            { "isDecalMaterial", materialDefinition.IsDecalMaterial },
            { "isParticleMaterial", materialDefinition.IsParticleMaterial },
            { "particleBlendMode", static_cast<int>(materialDefinition.ParticleBlend) },
            { "particleEmissiveIntensity", materialDefinition.ParticleEmissiveIntensity },
            { "particleLightingInfluence", materialDefinition.ParticleLightingInfluence },
            { "particleSphericalNormal", materialDefinition.ParticleSphericalNormal },
            { "particleCameraFadeDistance", materialDefinition.ParticleCameraFadeDistance },
            { "particleFlipbookColumns", materialDefinition.ParticleFlipbookColumns },
            { "particleFlipbookRows", materialDefinition.ParticleFlipbookRows },
            { "particleAlphaFromLuminance", materialDefinition.ParticleAlphaFromLuminance },
            {
                "textures",
                {
                    { "baseColor", materialDefinition.Textures.BaseColorTexturePath },
                    { "normal", materialDefinition.Textures.NormalTexturePath },
                    { "metallic", materialDefinition.Textures.MetallicTexturePath },
                    { "roughness", materialDefinition.Textures.RoughnessTexturePath },
                    { "metallicRoughness", materialDefinition.Textures.MetallicRoughnessTexturePath },
                    { "ambientOcclusion", materialDefinition.Textures.AmbientOcclusionTexturePath },
                    { "emissive", materialDefinition.Textures.EmissiveTexturePath },
                    { "height", materialDefinition.Textures.HeightTexturePath },
                    { "opacity", materialDefinition.Textures.OpacityTexturePath },
                }
            }
        };
    }

    bool TryReadOptionalFloatArray2(const json& sourceJson, const char* propertyName, std::array<float, 2>& outValues)
    {
        if (!sourceJson.contains(propertyName))
        {
            return true;
        }

        const json& propertyJson = sourceJson.at(propertyName);
        if (!propertyJson.is_array() || propertyJson.size() != outValues.size())
        {
            return false;
        }

        for (size_t elementIndex = 0; elementIndex < outValues.size(); ++elementIndex)
        {
            outValues[elementIndex] = propertyJson.at(elementIndex).get<float>();
        }

        return true;
    }

    bool TryReadOptionalFloatArray4(const json& sourceJson, const char* propertyName, std::array<float, 4>& outValues)
    {
        if (!sourceJson.contains(propertyName))
        {
            return true;
        }

        const json& propertyJson = sourceJson.at(propertyName);
        if (!propertyJson.is_array() || propertyJson.size() != outValues.size())
        {
            return false;
        }

        for (size_t elementIndex = 0; elementIndex < outValues.size(); ++elementIndex)
        {
            outValues[elementIndex] = propertyJson.at(elementIndex).get<float>();
        }

        return true;
    }

    bool TryReadOptionalFloatArray3(const json& sourceJson, const char* propertyName, std::array<float, 3>& outValues)
    {
        if (!sourceJson.contains(propertyName))
        {
            return true;
        }

        const json& propertyJson = sourceJson.at(propertyName);
        if (!propertyJson.is_array() || propertyJson.size() != outValues.size())
        {
            return false;
        }

        for (size_t elementIndex = 0; elementIndex < outValues.size(); ++elementIndex)
        {
            outValues[elementIndex] = propertyJson.at(elementIndex).get<float>();
        }

        return true;
    }

    bool DeserializeMaterialDefinition(const json& sourceJson, MaterialDefinition& outMaterialDefinition, std::string& outErrorMessage)
    {
        try
        {
            if (sourceJson.contains("name"))
            {
                outMaterialDefinition.Name = sourceJson.at("name").get<std::string>();
            }

            if (!TryReadOptionalFloatArray4(sourceJson, "baseColorTint", outMaterialDefinition.BaseColorTint))
            {
                outErrorMessage = "baseColorTint must be an array with 4 float values.";
                return false;
            }

            if (!TryReadOptionalFloatArray3(sourceJson, "emissiveColor", outMaterialDefinition.EmissiveColor))
            {
                outErrorMessage = "emissiveColor must be an array with 3 float values.";
                return false;
            }

            outMaterialDefinition.MetallicFactor = sourceJson.value("metallicFactor", outMaterialDefinition.MetallicFactor);
            outMaterialDefinition.RoughnessFactor = sourceJson.value("roughnessFactor", outMaterialDefinition.RoughnessFactor);
            outMaterialDefinition.SpecularFactor = sourceJson.value("specularFactor", outMaterialDefinition.SpecularFactor);
            outMaterialDefinition.NormalScale = sourceJson.value("normalScale", outMaterialDefinition.NormalScale);
            outMaterialDefinition.AmbientOcclusionStrength = sourceJson.value("ambientOcclusionStrength", outMaterialDefinition.AmbientOcclusionStrength);
            outMaterialDefinition.HeightScale = sourceJson.value("heightScale", outMaterialDefinition.HeightScale);
            outMaterialDefinition.HeightReference = sourceJson.value("heightReference", outMaterialDefinition.HeightReference);
            outMaterialDefinition.Opacity = sourceJson.value("opacity", outMaterialDefinition.Opacity);
            outMaterialDefinition.AlphaCutoff = sourceJson.value("alphaCutoff", outMaterialDefinition.AlphaCutoff);
            outMaterialDefinition.GlassIor = sourceJson.value("glassIor", outMaterialDefinition.GlassIor);
            outMaterialDefinition.GlassThickness = sourceJson.value("glassThickness", outMaterialDefinition.GlassThickness);
            outMaterialDefinition.GlassDispersion = sourceJson.value("glassDispersion", outMaterialDefinition.GlassDispersion);
            outMaterialDefinition.GlassCausticStrength = sourceJson.value("glassCausticStrength", outMaterialDefinition.GlassCausticStrength);

            if (!TryReadOptionalFloatArray2(sourceJson, "uvTiling", outMaterialDefinition.UvTiling))
            {
                outErrorMessage = "uvTiling must be an array with 2 float values.";
                return false;
            }

            if (!TryReadOptionalFloatArray2(sourceJson, "uvOffset", outMaterialDefinition.UvOffset))
            {
                outErrorMessage = "uvOffset must be an array with 2 float values.";
                return false;
            }

            outMaterialDefinition.UvRotationDegrees = sourceJson.value("uvRotationDegrees", outMaterialDefinition.UvRotationDegrees);
            outMaterialDefinition.UseParallaxOcclusion = sourceJson.value("useParallaxOcclusion", outMaterialDefinition.UseParallaxOcclusion);
            outMaterialDefinition.ParallaxMinSteps = sourceJson.value("parallaxMinSteps", outMaterialDefinition.ParallaxMinSteps);
            outMaterialDefinition.ParallaxMaxSteps = sourceJson.value("parallaxMaxSteps", outMaterialDefinition.ParallaxMaxSteps);
            outMaterialDefinition.ParallaxFadeDistance = sourceJson.value("parallaxFadeDistance", outMaterialDefinition.ParallaxFadeDistance);
            outMaterialDefinition.IsDoubleSided = sourceJson.value("doubleSided", outMaterialDefinition.IsDoubleSided);
            outMaterialDefinition.UseAlphaCutout = sourceJson.value("useAlphaCutout", outMaterialDefinition.UseAlphaCutout);
            outMaterialDefinition.UseTransparentBlend = sourceJson.value("useTransparentBlend", outMaterialDefinition.UseTransparentBlend);
            outMaterialDefinition.UseGlassRendering = sourceJson.value("useGlassRendering", outMaterialDefinition.UseGlassRendering);
            outMaterialDefinition.UseThinGlass = sourceJson.value("useThinGlass", outMaterialDefinition.UseThinGlass);
            outMaterialDefinition.IsDecalMaterial = sourceJson.value("isDecalMaterial", outMaterialDefinition.IsDecalMaterial);

            outMaterialDefinition.IsParticleMaterial = sourceJson.value("isParticleMaterial", outMaterialDefinition.IsParticleMaterial);
            {
                const int blendMode = sourceJson.value("particleBlendMode", static_cast<int>(outMaterialDefinition.ParticleBlend));
                outMaterialDefinition.ParticleBlend = (blendMode >= 0 && blendMode <= static_cast<int>(ParticleBlendMode::Premultiplied))
                    ? static_cast<ParticleBlendMode>(blendMode)
                    : ParticleBlendMode::Additive;
            }
            outMaterialDefinition.ParticleEmissiveIntensity = sourceJson.value("particleEmissiveIntensity", outMaterialDefinition.ParticleEmissiveIntensity);
            outMaterialDefinition.ParticleLightingInfluence = sourceJson.value("particleLightingInfluence", outMaterialDefinition.ParticleLightingInfluence);
            outMaterialDefinition.ParticleSphericalNormal = sourceJson.value("particleSphericalNormal", outMaterialDefinition.ParticleSphericalNormal);
            outMaterialDefinition.ParticleCameraFadeDistance = sourceJson.value("particleCameraFadeDistance", outMaterialDefinition.ParticleCameraFadeDistance);
            outMaterialDefinition.ParticleFlipbookColumns = sourceJson.value("particleFlipbookColumns", outMaterialDefinition.ParticleFlipbookColumns);
            outMaterialDefinition.ParticleFlipbookRows = sourceJson.value("particleFlipbookRows", outMaterialDefinition.ParticleFlipbookRows);
            outMaterialDefinition.ParticleAlphaFromLuminance = sourceJson.value("particleAlphaFromLuminance", outMaterialDefinition.ParticleAlphaFromLuminance);

            if (outMaterialDefinition.ParticleFlipbookColumns < 1)
            {
                outMaterialDefinition.ParticleFlipbookColumns = 1;
            }

            if (outMaterialDefinition.ParticleFlipbookRows < 1)
            {
                outMaterialDefinition.ParticleFlipbookRows = 1;
            }

            if (sourceJson.contains("textures"))
            {
                const json& texturesJson = sourceJson.at("textures");
                outMaterialDefinition.Textures.BaseColorTexturePath = texturesJson.value("baseColor", outMaterialDefinition.Textures.BaseColorTexturePath);
                outMaterialDefinition.Textures.NormalTexturePath = texturesJson.value("normal", outMaterialDefinition.Textures.NormalTexturePath);
                outMaterialDefinition.Textures.MetallicTexturePath = texturesJson.value("metallic", outMaterialDefinition.Textures.MetallicTexturePath);
                outMaterialDefinition.Textures.RoughnessTexturePath = texturesJson.value("roughness", outMaterialDefinition.Textures.RoughnessTexturePath);
                outMaterialDefinition.Textures.MetallicRoughnessTexturePath = texturesJson.value("metallicRoughness", outMaterialDefinition.Textures.MetallicRoughnessTexturePath);
                outMaterialDefinition.Textures.AmbientOcclusionTexturePath = texturesJson.value("ambientOcclusion", outMaterialDefinition.Textures.AmbientOcclusionTexturePath);
                outMaterialDefinition.Textures.EmissiveTexturePath = texturesJson.value("emissive", outMaterialDefinition.Textures.EmissiveTexturePath);
                outMaterialDefinition.Textures.HeightTexturePath = texturesJson.value("height", outMaterialDefinition.Textures.HeightTexturePath);
                outMaterialDefinition.Textures.OpacityTexturePath = texturesJson.value("opacity", outMaterialDefinition.Textures.OpacityTexturePath);
            }

            return true;
        }
        catch (const std::exception& exception)
        {
            outErrorMessage = exception.what();
            return false;
        }
    }

    bool ShowOpenFileDialog(
        HWND ownerWindowHandle,
        const char* dialogTitle,
        const char* filterString,
        char* inOutPathBuffer,
        const DWORD bufferCharacterCount)
    {
        OPENFILENAMEA openFileName{};
        openFileName.lStructSize = sizeof(openFileName);
        openFileName.hwndOwner = ownerWindowHandle;
        openFileName.lpstrTitle = dialogTitle;
        openFileName.lpstrFilter = filterString;
        openFileName.lpstrFile = inOutPathBuffer;
        openFileName.nMaxFile = bufferCharacterCount;
        openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        return GetOpenFileNameA(&openFileName) == TRUE;
    }

    bool ShowSaveFileDialog(
        HWND ownerWindowHandle,
        const char* dialogTitle,
        const char* filterString,
        char* inOutPathBuffer,
        const DWORD bufferCharacterCount)
    {
        OPENFILENAMEA saveFileName{};
        saveFileName.lStructSize = sizeof(saveFileName);
        saveFileName.hwndOwner = ownerWindowHandle;
        saveFileName.lpstrTitle = dialogTitle;
        saveFileName.lpstrFilter = filterString;
        saveFileName.lpstrFile = inOutPathBuffer;
        saveFileName.nMaxFile = bufferCharacterCount;
        saveFileName.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        saveFileName.lpstrDefExt = "json";
        return GetSaveFileNameA(&saveFileName) == TRUE;
    }

    bool ImportTextureAndStoreRelativePath(
        HWND ownerWindowHandle,
        const MaterialTextureSlot textureSlot,
        std::string& destinationPath,
        std::string& outErrorMessage)
    {
        char filePathBuffer[MAX_PATH] = {};
        if (!ShowOpenFileDialog(
            ownerWindowHandle,
            (std::string("Select ") + GetTextureSlotDisplayName(textureSlot) + " Texture").c_str(),
            "Texture Files\0*.png;*.jpg;*.jpeg;*.tga;*.dds;*.bmp;*.hdr\0All Files\0*.*\0",
            filePathBuffer,
            static_cast<DWORD>(std::size(filePathBuffer))))
        {
            return false;
        }

        char importStatusMessage[512] = {};
        if (!System_ImportTextureToData(
            filePathBuffer,
            "Textures",
            importStatusMessage,
            static_cast<int>(std::size(importStatusMessage))))
        {
            outErrorMessage = importStatusMessage;
            return false;
        }

        constexpr const char* importedPrefix = "Imported texture into Data: ";
        std::string importedPath = importStatusMessage;
        if (importedPath.rfind(importedPrefix, 0) == 0)
        {
            importedPath.erase(0, std::strlen(importedPrefix));
        }

        const std::filesystem::path importedTexturePath(importedPath);
        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (!dataDirectory.empty())
        {
            std::error_code relativePathError;
            const std::filesystem::path relativePath = std::filesystem::relative(importedTexturePath, dataDirectory, relativePathError);
            destinationPath = relativePathError
                ? importedTexturePath.string()
                : NormalizeRelativeDataPath(relativePath);
        }
        else
        {
            destinationPath = importedTexturePath.string();
        }

        outErrorMessage.clear();
        return true;
    }
}

std::string& MaterialTextureSet::GetTexturePath(const MaterialTextureSlot textureSlot)
{
    switch (textureSlot)
    {
    case MaterialTextureSlot::BaseColor:
        return BaseColorTexturePath;
    case MaterialTextureSlot::Normal:
        return NormalTexturePath;
    case MaterialTextureSlot::Metallic:
        return MetallicTexturePath;
    case MaterialTextureSlot::Roughness:
        return RoughnessTexturePath;
    case MaterialTextureSlot::MetallicRoughness:
        return MetallicRoughnessTexturePath;
    case MaterialTextureSlot::AmbientOcclusion:
        return AmbientOcclusionTexturePath;
    case MaterialTextureSlot::Emissive:
        return EmissiveTexturePath;
    case MaterialTextureSlot::Height:
        return HeightTexturePath;
    case MaterialTextureSlot::Opacity:
    default:
        return OpacityTexturePath;
    }
}

const std::string& MaterialTextureSet::GetTexturePath(const MaterialTextureSlot textureSlot) const
{
    switch (textureSlot)
    {
    case MaterialTextureSlot::BaseColor:
        return BaseColorTexturePath;
    case MaterialTextureSlot::Normal:
        return NormalTexturePath;
    case MaterialTextureSlot::Metallic:
        return MetallicTexturePath;
    case MaterialTextureSlot::Roughness:
        return RoughnessTexturePath;
    case MaterialTextureSlot::MetallicRoughness:
        return MetallicRoughnessTexturePath;
    case MaterialTextureSlot::AmbientOcclusion:
        return AmbientOcclusionTexturePath;
    case MaterialTextureSlot::Emissive:
        return EmissiveTexturePath;
    case MaterialTextureSlot::Height:
        return HeightTexturePath;
    case MaterialTextureSlot::Opacity:
    default:
        return OpacityTexturePath;
    }
}

MaterialEditor::MaterialEditor()
{
    NewMaterial();
}

void MaterialEditor::NewMaterial(const std::string& materialName)
{
    mCurrentMaterial = MaterialDefinition{};
    mCurrentMaterial.Name = materialName.empty() ? std::string("NewMaterial") : materialName;
    mCurrentMaterialPath.clear();
    mLastErrorMessage.clear();
}

bool MaterialEditor::LoadMaterialFromFile(const std::filesystem::path& materialFilePath)
{
    mLastErrorMessage.clear();

    std::ifstream inputStream(materialFilePath);
    if (!inputStream)
    {
        mLastErrorMessage = "Failed to open the material JSON file from disk.";
        return false;
    }

    json materialJson;
    try
    {
        inputStream >> materialJson;
    }
    catch (const std::exception& exception)
    {
        mLastErrorMessage = std::string("Failed to parse the material JSON: ") + exception.what();
        return false;
    }

    MaterialDefinition loadedMaterialDefinition;
    if (!DeserializeMaterialDefinition(materialJson, loadedMaterialDefinition, mLastErrorMessage))
    {
        return false;
    }

    mCurrentMaterial = loadedMaterialDefinition;
    mCurrentMaterialPath = std::filesystem::weakly_canonical(materialFilePath);
    return true;
}

bool MaterialEditor::SaveMaterialToFile(const std::filesystem::path& materialFilePath)
{
    mLastErrorMessage.clear();

    std::error_code createDirectoryError;
    std::filesystem::create_directories(materialFilePath.parent_path(), createDirectoryError);
    if (createDirectoryError)
    {
        mLastErrorMessage = std::string("Failed to create the destination material directory: ") + createDirectoryError.message();
        return false;
    }

    nlohmann::json materialJson = SerializeMaterialDefinition(mCurrentMaterial);

    // Carry over any keys this editor doesn't model (for example the "water"
    // block consumed by WaterRenderer).  Without this, simply opening and
    // saving such a material here silently deletes those settings, because the
    // serializer rebuilds the document from its own known fields only.
    // Read before opening the output stream, which truncates the file.
    {
        std::ifstream existingStream(materialFilePath);
        if (existingStream)
        {
            try
            {
                nlohmann::json existingJson;
                existingStream >> existingJson;
                if (existingJson.is_object())
                {
                    for (auto it = existingJson.begin(); it != existingJson.end(); ++it)
                    {
                        if (!materialJson.contains(it.key()))
                            materialJson[it.key()] = it.value();
                    }
                }
            }
            catch (...)
            {
                // Unreadable or corrupt: fall through and write a fresh document.
            }
        }
    }

    std::ofstream outputStream(materialFilePath);
    if (!outputStream)
    {
        mLastErrorMessage = "Failed to open the destination material file for writing.";
        return false;
    }

    outputStream << materialJson.dump(4);
    if (!outputStream.good())
    {
        mLastErrorMessage = "The material JSON file could not be fully written to disk.";
        return false;
    }

    mCurrentMaterialPath = std::filesystem::weakly_canonical(materialFilePath);
    return true;
}

bool MaterialEditor::SaveCurrentMaterial()
{
    if (mCurrentMaterialPath.empty())
    {
        return SaveMaterialToFile(BuildDefaultMaterialPath());
    }

    return SaveMaterialToFile(mCurrentMaterialPath);
}

bool MaterialEditor::OpenMaterialWithDialog(HWND ownerWindowHandle)
{
    char filePathBuffer[MAX_PATH] = {};
    if (!ShowOpenFileDialog(
        ownerWindowHandle,
        "Open Material",
        "Material JSON\0*.json\0All Files\0*.*\0",
        filePathBuffer,
        static_cast<DWORD>(std::size(filePathBuffer))))
    {
        return false;
    }

    return LoadMaterialFromFile(std::filesystem::path(filePathBuffer));
}

bool MaterialEditor::SaveMaterialWithDialog(HWND ownerWindowHandle)
{
    char filePathBuffer[MAX_PATH] = {};
    const std::filesystem::path defaultMaterialPath = BuildDefaultMaterialPath();
    strcpy_s(filePathBuffer, defaultMaterialPath.string().c_str());

    if (!ShowSaveFileDialog(
        ownerWindowHandle,
        "Save Material",
        "Material JSON\0*.json\0All Files\0*.*\0",
        filePathBuffer,
        static_cast<DWORD>(std::size(filePathBuffer))))
    {
        return false;
    }

    std::filesystem::path savePath(filePathBuffer);
    if (!savePath.has_extension())
    {
        savePath += ".json";
    }

    return SaveMaterialToFile(savePath);
}

bool MaterialEditor::BrowseForTexture(HWND ownerWindowHandle, const MaterialTextureSlot textureSlot)
{
    if (!ImportTextureAndStoreRelativePath(
        ownerWindowHandle,
        textureSlot,
        mCurrentMaterial.Textures.GetTexturePath(textureSlot),
        mLastErrorMessage))
    {
        return false;
    }

    if (textureSlot == MaterialTextureSlot::Opacity && !mCurrentMaterial.UseAlphaCutout)
    {
        mCurrentMaterial.UseTransparentBlend = true;
    }

    mLastErrorMessage.clear();
    return true;
}

const MaterialDefinition& MaterialEditor::GetCurrentMaterial() const
{
    return mCurrentMaterial;
}

MaterialDefinition& MaterialEditor::GetCurrentMaterial()
{
    return mCurrentMaterial;
}

const std::filesystem::path& MaterialEditor::GetCurrentMaterialPath() const
{
    return mCurrentMaterialPath;
}

const std::string& MaterialEditor::GetLastErrorMessage() const
{
    return mLastErrorMessage;
}

std::filesystem::path MaterialEditor::GetMaterialLibraryDirectory() const
{
    const std::filesystem::path dataDirectory = FindProjectDataDirectory();
    return dataDirectory.empty() ? std::filesystem::path{} : (dataDirectory / "Materials");
}

std::vector<std::filesystem::path> MaterialEditor::FindAvailableMaterials() const
{
    std::vector<std::filesystem::path> materialFiles;
    const std::filesystem::path materialLibraryDirectory = GetMaterialLibraryDirectory();
    if (materialLibraryDirectory.empty() || !std::filesystem::exists(materialLibraryDirectory))
    {
        return materialFiles;
    }

    std::error_code iteratorError;
    for (std::filesystem::recursive_directory_iterator it(
             materialLibraryDirectory,
             std::filesystem::directory_options::skip_permission_denied,
             iteratorError),
         end;
         it != end && !iteratorError;
         it.increment(iteratorError))
    {
        std::error_code statusError;
        if (!it->is_regular_file(statusError) || statusError)
        {
            continue;
        }

        if (_stricmp(it->path().extension().string().c_str(), ".json") == 0)
        {
            materialFiles.push_back(std::filesystem::weakly_canonical(it->path()));
        }
    }

    std::sort(materialFiles.begin(), materialFiles.end());
    return materialFiles;
}

std::filesystem::path MaterialEditor::BuildDefaultMaterialPath() const
{
    const std::filesystem::path materialLibraryDirectory = GetMaterialLibraryDirectory();
    if (materialLibraryDirectory.empty())
    {
        return std::filesystem::path(mCurrentMaterial.Name + ".json");
    }

    return materialLibraryDirectory / (mCurrentMaterial.Name + ".json");
}

// ---------------------------------------------------------------------------
// Multi-material helpers
// ---------------------------------------------------------------------------

namespace
{
    // Re-use the existing single-material serializer for each sub-material entry.
    json SerializeMultiMaterialDefinition(const MultiMaterialDefinition& multiMat)
    {
        json subMaterialsJson = json::array();
        for (const MaterialDefinition& subMat : multiMat.SubMaterials)
        {
            subMaterialsJson.push_back(SerializeMaterialDefinition(subMat));
        }

        return json{
            { "name", multiMat.Name },
            { "type", "MultiMaterial" },
            { "subMaterials", subMaterialsJson }
        };
    }

    bool DeserializeMultiMaterialDefinition(const json& sourceJson, MultiMaterialDefinition& outMultiMat, std::string& outErrorMessage)
    {
        try
        {
            outMultiMat.Name = sourceJson.value("name", outMultiMat.Name);
            outMultiMat.SubMaterials.clear();

            if (sourceJson.contains("subMaterials"))
            {
                for (const json& subMatJson : sourceJson.at("subMaterials"))
                {
                    MaterialDefinition subMat;
                    if (!DeserializeMaterialDefinition(subMatJson, subMat, outErrorMessage))
                    {
                        return false;
                    }
                    outMultiMat.SubMaterials.push_back(std::move(subMat));
                }
            }

            return true;
        }
        catch (const std::exception& ex)
        {
            outErrorMessage = ex.what();
            return false;
        }
    }
}

void MaterialEditor::NewMultiMaterial(const std::string& multiMaterialName)
{
    mCurrentMultiMaterial = MultiMaterialDefinition{};
    mCurrentMultiMaterial.Name = multiMaterialName.empty() ? std::string("NewMultiMaterial") : multiMaterialName;
    mCurrentMultiMaterialPath.clear();
    mLastErrorMessage.clear();
}

void MaterialEditor::AddSubMaterial(const std::string& subMaterialName)
{
    MaterialDefinition subMat;
    if (!subMaterialName.empty())
    {
        subMat.Name = subMaterialName;
    }
    else
    {
        // Auto-name like "Material_0", "Material_1" … matching the slot index.
        subMat.Name = mCurrentMultiMaterial.Name + "_" + std::to_string(mCurrentMultiMaterial.SubMaterials.size());
    }
    mCurrentMultiMaterial.SubMaterials.push_back(std::move(subMat));
    mLastErrorMessage.clear();
}

bool MaterialEditor::RemoveSubMaterial(int subMaterialIndex)
{
    if (subMaterialIndex < 0 || static_cast<size_t>(subMaterialIndex) >= mCurrentMultiMaterial.SubMaterials.size())
    {
        mLastErrorMessage = "Sub-material index is out of range.";
        return false;
    }
    mCurrentMultiMaterial.SubMaterials.erase(mCurrentMultiMaterial.SubMaterials.begin() + subMaterialIndex);
    mLastErrorMessage.clear();
    return true;
}

bool MaterialEditor::LoadMultiMaterialFromFile(const std::filesystem::path& filePath)
{
    mLastErrorMessage.clear();

    std::ifstream inputStream(filePath);
    if (!inputStream)
    {
        mLastErrorMessage = "Failed to open the multi-material JSON file from disk.";
        return false;
    }

    json multiMatJson;
    try
    {
        inputStream >> multiMatJson;
    }
    catch (const std::exception& ex)
    {
        mLastErrorMessage = std::string("Failed to parse the multi-material JSON: ") + ex.what();
        return false;
    }

    MultiMaterialDefinition loaded;
    if (!DeserializeMultiMaterialDefinition(multiMatJson, loaded, mLastErrorMessage))
    {
        return false;
    }

    mCurrentMultiMaterial = std::move(loaded);
    mCurrentMultiMaterialPath = std::filesystem::weakly_canonical(filePath);
    return true;
}

bool MaterialEditor::SaveMultiMaterialToFile(const std::filesystem::path& filePath)
{
    mLastErrorMessage.clear();

    std::error_code createDirError;
    std::filesystem::create_directories(filePath.parent_path(), createDirError);
    if (createDirError)
    {
        mLastErrorMessage = std::string("Failed to create the destination directory: ") + createDirError.message();
        return false;
    }

    std::ofstream outputStream(filePath);
    if (!outputStream)
    {
        mLastErrorMessage = "Failed to open the destination multi-material file for writing.";
        return false;
    }

    outputStream << SerializeMultiMaterialDefinition(mCurrentMultiMaterial).dump(4);
    if (!outputStream.good())
    {
        mLastErrorMessage = "The multi-material JSON file could not be fully written to disk.";
        return false;
    }

    mCurrentMultiMaterialPath = std::filesystem::weakly_canonical(filePath);
    return true;
}

bool MaterialEditor::SaveCurrentMultiMaterial()
{
    if (mCurrentMultiMaterialPath.empty())
    {
        return SaveMultiMaterialToFile(BuildDefaultMultiMaterialPath());
    }
    return SaveMultiMaterialToFile(mCurrentMultiMaterialPath);
}

bool MaterialEditor::OpenMultiMaterialWithDialog(HWND ownerWindowHandle)
{
    char filePathBuffer[MAX_PATH] = {};
    if (!ShowOpenFileDialog(
        ownerWindowHandle,
        "Open Multi-Material",
        "Multi-Material JSON\0*.json\0All Files\0*.*\0",
        filePathBuffer,
        static_cast<DWORD>(std::size(filePathBuffer))))
    {
        return false;
    }
    return LoadMultiMaterialFromFile(std::filesystem::path(filePathBuffer));
}

bool MaterialEditor::SaveMultiMaterialWithDialog(HWND ownerWindowHandle)
{
    char filePathBuffer[MAX_PATH] = {};
    const std::filesystem::path defaultPath = BuildDefaultMultiMaterialPath();
    strcpy_s(filePathBuffer, defaultPath.string().c_str());

    if (!ShowSaveFileDialog(
        ownerWindowHandle,
        "Save Multi-Material",
        "Multi-Material JSON\0*.json\0All Files\0*.*\0",
        filePathBuffer,
        static_cast<DWORD>(std::size(filePathBuffer))))
    {
        return false;
    }

    std::filesystem::path savePath(filePathBuffer);
    if (!savePath.has_extension())
    {
        savePath += ".json";
    }
    return SaveMultiMaterialToFile(savePath);
}

bool MaterialEditor::BrowseForSubMaterialTexture(HWND ownerWindowHandle, int subMaterialIndex, MaterialTextureSlot textureSlot)
{
    if (subMaterialIndex < 0 || static_cast<size_t>(subMaterialIndex) >= mCurrentMultiMaterial.SubMaterials.size())
    {
        mLastErrorMessage = "Sub-material index is out of range.";
        return false;
    }

    MaterialDefinition& subMat = mCurrentMultiMaterial.SubMaterials[subMaterialIndex];
    if (!ImportTextureAndStoreRelativePath(
        ownerWindowHandle,
        textureSlot,
        subMat.Textures.GetTexturePath(textureSlot),
        mLastErrorMessage))
    {
        return false;
    }

    if (textureSlot == MaterialTextureSlot::Opacity && !subMat.UseAlphaCutout)
    {
        subMat.UseTransparentBlend = true;
    }

    mLastErrorMessage.clear();
    return true;
}

const MultiMaterialDefinition& MaterialEditor::GetCurrentMultiMaterial() const
{
    return mCurrentMultiMaterial;
}

MultiMaterialDefinition& MaterialEditor::GetCurrentMultiMaterial()
{
    return mCurrentMultiMaterial;
}

const std::filesystem::path& MaterialEditor::GetCurrentMultiMaterialPath() const
{
    return mCurrentMultiMaterialPath;
}

std::vector<std::filesystem::path> MaterialEditor::FindAvailableMultiMaterials() const
{
    // Multi-materials are stored in Data/MultiMaterials and contain a "type":"MultiMaterial" field.
    // For a quick scan we just collect all .json files in that sub-folder.
    std::vector<std::filesystem::path> files;
    const std::filesystem::path dataDirectory = FindProjectDataDirectory();
    if (dataDirectory.empty())
    {
        return files;
    }

    const std::filesystem::path multiMatDir = dataDirectory / "MultiMaterials";
    if (!std::filesystem::exists(multiMatDir))
    {
        return files;
    }

    std::error_code iterErr;
    for (std::filesystem::recursive_directory_iterator it(multiMatDir,
             std::filesystem::directory_options::skip_permission_denied, iterErr), end;
         it != end && !iterErr;
         it.increment(iterErr))
    {
        std::error_code statusErr;
        if (it->is_regular_file(statusErr) && !statusErr &&
            _stricmp(it->path().extension().string().c_str(), ".json") == 0)
        {
            files.push_back(std::filesystem::weakly_canonical(it->path()));
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::filesystem::path MaterialEditor::BuildDefaultMultiMaterialPath() const
{
    const std::filesystem::path dataDirectory = FindProjectDataDirectory();
    const std::filesystem::path multiMatDir = dataDirectory.empty()
        ? std::filesystem::path{}
        : (dataDirectory / "MultiMaterials");

    return multiMatDir.empty()
        ? std::filesystem::path(mCurrentMultiMaterial.Name + ".json")
        : (multiMatDir / (mCurrentMultiMaterial.Name + ".json"));
}
