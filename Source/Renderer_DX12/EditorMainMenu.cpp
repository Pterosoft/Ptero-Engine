#include "pch.h"
#include "EditorMainMenu.h"

#include "Editor.h"
#include "TimeOfDaySettings.h"
#include "..\System\MaterialEditor.h"
#include "..\System\include\System\SystemAssetApi.h"

#include "../QtUi/QtUi.h"

#include <commdlg.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    bool SliderFloatWithInput(const char* label, float* value, float minValue, float maxValue, const char* format, float inputStep = 0.1f, float inputStepFast = 1.0f)
    {
        bool changed = false;
        QtUi::PushID(label);
        QtUi::BeginGroup();
        changed |= QtUi::SliderFloat("##Slider", value, minValue, maxValue, format);
        QtUi::SameLine();
        QtUi::SetNextItemWidth(90.0f);
        changed |= QtUi::InputFloat(label, value, inputStep, inputStepFast, format);
        *value = std::clamp(*value, minValue, maxValue);
        QtUi::EndGroup();
        QtUi::PopID();
        return changed;
    }

    bool SliderIntWithInput(const char* label, int* value, int minValue, int maxValue, const char* format = "%d")
    {
        bool changed = false;
        QtUi::PushID(label);
        QtUi::BeginGroup();
        changed |= QtUi::SliderInt("##Slider", value, minValue, maxValue, format);
        QtUi::SameLine();
        QtUi::SetNextItemWidth(90.0f);
        changed |= QtUi::InputInt(label, value);
        *value = std::clamp(*value, minValue, maxValue);
        QtUi::EndGroup();
        QtUi::PopID();
        return changed;
    }

    // Keep the menu-specific UI state in this file so the renderer API stays focused on frame orchestration.
    bool gShowAboutWindow = false;
    bool gShowSceneSettingsWindow = false;
    bool gShowGraphicsSettingsWindow = false;
    bool gShowAssetBrowserWindow = false;
    bool gShowMaterialEditorWindow = false;
    bool gShowTimeOfDayWindow = false;
    bool gShowGBufferDebugWindow = false;
    bool gShowShaderCompileWindow = false;
    bool gLastAssetActionSucceeded = false;
    bool gHasAssetActionResult = false;
    bool gOpenRenamePopup = false;
    bool gOpenNewFolderPopup = false;
    bool gClipboardHasValue = false;
    std::string gClipboardRelativePath;
    std::string gAssetStatusMessage = "Use the Asset Browser toolbar to import FBX files and manage Data content.";
    std::string gDataDirectoryDisplay = "Data folder not found yet.";
    std::string gSelectedFolderRelativePath;
    std::string gSelectedAssetRelativePath;
    MaterialEditor gMaterialEditor;
    char gRenameBuffer[MAX_PATH] = {};
    char gNewFolderBuffer[MAX_PATH] = {};
    // Height of the scrolling property region in both Material Editor tabs.
    //
    // Fixed rather than derived from the available content area: this UI layer
    // is immediate-mode over retained Qt widgets, so content drives the window's
    // minimum size, and sizing a region from the window it lives in makes the
    // window grow a little every frame. Chosen to leave room for the pinned
    // header and the status line inside the default 640x780 dialog.
    constexpr float kMaterialPropertiesHeight = 520.0f;

    char gMaterialNameBuffer[256] = {};
    char gMultiMaterialNameBuffer[256] = {};
    char gSceneFileBuffer[MAX_PATH] = {};
    // Index of the sub-material currently selected in the multi-material editor (-1 = none).
    int gSelectedSubMaterialIndex = -1;

    // -----------------------------------------------------------------------
    // Texture batch import progress state.
    // Import runs on a background thread so the UI stays responsive.
    // -----------------------------------------------------------------------
    struct TextureImportProgress
    {
        std::atomic<int>  Total{ 0 };      // total files queued
        std::atomic<int>  Done{ 0 };       // files completed (success or fail)
        std::atomic<bool> Running{ false };// background thread is active
        std::mutex        LogMutex;
        std::vector<std::string> Log;      // per-file result messages (guarded by LogMutex)
        std::string CurrentFile;           // file currently being imported (guarded by LogMutex)
        std::string TargetFolder;          // destination folder (set before thread launch)
    };
    TextureImportProgress gTextureImport;

    struct ShaderCompileMenuState
    {
        std::atomic<bool> Running{ false };
        std::mutex Mutex;
        std::string Status = "Shader compile has not been run yet.";
        bool HasResult = false;
        bool LastSucceeded = true;
    };
    ShaderCompileMenuState gShaderCompileMenu;

    void StartShaderCompileCommand(const CompileShadersCommandFn compileShadersCommand)
    {
        if (compileShadersCommand == nullptr)
        {
            std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
            gShaderCompileMenu.Status = "Shader compile command is not available.";
            gShaderCompileMenu.HasResult = true;
            gShaderCompileMenu.LastSucceeded = false;
            gShowShaderCompileWindow = true;
            return;
        }

        bool expectedRunning = false;
        if (!gShaderCompileMenu.Running.compare_exchange_strong(expectedRunning, true))
        {
            gShowShaderCompileWindow = true;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
            gShaderCompileMenu.Status = "Compiling shaders into Cache\\Shaders...";
            gShaderCompileMenu.HasResult = false;
            gShaderCompileMenu.LastSucceeded = true;
        }
        gShowShaderCompileWindow = true;

        std::thread([compileShadersCommand]()
        {
            std::string statusMessage;
            bool succeeded = true;
            try
            {
                statusMessage = compileShadersCommand();
                succeeded = statusMessage.find(" failed") == std::string::npos;
            }
            catch (const std::exception& exception)
            {
                statusMessage = std::string("Shader compile command failed: ") + exception.what();
                succeeded = false;
            }
            catch (...)
            {
                statusMessage = "Shader compile command failed with an unknown error.";
                succeeded = false;
            }

            {
                std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
                gShaderCompileMenu.Status = statusMessage;
                gShaderCompileMenu.HasResult = true;
                gShaderCompileMenu.LastSucceeded = succeeded;
            }

            gShaderCompileMenu.Running.store(false);
        }).detach();
    }

    void RefreshMaterialNameBuffer()
    {
        strcpy_s(gMaterialNameBuffer, gMaterialEditor.GetCurrentMaterial().Name.c_str());
    }

    void RefreshMultiMaterialNameBuffer()
    {
        strcpy_s(gMultiMaterialNameBuffer, gMaterialEditor.GetCurrentMultiMaterial().Name.c_str());
    }

    // UV transform + parallax occlusion controls. Shared by the single-material tab and
    // by each sub-material of the multi-material tab so both stay in step.
    void DrawMaterialUvAndParallaxControls(MaterialDefinition& materialDefinition)
    {
        QtUi::SeparatorText("UV Transform");

        QtUi::DragFloat("Tiling U", &materialDefinition.UvTiling[0], 0.05f, -256.0f, 256.0f, "%.3f");
        QtUi::SetItemTooltip("How many times the textures repeat across the mesh's U axis.");
        QtUi::DragFloat("Tiling V", &materialDefinition.UvTiling[1], 0.05f, -256.0f, 256.0f, "%.3f");
        QtUi::SetItemTooltip("How many times the textures repeat across the mesh's V axis.");

        if (QtUi::Button("Link V to U"))
        {
            materialDefinition.UvTiling[1] = materialDefinition.UvTiling[0];
        }
        QtUi::SameLine();
        if (QtUi::Button("Reset UV"))
        {
            materialDefinition.UvTiling = { 1.0f, 1.0f };
            materialDefinition.UvOffset = { 0.0f, 0.0f };
            materialDefinition.UvRotationDegrees = 0.0f;
        }

        QtUi::DragFloat("Offset U", &materialDefinition.UvOffset[0], 0.005f, -64.0f, 64.0f, "%.3f");
        QtUi::DragFloat("Offset V", &materialDefinition.UvOffset[1], 0.005f, -64.0f, 64.0f, "%.3f");
        QtUi::SliderFloat("Rotation", &materialDefinition.UvRotationDegrees, -180.0f, 180.0f, "%.1f deg");
        QtUi::SetItemTooltip("Rotates the source UVs about (0.5, 0.5) before tiling is applied.");

        QtUi::SeparatorText("Parallax Occlusion");

        const bool hasHeightTexture = !materialDefinition.Textures.HeightTexturePath.empty();
        QtUi::Checkbox("Parallax Occlusion Mapping", &materialDefinition.UseParallaxOcclusion);
        QtUi::SetItemTooltip("Ray marches the Height texture so the surface gains real depth "
                             "and self-occlusion without extra geometry.");
        if (materialDefinition.UseParallaxOcclusion && !hasHeightTexture)
        {
            QtUi::TextDisabled("Assign a Height texture below to see any effect.");
        }

        QtUi::BeginDisabled(!materialDefinition.UseParallaxOcclusion);
        QtUi::SliderFloat("Height Scale", &materialDefinition.HeightScale, 0.0f, 0.5f, "%.3f");
        QtUi::SetItemTooltip("Depth of the height volume in tiled UV units. Large values "
                             "exaggerate the effect and expose stretching at grazing angles.");
        QtUi::SliderInt("Min Steps", &materialDefinition.ParallaxMinSteps, 1, 64);
        QtUi::SetItemTooltip("Ray-march steps used when looking straight at the surface.");
        QtUi::SliderInt("Max Steps", &materialDefinition.ParallaxMaxSteps, 1, 256);
        QtUi::SetItemTooltip("Ray-march steps used at grazing angles, where the ray travels furthest.");
        if (materialDefinition.ParallaxMaxSteps < materialDefinition.ParallaxMinSteps)
        {
            materialDefinition.ParallaxMaxSteps = materialDefinition.ParallaxMinSteps;
        }
        QtUi::SliderFloat("Fade Distance", &materialDefinition.ParallaxFadeDistance, 0.0f, 300.0f, "%.1f m");
        QtUi::SetItemTooltip("Parallax fades out past this distance so far surfaces skip the march. 0 disables the fade.");
        QtUi::EndDisabled();
    }

    // Particle material controls. Shared by the single-material tab and by each
    // sub-material of the multi-material tab so both stay in step.
    void DrawMaterialParticleControls(MaterialDefinition& materialDefinition)
    {
        QtUi::SeparatorText("Particle");

        QtUi::Checkbox("Particle Material", &materialDefinition.IsParticleMaterial);
        QtUi::SetItemTooltip("Drawn as a camera-facing sprite by the particle renderer instead of "
                             "into the G-Buffer. Base Color is the sprite texture, Emissive Color is the glow.");

        QtUi::BeginDisabled(!materialDefinition.IsParticleMaterial);

        const char* blendModes[] = { "Additive", "Alpha Blend", "Premultiplied" };
        int blendMode = static_cast<int>(materialDefinition.ParticleBlend);
        if (QtUi::Combo("Blend Mode", &blendMode, blendModes, static_cast<int>(std::size(blendModes))))
        {
            materialDefinition.ParticleBlend = static_cast<ParticleBlendMode>(blendMode);
        }
        QtUi::SetItemTooltip("Additive for fire and sparks, Alpha Blend for smoke and steam, "
                             "Premultiplied for atlases exported from a fluid sim.");

        QtUi::DragFloat("Emissive Intensity", &materialDefinition.ParticleEmissiveIntensity, 0.05f, 0.0f, 200.0f, "%.2f");
        QtUi::SetItemTooltip("Multiplies Emissive Color. This is what pushes a flame into HDR and "
                             "what the emitter turns into light for the GI.");

        QtUi::SliderFloat("Scene Lighting", &materialDefinition.ParticleLightingInfluence, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("How much the sprite is lit by the scene. 0 for fire (it makes its own "
                             "light), around 1 for smoke so it darkens away from the flame.");

        QtUi::BeginDisabled(materialDefinition.ParticleLightingInfluence <= 0.0f);
        QtUi::SliderFloat("Spherical Normal", &materialDefinition.ParticleSphericalNormal, 0.0f, 1.0f, "%.2f");
        QtUi::SetItemTooltip("0 shades the sprite as a flat card, 1 as a sphere. Thick smoke wants "
                             "the sphere; thin wisps want the card.");
        QtUi::EndDisabled();

        QtUi::DragFloat("Camera Fade", &materialDefinition.ParticleCameraFadeDistance, 0.01f, 0.0f, 10.0f, "%.2f m");
        QtUi::SetItemTooltip("Fades the sprite out as the camera gets this close, so flying through "
                             "a fire does not end in a full-screen flash.");

        QtUi::Checkbox("Alpha From Luminance", &materialDefinition.ParticleAlphaFromLuminance);
        QtUi::SetItemTooltip("Take opacity from the brightness of the RGB instead of the alpha channel.\n"
                             "Turn this on for additive sheets authored as colour on black - flames,\n"
                             "sparks, explosions - whose alpha channel is empty or junk.");

        QtUi::SliderInt("Flipbook Columns", &materialDefinition.ParticleFlipbookColumns, 1, 16);
        QtUi::SliderInt("Flipbook Rows", &materialDefinition.ParticleFlipbookRows, 1, 16);
        QtUi::SetItemTooltip("Atlas layout of the Base Color texture, read left to right, top to "
                             "bottom. 1x1 uses the texture whole. An emitter left at 1x1 inherits this.");

        if (materialDefinition.ParticleFlipbookColumns < 1) materialDefinition.ParticleFlipbookColumns = 1;
        if (materialDefinition.ParticleFlipbookRows < 1)    materialDefinition.ParticleFlipbookRows = 1;

        QtUi::EndDisabled();
    }

    bool PromptForSceneOpenPath(HWND ownerWindowHandle, char* sceneFileBuffer, const DWORD sceneFileBufferSize)
    {
        OPENFILENAMEA openFileName{};
        openFileName.lStructSize = sizeof(openFileName);
        openFileName.hwndOwner = ownerWindowHandle;
        openFileName.lpstrTitle = "Open Scene";
        openFileName.lpstrFilter = "Scene JSON\0*.json\0All Files\0*.*\0";
        openFileName.lpstrFile = sceneFileBuffer;
        openFileName.nMaxFile = sceneFileBufferSize;
        openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        return QtUi::OpenFileName(&openFileName) == TRUE;
    }

    bool PromptForSceneSavePath(HWND ownerWindowHandle, char* sceneFileBuffer, const DWORD sceneFileBufferSize)
    {
        OPENFILENAMEA saveFileName{};
        saveFileName.lStructSize = sizeof(saveFileName);
        saveFileName.hwndOwner = ownerWindowHandle;
        saveFileName.lpstrTitle = "Save Scene";
        saveFileName.lpstrFilter = "Scene JSON\0*.json\0All Files\0*.*\0";
        saveFileName.lpstrFile = sceneFileBuffer;
        saveFileName.nMaxFile = sceneFileBufferSize;
        saveFileName.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        saveFileName.lpstrDefExt = "json";
        return QtUi::SaveFileName(&saveFileName) == TRUE;
    }

    struct AssetBrowserItem
    {
        std::string Name;
        std::string RelativePath;
        bool IsDirectory = false;
    };

    std::string NormalizeRelativeDataPath(const std::filesystem::path& relativePath);
    std::filesystem::path FindProjectDataDirectory();

    std::filesystem::path gCachedDataDirectory;
    bool gHasCachedDataDirectory = false;
    std::unordered_map<std::string, std::vector<std::string>> gCachedChildFolders;
    std::unordered_map<std::string, std::vector<AssetBrowserItem>> gCachedFolderItems;

    void InvalidateAssetBrowserDirectoryCache()
    {
        gCachedChildFolders.clear();
        gCachedFolderItems.clear();
    }

    const std::filesystem::path& GetProjectDataDirectoryCached()
    {
        if (!gHasCachedDataDirectory)
        {
            gCachedDataDirectory = FindProjectDataDirectory();
            gHasCachedDataDirectory = true;
        }

        return gCachedDataDirectory;
    }

    std::filesystem::path ResolveDataAssetPath(const std::string& assetPath)
    {
        if (assetPath.empty())
        {
            return {};
        }

        const std::filesystem::path candidatePath(assetPath);
        if (candidatePath.is_absolute())
        {
            return candidatePath;
        }

        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        return dataDirectory.empty() ? candidatePath : (dataDirectory / candidatePath);
    }

    std::optional<std::string> BuildDataRelativeAssetPath(const std::filesystem::path& assetFilePath)
    {
        if (assetFilePath.empty())
        {
            return std::nullopt;
        }

        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        if (dataDirectory.empty())
        {
            return std::nullopt;
        }

        std::error_code canonicalError;
        const std::filesystem::path canonicalDataDirectory = std::filesystem::weakly_canonical(dataDirectory, canonicalError);
        const std::filesystem::path dataRoot = canonicalError ? dataDirectory.lexically_normal() : canonicalDataDirectory;

        canonicalError.clear();
        std::filesystem::path absoluteAssetPath = assetFilePath.is_absolute()
            ? std::filesystem::weakly_canonical(assetFilePath, canonicalError)
            : std::filesystem::weakly_canonical(dataDirectory / assetFilePath, canonicalError);
        if (canonicalError)
        {
            absoluteAssetPath = assetFilePath.is_absolute()
                ? assetFilePath.lexically_normal()
                : (dataDirectory / assetFilePath).lexically_normal();
        }

        std::error_code relativeError;
        const std::filesystem::path relativePath = std::filesystem::relative(absoluteAssetPath, dataRoot, relativeError);
        if (relativeError || relativePath.empty())
        {
            return std::nullopt;
        }

        auto firstPathPart = relativePath.begin();
        if (firstPathPart != relativePath.end() && firstPathPart->string() == "..")
        {
            return std::nullopt;
        }

        const std::string normalizedPath = NormalizeRelativeDataPath(relativePath);
        if (normalizedPath.empty())
        {
            return std::nullopt;
        }

        return normalizedPath;
    }

    void DrawSelectedMeshMaterialBinding(Editor* editorInstance, const char* id, const std::filesystem::path& currentMaterialPath, const bool multiMaterialMode)
    {
        QtUi::PushID(id);
        QtUi::SeparatorText("Selected Mesh");

        Entity* selectedEntity = editorInstance != nullptr ? editorInstance->GetSelectedEntity() : nullptr;
        if (selectedEntity == nullptr)
        {
            QtUi::TextDisabled("No selected mesh.");
            QtUi::PopID();
            return;
        }

        if (!selectedEntity->Mesh.has_value())
        {
            QtUi::TextDisabled("Selected entity has no mesh component.");
            QtUi::PopID();
            return;
        }

        MeshComponent& meshComponent = *selectedEntity->Mesh;
        QtUi::TextWrapped("Selected Entity: %s", selectedEntity->Name.c_str());
        QtUi::TextWrapped("Assigned Material: %s", meshComponent.MaterialPath.empty() ? "(none)" : meshComponent.MaterialPath.c_str());

        const std::optional<std::string> currentAssignmentPath = BuildDataRelativeAssetPath(currentMaterialPath);
        const std::string assignedPath = NormalizeRelativeDataPath(std::filesystem::path(meshComponent.MaterialPath));
        if (currentAssignmentPath.has_value()
            && !assignedPath.empty()
            && assignedPath != *currentAssignmentPath)
        {
            QtUi::TextColored(UiVec4(1.0f, 0.78f, 0.25f, 1.0f), "Current editor file is not assigned to the selected mesh.");
        }

        const bool hasAssignedMaterial = !meshComponent.MaterialPath.empty();
        QtUi::BeginDisabled(!hasAssignedMaterial);
        if (QtUi::Button("Open Assigned"))
        {
            const std::filesystem::path assignedMaterialPath = ResolveDataAssetPath(meshComponent.MaterialPath);
            if (multiMaterialMode)
            {
                if (gMaterialEditor.LoadMultiMaterialFromFile(assignedMaterialPath))
                {
                    gSelectedSubMaterialIndex = -1;
                    RefreshMultiMaterialNameBuffer();
                }
            }
            else if (gMaterialEditor.LoadMaterialFromFile(assignedMaterialPath))
            {
                RefreshMaterialNameBuffer();
            }
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        const bool canAssignCurrentMaterial = currentAssignmentPath.has_value();
        QtUi::BeginDisabled(!canAssignCurrentMaterial);
        if (QtUi::Button("Save + Assign Current"))
        {
            bool savedCurrentMaterial = false;
            if (multiMaterialMode)
            {
                gMaterialEditor.GetCurrentMultiMaterial().Name = gMultiMaterialNameBuffer;
                savedCurrentMaterial = gMaterialEditor.SaveCurrentMultiMaterial();
            }
            else
            {
                gMaterialEditor.GetCurrentMaterial().Name = gMaterialNameBuffer;
                savedCurrentMaterial = gMaterialEditor.SaveCurrentMaterial();
            }

            if (savedCurrentMaterial)
            {
                meshComponent.MaterialPath = *currentAssignmentPath;
                if (editorInstance != nullptr)
                {
                    editorInstance->MarkSceneDirty();
                }
            }
        }
        QtUi::EndDisabled();

        if (!canAssignCurrentMaterial)
        {
            QtUi::TextDisabled("Save the current material before assigning it.");
        }

        QtUi::PopID();
    }

    std::string BuildChildRelativePath(const std::string& parentFolderRelativePath, const std::filesystem::path& childName)
    {
        return NormalizeRelativeDataPath(
            parentFolderRelativePath.empty()
            ? childName
            : (std::filesystem::path(parentFolderRelativePath) / childName));
    }

    using SystemImportFbxToDataFn = decltype(&System_ImportFbxToData);
    using SystemImportTextureToDataFn = decltype(&System_ImportTextureToData);
    using SystemGenerateMeshLodsFn = decltype(&System_GenerateMeshLods);
    using SystemGenerateCollisionsFn = decltype(&System_GenerateCollisions);

    void InvalidateEditorMeshAssets(void* editor, const std::string& relativeGeometryPath);

    std::string NormalizeRelativeDataPath(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path normalizedPath = relativePath.lexically_normal();
        return (normalizedPath.empty() || normalizedPath == ".") ? std::string{} : normalizedPath.generic_string();
    }

    bool IsPathInsideRoot(const std::filesystem::path& rootPath, const std::filesystem::path& candidatePath)
    {
        const std::filesystem::path normalizedRootPath = rootPath.lexically_normal();
        const std::filesystem::path normalizedCandidatePath = candidatePath.lexically_normal();

        auto rootIterator = normalizedRootPath.begin();
        auto candidateIterator = normalizedCandidatePath.begin();
        for (; rootIterator != normalizedRootPath.end(); ++rootIterator, ++candidateIterator)
        {
            if (candidateIterator == normalizedCandidatePath.end() || *rootIterator != *candidateIterator)
            {
                return false;
            }
        }

        return true;
    }

    std::filesystem::path FindProjectDataDirectory()
    {
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD characterCount = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (characterCount == 0 || characterCount == std::size(executablePath))
        {
            return {};
        }

        // Walk upward from the editor executable until the repository Data folder is found.
        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        while (!currentPath.empty())
        {
            const std::filesystem::path dataDirectory = currentPath / "Data";
            if (std::filesystem::exists(dataDirectory) && std::filesystem::is_directory(dataDirectory))
            {
                return dataDirectory;
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

    std::filesystem::path GetAbsoluteDataPath(const std::string& relativePath)
    {
        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        return relativePath.empty()
            ? dataDirectory
            : (dataDirectory / std::filesystem::path(relativePath)).lexically_normal();
    }

    void SetAssetBrowserStatus(const bool succeeded, const std::string& statusMessage)
    {
        gLastAssetActionSucceeded = succeeded;
        gHasAssetActionResult = true;
        gAssetStatusMessage = statusMessage;
    }

    void RefreshAssetBrowserState()
    {
        const std::filesystem::path& dataDirectory = GetProjectDataDirectoryCached();
        if (dataDirectory.empty())
        {
            gDataDirectoryDisplay = "Failed to locate the repository Data folder.";
            gSelectedFolderRelativePath.clear();
            gSelectedAssetRelativePath.clear();
            return;
        }

        gDataDirectoryDisplay = dataDirectory.string();

        const std::filesystem::path selectedFolderPath = GetAbsoluteDataPath(gSelectedFolderRelativePath);
        if (!gSelectedFolderRelativePath.empty() && !std::filesystem::exists(selectedFolderPath))
        {
            gSelectedFolderRelativePath.clear();
        }

        const std::filesystem::path selectedAssetPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        if (!gSelectedAssetRelativePath.empty() && !std::filesystem::exists(selectedAssetPath))
        {
            gSelectedAssetRelativePath.clear();
        }
    }

    const std::vector<std::string>& GetChildFolderRelativePaths(const std::string& parentFolderRelativePath)
    {
        static const std::vector<std::string> emptyChildFolders;

        const auto cachedFolders = gCachedChildFolders.find(parentFolderRelativePath);
        if (cachedFolders != gCachedChildFolders.end())
        {
            return cachedFolders->second;
        }

        std::vector<std::string> childFolders;

        const std::filesystem::path absoluteFolderPath = GetAbsoluteDataPath(parentFolderRelativePath);
        if (absoluteFolderPath.empty())
        {
            return emptyChildFolders;
        }

        std::error_code iteratorError;
        for (std::filesystem::directory_iterator it(absoluteFolderPath, iteratorError), end;
             it != end && !iteratorError;
             it.increment(iteratorError))
        {
            std::error_code statusError;
            if (!it->is_directory(statusError) || statusError)
            {
                continue;
            }

            childFolders.push_back(BuildChildRelativePath(parentFolderRelativePath, it->path().filename()));
        }

        std::sort(childFolders.begin(), childFolders.end());
        return gCachedChildFolders.emplace(parentFolderRelativePath, std::move(childFolders)).first->second;
    }

    const std::vector<AssetBrowserItem>& CollectFolderItems(const std::string& folderRelativePath)
    {
        static const std::vector<AssetBrowserItem> emptyItems;

        const auto cachedItems = gCachedFolderItems.find(folderRelativePath);
        if (cachedItems != gCachedFolderItems.end())
        {
            return cachedItems->second;
        }

        std::vector<AssetBrowserItem> items;

        const std::filesystem::path absoluteFolderPath = GetAbsoluteDataPath(folderRelativePath);
        if (absoluteFolderPath.empty())
        {
            return emptyItems;
        }

        std::error_code iteratorError;
        for (std::filesystem::directory_iterator it(absoluteFolderPath, iteratorError), end;
             it != end && !iteratorError;
             it.increment(iteratorError))
        {
            std::error_code statusError;
            const bool isDirectory = it->is_directory(statusError);
            const bool isRegularFile = it->is_regular_file(statusError);
            if (statusError || (!isDirectory && !isRegularFile))
            {
                continue;
            }

            AssetBrowserItem item;
            item.Name = it->path().filename().string();
            item.RelativePath = BuildChildRelativePath(folderRelativePath, it->path().filename());
            item.IsDirectory = isDirectory;
            items.push_back(item);
        }

        std::sort(
            items.begin(),
            items.end(),
            [](const AssetBrowserItem& leftItem, const AssetBrowserItem& rightItem)
            {
                if (leftItem.IsDirectory != rightItem.IsDirectory)
                {
                    return leftItem.IsDirectory > rightItem.IsDirectory;
                }

                return _stricmp(leftItem.Name.c_str(), rightItem.Name.c_str()) < 0;
            });

        return gCachedFolderItems.emplace(folderRelativePath, std::move(items)).first->second;
    }

    std::filesystem::path BuildUniqueDuplicatePath(const std::filesystem::path& sourcePath, const std::filesystem::path& destinationDirectory)
    {
        const std::string baseName = sourcePath.stem().string().empty()
            ? sourcePath.filename().string()
            : sourcePath.stem().string();
        const std::string extension = sourcePath.has_extension() ? sourcePath.extension().string() : std::string{};

        std::filesystem::path candidatePath = destinationDirectory / (baseName + "_Copy" + extension);
        int copyIndex = 2;
        while (std::filesystem::exists(candidatePath))
        {
            candidatePath = destinationDirectory / (baseName + "_Copy" + std::to_string(copyIndex) + extension);
            ++copyIndex;
        }

        return candidatePath;
    }

    HMODULE EnsureSystemModuleLoaded(char* statusMessage, const int statusMessageCapacity)
    {
        // The renderer resolves the System DLL on demand so the editor UI can stay responsive even if the DLL is missing.
        HMODULE systemModule = GetModuleHandleW(L"System.dll");
        if (systemModule == nullptr)
        {
            systemModule = LoadLibraryW(L"System.dll");
        }

        if (systemModule == nullptr && statusMessage != nullptr && statusMessageCapacity > 0)
        {
            _snprintf_s(
                statusMessage,
                static_cast<size_t>(statusMessageCapacity),
                _TRUNCATE,
                "Failed to load System.dll. Win32 error: %lu",
                GetLastError());
        }

        return systemModule;
    }

    bool ImportFbxIntoDataFromSystem(
        const char* sourceFbxPath,
        const char* targetDirectoryRelativeToData,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto importFbx = reinterpret_cast<SystemImportFbxToDataFn>(GetProcAddress(systemModule, "System_ImportFbxToData"));
        if (importFbx == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_ImportFbxToData export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return importFbx(sourceFbxPath, targetDirectoryRelativeToData, statusMessage, statusMessageCapacity);
    }

    bool ImportTextureIntoDataFromSystem(
        const char* sourceTexturePath,
        const char* targetDirectoryRelativeToData,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto importTexture = reinterpret_cast<SystemImportTextureToDataFn>(GetProcAddress(systemModule, "System_ImportTextureToData"));
        if (importTexture == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_ImportTextureToData export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return importTexture(sourceTexturePath, targetDirectoryRelativeToData, statusMessage, statusMessageCapacity);
    }

    bool GenerateMeshLodsFromSystem(
        const char* geometryPath,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto generateMeshLods = reinterpret_cast<SystemGenerateMeshLodsFn>(GetProcAddress(systemModule, "System_GenerateMeshLods"));
        if (generateMeshLods == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_GenerateMeshLods export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return generateMeshLods(geometryPath, statusMessage, statusMessageCapacity);
    }

    bool GenerateCollisionsFromSystem(
        const char* fbxOrPteroPath,
        char* statusMessage,
        const int statusMessageCapacity)
    {
        if (statusMessage != nullptr && statusMessageCapacity > 0)
        {
            statusMessage[0] = '\0';
        }

        HMODULE systemModule = EnsureSystemModuleLoaded(statusMessage, statusMessageCapacity);
        if (systemModule == nullptr)
        {
            return false;
        }

        const auto generateCollisions = reinterpret_cast<SystemGenerateCollisionsFn>(GetProcAddress(systemModule, "System_GenerateCollisions"));
        if (generateCollisions == nullptr)
        {
            if (statusMessage != nullptr && statusMessageCapacity > 0)
            {
                _snprintf_s(
                    statusMessage,
                    static_cast<size_t>(statusMessageCapacity),
                    _TRUNCATE,
                    "System.dll is missing the System_GenerateCollisions export. Win32 error: %lu",
                    GetLastError());
            }

            return false;
        }

        return generateCollisions(fbxOrPteroPath, statusMessage, statusMessageCapacity);
    }

    bool IsGeometryAssetPath(const std::string& relativePath)
    {
        const std::string extension = std::filesystem::path(relativePath).extension().string();
        return _stricmp(extension.c_str(), ".ptero") == 0 || _stricmp(extension.c_str(), ".fbx") == 0;
    }

    bool RegenerateSelectedGeometryLods(void* editor)
    {
        if (gSelectedAssetRelativePath.empty() || !IsGeometryAssetPath(gSelectedAssetRelativePath))
        {
            SetAssetBrowserStatus(false, "Select an FBX or .ptero geometry asset before generating LODs.");
            return false;
        }

        const std::filesystem::path geometryPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        char statusMessage[512] = {};
        const bool succeeded = GenerateMeshLodsFromSystem(
            geometryPath.string().c_str(),
            statusMessage,
            static_cast<int>(std::size(statusMessage)));
        SetAssetBrowserStatus(succeeded, statusMessage);
        if (succeeded)
        {
            InvalidateEditorMeshAssets(editor, gSelectedAssetRelativePath);
        }
        return succeeded;
    }

    bool GenerateSelectedGeometryCollisions()
    {
        if (gSelectedAssetRelativePath.empty() || !IsGeometryAssetPath(gSelectedAssetRelativePath))
        {
            SetAssetBrowserStatus(false, "Select an FBX or .ptero geometry asset before generating collisions.");
            return false;
        }

        const std::filesystem::path geometryPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        char statusMessage[512] = {};
        const bool succeeded = GenerateCollisionsFromSystem(
            geometryPath.string().c_str(),
            statusMessage,
            static_cast<int>(std::size(statusMessage)));
        SetAssetBrowserStatus(succeeded, statusMessage);
        return succeeded;
    }

    void InvalidateEditorMeshAssets(void* editor, const std::string& relativeGeometryPath)
    {
        if (editor == nullptr || relativeGeometryPath.empty())
        {
            return;
        }

        Editor* editorInstance = static_cast<Editor*>(editor);
        const std::filesystem::path requestedPath(relativeGeometryPath);
        const std::string requestedPathString = requestedPath.generic_string();
        const std::string requestedStem = requestedPath.stem().generic_string();
        const bool requestedIsPtero = _stricmp(requestedPath.extension().string().c_str(), ".ptero") == 0;

        for (Entity& entity : editorInstance->GetEntities())
        {
            if (!entity.Mesh.has_value())
            {
                continue;
            }

            MeshComponent& meshComponent = *entity.Mesh;
            if (meshComponent.MeshPath.empty())
            {
                continue;
            }

            const std::filesystem::path entityMeshPath(meshComponent.MeshPath);
            const std::string entityPathString = entityMeshPath.generic_string();
            const std::string entityStem = entityMeshPath.stem().generic_string();
            const bool matchesExactPath = _stricmp(entityPathString.c_str(), requestedPathString.c_str()) == 0;
            const bool matchesCookedPair = requestedIsPtero && _stricmp(entityStem.c_str(), requestedStem.c_str()) == 0;
            const bool matchesFbxPair = !requestedIsPtero && _stricmp(entityPathString.c_str(), requestedPathString.c_str()) == 0;
            if (matchesExactPath || matchesCookedPair || matchesFbxPair)
            {
                meshComponent.MeshAsset.reset();
            }
        }
    }

    bool PromptForAndImportFbx(HWND windowHandle, const std::string& targetFolderRelativePath)
    {
        char selectedFilePath[MAX_PATH] = {};
        OPENFILENAMEA openFileName{};
        openFileName.lStructSize = sizeof(openFileName);
        openFileName.hwndOwner = windowHandle;
        openFileName.lpstrFilter = "FBX Files\0*.fbx\0All Files\0*.*\0";
        openFileName.lpstrFile = selectedFilePath;
        openFileName.nMaxFile = static_cast<DWORD>(std::size(selectedFilePath));
        openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

        if (!QtUi::OpenFileName(&openFileName))
        {
            return false;
        }

        char importStatusMessage[512] = {};
        SetAssetBrowserStatus(
            ImportFbxIntoDataFromSystem(
                selectedFilePath,
                targetFolderRelativePath.c_str(),
                importStatusMessage,
                static_cast<int>(std::size(importStatusMessage))),
            importStatusMessage);
        if (gLastAssetActionSucceeded)
        {
            InvalidateAssetBrowserDirectoryCache();
        }
        return true;
    }

    bool PromptForAndImportTexture(HWND windowHandle, const std::string& targetFolderRelativePath)
    {
        // Use a large buffer so Windows can pack multiple selected paths.
        // Format: "dir\0file1\0file2\0...\0\0"  (or just "full\path\0\0" for single selection)
        constexpr DWORD kMultiSelectBufferSize = 32768;
        std::vector<char> fileBuffer(kMultiSelectBufferSize, '\0');

        OPENFILENAMEA openFileName{};
        openFileName.lStructSize  = sizeof(openFileName);
        openFileName.hwndOwner    = windowHandle;
        openFileName.lpstrFilter  = "Texture Files\0*.png;*.jpg;*.jpeg;*.tga;*.dds;*.bmp;*.hdr\0All Files\0*.*\0";
        openFileName.lpstrFile    = fileBuffer.data();
        openFileName.nMaxFile     = kMultiSelectBufferSize;
        openFileName.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;

        if (!QtUi::OpenFileName(&openFileName))
        {
            return false;
        }

        // Parse the multi-select result buffer.
        // Single selection: the buffer is just the full path (no embedded nulls after the name).
        // Multi selection:  buffer = "directory\0name1\0name2\0...\0\0"
        std::vector<std::string> sourcePaths;
        const char* p = fileBuffer.data();
        std::string directory = p;
        p += directory.size() + 1;

        if (*p == '\0')
        {
            // Single file – the whole string is the full path.
            sourcePaths.push_back(directory);
        }
        else
        {
            // Multiple files – prepend directory to each name.
            while (*p != '\0')
            {
                std::string name = p;
                sourcePaths.push_back(directory + "\\" + name);
                p += name.size() + 1;
            }
        }

        if (sourcePaths.empty())
        {
            return false;
        }

        // If another import is still running, ignore this request.
        if (gTextureImport.Running.load())
        {
            SetAssetBrowserStatus(false, "A texture import is already in progress.");
            return false;
        }

        // Reset progress state and launch the background import thread.
        {
            std::lock_guard<std::mutex> lock(gTextureImport.LogMutex);
            gTextureImport.Log.clear();
            gTextureImport.CurrentFile.clear();
        }
        gTextureImport.Total.store(static_cast<int>(sourcePaths.size()));
        gTextureImport.Done.store(0);
        gTextureImport.TargetFolder = targetFolderRelativePath;
        gTextureImport.Running.store(true);

        std::thread([paths = std::move(sourcePaths), targetFolder = targetFolderRelativePath]()
        {
            for (const std::string& sourcePath : paths)
            {
                const std::string fileName = std::filesystem::path(sourcePath).filename().string();
                {
                    std::lock_guard<std::mutex> lock(gTextureImport.LogMutex);
                    gTextureImport.CurrentFile = fileName;
                }

                char statusMsg[512] = {};
                const bool ok = ImportTextureIntoDataFromSystem(
                    sourcePath.c_str(),
                    targetFolder.c_str(),
                    statusMsg,
                    static_cast<int>(std::size(statusMsg)));

                std::string logLine = ok
                    ? (std::string("OK  ") + fileName)
                    : (std::string("ERR ") + fileName + ": " + statusMsg);

                if (ok)
                {
                    InvalidateAssetBrowserDirectoryCache();
                }

                {
                    std::lock_guard<std::mutex> lock(gTextureImport.LogMutex);
                    gTextureImport.Log.push_back(std::move(logLine));
                }
                gTextureImport.Done.fetch_add(1);
            }

            {
                std::lock_guard<std::mutex> lock(gTextureImport.LogMutex);
                gTextureImport.CurrentFile.clear();
            }
            gTextureImport.Running.store(false);
        }).detach();

        return true;
    }

    void CopySelectedAssetToClipboard()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            return;
        }

        gClipboardRelativePath = gSelectedAssetRelativePath;
        gClipboardHasValue = true;
        SetAssetBrowserStatus(true, "Copied asset selection to the browser clipboard.");
    }

    bool PasteClipboardIntoSelectedFolder()
    {
        if (!gClipboardHasValue || gClipboardRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Copy a file or folder before using Paste.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path sourcePath = GetAbsoluteDataPath(gClipboardRelativePath);
        const std::filesystem::path destinationDirectory = GetAbsoluteDataPath(gSelectedFolderRelativePath);
        if (dataDirectory.empty() || !std::filesystem::exists(sourcePath) || !std::filesystem::exists(destinationDirectory))
        {
            SetAssetBrowserStatus(false, "The source or destination path is no longer available inside Data.");
            return false;
        }

        const std::filesystem::path destinationPath = BuildUniqueDuplicatePath(sourcePath, destinationDirectory);
        if (!IsPathInsideRoot(dataDirectory, destinationPath))
        {
            SetAssetBrowserStatus(false, "Paste must stay inside the Data directory.");
            return false;
        }

        // Prevent recursive folder duplication by blocking paste operations into the copied folder or one of its children.
        if (std::filesystem::is_directory(sourcePath) && IsPathInsideRoot(sourcePath, destinationDirectory))
        {
            SetAssetBrowserStatus(false, "Cannot paste a folder into itself or one of its children.");
            return false;
        }

        std::error_code copyError;
        if (std::filesystem::is_directory(sourcePath))
        {
            std::filesystem::copy(sourcePath, destinationPath, std::filesystem::copy_options::recursive, copyError);
        }
        else
        {
            std::filesystem::copy_file(sourcePath, destinationPath, std::filesystem::copy_options::overwrite_existing, copyError);
        }

        if (copyError)
        {
            SetAssetBrowserStatus(false, std::string("Paste failed: ") + copyError.message());
            return false;
        }

        gSelectedAssetRelativePath = NormalizeRelativeDataPath(std::filesystem::relative(destinationPath, dataDirectory));
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Pasted asset into the selected folder.");
        return true;
    }

    bool DeleteSelectedAsset()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Select a file or folder before removing it.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path selectedPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        if (dataDirectory.empty() || !std::filesystem::exists(selectedPath) || !IsPathInsideRoot(dataDirectory, selectedPath))
        {
            SetAssetBrowserStatus(false, "The selected asset is no longer available inside Data.");
            return false;
        }

        std::error_code removeError;
        if (std::filesystem::is_directory(selectedPath))
        {
            std::filesystem::remove_all(selectedPath, removeError);
        }
        else
        {
            std::filesystem::remove(selectedPath, removeError);
        }

        if (removeError)
        {
            SetAssetBrowserStatus(false, std::string("Remove failed: ") + removeError.message());
            return false;
        }

        gSelectedAssetRelativePath.clear();
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Removed asset from the Data folder.");
        return true;
    }

    bool RenameSelectedAsset()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Select a file or folder before renaming it.");
            return false;
        }

        const std::string requestedName = gRenameBuffer;
        if (requestedName.empty() || requestedName.find_first_of("\\/") != std::string::npos)
        {
            SetAssetBrowserStatus(false, "Enter a valid file or folder name.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path selectedPath = GetAbsoluteDataPath(gSelectedAssetRelativePath);
        const std::filesystem::path renamedPath = selectedPath.parent_path() / requestedName;
        if (dataDirectory.empty() || !std::filesystem::exists(selectedPath) || !IsPathInsideRoot(dataDirectory, renamedPath))
        {
            SetAssetBrowserStatus(false, "The selected asset cannot be renamed outside the Data directory.");
            return false;
        }

        if (std::filesystem::exists(renamedPath))
        {
            SetAssetBrowserStatus(false, "An asset with that name already exists in the selected folder.");
            return false;
        }

        std::error_code renameError;
        std::filesystem::rename(selectedPath, renamedPath, renameError);
        if (renameError)
        {
            SetAssetBrowserStatus(false, std::string("Rename failed: ") + renameError.message());
            return false;
        }

        gSelectedAssetRelativePath = NormalizeRelativeDataPath(std::filesystem::relative(renamedPath, dataDirectory));
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Renamed asset successfully.");
        return true;
    }

    bool CreateFolderInSelectedFolder()
    {
        const std::string requestedFolderName = gNewFolderBuffer;
        if (requestedFolderName.empty() || requestedFolderName.find_first_of("\\/") != std::string::npos)
        {
            SetAssetBrowserStatus(false, "Enter a valid folder name.");
            return false;
        }

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path newFolderPath = GetAbsoluteDataPath(gSelectedFolderRelativePath) / requestedFolderName;
        if (dataDirectory.empty() || !IsPathInsideRoot(dataDirectory, newFolderPath))
        {
            SetAssetBrowserStatus(false, "New folders must stay inside the Data directory.");
            return false;
        }

        std::error_code createError;
        if (!std::filesystem::create_directory(newFolderPath, createError) || createError)
        {
            SetAssetBrowserStatus(false, createError ? std::string("Create folder failed: ") + createError.message() : "A folder with that name already exists.");
            return false;
        }

        gSelectedAssetRelativePath = NormalizeRelativeDataPath(std::filesystem::relative(newFolderPath, dataDirectory));
        InvalidateAssetBrowserDirectoryCache();
        SetAssetBrowserStatus(true, "Created a new folder inside the selected directory.");
        return true;
    }

    void BeginRenameSelectedAsset()
    {
        if (gSelectedAssetRelativePath.empty())
        {
            SetAssetBrowserStatus(false, "Select a file or folder before renaming it.");
            return;
        }

        const std::filesystem::path selectedPath(gSelectedAssetRelativePath);
        strcpy_s(gRenameBuffer, selectedPath.filename().string().c_str());
        gOpenRenamePopup = true;
    }

    void BeginCreateFolder()
    {
        gNewFolderBuffer[0] = '\0';
        gOpenNewFolderPopup = true;
    }

    void SelectFolder(const std::string& folderRelativePath)
    {
        gSelectedFolderRelativePath = folderRelativePath;
        gSelectedAssetRelativePath.clear();
    }

    void RenderFolderTreeNode(const std::string& folderRelativePath)
    {
        const std::filesystem::path folderPath = folderRelativePath.empty()
            ? std::filesystem::path("Data")
            : std::filesystem::path(folderRelativePath).filename();
        const std::vector<std::string> childFolders = GetChildFolderRelativePaths(folderRelativePath);
        const QtUiTreeNodeFlags nodeFlags = QtUiTreeNodeFlags_OpenOnArrow
            | QtUiTreeNodeFlags_OpenOnDoubleClick
            | (childFolders.empty() ? QtUiTreeNodeFlags_Leaf : 0)
            | (gSelectedFolderRelativePath == folderRelativePath ? QtUiTreeNodeFlags_Selected : 0);

        const bool isOpen = QtUi::TreeNodeEx(folderRelativePath.empty() ? "DataRoot" : folderRelativePath.c_str(), nodeFlags, "%s", folderPath.string().c_str());
        if (QtUi::IsItemClicked())
        {
            SelectFolder(folderRelativePath);
        }

        if (QtUi::BeginPopupContextItem())
        {
            if (QtUi::MenuItem("Import FBX Here..."))
            {
                PromptForAndImportFbx(GetActiveWindow(), folderRelativePath);
                RefreshAssetBrowserState();
            }

            if (QtUi::MenuItem("Import Texture Here..."))
            {
                PromptForAndImportTexture(GetActiveWindow(), folderRelativePath);
                RefreshAssetBrowserState();
            }

            if (QtUi::MenuItem("New Folder"))
            {
                SelectFolder(folderRelativePath);
                BeginCreateFolder();
            }

            QtUi::EndPopup();
        }

        if (isOpen)
        {
            for (const std::string& childFolderRelativePath : childFolders)
            {
                RenderFolderTreeNode(childFolderRelativePath);
            }

            QtUi::TreePop();
        }
    }
}

void RenderEditorMainMenu(
    HWND windowHandle,
    void* editor,
    UiTextureID sceneTextureId,
    const char* sceneStatusMessage,
    const char* statisticsText,
    float* cameraSpeed,
    float* viewDistanceMeters,
    bool* gridEnabled,
    bool* showStatistics,
    bool* showViewportPlacementIcons,
    bool* showComponentsPanel,
    bool* showLevelExplorerPanel,
    bool* showPropertiesPanel,
    bool* showResourceDebugPanel,
    TaaSettings* taaSettings,
    SMAASettings* smaaSettings,
    MsaaSettings* msaaSettings,
    SharpenSettings* sharpenSettings,
    DlssSettings* dlssSettings,
    TimeOfDaySettings* timeOfDaySettings,
    WindSettings* windSettings,
    GlobalIlluminationMode* globalIlluminationMode,
    RtGISettings* rtgiSettings,
    RadianceCascadesSettings* radianceCascadesSettings,
    RadianceProbeSettings* probeSettings,
    RtAOSettings* rtaoSettings,
    GtaoSettings* gtaoSettings,
    SsrSettings* ssrSettings,
    ChromaticAberrationSettings* chromaticAberrationSettings,
    AgxTonemapSettings* agxSettings,
    VolumetricFogSettings* volumetricFogSettings,
    VolumetricCloudSettings* volumetricCloudSettings,
    BloomSettings* bloomSettings,
    PointShadowSettings* pointShadowSettings,
    const GBufferDebugTextureIds* gbufferTextureIds,
    AudioManager* audioManager,
    CompileShadersCommandFn compileShadersCommand,
    float msaaResolveTimeMs)
{
    UNREFERENCED_PARAMETER(sceneTextureId);
    UNREFERENCED_PARAMETER(statisticsText);

    Editor* editorInstance = static_cast<Editor*>(editor);

    if (QtUi::BeginMainMenuBar())
    {
        const bool sceneLoading = editorInstance != nullptr && editorInstance->IsSceneLoading();

        if (QtUi::BeginMenu("File"))
        {
            if (editorInstance != nullptr && QtUi::MenuItem("New", nullptr, false, !sceneLoading))
            {
                editorInstance->NewScene(windowHandle);
            }

            if (editorInstance != nullptr && QtUi::MenuItem("Open...", nullptr, false, !sceneLoading))
            {
                if (PromptForSceneOpenPath(windowHandle, gSceneFileBuffer, static_cast<DWORD>(std::size(gSceneFileBuffer))))
                {
                    editorInstance->BeginLoadSceneFromFile(gSceneFileBuffer);
                }
            }

            if (editorInstance != nullptr && QtUi::MenuItem("Save", nullptr, false, !sceneLoading))
            {
                if (!editorInstance->GetCurrentSceneFilePath().empty())
                {
                    editorInstance->SaveSceneToFile(editorInstance->GetCurrentSceneFilePath());
                }
                else if (PromptForSceneSavePath(windowHandle, gSceneFileBuffer, static_cast<DWORD>(std::size(gSceneFileBuffer))))
                {
                    editorInstance->SaveSceneToFile(gSceneFileBuffer);
                }
            }

            if (editorInstance != nullptr && QtUi::MenuItem("Save As...", nullptr, false, !sceneLoading))
            {
                if (!editorInstance->GetCurrentSceneFilePath().empty())
                {
                    strcpy_s(gSceneFileBuffer, editorInstance->GetCurrentSceneFilePath().c_str());
                }

                if (PromptForSceneSavePath(windowHandle, gSceneFileBuffer, static_cast<DWORD>(std::size(gSceneFileBuffer))))
                {
                    editorInstance->SaveSceneToFile(gSceneFileBuffer);
                }
            }

            QtUi::Separator();
            const bool shaderCompileRunning = gShaderCompileMenu.Running.load();
            if (QtUi::MenuItem("Compile Shaders", nullptr, false, compileShadersCommand != nullptr && !shaderCompileRunning))
            {
                StartShaderCompileCommand(compileShadersCommand);
            }

            QtUi::Separator();
            if (QtUi::MenuItem("Exit"))
            {
                PostMessage(windowHandle, WM_CLOSE, 0, 0);
            }

            QtUi::EndMenu();
        }

        if (editorInstance != nullptr && QtUi::BeginMenu("Edit"))
        {
            const bool canCopy = editorInstance->CanCopySelectedEntity();
            const bool canPaste = editorInstance->CanPasteEntity();
            const bool canDelete = editorInstance->CanDeleteSelectedEntity();

            if (QtUi::MenuItem("Copy", "Ctrl+C", false, canCopy))
            {
                editorInstance->CopySelectedEntity();
            }

            if (QtUi::MenuItem("Paste", "Ctrl+V", false, canPaste))
            {
                editorInstance->PasteCopiedEntity();
            }

            if (QtUi::MenuItem("Delete", "Delete", false, canDelete))
            {
                editorInstance->DeleteSelectedEntity();
            }

            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("View"))
        {
            if (QtUi::MenuItem("G-Buffer / RT Debug..."))
                gShowGBufferDebugWindow = true;

            QtUi::EndMenu();
        }

        if (audioManager != nullptr && audioManager->IsInitialized() && QtUi::BeginMenu("Audio"))
        {
            if (QtUi::MenuItem("Audio Manager..."))
                if (editorInstance) *editorInstance->GetShowAudioManagerPanelPointer() = true;
            if (QtUi::MenuItem("Stop All"))
                audioManager->StopAll();
            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("Windows"))
        {
            if (QtUi::MenuItem("Scene Settings..."))
            {
                gShowSceneSettingsWindow = true;
            }

            if (QtUi::MenuItem("Material Editor..."))
            {
                gShowMaterialEditorWindow = true;
                RefreshMaterialNameBuffer();
            }

            if (QtUi::MenuItem("Node Graph..."))
            {
                NodeGraphEditor::Show();
            }

            if (showComponentsPanel != nullptr)
            {
                QtUi::MenuItem("Components", nullptr, showComponentsPanel);
            }

            if (showLevelExplorerPanel != nullptr)
            {
                QtUi::MenuItem("Level Explorer", nullptr, showLevelExplorerPanel);
            }

            if (showPropertiesPanel != nullptr)
            {
                QtUi::MenuItem("Properties", nullptr, showPropertiesPanel);
            }

            if (showResourceDebugPanel != nullptr)
            {
                QtUi::MenuItem("Resource Debug", nullptr, showResourceDebugPanel);
            }

            if (editorInstance != nullptr)
            {
                QtUi::MenuItem("Console", nullptr, editorInstance->GetShowConsolePanelPointer());
                QtUi::MenuItem("UI Editor", nullptr, editorInstance->GetShowUiEditorPanelPointer());
            }

            if (editorInstance != nullptr)
            {
                bool* showTerrainToolWindow = editorInstance->GetShowTerrainToolWindowPointer();
                QtUi::MenuItem("Terrain Tool", nullptr, showTerrainToolWindow);
            }

            if (QtUi::MenuItem("Asset Browser..."))
            {
                gShowAssetBrowserWindow = true;
                InvalidateAssetBrowserDirectoryCache();
                RefreshAssetBrowserState();
            }

            QtUi::Separator();

            if (QtUi::MenuItem("Time of Day..."))
            {
                gShowTimeOfDayWindow = true;
            }

            if (QtUi::MenuItem("Graphics Settings..."))
            {
                gShowGraphicsSettingsWindow = true;
            }

            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("Help"))
        {
            if (QtUi::MenuItem("About Ptero Editor"))
            {
                gShowAboutWindow = true;
            }

            QtUi::EndMenu();
        }

        QtUi::EndMainMenuBar();
    }


    if (gShowSceneSettingsWindow)
    {
        QtUi::Begin("Scene Settings", &gShowSceneSettingsWindow, QtUiWindowFlags_AlwaysAutoResize);

        if (cameraSpeed != nullptr)
        {
            // Expose the camera speed directly in the editor so movement tuning does not require a rebuild.
            // The upper bound also caps what the right-drag mouse wheel can reach, since the
            // slider writes its own clamped value back every frame.
            QtUi::SliderFloat("Camera Speed", cameraSpeed, 0.0f, 200.0f, "%.2f");
            QtUi::SetItemTooltip("Hold the right mouse button in the viewport and scroll the wheel to change this while flying.");
        }

        if (gridEnabled != nullptr)
        {
            // Let the user hide the reference grid without changing the renderer's geometry setup.
            QtUi::Checkbox("Show Grid", gridEnabled);
        }

        if (showStatistics != nullptr)
        {
            // Keep the statistics toggle in scene settings so the viewport overlay can be disabled without code changes.
            QtUi::Checkbox("Show Renderer Statistics", showStatistics);
        }

        if (showViewportPlacementIcons != nullptr)
        {
            // Let the user hide the placement markers when they want an unobstructed view of the scene.
            QtUi::Checkbox("Show Geometry Placement Icons", showViewportPlacementIcons);
        }

        QtUi::End();
    }

    if (gShowShaderCompileWindow)
    {
        QtUi::SetNextWindowSize(UiVec2(560.0f, 150.0f), QtUiCond_Appearing);
        if (QtUi::Begin("Shader Compile", &gShowShaderCompileWindow, QtUiWindowFlags_AlwaysAutoResize))
        {
            const bool running = gShaderCompileMenu.Running.load();
            std::string statusMessage;
            bool hasResult = false;
            bool succeeded = true;
            {
                std::lock_guard<std::mutex> lock(gShaderCompileMenu.Mutex);
                statusMessage = gShaderCompileMenu.Status;
                hasResult = gShaderCompileMenu.HasResult;
                succeeded = gShaderCompileMenu.LastSucceeded;
            }

            if (running)
            {
                QtUi::ProgressBar(-1.0f * static_cast<float>(QtUi::GetTime()), UiVec2(-FLT_MIN, 0.0f), "Compiling");
            }

            const UiVec4 statusColor = (!hasResult || succeeded)
                ? UiVec4(0.35f, 0.85f, 0.45f, 1.0f)
                : UiVec4(0.90f, 0.45f, 0.35f, 1.0f);
            QtUi::TextColored(statusColor, "%s", statusMessage.c_str());

            QtUi::BeginDisabled(running);
            if (QtUi::Button("Close"))
            {
                gShowShaderCompileWindow = false;
            }
            QtUi::EndDisabled();
        }
        QtUi::End();
    }

    if (gShowTimeOfDayWindow && timeOfDaySettings != nullptr)
    {
        const TimeOfDaySettings defaultTimeOfDaySettings{};
        QtUi::SetNextWindowSize(UiVec2(420.0f, 0.0f), QtUiCond_FirstUseEver);
        QtUi::Begin("Time of Day", &gShowTimeOfDayWindow, QtUiWindowFlags_AlwaysAutoResize);
        if (QtUi::Button("Revert All##timeofday"))
        {
            *timeOfDaySettings = defaultTimeOfDaySettings;
        }

        QtUi::Checkbox("Enable Time Of Day##timeofday", &timeOfDaySettings->Enabled);
        QtUi::SetItemTooltip("Turns off the sun, the sky ambient, the procedural sky and the cloud "
                             "layer, leaving the scene lit only by the lights placed in it. Wind is "
                             "independent and keeps running.");

        // Everything down to the Wind section describes the sky, so it all greys out
        // together. Wind stays live: vegetation and rain read it regardless of the sky.
        QtUi::BeginDisabled(!timeOfDaySettings->Enabled);

        // ---- Time of day ------------------------------------------------
        QtUi::SeparatorText("Time of Day");

        // Display as HH:MM for readability.
        int hours   = static_cast<int>(timeOfDaySettings->TimeOfDay);
        int minutes = static_cast<int>((timeOfDaySettings->TimeOfDay - hours) * 60.0f);
        QtUi::Text("  %02d:%02d", hours, minutes);
        SliderFloatWithInput("Time Of Day", &timeOfDaySettings->TimeOfDay, 0.0f, 23.99f, "%.2f h", 0.05f, 1.0f);

        QtUi::Spacing();
        QtUi::SeparatorText("Atmosphere");
        // Turbidity: 1 = pristine, 10 = heavy haze. Tooltip describes the range.
        SliderFloatWithInput("Turbidity", &timeOfDaySettings->Turbidity, 1.0f, 10.0f, "%.1f");
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("1 = very clear sky, 10 = heavy haze / smog");
        SliderFloatWithInput("Ground Albedo", &timeOfDaySettings->GroundAlbedo, 0.0f, 1.0f, "%.2f");
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("Reflectivity of the ground (affects horizon brightness)");

        // ---- Sun --------------------------------------------------------
        QtUi::Spacing();
        QtUi::SeparatorText("Sun");

        SliderFloatWithInput("Sun Intensity (lux)", &timeOfDaySettings->SunIntensityLux,
            0.0f, 200000.0f, "%.0f lx", 100.0f, 1000.0f);
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("Real-world noon sun: ~100 000 lx\nOvercast day: ~1 000 lx");

        QtUi::Checkbox("Override Sun Color##sun", &timeOfDaySettings->OverrideSunColor);
        if (timeOfDaySettings->OverrideSunColor)
        {
            float sunCol[3] = { timeOfDaySettings->SunColorR,
                                timeOfDaySettings->SunColorG,
                                timeOfDaySettings->SunColorB };
            if (QtUi::ColorEdit3("Sun Color", sunCol, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
            {
                timeOfDaySettings->SunColorR = sunCol[0];
                timeOfDaySettings->SunColorG = sunCol[1];
                timeOfDaySettings->SunColorB = sunCol[2];
            }
        }

        // ---- Sky --------------------------------------------------------
        QtUi::Spacing();
        QtUi::SeparatorText("Sky");

        SliderFloatWithInput("Sky Intensity (lux)", &timeOfDaySettings->SkyIntensityLux,
            0.0f, 100000.0f, "%.0f lx", 100.0f, 1000.0f);
        if (QtUi::IsItemHovered())
            QtUi::SetTooltip("Clear blue sky: ~20 000 lx\nOvercast: ~10 000 lx");

        QtUi::Checkbox("Override Sky Color##sky", &timeOfDaySettings->OverrideSkyColor);
        if (timeOfDaySettings->OverrideSkyColor)
        {
            float skyCol[3] = { timeOfDaySettings->SkyColorR,
                                timeOfDaySettings->SkyColorG,
                                timeOfDaySettings->SkyColorB };
            if (QtUi::ColorEdit3("Sky Color", skyCol, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
            {
                timeOfDaySettings->SkyColorR = skyCol[0];
                timeOfDaySettings->SkyColorG = skyCol[1];
                timeOfDaySettings->SkyColorB = skyCol[2];
            }
        }

        QtUi::EndDisabled();

        // ---- Wind -------------------------------------------------------
        // Scene-wide, and deliberately not per-component: vegetation bending
        // and the rain simulation both read these values, so there is no way
        // for the two to end up disagreeing about the weather.
        if (windSettings != nullptr)
        {
            QtUi::Spacing();
            QtUi::SeparatorText("Wind");

            QtUi::Checkbox("Enable Wind##wind", &windSettings->Enabled);

            float directionDegrees = windSettings->DirectionRadians * (180.0f / 3.14159265358979323846f);
            if (SliderFloatWithInput("Direction##wind", &directionDegrees, 0.0f, 360.0f, "%.0f deg", 1.0f, 15.0f))
            {
                windSettings->DirectionRadians = directionDegrees * (3.14159265358979323846f / 180.0f);
            }
            if (QtUi::IsItemHovered())
                QtUi::SetTooltip("Heading, counter-clockwise from world +X.");

            SliderFloatWithInput("Speed##wind", &windSettings->Strength, 0.0f, 30.0f, "%.1f m/s", 0.1f, 1.0f);
            if (QtUi::IsItemHovered())
            {
                QtUi::SetTooltip(
                    "2 m/s: leaves barely move\n"
                    "8 m/s: branches in motion\n"
                    "15 m/s: whole trees swaying");
            }

            SliderFloatWithInput("Gust Amplitude##wind", &windSettings->GustAmplitude, 0.0f, 20.0f, "%.1f m/s", 0.1f, 1.0f);
            SliderFloatWithInput("Gust Frequency##wind", &windSettings->GustFrequency, 0.0f, 3.0f, "%.2f Hz", 0.01f, 0.1f);
            SliderFloatWithInput("Gust Wavelength##wind", &windSettings->GustWavelength, 1.0f, 400.0f, "%.0f m", 1.0f, 10.0f);
            if (QtUi::IsItemHovered())
            {
                QtUi::SetTooltip(
                    "Size of one gust cell.  Larger than the visible area reads as\n"
                    "a breeze crossing a meadow; smaller makes the whole field\n"
                    "twitch at once.");
            }

            SliderFloatWithInput("Flutter Frequency##wind", &windSettings->FlutterFrequency, 0.0f, 8.0f, "%.2f Hz", 0.05f, 0.5f);
            SliderFloatWithInput("Vegetation Bend Scale##wind", &windSettings->VegetationBendScale, 0.0f, 4.0f, "%.2f", 0.05f, 0.25f);
            if (QtUi::IsItemHovered())
                QtUi::SetTooltip("Global multiplier on all vegetation bending.");
        }

        QtUi::End();
    }

    if (gShowGraphicsSettingsWindow)
    {
        const TaaSettings defaultTaaSettings{};
        const SMAASettings defaultSmaaSettings{};
        const MsaaSettings defaultMsaaSettings{};
        const SharpenSettings defaultSharpenSettings{};
        const DlssSettings defaultDlssSettings{};
        const RtGISettings defaultRtgiSettings{};
        const RadianceCascadesSettings defaultRadianceCascadesSettings{};
        const RtAOSettings defaultRtaoSettings{};
        const AgxTonemapSettings defaultAgxSettings{};
        const VolumetricFogSettings defaultVolumetricFogSettings{};
        const PointShadowSettings defaultPointShadowSettings{};
        QtUi::Begin("Graphics Settings", &gShowGraphicsSettingsWindow, QtUiWindowFlags_AlwaysAutoResize);

        if (viewDistanceMeters != nullptr)
        {
            if (QtUi::CollapsingHeader("Rendering", QtUiTreeNodeFlags_DefaultOpen))
            {
                float viewDistanceKilometers = *viewDistanceMeters / 1000.0f;
                if (SliderFloatWithInput(
                    "View Distance",
                    &viewDistanceKilometers,
                    0.5f,
                    100.0f,
                    "%.1f km",
                    0.5f,
                    2.0f))
                {
                    *viewDistanceMeters = viewDistanceKilometers * 1000.0f;
                }
                QtUi::SetItemTooltip("Maximum camera and terrain rendering distance. Higher values may reduce depth precision and performance.");
            }
        }

        if (taaSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Temporal Anti-Aliasing (TAA)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##taa"))
                {
                    *taaSettings = defaultTaaSettings;
                }
                QtUi::Checkbox("Enable TAA", &taaSettings->Enabled);

                if (taaSettings->Enabled)
                {
                    // Higher blend factor = more current frame weight = less temporal smoothing but less ghosting.
                    SliderFloatWithInput("Blend Factor", &taaSettings->BlendFactor, 0.05f, 1.0f, "%.2f");
                    SliderFloatWithInput("Jitter Scale", &taaSettings->JitterScale, 0.0f, 2.0f, "%.2f");
                }
            }
        }

        if (smaaSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Subpixel Morphological Anti-Aliasing (SMAA)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##smaa"))
                {
                    *smaaSettings = defaultSmaaSettings;
                }
                QtUi::Checkbox("Enable SMAA", &smaaSettings->Enabled);
                if (smaaSettings->Enabled)
                {
                    SliderFloatWithInput("Edge Threshold##smaa", &smaaSettings->EdgeThreshold, 0.01f, 0.30f, "%.2f", 0.01f, 0.05f);
                    SliderFloatWithInput("Search Steps##smaa", &smaaSettings->MaxSearchSteps, 1.0f, 32.0f, "%.0f", 1.0f, 2.0f);
                    SliderFloatWithInput("Diagonal Search Steps##smaa", &smaaSettings->MaxSearchStepsDiag, 0.0f, 16.0f, "%.0f", 1.0f, 2.0f);
                    SliderFloatWithInput("Corner Rounding##smaa", &smaaSettings->CornerRounding, 0.0f, 100.0f, "%.0f", 1.0f, 5.0f);
                    const char* smaaDebugModes[] = { "Final Output", "Edges", "Blend Weights" };
                    QtUi::Combo("Debug View##smaa", &smaaSettings->DebugView, smaaDebugModes, std::size(smaaDebugModes));
                }
            }
        }

        if (msaaSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Multi-Sample Anti-Aliasing (MSAA)", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##msaa"))
                {
                    *msaaSettings = defaultMsaaSettings;
                }
                
                QtUi::Checkbox("Enable MSAA##msaa", &msaaSettings->Enabled);
                QtUi::SetItemTooltip("Hardware MSAA for geometry edges. Requires scene restart to take effect.\nNote: MSAA significantly increases memory usage (2-8x G-Buffer size).");
                
                if (msaaSettings->Enabled)
                {
                    // Sample count selector
                    const char* sampleCounts[] = { "2x MSAA", "4x MSAA", "8x MSAA" };
                    int sampleIndex = 1; // Default to 4x
                    if (msaaSettings->SampleCount == 2) sampleIndex = 0;
                    else if (msaaSettings->SampleCount == 4) sampleIndex = 1;
                    else if (msaaSettings->SampleCount == 8) sampleIndex = 2;
                    
                    if (QtUi::Combo("Sample Count##msaa", &sampleIndex, sampleCounts, std::size(sampleCounts)))
                    {
                        if (sampleIndex == 0) msaaSettings->SampleCount = 2;
                        else if (sampleIndex == 1) msaaSettings->SampleCount = 4;
                        else if (sampleIndex == 2) msaaSettings->SampleCount = 8;
                    }
                    QtUi::SetItemTooltip("Higher sample counts provide better quality but use more memory and bandwidth.");
                    
                    // Display memory impact
                    const float memoryMultiplier = msaaSettings->GetMemoryMultiplier();
                    QtUi::Text("Memory Impact: %.1fx G-Buffer size", memoryMultiplier);
                    
                    // Display resolve timing if available
                    if (msaaResolveTimeMs > 0.0f)
                    {
                        QtUi::Text("Resolve Time: %.2f ms", msaaResolveTimeMs);
                    }
                    
                    QtUi::Separator();
                    QtUi::TextWrapped("MSAA provides excellent geometric edge anti-aliasing. "
                                      "It works best for static scenes and complements TAA for temporal stability.");
                    
                    // Warning about combining with other AA
                    if (taaSettings && taaSettings->Enabled)
                    {
                        QtUi::Spacing();
                        QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                        QtUi::TextWrapped("Note: MSAA + TAA can be combined for best quality, but may impact performance.");
                        QtUi::PopStyleColor();
                    }
                    if (smaaSettings && smaaSettings->Enabled)
                    {
                        QtUi::Spacing();
                        QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.8f, 0.2f, 1.0f));
                        QtUi::TextWrapped("Note: MSAA + SMAA is redundant. Consider disabling one for better performance.");
                        QtUi::PopStyleColor();
                    }
                }
            }
        }

        if (sharpenSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Sharpening", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##sharpen"))
                {
                    *sharpenSettings = defaultSharpenSettings;
                }

                QtUi::Checkbox("Image Sharpening##sharpen", &sharpenSettings->ImageSharpeningEnabled);
                if (sharpenSettings->ImageSharpeningEnabled)
                {
                    SliderFloatWithInput("Image Strength##sharpen", &sharpenSettings->ImageSharpeningStrength, 0.0f, 1.5f, "%.2f", 0.01f, 0.05f);
                }

                QtUi::Checkbox("Texture Sharpening##sharpen", &sharpenSettings->TextureSharpeningEnabled);
                if (sharpenSettings->TextureSharpeningEnabled)
                {
                    SliderFloatWithInput("Texture Mip Bias##sharpen", &sharpenSettings->TextureMipLODBias, -3.0f, 0.0f, "%.2f", 0.05f, 0.25f);
                }

                sharpenSettings->Validate();
            }
        }

        if (dlssSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("DLSS Super Resolution", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##dlss"))
                {
                    *dlssSettings = defaultDlssSettings;
                }

                QtUi::Checkbox("Enable DLSS Super Resolution", &dlssSettings->Enabled);
                if (dlssSettings->Enabled)
                {
                    const char* modes[] =
                    {
                        "Off",
                        "Max Performance",
                        "Balanced",
                        "Max Quality",
                        "Ultra Performance",
                        "Ultra Quality",
                        "DLAA"
                    };
                    QtUi::Combo("Mode##dlss", &dlssSettings->Mode, modes, std::size(modes));
                }
            }
        }

        if (globalIlluminationMode != nullptr)
        {
            const char* giModes[] = { "Disabled", "RTGI", "Radiance Cascades" };
            int giMode = static_cast<int>(*globalIlluminationMode);
            if (QtUi::Combo("Global Illumination Solution", &giMode, giModes, std::size(giModes)))
                *globalIlluminationMode = static_cast<GlobalIlluminationMode>(giMode);
        }

        // -------------------------------------------------------------------
        // RTGI – ReSTIR Global Illumination
        // -------------------------------------------------------------------
        if (rtgiSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("RTGI", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##rtgi"))
                {
                    *rtgiSettings = defaultRtgiSettings;
                }
                QtUi::Checkbox("Enable RTGI##rtgi", &rtgiSettings->Enabled);
                QtUi::SetItemTooltip("Enable custom ReSTIR GI ray-traced global illumination.");

                // Show any runtime error (e.g. shader compile failure) in red.
                if (sceneStatusMessage && sceneStatusMessage[0] != '\0')
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.3f, 0.3f, 1.0f));
                    QtUi::TextWrapped("Error: %s", sceneStatusMessage);
                    QtUi::PopStyleColor();
                }

                if (rtgiSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Denoiser");

                    QtUi::Checkbox("Use NVIDIA NRD##rtgi", &rtgiSettings->UseNrdDenoiser);
                    QtUi::SetItemTooltip("Use NVIDIA NRD RELAX diffuse denoising for the RTGI result.");

                    // --- Ray generation ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Ray Generation");

                    SliderIntWithInput("Rays Per Pixel##rtgi", &rtgiSettings->RaysPerPixel, 1, 8);
                    QtUi::SetItemTooltip("Number of GI rays fired per pixel each frame. Higher = better quality, lower performance.");
                    SliderIntWithInput("Max Bounces##rtgi", &rtgiSettings->MaxBounces, 1, 4);
                    QtUi::SetItemTooltip("Maximum indirect bounce depth for GI rays.");
                    QtUi::Checkbox("Next Event Estimation##rtgi", &rtgiSettings->NextEventEstimation);
                    QtUi::SetItemTooltip("Trace a shadow ray from the secondary hit toward the sun to explicitly estimate direct lighting in the GI path.");

                    // --- Firefly suppression ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Firefly Suppression");

                    SliderFloatWithInput("Radiance Clamp##rtgi", &rtgiSettings->RadianceClamp, 0.0f, 50.0f, "%.1f");
                    QtUi::SetItemTooltip("Clamp incoming radiance to this value to suppress fireflies. Set to 0 to disable.");

                    // --- Temporal accumulation denoiser ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Temporal Accumulation");

                    SliderFloatWithInput("Accumulation Blend##rtgi", &rtgiSettings->AccumulationBlend, 0.01f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("Blend factor between current frame (1.0) and accumulated history (0.0). Lower = smoother but ghosts on motion.");

                    if (rtgiSettings->UseNrdDenoiser)
                    {
                        QtUi::Separator();
                        QtUi::TextDisabled("NRD RELAX");

                        SliderFloatWithInput("Max Accumulation Time##rtgi_nrd", &rtgiSettings->NrdMaxAccumulationTime, 0.01f, 5.0f, "%.2f s");
                        QtUi::SetItemTooltip("How long NRD is allowed to accumulate diffuse history. Higher values can reduce motion artifacts but respond more slowly to lighting changes.");
                        SliderFloatWithInput("Disocclusion Threshold##rtgi_nrd", &rtgiSettings->NrdDisocclusionThreshold, 0.001f, 0.5f, "%.3f", 0.001f, 0.01f);
                        QtUi::SetItemTooltip("Depth-based history rejection threshold used by NRD. Higher values can stabilize camera motion but may preserve more history across edges.");
                        SliderIntWithInput("A-Trous Iterations##rtgi_nrd", &rtgiSettings->NrdAtrousIterations, 2, 8);
                        QtUi::SetItemTooltip("Number of RELAX wavelet filter iterations.");
                        SliderFloatWithInput("Sharpen##rtgi_nrd", &rtgiSettings->NrdSharpenAmount, 0.0f, 1.5f, "%.2f");
                        QtUi::SetItemTooltip("Applies a lightweight post-denoise sharpen to recover lost detail. Higher values can help with softness but may re-introduce noise halos.");
                    }

                    // --- Output ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Output");

                    SliderFloatWithInput("GI Intensity##rtgi", &rtgiSettings->GiIntensity, 0.0f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Multiplier applied to the RTGI output before compositing. 1.0 = physically correct.");
                    SliderFloatWithInput("Color Leak Intensity##rtgi", &rtgiSettings->ColorLeakIntensity, 0.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Boosts bounced material tinting for stronger color bleed. 1.0 = physically based transport.");

                    // --- Specular Reflections ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Specular Reflections");

                    QtUi::Checkbox("Enable Specular##rtgi", &rtgiSettings->SpecularEnabled);
                    QtUi::SetItemTooltip("Ray-traced GGX specular reflections composited on top of the deferred lighting pass.");
                    if (rtgiSettings->SpecularEnabled)
                    {
                        SliderFloatWithInput("Roughness Threshold##rtgi", &rtgiSettings->SpecularRoughnessThreshold, 0.0f, 1.0f, "%.2f");
                        QtUi::SetItemTooltip("Surfaces with perceptual roughness above this value skip the specular ray.");
                        SliderFloatWithInput("Specular Intensity##rtgi", &rtgiSettings->SpecularIntensity, 0.0f, 4.0f, "%.2f");
                        QtUi::SetItemTooltip("Intensity multiplier for ray-traced specular reflections.");
                    }

                    // --- Debug ---
                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    // The last four replace the GI signal with one of its own
                    // inputs, so an instability can be attributed to a stage
                    // rather than guessed at. Their ids are not contiguous with
                    // the display modes, hence the explicit value map.
                    const char* debugModes[] = {
                        "Final Lit",
                        "Raw GI Radiance",
                        "GI Luminance",
                        "Specular Reflections",
                        "Diag: World Position",
                        "Diag: G-Buffer Normal",
                        "Diag: G-Buffer Depth",
                        "Diag: Flat White",
                        "Diag: Deterministic Probe",
                    };
                    const int debugModeValues[] = { 0, 1, 2, 3, 10, 11, 12, 13, 14 };

                    int debugModeIndex = 0;
                    for (int i = 0; i < static_cast<int>(std::size(debugModeValues)); ++i)
                    {
                        if (debugModeValues[i] == rtgiSettings->DebugView)
                        {
                            debugModeIndex = i;
                            break;
                        }
                    }

                    if (QtUi::Combo("Debug View##rtgi", &debugModeIndex, debugModes, static_cast<int>(std::size(debugModes))))
                    {
                        rtgiSettings->DebugView = debugModeValues[debugModeIndex];
                    }
                    QtUi::SetItemTooltip(
                        "Overlay mode for inspecting intermediate RTGI data.\n\n"
                        "The Diag modes replace the GI signal with one of its inputs, to\n"
                        "localise an instability. Read them with the denoiser OFF, or it\n"
                        "will filter out the very thing being looked for.\n\n"
                        "World Position / Normal / Depth flickering means the G-Buffer or\n"
                        "the camera matrices disagree between frames. Flat White flickering\n"
                        "means the fault is not in ray generation at all.\n\n"
                        "Deterministic Probe fires one ray along the surface normal from a\n"
                        "fixed seed, so its result depends only on where the surface is -\n"
                        "never on the camera or the frame. It will look flat and wrong as an\n"
                        "image; that is fine. What matters is whether it is STABLE. Any\n"
                        "flicker there is real instability in traversal or shading, not the\n"
                        "sampling variance that normal GI legitimately has under motion.");
                }
            }
        }

        if (radianceCascadesSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Radiance Cascades GI", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##radiancecascades"))
                    *radianceCascadesSettings = defaultRadianceCascadesSettings;

                QtUi::Checkbox("Enable Radiance Cascades##radiancecascades", &radianceCascadesSettings->Enabled);
                QtUi::SetItemTooltip("Enable radiance cascades as an alternate GI solution.");

                if (radianceCascadesSettings->Enabled)
                {
                    int cascadeCount = static_cast<int>(radianceCascadesSettings->CascadeCount);
                    int probeSpacingBase = static_cast<int>(radianceCascadesSettings->ProbeSpacingBase);
                    int raysPerProbe = static_cast<int>(radianceCascadesSettings->RaysPerProbe);
                    int sparseProbeTableCapacity = static_cast<int>(radianceCascadesSettings->SparseProbeTableCapacity);
                    int sparseProbeCellSize = static_cast<int>(radianceCascadesSettings->SparseProbeCellSize);
                    int sparseProbeSearchSteps = static_cast<int>(radianceCascadesSettings->SparseProbeSearchSteps);

                    QtUi::Separator();
                    QtUi::TextDisabled("Radiance Field");
                    if (SliderIntWithInput("Cascade Count##radiancecascades", &cascadeCount, 1, 8))
                        radianceCascadesSettings->CascadeCount = static_cast<unsigned int>(cascadeCount);
                    if (SliderIntWithInput("Probe Spacing Base##radiancecascades", &probeSpacingBase, 1, 64))
                        radianceCascadesSettings->ProbeSpacingBase = static_cast<unsigned int>(probeSpacingBase);
                    SliderFloatWithInput("Ray Length Base##radiancecascades", &radianceCascadesSettings->RayLengthBase, 0.1f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Base world-space query distance for the nearest cascade.");
                    SliderFloatWithInput("Ray Length Scale##radiancecascades", &radianceCascadesSettings->RayLengthScale, 1.0f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Distance multiplier between cascades.");
                    SliderFloatWithInput("Interval Scale##radiancecascades", &radianceCascadesSettings->IntervalLengthScale, 1.0f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Additional interval growth per cascade for wider far-field GI coverage.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Hardware RT Queries");
                    if (SliderIntWithInput("Rays Per Probe##radiancecascades", &raysPerProbe, 1, 16))
                        radianceCascadesSettings->RaysPerProbe = static_cast<unsigned int>(raysPerProbe);
                    QtUi::SetItemTooltip("Number of inline ray queries fired by each active sparse radiance probe.");
                    SliderFloatWithInput("Ray Bias##radiancecascades", &radianceCascadesSettings->RayBias, 0.001f, 0.20f, "%.3f", 0.001f, 0.01f);
                    QtUi::SetItemTooltip("World-space ray origin offset used for GI rays and shadow visibility rays.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Sparse Structure");
                    if (SliderIntWithInput("Sparse Probe Table Capacity##radiancecascades", &sparseProbeTableCapacity, 1024, 262144))
                        radianceCascadesSettings->SparseProbeTableCapacity = static_cast<unsigned int>(sparseProbeTableCapacity);
                    QtUi::SetItemTooltip("Maximum hash-table entries for visible sparse probes. Higher values reduce collisions without allocating a dense 3D volume.");
                    if (SliderIntWithInput("Sparse Probe Cell Size##radiancecascades", &sparseProbeCellSize, 1, 128))
                        radianceCascadesSettings->SparseProbeCellSize = static_cast<unsigned int>(sparseProbeCellSize);
                    QtUi::SetItemTooltip("World-space cell size for sparse radiance probes.");
                    if (SliderIntWithInput("Sparse Search Steps##radiancecascades", &sparseProbeSearchSteps, 4, 128))
                        radianceCascadesSettings->SparseProbeSearchSteps = static_cast<unsigned int>(sparseProbeSearchSteps);
                    QtUi::SetItemTooltip("Linear-probe attempts used when resolving hash collisions in the sparse probe table.");
                    SliderFloatWithInput("Sparse Reuse Strength##radiancecascades", &radianceCascadesSettings->SparseProbeReuseStrength, 0.0f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("How strongly adjacent sparse probe cells are reused when reconstructing GI for pixels without an exact cell match.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Reconstruction");
                    SliderFloatWithInput("Hysteresis##radiancecascades", &radianceCascadesSettings->Hysteresis, 0.0f, 0.99f, "%.2f");
                    QtUi::SetItemTooltip("Temporal history weight after depth and normal rejection.");
                    SliderFloatWithInput("Spatial Filter Strength##radiancecascades", &radianceCascadesSettings->SpatialFilterStrength, 0.0f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Neighborhood reconstruction strength for filling sparse probe GI across nearby pixels.");
                    SliderFloatWithInput("History Clamp Scale##radiancecascades", &radianceCascadesSettings->HistoryClampScale, 0.0f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Allowed temporal history range around the current radiance before clamping.");
                    SliderFloatWithInput("History Depth Sensitivity##radiancecascades", &radianceCascadesSettings->HistoryDepthSensitivity, 1.0f, 512.0f, "%.0f", 1.0f, 16.0f);
                    QtUi::SetItemTooltip("Higher values reject temporal and spatial samples across smaller depth differences.");
                    SliderFloatWithInput("History Normal Threshold##radiancecascades", &radianceCascadesSettings->HistoryNormalThreshold, 0.0f, 0.999f, "%.3f", 0.001f, 0.01f);
                    QtUi::SetItemTooltip("Minimum normal similarity used for history and spatial reconstruction.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Output");
                    SliderFloatWithInput("GI Intensity##radiancecascades", &radianceCascadesSettings->GiIntensity, 0.0f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Multiplier applied to the radiance cascades GI contribution during deferred lighting.");
                    SliderFloatWithInput("Color Bleed##radiancecascades", &radianceCascadesSettings->ColorBleedingStrength, 0.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Boosts bounced material tinting for stronger secondary color bleed. 1.0 = physically based transport.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");
                    const char* debugModes[] = { "Final Lit", "Radiance", "Luminance" };
                    QtUi::Combo("Debug View##radiancecascades", &radianceCascadesSettings->DebugView, debugModes, std::size(debugModes));
                }
            }
        }

        // -------------------------------------------------------------------
        // Radiance Probes – world-space irradiance SH probe grid
        // -------------------------------------------------------------------
        if (probeSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Radiance Probes"))
            {
                static RadianceProbeSettings defaultProbeSettings{};
                if (QtUi::Button("Revert All##probe"))
                    *probeSettings = defaultProbeSettings;

                QtUi::Checkbox("Enable Radiance Probes##probe", &probeSettings->Enabled);
                QtUi::SetItemTooltip("Maintain a world-space grid of ray-traced SH irradiance probes.");

                if (probeSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Grid");

                    SliderIntWithInput("Grid X##probe", &probeSettings->GridX, 1, 64);
                    SliderIntWithInput("Grid Y##probe", &probeSettings->GridY, 1, 64);
                    SliderIntWithInput("Grid Z##probe", &probeSettings->GridZ, 1, 64);
                    SliderFloatWithInput("Spacing (m)##probe", &probeSettings->Spacing, 0.5f, 10.0f, "%.2f");
                    QtUi::Checkbox("Follow Camera##probe", &probeSettings->FollowCamera);
                    QtUi::SetItemTooltip("Snaps the probe grid origin to the camera position each frame.");

                    if (!probeSettings->FollowCamera)
                    {
                        SliderFloatWithInput("Origin X##probe", &probeSettings->OriginX, -200.0f, 200.0f, "%.1f");
                        SliderFloatWithInput("Origin Y##probe", &probeSettings->OriginY, -50.0f, 50.0f,   "%.1f");
                        SliderFloatWithInput("Origin Z##probe", &probeSettings->OriginZ, -200.0f, 200.0f, "%.1f");
                    }

                    QtUi::Separator();
                    QtUi::TextDisabled("Update");

                    SliderIntWithInput("Rays Per Probe##probe", &probeSettings->RaysPerProbe, 16, 512);
                    SliderFloatWithInput("Update Blend##probe", &probeSettings->UpdateBlend, 0.01f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("Blend factor between old and new probe SH each frame. Lower = more temporal accumulation.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    QtUi::Checkbox("Show Probe Spheres##probe", &probeSettings->DebugShowProbes);
                    QtUi::SetItemTooltip("Overlay coloured spheres at each probe position visualising stored irradiance.");
                    if (probeSettings->DebugShowProbes)
                    {
                        const char* probeDebugModes[] = { "Diffuse", "Specular" };
                        QtUi::Combo("Lighting Mode##probe", &probeSettings->DebugLightingMode, probeDebugModes, std::size(probeDebugModes));
                        QtUi::SetItemTooltip("Choose whether debug spheres visualize diffuse probe irradiance or a specular-style directional response.");
                        SliderFloatWithInput("Sphere Radius##probe", &probeSettings->DebugSphereRadius, 0.05f, 2.0f, "%.2f");
                    }
                }
            }
        }

        // -------------------------------------------------------------------
        // RTAO – Ray Traced Ambient Occlusion
        // -------------------------------------------------------------------
        if (rtaoSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Ray Traced Ambient Occlusion", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##rtao"))
                {
                    *rtaoSettings = defaultRtaoSettings;
                }
                QtUi::Checkbox("Enable RTAO (Experimental)##rtao", &rtaoSettings->Enabled);
                QtUi::SetItemTooltip("Enable ray-traced ambient occlusion using DXR inline RayQuery.");

                if (rtaoSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Ray Generation");

                    SliderIntWithInput("Rays Per Pixel##rtao", &rtaoSettings->RaysPerPixel, 1, 16);
                    QtUi::SetItemTooltip("Number of occlusion rays per pixel per frame. Higher = less noise, lower performance.");

                    SliderFloatWithInput("Max Ray Length##rtao", &rtaoSettings->MaxRayLength, 0.01f, 10.0f, "%.3f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Maximum length (world units) of each occlusion ray.");

                    SliderFloatWithInput("Ray Bias##rtao", &rtaoSettings->RayBias, 0.0001f, 0.05f, "%.4f", 0.0001f, 0.001f);
                    QtUi::SetItemTooltip("Normal-direction offset applied to ray origins to avoid self-intersection.");

                    SliderFloatWithInput("AO Power##rtao", &rtaoSettings->AOPower, 0.5f, 4.0f, "%.2f");
                    QtUi::SetItemTooltip("Gamma exponent applied to the raw AO value. >1 darkens, <1 brightens.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Output");

                    SliderFloatWithInput("AO Intensity##rtao", &rtaoSettings->Intensity, 0.0f, 1.0f, "%.2f");
                    QtUi::SetItemTooltip("Blend weight of RTAO over the G-Buffer baked AO. 1.0 = full RTAO.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");

                    const char* aoDebugModes[] = { "Final AO", "Raw Noisy AO", "NRD AO" };
                    QtUi::Combo("Debug View##rtao", &rtaoSettings->DebugView, aoDebugModes, std::size(aoDebugModes));
                    QtUi::SetItemTooltip("Overlay mode for inspecting raw versus NRD-denoised RTAO.");
                }
            }
        }

        // -------------------------------------------------------------------
        // XeGTAO – Screen-Space Ground Truth Ambient Occlusion
        // -------------------------------------------------------------------
        if (gtaoSettings != nullptr)
        {
            const GtaoSettings defaultGtaoSettings{};
            if (QtUi::CollapsingHeader("XeGTAO (Screen-Space AO)"))
            {
                if (QtUi::Button("Revert All##gtao"))
                {
                    *gtaoSettings = defaultGtaoSettings;
                }
                QtUi::Checkbox("Enable XeGTAO##gtao", &gtaoSettings->Enabled);
                QtUi::SetItemTooltip("Intel XeGTAO screen-space ambient occlusion. Runs in parallel with (or instead of) RTAO.");

                if (gtaoSettings->Enabled)
                {
                    const char* qualityNames[] = { "Low", "Medium", "High", "Ultra" };
                    QtUi::Combo("Quality Level##gtao", &gtaoSettings->QualityLevel, qualityNames, std::size(qualityNames));
                    QtUi::SetItemTooltip("Higher quality uses more samples per pixel.");

                    SliderFloatWithInput("Radius##gtao",            &gtaoSettings->Radius,                   0.1f, 5.0f,  "%.2f");
                    QtUi::SetItemTooltip("World-space maximum AO radius.");

                    SliderFloatWithInput("Radius Multiplier##gtao", &gtaoSettings->RadiusMultiplier,         0.5f, 3.0f,  "%.3f");
                    QtUi::SetItemTooltip("Scales the effective radius to counteract screen-space bias.");

                    SliderFloatWithInput("Falloff Range##gtao",     &gtaoSettings->FalloffRange,             0.0f, 1.0f,  "%.3f");
                    QtUi::SetItemTooltip("Fraction of the radius over which distant samples fade out.");

                    SliderIntWithInput("Denoise Passes##gtao",      &gtaoSettings->DenoisePasses,            0,   2);
                    QtUi::SetItemTooltip("0 = no spatial denoise, 1-2 = iterative bilateral blur.");

                    SliderFloatWithInput("Final Value Power##gtao", &gtaoSettings->FinalValuePower,          0.5f, 5.0f,  "%.2f");
                    QtUi::SetItemTooltip("Power curve applied to the final AO value.");

                    SliderFloatWithInput("AO Intensity##gtao",      &gtaoSettings->Intensity,                0.0f, 1.0f,  "%.2f");
                    QtUi::SetItemTooltip("Blend weight of XeGTAO over the G-Buffer baked AO. 1.0 = full GTAO.");

                    SliderFloatWithInput("Sample Distribution Power##gtao", &gtaoSettings->SampleDistributionPower, 1.0f, 3.0f, "%.2f");
                    QtUi::SetItemTooltip("Concentrates more samples toward small crevices.");

                    SliderFloatWithInput("Thin Occluder Compensation##gtao", &gtaoSettings->ThinOccluderCompensation, 0.0f, 0.7f, "%.2f");
                    QtUi::SetItemTooltip("Reduces over-occlusion on thin geometry.");

                    SliderFloatWithInput("Depth MIP Sampling Offset##gtao", &gtaoSettings->DepthMIPSamplingOffset, 2.0f, 6.0f, "%.2f");
                    QtUi::SetItemTooltip("Trade-off between performance (bandwidth) and quality.");

                    const char* gtaoDebugModes[] = { "Disabled", "AO Only" };
                    QtUi::Combo("Debug View##gtao", &gtaoSettings->DebugView, gtaoDebugModes, std::size(gtaoDebugModes));
                    QtUi::SetItemTooltip("Overlay the raw GTAO output for inspection.");
                }
            }
        }

        // -------------------------------------------------------------------
        // Screen-Space Reflections
        // -------------------------------------------------------------------
        if (ssrSettings != nullptr)
        {
            const SsrSettings defaultSsrSettings{};
            if (QtUi::CollapsingHeader("Screen-Space Reflections", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##ssr"))
                    *ssrSettings = defaultSsrSettings;

                QtUi::Checkbox("Enable SSR##ssr", &ssrSettings->Enabled);
                QtUi::SetItemTooltip("Reflects what is already on screen. Anything off screen, "
                                     "behind the camera, or hidden behind other geometry cannot be "
                                     "reflected - that is the standing limitation of the technique.");

                if (ssrSettings->Enabled)
                {
                    QtUi::Separator();
                    SliderFloatWithInput("Intensity##ssr", &ssrSettings->Intensity, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Scales the reflection before it is added. 1.0 is physical strength.");

                    SliderFloatWithInput("Max Roughness##ssr", &ssrSettings->MaxRoughness, 0.0f, 1.0f, "%.2f", 0.01f, 0.05f);
                    QtUi::SetItemTooltip("Surfaces rougher than this get no reflection - a single sharp ray "
                                         "cannot stand in for the blurred lobe they need.");

                    SliderFloatWithInput("Max Distance##ssr", &ssrSettings->MaxDistance, 1.0f, 400.0f, "%.1f m", 1.0f, 10.0f);
                    QtUi::SetItemTooltip("Longest ray, and the distance over which reflections fade out.");

                    QtUi::SeparatorText("Ray March");
                    SliderIntWithInput("Max Steps##ssr", &ssrSettings->MaxSteps, 8, 256);
                    QtUi::SetItemTooltip("Ray-march budget. The main cost control.");

                    SliderFloatWithInput("Step Size##ssr", &ssrSettings->StepSize, 0.01f, 2.0f, "%.3f m", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Length of the first step. Smaller resolves contact reflections better.");

                    SliderFloatWithInput("Step Growth##ssr", &ssrSettings->StepGrowth, 1.0f, 1.5f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("Geometric growth per step, so reach costs less than precision. 1.0 marches evenly.");

                    SliderFloatWithInput("Thickness##ssr", &ssrSettings->Thickness, 0.01f, 5.0f, "%.3f m", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("How far behind the depth buffer a sample still counts as a hit. "
                                         "Too small and rays pass through thin geometry; too large and they "
                                         "latch onto the wrong surface.");

                    SliderIntWithInput("Refine Steps##ssr", &ssrSettings->RefineSteps, 0, 16);
                    QtUi::SetItemTooltip("Binary-search iterations that pin down the hit after the coarse march.");

                    SliderFloatWithInput("Edge Fade##ssr", &ssrSettings->EdgeFadeStart, 0.0f, 0.99f, "%.2f", 0.01f, 0.05f);
                    QtUi::SetItemTooltip("Where reflections start fading toward the screen border, which has no data behind it.");

                    QtUi::Separator();
                    const char* ssrDebugModes[] = { "Composite", "Reflection Only", "Confidence Mask" };
                    QtUi::Combo("Debug View##ssr", &ssrSettings->DebugView, ssrDebugModes, std::size(ssrDebugModes));
                    QtUi::SetItemTooltip("Isolates the reflection term, or shows where SSR found a hit and how much it trusts it.");
                }
            }
        }

        // -------------------------------------------------------------------
        // Chromatic Aberration
        // -------------------------------------------------------------------
        if (chromaticAberrationSettings != nullptr)
        {
            const ChromaticAberrationSettings defaultChromaticAberrationSettings{};
            if (QtUi::CollapsingHeader("Chromatic Aberration", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##ca"))
                    *chromaticAberrationSettings = defaultChromaticAberrationSettings;

                QtUi::Checkbox("Enable Chromatic Aberration##ca", &chromaticAberrationSettings->Enabled);
                QtUi::SetItemTooltip("Splits the colour channels toward the edges of the frame, the way a "
                                     "lens focuses each wavelength at a slightly different magnification. "
                                     "Applied last, on the tonemapped image.");

                if (chromaticAberrationSettings->Enabled)
                {
                    QtUi::Separator();
                    SliderFloatWithInput("Strength##ca", &chromaticAberrationSettings->Strength, 0.0f, 20.0f, "%.2f px", 0.1f, 1.0f);
                    QtUi::SetItemTooltip("Channel displacement at the corner of the frame, in pixels. The centre "
                                         "is always clean. 1-3 reads as lens character; past 8 it reads as an effect.");

                    SliderFloatWithInput("Falloff##ca", &chromaticAberrationSettings->Falloff, 0.0f, 6.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("How quickly the split grows from the centre outward. 1 is a linear ramp; "
                                         "higher values keep the middle of the frame clean.");

                    SliderIntWithInput("Sample Count##ca", &chromaticAberrationSettings->SampleCount, 1, 16);
                    QtUi::SetItemTooltip("1 does a plain three-channel split - cheapest, and fine at low strength. "
                                         "Higher counts smear spectrally along the same line, which stops the fringe "
                                         "banding into hard red/blue edges when the strength is pushed up.");

                    QtUi::SeparatorText("Optical Centre");
                    SliderFloatWithInput("Center X##ca", &chromaticAberrationSettings->CenterX, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    SliderFloatWithInput("Center Y##ca", &chromaticAberrationSettings->CenterY, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("The point the aberration radiates from. Move it off centre to imitate a decentred lens.");
                }
            }
        }

        // -------------------------------------------------------------------
        // AgX Tonemapper
        // -------------------------------------------------------------------
        if (agxSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("AgX Tonemapper", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##agx"))
                {
                    *agxSettings = defaultAgxSettings;
                }
                QtUi::Checkbox("Enable AgX Tonemapper##agx", &agxSettings->Enabled);
                QtUi::SetItemTooltip("Apply the AgX display transform to the final image.");

                if (agxSettings->Enabled)
                {
                    auto drawGradeControl = [](const char* label, AgxColorGradeControl& control)
                    {
                        QtUi::PushID(label);
                        if (QtUi::TreeNode(label))
                        {
                            SliderFloatWithInput("Total", &control.Total, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Red", &control.Red, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Green", &control.Green, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Blue", &control.Blue, -1.0f, 1.0f, "%.2f");
                            SliderFloatWithInput("Yellow", &control.Yellow, -1.0f, 1.0f, "%.2f");
                            QtUi::TreePop();
                        }
                        QtUi::PopID();
                    };

                    auto drawGradeRegion = [&drawGradeControl](const char* label, AgxColorGradeRegion& region)
                    {
                        QtUi::PushID(label);
                        if (QtUi::CollapsingHeader(label, QtUiTreeNodeFlags_DefaultOpen))
                        {
                            drawGradeControl("Contrast", region.Contrast);
                            drawGradeControl("Gamma", region.Gamma);
                            drawGradeControl("Gain", region.Gain);
                            drawGradeControl("Saturation", region.Saturation);
                            drawGradeControl("Vibrance", region.Vibrance);
                        }
                        QtUi::PopID();
                    };

                    QtUi::Separator();
                    QtUi::TextDisabled("Exposure");

                    SliderFloatWithInput("Exposure (EV)##agx", &agxSettings->Exposure, -5.0f, 5.0f, "%.2f");
                    QtUi::SetItemTooltip("Exposure adjustment in EV stops. 0 = no change, positive = brighter.");

                    SliderFloatWithInput("EV100 Min##agx", &agxSettings->Ev100Min, -6.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Lower AgX exposure bound. Typical targets: -6 starlight night, -3 full moon, 3 blue hour, 8 golden hour, 11 overcast, 16 clear midday.");

                    SliderFloatWithInput("EV100 Max##agx", &agxSettings->Ev100Max, -6.0f, 16.0f, "%.2f");
                    QtUi::SetItemTooltip("Upper AgX exposure bound. Typical targets: -6 starlight night, -3 full moon, 3 blue hour, 8 golden hour, 11 overcast, 16 clear midday.");

                    if (agxSettings->Ev100Max <= agxSettings->Ev100Min)
                    {
                        agxSettings->Ev100Max = agxSettings->Ev100Min + 0.001f;
                    }

                    QtUi::Separator();
                    QtUi::TextDisabled("Curve");

                    SliderFloatWithInput("Toe Strength##agx", &agxSettings->ToeStrength, 0.1f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Controls how quickly shadows roll off. 1.0 = default.");

                    SliderFloatWithInput("Shoulder Strength##agx", &agxSettings->ShoulderStrength, 0.1f, 2.0f, "%.2f");
                    QtUi::SetItemTooltip("Controls how quickly highlights compress. 1.0 = default.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Color Grading");

                    drawGradeRegion("Global", agxSettings->Global);
                    drawGradeRegion("Shadows", agxSettings->Shadows);
                    drawGradeRegion("Midtones", agxSettings->Midtones);
                    drawGradeRegion("Highlights", agxSettings->Highlights);
                }
            }
        }

        if (volumetricFogSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Volumetric Fog", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##volfog"))
                {
                    *volumetricFogSettings = defaultVolumetricFogSettings;
                }

                QtUi::Checkbox("Enable Volumetric Fog", &volumetricFogSettings->Enabled);
                QtUi::SetItemTooltip("Enable froxel-based volumetric fog with sun-light injection and deferred compositing.");

                if (volumetricFogSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Froxel Grid");
                    SliderIntWithInput("Froxel Tile Size##volfog", &volumetricFogSettings->FroxelTileSize, 4, 16);
                    QtUi::SetItemTooltip("Screen-space froxel size in pixels. Lower = more detail and much higher cost. Very small values can create extremely large 3D fog volumes.");
                    SliderIntWithInput("Depth Slices##volfog", &volumetricFogSettings->DepthSlices, 8, 128);
                    QtUi::SetItemTooltip("Number of logarithmic depth slices in the froxel volume.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Medium");
                    SliderFloatWithInput("Start Distance##volfog", &volumetricFogSettings->StartDistance, 0.0f, 50.0f, "%.2f m");
                    SliderFloatWithInput("Max Distance##volfog", &volumetricFogSettings->MaxDistance, 1.0f, 500.0f, "%.1f m", 1.0f, 10.0f);
                    if (volumetricFogSettings->MaxDistance <= volumetricFogSettings->StartDistance)
                        volumetricFogSettings->MaxDistance = volumetricFogSettings->StartDistance + 0.001f;
                    SliderFloatWithInput("Density##volfog", &volumetricFogSettings->Density, 0.0f, 1.0f, "%.4f", 0.001f, 0.01f);
                    SliderFloatWithInput("Anisotropy##volfog", &volumetricFogSettings->Anisotropy, -0.95f, 0.95f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Positive values bias forward scattering for stronger god rays.");
                    SliderFloatWithInput("Base Height##volfog", &volumetricFogSettings->BaseHeight, -100.0f, 1000.0f, "%.2f m", 0.1f, 1.0f);
                    SliderFloatWithInput("Height Falloff##volfog", &volumetricFogSettings->HeightFalloff, 0.0f, 2.0f, "%.3f", 0.001f, 0.01f);

                    float fogColor[3] =
                    {
                        volumetricFogSettings->ColorR,
                        volumetricFogSettings->ColorG,
                        volumetricFogSettings->ColorB
                    };
                    if (QtUi::ColorEdit3("Fog Color", fogColor, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
                    {
                        volumetricFogSettings->ColorR = fogColor[0];
                        volumetricFogSettings->ColorG = fogColor[1];
                        volumetricFogSettings->ColorB = fogColor[2];
                    }

                    float fogEmissiveColor[3] =
                    {
                        volumetricFogSettings->EmissiveColorR,
                        volumetricFogSettings->EmissiveColorG,
                        volumetricFogSettings->EmissiveColorB
                    };
                    if (QtUi::ColorEdit3("Emissive Color##volfog", fogEmissiveColor, QtUiColorEditFlags_Float | QtUiColorEditFlags_HDR))
                    {
                        volumetricFogSettings->EmissiveColorR = fogEmissiveColor[0];
                        volumetricFogSettings->EmissiveColorG = fogEmissiveColor[1];
                        volumetricFogSettings->EmissiveColorB = fogEmissiveColor[2];
                    }
                    SliderFloatWithInput("Emissive Intensity##volfog", &volumetricFogSettings->EmissiveIntensity, 0.0f, 50.0f, "%.2f", 0.1f, 1.0f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");
                    const char* fogDebugModes[] = { "Composite", "Scattering", "Transmittance" };
                    QtUi::Combo("Debug View##volfog", &volumetricFogSettings->DebugView, fogDebugModes, std::size(fogDebugModes));
                }
            }
        }

        if (volumetricCloudSettings != nullptr)
        {
            const VolumetricCloudSettings defaultVolumetricCloudSettings{};
            if (QtUi::CollapsingHeader("Volumetric Clouds", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##volclouds"))
                {
                    *volumetricCloudSettings = defaultVolumetricCloudSettings;
                }

                QtUi::Checkbox("Enable Volumetric Clouds", &volumetricCloudSettings->Enabled);
                QtUi::SetItemTooltip("Raymarched cloud layer with multiple-scattering, self-shadowing and temporal upsampling.");

                if (volumetricCloudSettings->Enabled)
                {
                    QtUi::Separator();
                    QtUi::TextDisabled("Presets");
                    if (QtUi::Button("Clear##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::ClearSkies);
                    QtUi::SameLine();
                    if (QtUi::Button("Scattered##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::ScatteredCumulus);
                    QtUi::SameLine();
                    if (QtUi::Button("Overcast##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::Overcast);
                    QtUi::SameLine();
                    if (QtUi::Button("Stormy##volclouds"))
                        ApplyVolumetricCloudPreset(*volumetricCloudSettings, VolumetricCloudPreset::Stormy);
                    QtUi::SetItemTooltip("Presets only touch the weather sliders; lighting and performance tuning is preserved.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Layer");
                    SliderFloatWithInput("Base Altitude##volclouds", &volumetricCloudSettings->LayerBottomMeters, 100.0f, 8000.0f, "%.0f m", 10.0f, 100.0f);
                    QtUi::SetItemTooltip("Height of the cloud base above ground level.");
                    SliderFloatWithInput("Layer Thickness##volclouds", &volumetricCloudSettings->LayerThicknessMeters, 200.0f, 12000.0f, "%.0f m", 10.0f, 100.0f);
                    SliderFloatWithInput("Planet Radius##volclouds", &volumetricCloudSettings->PlanetRadiusKm, 100.0f, 20000.0f, "%.0f km", 10.0f, 100.0f);
                    QtUi::SetItemTooltip("Curvature of the layer. Smaller values pull the horizon in and exaggerate the curve.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Weather");
                    SliderFloatWithInput("Coverage##volclouds", &volumetricCloudSettings->Coverage, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("Threshold applied to the weather map. 0 clears the sky, 1 fills it completely.");
                    SliderFloatWithInput("Cloud Type##volclouds", &volumetricCloudSettings->CloudType, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("0 = flat stratus, 0.5 = cumulus, 1 = towering cumulonimbus. Blended with the weather map.");
                    SliderFloatWithInput("Density##volclouds", &volumetricCloudSettings->Density, 0.0f, 2.0f, "%.3f", 0.01f, 0.1f);
                    SliderFloatWithInput("Anvil Bias##volclouds", &volumetricCloudSettings->AnvilBias, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Spreads the tops of tall clouds outwards into anvil shapes.");
                    SliderFloatWithInput("Weather Scale##volclouds", &volumetricCloudSettings->WeatherScaleMeters, 5000.0f, 400000.0f, "%.0f m", 1000.0f, 10000.0f);
                    QtUi::SetItemTooltip("World size of one tile of the weather map, i.e. the size of the storm systems.");

                    QtUi::TextDisabled("Weather Map (re-bakes on change)");
                    SliderIntWithInput("Seed##volclouds", &volumetricCloudSettings->WeatherSeed, 0, 9999);
                    SliderFloatWithInput("Cell Size##volclouds", &volumetricCloudSettings->WeatherCellSize, 0.25f, 6.0f, "%.2f", 0.05f, 0.25f);
                    SliderFloatWithInput("Coverage Bias##volclouds", &volumetricCloudSettings->WeatherCoverageBias, -1.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Type Bias##volclouds", &volumetricCloudSettings->WeatherTypeBias, -1.0f, 1.0f, "%.2f", 0.01f, 0.1f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Shape Detail");
                    SliderFloatWithInput("Base Noise Scale##volclouds", &volumetricCloudSettings->BaseNoiseScaleMeters, 2000.0f, 120000.0f, "%.0f m", 100.0f, 1000.0f);
                    SliderFloatWithInput("Detail Noise Scale##volclouds", &volumetricCloudSettings->DetailNoiseScaleMeters, 100.0f, 10000.0f, "%.0f m", 10.0f, 100.0f);
                    SliderFloatWithInput("Detail Strength##volclouds", &volumetricCloudSettings->DetailStrength, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("How aggressively high frequency noise erodes the cloud silhouette.");
                    SliderFloatWithInput("Curl Strength##volclouds", &volumetricCloudSettings->CurlStrength, 0.0f, 3.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Swirls the erosion lookup so cloud edges read as turbulence.");
                    SliderFloatWithInput("Top Lean##volclouds", &volumetricCloudSettings->CloudTopOffsetMeters, -2000.0f, 2000.0f, "%.0f m", 10.0f, 100.0f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Wind");
                    SliderFloatWithInput("Direction##volclouds", &volumetricCloudSettings->WindDirectionDegrees, 0.0f, 360.0f, "%.1f deg", 1.0f, 10.0f);
                    SliderFloatWithInput("Speed##volclouds", &volumetricCloudSettings->WindSpeed, 0.0f, 80.0f, "%.1f m/s", 0.5f, 5.0f);
                    SliderFloatWithInput("Shear##volclouds", &volumetricCloudSettings->WindSkew, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Horizontal offset between the base and the top of the layer, as a fraction of its thickness.");
                    SliderFloatWithInput("Detail Speed Scale##volclouds", &volumetricCloudSettings->DetailWindSpeedScale, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                    QtUi::SetItemTooltip("How much faster the fine detail advects than the base shape. This is what makes cloud interiors churn.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Lighting");
                    float scatteringColor[3] =
                    {
                        volumetricCloudSettings->ScatteringColorR,
                        volumetricCloudSettings->ScatteringColorG,
                        volumetricCloudSettings->ScatteringColorB
                    };
                    if (QtUi::ColorEdit3("Scattering Albedo##volclouds", scatteringColor, QtUiColorEditFlags_Float))
                    {
                        volumetricCloudSettings->ScatteringColorR = scatteringColor[0];
                        volumetricCloudSettings->ScatteringColorG = scatteringColor[1];
                        volumetricCloudSettings->ScatteringColorB = scatteringColor[2];
                    }
                    SliderFloatWithInput("Extinction##volclouds", &volumetricCloudSettings->ExtinctionScale, 0.001f, 0.3f, "%.4f /m", 0.001f, 0.01f);
                    QtUi::SetItemTooltip("Extinction per metre at density 1. Real cumulus sit around 0.04 - 0.1.");
                    SliderFloatWithInput("Sun Intensity##volclouds", &volumetricCloudSettings->SunIntensityScale, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                    SliderFloatWithInput("Ambient Intensity##volclouds", &volumetricCloudSettings->AmbientIntensityScale, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                    SliderFloatWithInput("Ground Bounce##volclouds", &volumetricCloudSettings->GroundBounceScale, 0.0f, 2.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("How much sky light still reaches the shadowed base of the layer.");
                    SliderFloatWithInput("Forward Lobe (g0)##volclouds", &volumetricCloudSettings->PhaseG0, -0.99f, 0.99f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Drives the bright silver lining when looking towards the sun.");
                    SliderFloatWithInput("Back Lobe (g1)##volclouds", &volumetricCloudSettings->PhaseG1, -0.99f, 0.99f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Lobe Blend##volclouds", &volumetricCloudSettings->PhaseBlend, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Powder##volclouds", &volumetricCloudSettings->PowderStrength, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    QtUi::SetItemTooltip("Darkens cloud faces that turn towards the light, which restores the depth single scattering loses.");

                    QtUi::TextDisabled("Multiple Scattering");
                    SliderIntWithInput("Octaves##volclouds", &volumetricCloudSettings->MultiScatterOctaves, 1, 4);
                    QtUi::SetItemTooltip("Wrenninge octave approximation. 1 is single scattering only; each extra octave recovers more of the light real clouds bounce internally.");
                    SliderFloatWithInput("Scatter Falloff##volclouds", &volumetricCloudSettings->MsScatterFalloff, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Extinction Falloff##volclouds", &volumetricCloudSettings->MsExtinctionFalloff, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Phase Falloff##volclouds", &volumetricCloudSettings->MsPhaseFalloff, 0.0f, 1.0f, "%.2f", 0.01f, 0.1f);

                    QtUi::Separator();
                    QtUi::TextDisabled("Quality And Performance");
                    const char* cloudResolutionModes[] = { "Full", "Half", "Third", "Quarter" };
                    int resolutionIndex = std::clamp(volumetricCloudSettings->ResolutionDivisor, 1, 4) - 1;
                    if (QtUi::Combo("Trace Resolution##volclouds", &resolutionIndex, cloudResolutionModes, std::size(cloudResolutionModes)))
                        volumetricCloudSettings->ResolutionDivisor = resolutionIndex + 1;
                    QtUi::SetItemTooltip("Resolution the raymarch runs at before temporal reconstruction. Half is the usual shipping setting.");

                    QtUi::Checkbox("Temporal Upsampling##volclouds", &volumetricCloudSettings->TemporalUpsampling);
                    QtUi::SetItemTooltip("Blends with the reprojected previous frame. Turn off to see the raw trace.");
                    if (volumetricCloudSettings->TemporalUpsampling)
                    {
                        SliderFloatWithInput("Temporal Blend##volclouds", &volumetricCloudSettings->TemporalBlend, 0.0f, 0.98f, "%.3f", 0.005f, 0.05f);
                        QtUi::SetItemTooltip("Weight given to history. Higher is smoother but slower to react.");
                    }

                    SliderIntWithInput("View Steps##volclouds", &volumetricCloudSettings->MaxSteps, 16, 256);
                    QtUi::SetItemTooltip("Steps for a vertical traversal of the layer. Grazing rays scale up to twice this.");
                    SliderIntWithInput("Light Steps##volclouds", &volumetricCloudSettings->LightSteps, 1, 6);
                    SliderFloatWithInput("Light March Distance##volclouds", &volumetricCloudSettings->LightMarchDistanceMeters, 100.0f, 8000.0f, "%.0f m", 10.0f, 100.0f);
                    SliderFloatWithInput("Shadow Cone Spread##volclouds", &volumetricCloudSettings->ShadowConeSpread, 0.0f, 1.0f, "%.3f", 0.005f, 0.05f);
                    QtUi::SetItemTooltip("Widens the light march into a cone so self-shadowing picks up neighbouring cloud mass.");
                    SliderFloatWithInput("Shadow Step Growth##volclouds", &volumetricCloudSettings->ShadowStepGrowth, 1.0f, 3.0f, "%.2f", 0.01f, 0.1f);
                    SliderFloatWithInput("Max Trace Distance##volclouds", &volumetricCloudSettings->MaxTraceDistanceMeters, 5000.0f, 400000.0f, "%.0f m", 1000.0f, 10000.0f);
                    SliderFloatWithInput("Distance Fade Start##volclouds", &volumetricCloudSettings->DistanceFadeStartMeters, 1000.0f, 400000.0f, "%.0f m", 1000.0f, 10000.0f);
                    if (volumetricCloudSettings->DistanceFadeStartMeters >= volumetricCloudSettings->MaxTraceDistanceMeters)
                        volumetricCloudSettings->DistanceFadeStartMeters = volumetricCloudSettings->MaxTraceDistanceMeters - 1.0f;
                    SliderFloatWithInput("Detail Fade Distance##volclouds", &volumetricCloudSettings->DetailFadeDistanceMeters, 1000.0f, 200000.0f, "%.0f m", 500.0f, 5000.0f);
                    QtUi::SetItemTooltip("Beyond this, high frequency erosion is skipped. Lower values are much cheaper and reduce distant shimmer.");

                    QtUi::Separator();
                    QtUi::TextDisabled("Debug");
                    const char* cloudDebugModes[] = { "Composite", "Luminance", "Transmittance", "Coverage", "Cloud Distance" };
                    QtUi::Combo("Debug View##volclouds", &volumetricCloudSettings->DebugView, cloudDebugModes, std::size(cloudDebugModes));
                }
            }
        }

        if (bloomSettings != nullptr)
        {
            const BloomSettings defaultBloomSettings{};
            if (QtUi::CollapsingHeader("Bloom", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##bloom"))
                    *bloomSettings = defaultBloomSettings;

                QtUi::Checkbox("Enable Bloom", &bloomSettings->Enabled);

                if (bloomSettings->Enabled)
                {
                    QtUi::Separator();
                    SliderFloatWithInput("Intensity##bloom",  &bloomSettings->Intensity,  0.0f,  1.0f,  "%.3f", 0.005f, 0.05f);
                    SliderFloatWithInput("Threshold##bloom",  &bloomSettings->Threshold,  0.0f,  10.0f, "%.2f");
                    SliderFloatWithInput("Knee##bloom",       &bloomSettings->Knee,       0.0f,  2.0f,  "%.2f");
                    SliderFloatWithInput("Radius##bloom",     &bloomSettings->Radius,     0.1f,  4.0f,  "%.2f");
                    SliderIntWithInput("Mip Levels##bloom",   &bloomSettings->MipLevels,  2,     8);
                }
            }
        }

        if (pointShadowSettings != nullptr)
        {
            if (QtUi::CollapsingHeader("Point Light Shadows", QtUiTreeNodeFlags_DefaultOpen))
            {
                if (QtUi::Button("Revert All##pointshadows"))
                    *pointShadowSettings = defaultPointShadowSettings;

                SliderIntWithInput("Map Size##pointshadows", &pointShadowSettings->MapSize, 256, 4096);
                QtUi::SetItemTooltip("Resolution of each point-light shadow face. Higher values improve detail but cost more memory and rendering time.");

                SliderFloatWithInput("Shadow Bias##pointshadows", &pointShadowSettings->Bias, 0.0001f, 0.1f, "%.4f", 0.0001f, 0.001f);
                QtUi::SetItemTooltip("Depth bias applied when sampling omni shadow maps to reduce self-shadow acne.");

                SliderFloatWithInput("Slope Scaled Bias##pointshadows", &pointShadowSettings->SlopeScaledDepthBias, 0.0f, 8.0f, "%.2f", 0.05f, 0.25f);
                QtUi::SetItemTooltip("Rasterizer slope-scaled depth bias for point-shadow depth rendering. Increases bias on steep polygons to reduce acne on curved/angled surfaces.");

                SliderFloatWithInput("Normal Offset##pointshadows", &pointShadowSettings->NormalOffset, 0.0f, 4.0f, "%.2f", 0.01f, 0.1f);
                QtUi::SetItemTooltip("Offsets shadow test position along the geometric normal before sampling point shadows to reduce acne without large peter-panning.");

                SliderIntWithInput("Filter Radius##pointshadows", &pointShadowSettings->FilterRadius, 0, 4);
                QtUi::SetItemTooltip("PCF filter radius for point-light shadows. Higher values reduce banding but cost more.");

                SliderFloatWithInput("Seam Blend##pointshadows", &pointShadowSettings->SeamBlendDistance, 0.0f, 0.25f, "%.3f", 0.001f, 0.01f);
                QtUi::SetItemTooltip("Blends point-shadow visibility across cubemap face edges to reduce seam artifacts.");

                const char* pointShadowDebugModes[] = { "Lighting", "Shadow Factor" };
                QtUi::Combo("Debug View##pointshadows", &pointShadowSettings->DebugView, pointShadowDebugModes, std::size(pointShadowDebugModes));
                QtUi::SetItemTooltip("Shows raw point-shadow visibility to diagnose sampling, seams, and filtering.");
            }
        }

        QtUi::End();
    }

    if (gShowMaterialEditorWindow)
    {
        QtUi::SetNextWindowSize(UiVec2(640.0f, 780.0f), QtUiCond_FirstUseEver);
        if (QtUi::Begin("Material Editor", &gShowMaterialEditorWindow))
        {
            // Tab bar splits the editor into single-material mode and the CryEngine-style multi-material mode.
            if (QtUi::BeginTabBar("MaterialEditorTabs"))
            {
                // ----------------------------------------------------------------
                // Tab 1: Single Material (original behaviour, unchanged)
                // ----------------------------------------------------------------
                if (QtUi::BeginTabItem("Single Material"))
                {
                    MaterialDefinition& materialDefinition = gMaterialEditor.GetCurrentMaterial();
                    if (gMaterialNameBuffer[0] == '\0')
                    {
                        RefreshMaterialNameBuffer();
                    }

                    if (QtUi::Button("New"))
                    {
                        gMaterialEditor.NewMaterial();
                        RefreshMaterialNameBuffer();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Open..."))
                    {
                        if (gMaterialEditor.OpenMaterialWithDialog(windowHandle))
                        {
                            RefreshMaterialNameBuffer();
                        }
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save"))
                    {
                        materialDefinition.Name = gMaterialNameBuffer;
                        gMaterialEditor.SaveCurrentMaterial();
                        RefreshMaterialNameBuffer();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save As..."))
                    {
                        materialDefinition.Name = gMaterialNameBuffer;
                        if (gMaterialEditor.SaveMaterialWithDialog(windowHandle))
                        {
                            RefreshMaterialNameBuffer();
                        }
                    }

                    QtUi::Separator();
                    QtUi::InputText("Material Name", gMaterialNameBuffer, std::size(gMaterialNameBuffer));
                    materialDefinition.Name = gMaterialNameBuffer;

                    // Everything below scrolls inside its own region so the file
                    // buttons and the name stay put. The window body is
                    // scroll-wrapped as a whole, so without this the Save button
                    // scrolls off the top as soon as the property list is long -
                    // and it is long.
                    QtUi::BeginChild("SingleMaterialProperties", UiVec2(0.0f, kMaterialPropertiesHeight));

                    QtUi::ColorEdit4("Base Color Tint", materialDefinition.BaseColorTint.data());
                    QtUi::ColorEdit3("Emissive Color", materialDefinition.EmissiveColor.data());
                    QtUi::SliderFloat("Metallic Factor", &materialDefinition.MetallicFactor, 0.0f, 1.0f);
                    QtUi::SliderFloat("Roughness Factor", &materialDefinition.RoughnessFactor, 0.0f, 1.0f);
                    QtUi::SliderFloat("Normal Scale", &materialDefinition.NormalScale, 0.0f, 4.0f);
                    QtUi::SliderFloat("Ambient Occlusion Strength", &materialDefinition.AmbientOcclusionStrength, 0.0f, 4.0f);
                    QtUi::SliderFloat("Opacity", &materialDefinition.Opacity, 0.0f, 1.0f);
                    QtUi::SliderFloat("Alpha Cutoff", &materialDefinition.AlphaCutoff, 0.0f, 1.0f);
                    QtUi::Checkbox("Double Sided", &materialDefinition.IsDoubleSided);
                    QtUi::Checkbox("Use Alpha Cutout", &materialDefinition.UseAlphaCutout);
                    QtUi::Checkbox("Use Transparent Blend", &materialDefinition.UseTransparentBlend);
                    QtUi::Checkbox("Decal Material", &materialDefinition.IsDecalMaterial);
                    QtUi::Separator();
                    QtUi::Checkbox("Glass Rendering", &materialDefinition.UseGlassRendering);
                    QtUi::BeginDisabled(!materialDefinition.UseGlassRendering);
                    QtUi::Checkbox("Thin Glass", &materialDefinition.UseThinGlass);
                    QtUi::SliderFloat("Glass IOR", &materialDefinition.GlassIor, 1.0f, 2.5f, "%.2f");
                    QtUi::SliderFloat("Glass Thickness", &materialDefinition.GlassThickness, 0.0f, 1.0f, "%.3f");
                    QtUi::SliderFloat("Glass Dispersion", &materialDefinition.GlassDispersion, 0.0f, 2.0f, "%.2f");
                    QtUi::SliderFloat("Fake Caustics", &materialDefinition.GlassCausticStrength, 0.0f, 1.0f, "%.2f");
                    QtUi::EndDisabled();

                    DrawMaterialParticleControls(materialDefinition);

                    DrawMaterialUvAndParallaxControls(materialDefinition);

                    const auto drawTextureField = [&](const char* label, const MaterialTextureSlot textureSlot)
                    {
                        std::string& texturePath = materialDefinition.Textures.GetTexturePath(textureSlot);
                        QtUi::PushID(label);
                        QtUi::TextUnformatted(label);
                        QtUi::SameLine(180.0f);
                        QtUi::TextWrapped("%s", texturePath.empty() ? "<none>" : texturePath.c_str());
                        if (QtUi::Button("Browse..."))
                        {
                            gMaterialEditor.BrowseForTexture(windowHandle, textureSlot);
                        }
                        QtUi::SameLine();
                        if (QtUi::Button("Clear"))
                        {
                            texturePath.clear();
                        }
                        QtUi::PopID();
                    };

                    QtUi::Separator();
                    QtUi::TextUnformatted("Textures");
                    drawTextureField("Base Color", MaterialTextureSlot::BaseColor);
                    drawTextureField("Normal", MaterialTextureSlot::Normal);
                    drawTextureField("Metallic", MaterialTextureSlot::Metallic);
                    drawTextureField("Roughness", MaterialTextureSlot::Roughness);
                    drawTextureField("Metallic Roughness", MaterialTextureSlot::MetallicRoughness);
                    drawTextureField("Ambient Occlusion", MaterialTextureSlot::AmbientOcclusion);
                    drawTextureField("Emissive", MaterialTextureSlot::Emissive);
                    drawTextureField("Height", MaterialTextureSlot::Height);
                    drawTextureField("Opacity", MaterialTextureSlot::Opacity);

                    QtUi::Separator();
                    const std::vector<std::filesystem::path> availableMaterials = gMaterialEditor.FindAvailableMaterials();
                    QtUi::TextUnformatted("Material Library");
                    if (availableMaterials.empty())
                    {
                        QtUi::TextDisabled("No saved materials found under Data/Materials.");
                    }
                    else
                    {
                        if (QtUi::BeginChild("MaterialLibrary", UiVec2(0.0f, 150.0f), true))
                        {
                            for (const std::filesystem::path& materialPath : availableMaterials)
                            {
                                const std::string itemLabel = materialPath.filename().string();
                                if (QtUi::Selectable(itemLabel.c_str(), gMaterialEditor.GetCurrentMaterialPath() == materialPath))
                                {
                                    if (gMaterialEditor.LoadMaterialFromFile(materialPath))
                                    {
                                        RefreshMaterialNameBuffer();
                                    }
                                }
                            }
                        }
                        QtUi::EndChild();
                    }

                    if (!gMaterialEditor.GetCurrentMaterialPath().empty())
                    {
                        QtUi::TextWrapped("Current File: %s", gMaterialEditor.GetCurrentMaterialPath().string().c_str());
                    }

                    DrawSelectedMeshMaterialBinding(editorInstance, "SingleMaterialBinding", gMaterialEditor.GetCurrentMaterialPath(), false);

                    QtUi::EndChild();

                    // Outside the scroll region: a save failure is worth seeing
                    // without having to hunt for it.
                    if (!gMaterialEditor.GetLastErrorMessage().empty())
                    {
                        QtUi::TextWrapped("Status: %s", gMaterialEditor.GetLastErrorMessage().c_str());
                    }

                    QtUi::EndTabItem();
                }

                // ----------------------------------------------------------------
                // Tab 2: Multi-Material  (CryEngine-style parent + sub-materials)
                // Each sub-material index == FBX material ID / mesh sub-mesh index.
                // ----------------------------------------------------------------
                if (QtUi::BeginTabItem("Multi-Material"))
                {
                    MultiMaterialDefinition& multiMat = gMaterialEditor.GetCurrentMultiMaterial();

                    if (gMultiMaterialNameBuffer[0] == '\0')
                    {
                        RefreshMultiMaterialNameBuffer();
                    }

                    // --- Toolbar ---
                    if (QtUi::Button("New##Multi"))
                    {
                        gMaterialEditor.NewMultiMaterial();
                        gSelectedSubMaterialIndex = -1;
                        RefreshMultiMaterialNameBuffer();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Open...##Multi"))
                    {
                        if (gMaterialEditor.OpenMultiMaterialWithDialog(windowHandle))
                        {
                            gSelectedSubMaterialIndex = -1;
                            RefreshMultiMaterialNameBuffer();
                        }
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save##Multi"))
                    {
                        multiMat.Name = gMultiMaterialNameBuffer;
                        gMaterialEditor.SaveCurrentMultiMaterial();
                    }

                    QtUi::SameLine();
                    if (QtUi::Button("Save As...##Multi"))
                    {
                        multiMat.Name = gMultiMaterialNameBuffer;
                        gMaterialEditor.SaveMultiMaterialWithDialog(windowHandle);
                    }

                    QtUi::Separator();

                    // Parent name field
                    QtUi::InputText("Multi-Material Name", gMultiMaterialNameBuffer, std::size(gMultiMaterialNameBuffer));
                    multiMat.Name = gMultiMaterialNameBuffer;

                    QtUi::Separator();

                    // Same as the single-material tab: scroll the sub-material
                    // editing below, keep the file buttons and name pinned.
                    QtUi::BeginChild("MultiMaterialProperties", UiVec2(0.0f, kMaterialPropertiesHeight));

                    // Two-column layout: sub-material list on the left, properties on the right
                    const float subMatListWidth = 200.0f;
                    QtUi::BeginGroup();

                    QtUi::TextUnformatted("Sub-Materials");
                    QtUi::SameLine();

                    // Add button inserts a new sub-material at the next slot
                    if (QtUi::SmallButton("+  Add"))
                    {
                        gMaterialEditor.AddSubMaterial();
                        gSelectedSubMaterialIndex = static_cast<int>(multiMat.SubMaterials.size()) - 1;
                    }

                    QtUi::SameLine();
                    QtUi::BeginDisabled(gSelectedSubMaterialIndex < 0 ||
                        gSelectedSubMaterialIndex >= static_cast<int>(multiMat.SubMaterials.size()));
                    if (QtUi::SmallButton("-  Remove"))
                    {
                        gMaterialEditor.RemoveSubMaterial(gSelectedSubMaterialIndex);
                        // Clamp selection to valid range after removal
                        const int newCount = static_cast<int>(multiMat.SubMaterials.size());
                        if (gSelectedSubMaterialIndex >= newCount)
                        {
                            gSelectedSubMaterialIndex = newCount - 1;
                        }
                    }
                    QtUi::EndDisabled();

                    // Sub-material list box – each row shows "Mat ID X – Name"
                    if (QtUi::BeginChild("SubMatList", UiVec2(subMatListWidth, 0.0f), true))
                    {
                        for (int i = 0; i < static_cast<int>(multiMat.SubMaterials.size()); ++i)
                        {
                            char label[128];
                            std::snprintf(label, sizeof(label), "ID %d – %s", i, multiMat.SubMaterials[i].Name.c_str());
                            if (QtUi::Selectable(label, gSelectedSubMaterialIndex == i))
                            {
                                gSelectedSubMaterialIndex = i;
                            }
                        }

                        if (multiMat.SubMaterials.empty())
                        {
                            QtUi::TextDisabled("No sub-materials.\nClick '+ Add'.");
                        }
                    }
                    QtUi::EndChild();

                    QtUi::EndGroup();

                    QtUi::SameLine();

                    // Right panel: properties of the selected sub-material
                    QtUi::BeginGroup();
                    if (gSelectedSubMaterialIndex >= 0 &&
                        gSelectedSubMaterialIndex < static_cast<int>(multiMat.SubMaterials.size()))
                    {
                        MaterialDefinition& subMat = multiMat.SubMaterials[gSelectedSubMaterialIndex];

                        QtUi::TextDisabled("Material ID: %d  (matches FBX material slot)", gSelectedSubMaterialIndex);

                        // Sub-material name – use a per-index buffer via InputText with a PushID
                        QtUi::PushID(gSelectedSubMaterialIndex);
                        char subNameBuf[256];
                        strcpy_s(subNameBuf, subMat.Name.c_str());
                        if (QtUi::InputText("Sub-Material Name", subNameBuf, std::size(subNameBuf)))
                        {
                            subMat.Name = subNameBuf;
                        }

                        QtUi::ColorEdit4("Base Color Tint", subMat.BaseColorTint.data());
                        QtUi::ColorEdit3("Emissive Color", subMat.EmissiveColor.data());
                        QtUi::SliderFloat("Metallic Factor", &subMat.MetallicFactor, 0.0f, 1.0f);
                        QtUi::SliderFloat("Roughness Factor", &subMat.RoughnessFactor, 0.0f, 1.0f);
                        QtUi::SliderFloat("Normal Scale", &subMat.NormalScale, 0.0f, 4.0f);
                        QtUi::SliderFloat("AO Strength", &subMat.AmbientOcclusionStrength, 0.0f, 4.0f);
                        QtUi::SliderFloat("Opacity", &subMat.Opacity, 0.0f, 1.0f);
                        QtUi::SliderFloat("Alpha Cutoff", &subMat.AlphaCutoff, 0.0f, 1.0f);
                        QtUi::Checkbox("Double Sided", &subMat.IsDoubleSided);
                        QtUi::Checkbox("Alpha Cutout", &subMat.UseAlphaCutout);
                        QtUi::Checkbox("Transparent Blend", &subMat.UseTransparentBlend);
                        QtUi::Checkbox("Decal Material", &subMat.IsDecalMaterial);
                        QtUi::Separator();
                        QtUi::Checkbox("Glass Rendering", &subMat.UseGlassRendering);
                        QtUi::BeginDisabled(!subMat.UseGlassRendering);
                        QtUi::Checkbox("Thin Glass", &subMat.UseThinGlass);
                        QtUi::SliderFloat("Glass IOR", &subMat.GlassIor, 1.0f, 2.5f, "%.2f");
                        QtUi::SliderFloat("Glass Thickness", &subMat.GlassThickness, 0.0f, 1.0f, "%.3f");
                        QtUi::SliderFloat("Glass Dispersion", &subMat.GlassDispersion, 0.0f, 2.0f, "%.2f");
                        QtUi::SliderFloat("Fake Caustics", &subMat.GlassCausticStrength, 0.0f, 1.0f, "%.2f");
                        QtUi::EndDisabled();

                        DrawMaterialParticleControls(subMat);

                        DrawMaterialUvAndParallaxControls(subMat);

                        const auto drawSubTexField = [&](const char* label, const MaterialTextureSlot slot)
                        {
                            std::string& path = subMat.Textures.GetTexturePath(slot);
                            QtUi::PushID(label);
                            QtUi::TextUnformatted(label);
                            QtUi::SameLine(160.0f);
                            QtUi::TextWrapped("%s", path.empty() ? "<none>" : path.c_str());
                            if (QtUi::Button("Browse..."))
                            {
                                gMaterialEditor.BrowseForSubMaterialTexture(windowHandle, gSelectedSubMaterialIndex, slot);
                            }
                            QtUi::SameLine();
                            if (QtUi::Button("Clear"))
                            {
                                path.clear();
                            }
                            QtUi::PopID();
                        };

                        QtUi::Separator();
                        QtUi::TextUnformatted("Textures");
                        drawSubTexField("Base Color", MaterialTextureSlot::BaseColor);
                        drawSubTexField("Normal", MaterialTextureSlot::Normal);
                        drawSubTexField("Metallic", MaterialTextureSlot::Metallic);
                        drawSubTexField("Roughness", MaterialTextureSlot::Roughness);
                        drawSubTexField("Metallic Roughness", MaterialTextureSlot::MetallicRoughness);
                        drawSubTexField("Ambient Occlusion", MaterialTextureSlot::AmbientOcclusion);
                        drawSubTexField("Emissive", MaterialTextureSlot::Emissive);
                        drawSubTexField("Height", MaterialTextureSlot::Height);
                        drawSubTexField("Opacity", MaterialTextureSlot::Opacity);

                        QtUi::PopID();
                    }
                    else
                    {
                        QtUi::TextDisabled("Select a sub-material from the list on the left,\nor click '+ Add' to create one.");
                    }
                    QtUi::EndGroup();

                    QtUi::Separator();

                    // Multi-material library list (Data/MultiMaterials/)
                    const std::vector<std::filesystem::path> availableMultiMats = gMaterialEditor.FindAvailableMultiMaterials();
                    QtUi::TextUnformatted("Multi-Material Library");
                    if (availableMultiMats.empty())
                    {
                        QtUi::TextDisabled("No saved multi-materials found under Data/MultiMaterials.");
                    }
                    else
                    {
                        if (QtUi::BeginChild("MultiMatLibrary", UiVec2(0.0f, 100.0f), true))
                        {
                            for (const std::filesystem::path& mmPath : availableMultiMats)
                            {
                                const std::string itemLabel = mmPath.filename().string();
                                if (QtUi::Selectable(itemLabel.c_str(), gMaterialEditor.GetCurrentMultiMaterialPath() == mmPath))
                                {
                                    if (gMaterialEditor.LoadMultiMaterialFromFile(mmPath))
                                    {
                                        gSelectedSubMaterialIndex = -1;
                                        RefreshMultiMaterialNameBuffer();
                                    }
                                }
                            }
                        }
                        QtUi::EndChild();
                    }

                    if (!gMaterialEditor.GetCurrentMultiMaterialPath().empty())
                    {
                        QtUi::TextWrapped("Current File: %s", gMaterialEditor.GetCurrentMultiMaterialPath().string().c_str());
                    }

                    DrawSelectedMeshMaterialBinding(editorInstance, "MultiMaterialBinding", gMaterialEditor.GetCurrentMultiMaterialPath(), true);

                    QtUi::EndChild();

                    if (!gMaterialEditor.GetLastErrorMessage().empty())
                    {
                        QtUi::TextWrapped("Status: %s", gMaterialEditor.GetLastErrorMessage().c_str());
                    }

                    QtUi::EndTabItem();
                }

                QtUi::EndTabBar();
            }
        }
        QtUi::End();
    }

    if (gShowAssetBrowserWindow)
    {
        RefreshAssetBrowserState();
        QtUi::Begin("Asset Browser", &gShowAssetBrowserWindow, QtUiWindowFlags_NoScrollbar | QtUiWindowFlags_NoScrollWithMouse);

        if (gOpenRenamePopup)
        {
            QtUi::OpenPopup("Rename Asset");
            gOpenRenamePopup = false;
        }

        if (gOpenNewFolderPopup)
        {
            QtUi::OpenPopup("Create Folder");
            gOpenNewFolderPopup = false;
        }

        const bool hasSelectedAsset = !gSelectedAssetRelativePath.empty();
        const bool selectedAssetIsGeometry = hasSelectedAsset && IsGeometryAssetPath(gSelectedAssetRelativePath);
        const bool canPaste = gClipboardHasValue;

        // The toolbar keeps common content-browser actions visible, similar to the asset views used by large game editors.
        if (QtUi::Button("Import FBX"))
        {
            if (PromptForAndImportFbx(windowHandle, gSelectedFolderRelativePath))
            {
                RefreshAssetBrowserState();
            }
        }

        QtUi::SameLine();
        if (QtUi::Button("Import Texture"))
        {
            if (PromptForAndImportTexture(windowHandle, gSelectedFolderRelativePath))
            {
                RefreshAssetBrowserState();
            }
        }

        QtUi::SameLine();
        if (QtUi::Button("New Folder"))
        {
            BeginCreateFolder();
        }

        QtUi::SameLine();
        QtUi::BeginDisabled(!hasSelectedAsset);
        if (QtUi::Button("Rename"))
        {
            BeginRenameSelectedAsset();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!hasSelectedAsset);
        if (QtUi::Button("Copy"))
        {
            CopySelectedAssetToClipboard();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!canPaste);
        if (QtUi::Button("Paste"))
        {
            PasteClipboardIntoSelectedFolder();
            RefreshAssetBrowserState();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!hasSelectedAsset);
        if (QtUi::Button("Remove"))
        {
            DeleteSelectedAsset();
            RefreshAssetBrowserState();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!selectedAssetIsGeometry);
        if (QtUi::Button("Generate LODs"))
        {
            RegenerateSelectedGeometryLods(editor);
            RefreshAssetBrowserState();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        QtUi::BeginDisabled(!selectedAssetIsGeometry);
        if (QtUi::Button("Generate Collisions"))
        {
            GenerateSelectedGeometryCollisions();
        }
        QtUi::EndDisabled();

        QtUi::SameLine();
        if (QtUi::Button("Refresh"))
        {
            InvalidateAssetBrowserDirectoryCache();
            RefreshAssetBrowserState();
        }

        QtUi::Separator();
        QtUi::TextWrapped("Data Folder: %s", gDataDirectoryDisplay.c_str());
        QtUi::TextWrapped(
            "Current Folder: %s",
            gSelectedFolderRelativePath.empty() ? "Data" : gSelectedFolderRelativePath.c_str());
        QtUi::Separator();

        if (gHasAssetActionResult)
        {
            const UiVec4 statusColor = gLastAssetActionSucceeded
                ? UiVec4(0.35f, 0.85f, 0.45f, 1.0f)
                : UiVec4(0.90f, 0.45f, 0.35f, 1.0f);
            QtUi::TextColored(statusColor, "%s", gAssetStatusMessage.c_str());
        }
        else
        {
            QtUi::TextWrapped("%s", gAssetStatusMessage.c_str());
        }

        if (QtUi::BeginPopupModal("Rename Asset", nullptr, QtUiWindowFlags_AlwaysAutoResize))
        {
            QtUi::InputText("New Name", gRenameBuffer, std::size(gRenameBuffer));

            if (QtUi::Button("Apply"))
            {
                if (RenameSelectedAsset())
                {
                    RefreshAssetBrowserState();
                    QtUi::CloseCurrentPopup();
                }
            }

            QtUi::SameLine();
            if (QtUi::Button("Cancel"))
            {
                QtUi::CloseCurrentPopup();
            }

            QtUi::EndPopup();
        }

        if (QtUi::BeginPopupModal("Create Folder", nullptr, QtUiWindowFlags_AlwaysAutoResize))
        {
            QtUi::InputText("Folder Name", gNewFolderBuffer, std::size(gNewFolderBuffer));

            if (QtUi::Button("Create"))
            {
                if (CreateFolderInSelectedFolder())
                {
                    RefreshAssetBrowserState();
                    QtUi::CloseCurrentPopup();
                }
            }

            QtUi::SameLine();
            if (QtUi::Button("Cancel"))
            {
                QtUi::CloseCurrentPopup();
            }

            QtUi::EndPopup();
        }

        // Open the import progress modal whenever a batch import starts.
        if (gTextureImport.Running.load() || gTextureImport.Done.load() > 0)
        {
            QtUi::OpenPopup("Importing Textures...");
        }

        QtUi::SetNextWindowSize(UiVec2(760.0f, 420.0f), QtUiCond_Appearing);
        QtUi::SetNextWindowSizeConstraints(UiVec2(560.0f, 260.0f), UiVec2(FLT_MAX, FLT_MAX));
        if (QtUi::BeginPopupModal("Importing Textures...", nullptr, QtUiWindowFlags_None))
        {
            const int total = gTextureImport.Total.load();
            const int done  = gTextureImport.Done.load();
            const bool running = gTextureImport.Running.load();
            std::string currentFile;
            {
                std::lock_guard<std::mutex> lock(gTextureImport.LogMutex);
                currentFile = gTextureImport.CurrentFile;
            }

            float fraction = (total > 0) ? (static_cast<float>(done) / static_cast<float>(total)) : 0.0f;
            if (running && total > 0)
            {
                fraction = (std::max)(fraction, 0.02f);
            }
            char progressLabel[64];
            _snprintf_s(progressLabel, _TRUNCATE, "%d / %d", done, total);
            QtUi::ProgressBar(fraction, UiVec2(-FLT_MIN, 0.0f), progressLabel);

            if (running)
            {
                if (!currentFile.empty())
                {
                    QtUi::TextWrapped("Processing: %s", currentFile.c_str());
                }
                else
                {
                    QtUi::TextUnformatted("Preparing import...");
                }

                if (done == 0)
                {
                    QtUi::TextWrapped("The first texture is still being compressed. Large 4K imports can take a bit before the first file completes.");
                }
            }
            else
            {
                QtUi::TextUnformatted("Import complete.");
            }

            // Scrollable log of per-file results.
            QtUi::Separator();
            const float footerHeight = QtUi::GetFrameHeightWithSpacing() + QtUi::GetStyle().ItemSpacing.y;
            QtUi::BeginChild("ImportLog", UiVec2(0.0f, -footerHeight), true);
            {
                std::lock_guard<std::mutex> lock(gTextureImport.LogMutex);
                for (const std::string& line : gTextureImport.Log)
                {
                    const bool isError = line.size() >= 3 && line.substr(0, 3) == "ERR";
                    if (isError)
                    {
                        QtUi::TextColored(UiVec4(0.9f, 0.4f, 0.3f, 1.0f), "%s", line.c_str());
                    }
                    else
                    {
                        QtUi::TextColored(UiVec4(0.35f, 0.85f, 0.45f, 1.0f), "%s", line.c_str());
                    }
                }
                // Auto-scroll to the bottom.
                if (QtUi::GetScrollY() >= QtUi::GetScrollMaxY())
                {
                    QtUi::SetScrollHereY(1.0f);
                }
            }
            QtUi::EndChild();

            // Only allow closing once the thread is done.
            QtUi::BeginDisabled(running);
            if (QtUi::Button("Close"))
            {
                // Reset so the modal won't reopen next frame.
                gTextureImport.Total.store(0);
                gTextureImport.Done.store(0);
                {
                    std::lock_guard<std::mutex> lock(gTextureImport.LogMutex);
                    gTextureImport.Log.clear();
                    gTextureImport.CurrentFile.clear();
                }
                RefreshAssetBrowserState();
                QtUi::CloseCurrentPopup();
            }
            QtUi::EndDisabled();

            QtUi::EndPopup();
        }

        QtUi::Separator();

        const float folderPaneWidth = 240.0f;
        if (QtUi::BeginChild("AssetFolders", UiVec2(folderPaneWidth, 0.0f), true))
        {
            RenderFolderTreeNode({});
        }
        QtUi::EndChild();

        QtUi::SameLine();

        if (QtUi::BeginChild("AssetContents", UiVec2(0.0f, 0.0f), true))
        {
            const std::vector<AssetBrowserItem>& items = CollectFolderItems(gSelectedFolderRelativePath);
            if (items.empty())
            {
                QtUi::TextUnformatted("This folder is empty.");
            }
            else
            {
                for (const AssetBrowserItem& item : items)
                {
                    const std::string itemLabel = std::string(item.IsDirectory ? "[Folder] " : "[File] ") + item.Name;
                    if (QtUi::Selectable(itemLabel.c_str(), gSelectedAssetRelativePath == item.RelativePath))
                    {
                        gSelectedAssetRelativePath = item.RelativePath;
                    }

                    if (QtUi::IsItemHovered() && QtUi::IsMouseDoubleClicked(QtUiMouseButton_Left) && item.IsDirectory)
                    {
                        SelectFolder(item.RelativePath);
                    }

                    if (QtUi::BeginPopupContextItem(item.RelativePath.c_str()))
                    {
                        gSelectedAssetRelativePath = item.RelativePath;

                        if (item.IsDirectory && QtUi::MenuItem("Open"))
                        {
                            SelectFolder(item.RelativePath);
                        }

                        if (item.IsDirectory && QtUi::MenuItem("Import FBX Here..."))
                        {
                            SelectFolder(item.RelativePath);
                            PromptForAndImportFbx(windowHandle, item.RelativePath);
                            RefreshAssetBrowserState();
                        }

                        if (item.IsDirectory && QtUi::MenuItem("Import Texture Here..."))
                        {
                            SelectFolder(item.RelativePath);
                            PromptForAndImportTexture(windowHandle, item.RelativePath);
                            RefreshAssetBrowserState();
                        }

                        if (QtUi::MenuItem("Rename"))
                        {
                            BeginRenameSelectedAsset();
                        }

                        if (QtUi::MenuItem("Copy"))
                        {
                            CopySelectedAssetToClipboard();
                        }

                        if (!item.IsDirectory && IsGeometryAssetPath(item.RelativePath) && QtUi::MenuItem("Generate LODs"))
                        {
                            RegenerateSelectedGeometryLods(editor);
                            RefreshAssetBrowserState();
                        }

                        if (!item.IsDirectory && IsGeometryAssetPath(item.RelativePath) && QtUi::MenuItem("Generate Collisions"))
                        {
                            GenerateSelectedGeometryCollisions();
                        }

                        if (QtUi::MenuItem("Remove"))
                        {
                            DeleteSelectedAsset();
                            RefreshAssetBrowserState();
                        }

                        QtUi::EndPopup();
                    }
                }
            }

            if (QtUi::BeginPopupContextWindow("AssetBrowserBackgroundContext", QtUiPopupFlags_NoOpenOverItems | QtUiPopupFlags_MouseButtonRight))
            {
                if (QtUi::MenuItem("Import FBX Here..."))
                {
                    PromptForAndImportFbx(windowHandle, gSelectedFolderRelativePath);
                    RefreshAssetBrowserState();
                }

                if (QtUi::MenuItem("Import Texture Here..."))
                {
                    PromptForAndImportTexture(windowHandle, gSelectedFolderRelativePath);
                    RefreshAssetBrowserState();
                }

                if (QtUi::MenuItem("New Folder"))
                {
                    BeginCreateFolder();
                }

                QtUi::BeginDisabled(!canPaste);
                if (QtUi::MenuItem("Paste"))
                {
                    PasteClipboardIntoSelectedFolder();
                    RefreshAssetBrowserState();
                }
                QtUi::EndDisabled();

                QtUi::EndPopup();
            }
        }
        QtUi::EndChild();
        QtUi::End();
    }

    if (gShowAboutWindow)
    {
        QtUi::Begin("About Ptero Editor", &gShowAboutWindow, QtUiWindowFlags_AlwaysAutoResize);
        QtUi::TextUnformatted("Ptero Editor");
        QtUi::Separator();
        QtUi::TextUnformatted("Interface: Qt Widgets | Rendering: DirectX 12");
        QtUi::End();
    }

    // -----------------------------------------------------------------------
    // G-Buffer / RT Debug window
    // Displays each G-Buffer layer and the RT GI accumulation texture as a
    // tiled image grid so rendering issues can be diagnosed visually.
    // -----------------------------------------------------------------------
    if (gShowGBufferDebugWindow && gbufferTextureIds != nullptr)
    {
        // Force the window large enough for all 5 tiles every time it is opened
        // so that stale imgui.ini sizes from earlier sessions don't cut off tiles.
        QtUi::SetNextWindowSize(UiVec2(900.0f, 980.0f), QtUiCond_Appearing);
        if (QtUi::Begin("G-Buffer / RT Debug", &gShowGBufferDebugWindow))
        {
            // Each tile shows a label, then the texture at a fixed size.
            //
            // The tile size is deliberately NOT derived from the available
            // content region. This layer is immediate-mode over retained Qt
            // widgets, so the content determines the window's minimum size -
            // sizing the tiles from the window therefore feeds back on itself
            // and the window grows a little every frame until it fills the
            // screen. Two fixed columns at this width fit the 900px default.
            const float padding    = 8.0f;
            const float tileWidth  = 420.0f;
            const float tileHeight = tileWidth * (9.0f / 16.0f); // 16:9 aspect

            // Order tiles so the most diagnostic ones appear first (top-left).
            struct Tile { const char* label; UiTextureID id; };
            Tile tiles[] =
            {
                { "RT GI Accumulation (RGB)",              gbufferTextureIds->GiAccum  },
                { "Albedo (RGB)",                          gbufferTextureIds->Albedo   },
                { "World Normal (RGB)",                    gbufferTextureIds->Normal   },
                { "Material (R=Roughness, G=Metallic, B=AO)", gbufferTextureIds->Material },
                { "Depth (reversed preview of scene depth)",  gbufferTextureIds->Depth    },
                { "Point Shadow Array (slice 0 view)",       gbufferTextureIds->PointShadowArray },
            };

            for (int i = 0; i < static_cast<int>(std::size(tiles)); ++i)
            {
                // Two columns: even indices go left, odd indices go right (SameLine).
                if (i > 0 && (i % 2) == 1)
                    QtUi::SameLine(tileWidth + padding);

                QtUi::BeginGroup();
                QtUi::TextUnformatted(tiles[i].label);
                if (tiles[i].id != UiTextureID_Invalid)
                    QtUi::Image(tiles[i].id, UiVec2(tileWidth, tileHeight));
                else
                    QtUi::TextDisabled("(not available)");
                QtUi::EndGroup();
            }
        }
        QtUi::End();
    }
}
