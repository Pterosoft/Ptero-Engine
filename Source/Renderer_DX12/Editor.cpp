#include "pch.h"
#include "Editor.h"

#include "System/PteroLog.h"


#include "SceneSerializer.h"
#include "HeightmapImporter.h"
#include "DX12SceneRenderer.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <commdlg.h>
#include <wincodec.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12CommandQueue* __stdcall DX12Context_GetCommandQueue();
    HWND __stdcall DX12Context_GetWindowHandle();
    bool __stdcall DX12Context_GetRenderSize(UINT* width, UINT* height);
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    // Project a 3D world-space point onto the 2D viewport.
    // Returns false if the point is behind the camera.
    bool ProjectPoint(
        const DirectX::XMMATRIX& viewProj,
        const UiVec2& vpOrigin,
        const UiVec2& vpSize,
        const DirectX::XMFLOAT3& worldPos,
        UiVec2& outScreen)
    {
        using namespace DirectX;
        const XMVECTOR clip = XMVector4Transform(
            XMVectorSet(worldPos.x, worldPos.y, worldPos.z, 1.0f), viewProj);
        const float w = XMVectorGetW(clip);
        if (w <= 0.0f) return false;
        const float ndcX = XMVectorGetX(clip) / w;
        const float ndcY = XMVectorGetY(clip) / w;
        outScreen.x = vpOrigin.x + (ndcX  * 0.5f + 0.5f) * vpSize.x;
        outScreen.y = vpOrigin.y + (-ndcY * 0.5f + 0.5f) * vpSize.y;
        return true;
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

    UiTextureID TextureIdFromHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle)
    {
        return static_cast<UiTextureID>(handle.ptr);
    }

    std::string NormalizeRelativeDataPath(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path normalizedPath = relativePath.lexically_normal();
        return (normalizedPath.empty() || normalizedPath == ".") ? std::string{} : normalizedPath.generic_string();
    }

    std::filesystem::path FindProjectDataDirectory()
    {
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD characterCount = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (characterCount == 0 || characterCount == std::size(executablePath))
            return {};

        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        while (!currentPath.empty())
        {
            const std::filesystem::path dataDirectory = currentPath / "Data";
            if (std::filesystem::exists(dataDirectory) && std::filesystem::is_directory(dataDirectory))
                return dataDirectory;

            const std::filesystem::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
                break;

            currentPath = parentPath;
        }

        return {};
    }

    bool PromptForDataFile(HWND ownerWindowHandle, const char* title, const char* filter, std::string& inOutRelativePath)
    {
        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (dataDirectory.empty())
            return false;

        char fileBuffer[MAX_PATH] = {};
        if (!inOutRelativePath.empty())
        {
            const std::filesystem::path absolutePath = (dataDirectory / std::filesystem::path(inOutRelativePath)).lexically_normal();
            strcpy_s(fileBuffer, absolutePath.string().c_str());
        }

        OPENFILENAMEA openFileName{};
        openFileName.lStructSize = sizeof(openFileName);
        openFileName.hwndOwner = ownerWindowHandle;
        openFileName.lpstrTitle = title;
        openFileName.lpstrFilter = filter;
        openFileName.lpstrFile = fileBuffer;
        openFileName.nMaxFile = static_cast<DWORD>(std::size(fileBuffer));
        openFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        openFileName.lpstrInitialDir = dataDirectory.string().c_str();
        if (!QtUi::OpenFileName(&openFileName))
            return false;

        std::error_code relativeError;
        const std::filesystem::path selectedPath(fileBuffer);
        const std::filesystem::path relativePath = std::filesystem::relative(selectedPath, dataDirectory, relativeError);
        if (relativeError)
            return false;

        inOutRelativePath = NormalizeRelativeDataPath(relativePath);
        return true;
    }

    bool DataRelativeFileExists(const std::string& relativePath)
    {
        if (relativePath.empty())
            return false;

        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        if (dataDirectory.empty())
            return false;

        std::error_code errorCode;
        return std::filesystem::exists((dataDirectory / std::filesystem::path(relativePath)).lexically_normal(), errorCode);
    }

    std::string FindDefaultMaterialPathForMesh(const std::string& meshRelativePath)
    {
        if (meshRelativePath.empty())
            return {};

        const std::filesystem::path meshPath(meshRelativePath);
        const std::filesystem::path meshDirectory = meshPath.parent_path();
        std::filesystem::path meshFileName = meshPath.filename();

        if (meshFileName.extension() == ".ptero")
            meshFileName = meshFileName.stem();
        if (meshFileName.extension() == ".fbx")
            meshFileName = meshFileName.stem();

        const std::filesystem::path directCandidate = meshDirectory / (meshFileName.string() + ".json");
        if (DataRelativeFileExists(directCandidate.generic_string()))
            return NormalizeRelativeDataPath(directCandidate);

        const std::filesystem::path folderNamedCandidate = meshDirectory / (meshDirectory.filename().string() + ".json");
        if (DataRelativeFileExists(folderNamedCandidate.generic_string()))
            return NormalizeRelativeDataPath(folderNamedCandidate);

        std::error_code iteratorError;
        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path absoluteMeshDirectory = (dataDirectory / meshDirectory).lexically_normal();
        for (std::filesystem::directory_iterator it(absoluteMeshDirectory, iteratorError), end;
             it != end && !iteratorError;
             it.increment(iteratorError))
        {
            if (it->is_regular_file(iteratorError) && it->path().extension() == ".json")
            {
                const std::filesystem::path relativePath = std::filesystem::relative(it->path(), dataDirectory, iteratorError);
                if (!iteratorError)
                    return NormalizeRelativeDataPath(relativePath);
            }
        }

        return {};
    }

    float RadiansToDegrees(float radians)
    {
        return radians * (180.0f / DirectX::XM_PI);
    }

    float DegreesToRadians(float degrees)
    {
        return degrees * (DirectX::XM_PI / 180.0f);
    }

    float SnapToStep(float value, float step)
    {
        if (step <= 0.000001f)
            return value;

        return std::round(value / step) * step;
    }

    bool NearlyEqual(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
    {
        constexpr float epsilon = 0.00001f;
        return std::abs(a.x - b.x) <= epsilon
            && std::abs(a.y - b.y) <= epsilon
            && std::abs(a.z - b.z) <= epsilon;
    }

    std::filesystem::path ResolveEditorIconPath(const wchar_t* fileName)
    {
        if (fileName == nullptr || fileName[0] == L'\0')
            return {};

        std::filesystem::path requestedPath(fileName);
        if (requestedPath.is_absolute() && std::filesystem::exists(requestedPath))
            return requestedPath;

        wchar_t executablePath[MAX_PATH] = {};
        const DWORD characterCount = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (characterCount == 0 || characterCount == std::size(executablePath))
            return requestedPath;

        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        const std::filesystem::path fileNameOnly = requestedPath.filename();
        while (!currentPath.empty())
        {
            const std::filesystem::path candidate = currentPath / L"Data" / L"Icons" / fileNameOnly;
            if (std::filesystem::exists(candidate))
                return candidate;

            const std::filesystem::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
                break;

            currentPath = parentPath;
        }

        return requestedPath;
    }
}

UiTextureID Editor::TextureIdFromHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    return static_cast<UiTextureID>(handle.ptr);
}

// ---------------------------------------------------------------------------
// Constructor / Initialize / Shutdown
// ---------------------------------------------------------------------------

Editor::Editor() = default;

void Editor::ReportProgress(const wchar_t* message) const
{
    if (mProgressCallback)
        mProgressCallback(message);
}

bool Editor::Initialize(ID3D12GraphicsCommandList* commandList)
{
    if (mIsInitialized)
    {
        return true;
    }

    // Baseline the undo history against whatever the editor starts with, so the
    // first edit records that state rather than the default-constructed empty
    // one an undo would then restore.
    ResetUndoHistory();

    ReportProgress(L"Loading editor geometry icon...");
    LoadGeometryIcon(commandList);
    std::string iconStatus;
    ReportProgress(L"Loading editor placement icons...");
    LoadIconTexture(L"Geometry.png", mGeometryIcon, &iconStatus, commandList);
    LoadIconTexture(L"PointLight.png", mPointLightIcon, &iconStatus, commandList);
    LoadIconTexture(L"AudioEmitter.png", mAudioEmitterIcon, &iconStatus, commandList);
    LoadIconTexture(L"Decal.png", mDecalIcon, &iconStatus, commandList);
    LoadIconTexture(L"Rain.png", mRainIcon, &iconStatus, commandList);
    LoadIconTexture(L"ParticleSystem.png", mParticleSystemIcon, &iconStatus, commandList);
    ReportProgress(L"Loading editor toolbar icons...");
    LoadIconTexture(L"Select.png", mSelectIcon, &iconStatus, commandList);
    LoadIconTexture(L"Move.png", mMoveIcon, &iconStatus, commandList);
    LoadIconTexture(L"Rotate.png", mRotateIcon, &iconStatus, commandList);
    LoadIconTexture(L"Scale.png", mScaleIcon, &iconStatus, commandList);
    LoadIconTexture(L"Wireframe.png", mWireframeIcon, &iconStatus, commandList);
    LoadIconTexture(L"Proxy.png", mProxyIcon, &iconStatus, commandList);
    LoadIconTexture(L"Game.png", mGameIcon, &iconStatus, commandList);
    ReportProgress(L"Editor UI assets ready.");
    mIsInitialized = true;
    return true;
}

void Editor::Shutdown()
{
    RestoreEditorAfterPlay();
    mPlayStartRequested=false;
    if (mSceneLoadWorker.joinable())
    {
        mSceneLoadState.CancelRequested.store(true);
        mSceneLoadWorker.join();
    }

    ReleaseIconTexture(mGeometryIcon);
    ReleaseIconTexture(mPointLightIcon);
    ReleaseIconTexture(mAudioEmitterIcon);
    ReleaseIconTexture(mDecalIcon);
    ReleaseIconTexture(mRainIcon);
    ReleaseIconTexture(mParticleSystemIcon);
    ReleaseIconTexture(mSelectIcon);
    ReleaseIconTexture(mMoveIcon);
    ReleaseIconTexture(mRotateIcon);
    ReleaseIconTexture(mScaleIcon);
    ReleaseIconTexture(mWireframeIcon);
    ReleaseIconTexture(mProxyIcon);
    ReleaseIconTexture(mGameIcon);

    // Before QtUi tears Qt down: the node graph window is a child of the editor shell and
    // must not outlive the QApplication.
    NodeGraphEditor::Shutdown();

    mIsInitialized = false;
}

// ---------------------------------------------------------------------------
// Icon loading helpers (stubs - textures loaded externally via DX12RendererAPI)
// ---------------------------------------------------------------------------

bool Editor::LoadGeometryIcon(ID3D12GraphicsCommandList* commandList)
{
    std::string statusMessage;
    return LoadIconTexture(L"Geometry.png", mGeometryIcon, &statusMessage, commandList);
}

bool Editor::LoadIconTexture(const wchar_t* fileName, IconTexture& iconTexture,
                              std::string* statusMessage, ID3D12GraphicsCommandList* commandList)
{
    if (fileName == nullptr || commandList == nullptr)
    {
        if (statusMessage) *statusMessage = "Invalid icon load request.";
        return false;
    }

    if (iconTexture.Texture)
    {
        return true;
    }

    ID3D12Device* device = DX12Context_GetDevice();
    ID3D12CommandQueue* queue = DX12Context_GetCommandQueue();
    if (!device || !queue)
    {
        if (statusMessage) *statusMessage = "DX12 device or command queue unavailable for icon load.";
        return false;
    }

    const std::filesystem::path resolvedPath = ResolveEditorIconPath(fileName);

    ComPtr<IWICImagingFactory> imagingFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&imagingFactory));
    if (FAILED(hr) || !imagingFactory)
    {
        if (statusMessage) *statusMessage = "Failed to create WIC imaging factory.";
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = imagingFactory->CreateDecoderFromFilename(
        resolvedPath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnLoad,
        &decoder);
    if (FAILED(hr) || !decoder)
    {
        if (statusMessage) *statusMessage = "Failed to open icon image.";
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame)
    {
        if (statusMessage) *statusMessage = "Failed to read icon frame.";
        return false;
    }

    ComPtr<IWICFormatConverter> formatConverter;
    hr = imagingFactory->CreateFormatConverter(&formatConverter);
    if (FAILED(hr) || !formatConverter)
    {
        if (statusMessage) *statusMessage = "Failed to create icon format converter.";
        return false;
    }

    hr = formatConverter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        if (statusMessage) *statusMessage = "Failed to convert icon image format.";
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = formatConverter->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
    {
        if (statusMessage) *statusMessage = "Failed to read icon dimensions.";
        return false;
    }

    const UINT rowPitch = width * 4;
    const UINT imageSize = rowPitch * height;
    std::vector<uint8_t> pixelData(imageSize);
    hr = formatConverter->CopyPixels(nullptr, rowPitch, imageSize, pixelData.data());
    if (FAILED(hr))
    {
        if (statusMessage) *statusMessage = "Failed to copy icon pixels.";
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    hr = device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&iconTexture.Texture));
    if (FAILED(hr) || !iconTexture.Texture)
    {
        if (statusMessage) *statusMessage = "Failed to create icon texture resource.";
        return false;
    }

    D3D12_SUBRESOURCE_DATA subresourceData{};
    subresourceData.pData = pixelData.data();
    subresourceData.RowPitch = rowPitch;
    subresourceData.SlicePitch = imageSize;

    const UINT64 uploadSize = GetRequiredIntermediateSize(iconTexture.Texture.Get(), 0, 1);
    const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    hr = device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&iconTexture.Upload));
    if (FAILED(hr) || !iconTexture.Upload)
    {
        iconTexture.Texture.Reset();
        if (statusMessage) *statusMessage = "Failed to create icon upload buffer.";
        return false;
    }

    UpdateSubresources(commandList, iconTexture.Texture.Get(), iconTexture.Upload.Get(), 0, 0, 1, &subresourceData);

    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        iconTexture.Texture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);

    if (!DX12Context_AllocateSrvDescriptor(&iconTexture.CpuHandle, &iconTexture.GpuHandle))
    {
        iconTexture.Texture.Reset();
        iconTexture.Upload.Reset();
        if (statusMessage) *statusMessage = "Failed to allocate icon SRV descriptor.";
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(iconTexture.Texture.Get(), &srvDesc, iconTexture.CpuHandle);
    QtUi::RegisterIcon(iconTexture.GpuHandle.ptr, resolvedPath.c_str());

    if (statusMessage) *statusMessage = "";
    return true;
}

void Editor::ReleaseIconTexture(IconTexture& iconTexture)
{
    iconTexture.Texture.Reset();
    iconTexture.Upload.Reset();
    iconTexture = IconTexture{};
}

// ---------------------------------------------------------------------------
// Entity accessors
// ---------------------------------------------------------------------------

Entity* Editor::GetSelectedEntity()
{
    if (mSelectedEntityIndex < 0 || mSelectedEntityIndex >= static_cast<int>(mEntities.size()))
        return nullptr;
    return &mEntities[mSelectedEntityIndex];
}

bool Editor::IsEntitySelected(int entityIndex) const
{
    return std::find(mSelectedEntityIndices.begin(), mSelectedEntityIndices.end(), entityIndex) != mSelectedEntityIndices.end();
}

// ---------------------------------------------------------------------------
// Copy / Paste / Delete
// ---------------------------------------------------------------------------

bool Editor::CanCopySelectedEntity() const
{
    return mSelectedEntityIndex >= 0 && mSelectedEntityIndex < static_cast<int>(mEntities.size());
}

bool Editor::CanPasteEntity() const
{
    return mCopiedEntity.has_value();
}

bool Editor::CanDeleteSelectedEntity() const
{
    return !mSelectedEntityIndices.empty()
        || (mSelectedEntityIndex >= 0 && mSelectedEntityIndex < static_cast<int>(mEntities.size()));
}

bool Editor::CopySelectedEntity()
{
    if (!CanCopySelectedEntity()) return false;
    mCopiedEntity = mEntities[mSelectedEntityIndex];
    PTERO_LOG_INFO("Editor", "Copied entity '%s' (index %d).",
                   mCopiedEntity->Name.c_str(), mSelectedEntityIndex);
    return true;
}

bool Editor::PasteCopiedEntity()
{
    if (!CanPasteEntity()) return false;
    mEntities.push_back(*mCopiedEntity);
    mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
    mSelectedEntityIndices.clear();
    mSelectedEntityIndices.push_back(mSelectedEntityIndex);
    MarkSceneChanged();
    PTERO_LOG_INFO("Editor", "Pasted entity '%s' at index %d. Scene now holds %llu entities.",
                   mEntities.back().Name.c_str(), mSelectedEntityIndex,
                   static_cast<unsigned long long>(mEntities.size()));
    return true;
}

bool Editor::DeleteSelectedEntity()
{
    if (!CanDeleteSelectedEntity()) return false;

    if (!mSelectedEntityIndices.empty())
    {
        std::vector<int> indices = mSelectedEntityIndices;
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        {
            if (*it >= 0 && *it < static_cast<int>(mEntities.size()))
            {
                mEntities.erase(mEntities.begin() + *it);
            }
        }
    }
    else
    {
        mEntities.erase(mEntities.begin() + mSelectedEntityIndex);
    }

    mSelectedEntityIndex = -1;
    mSelectedEntityIndices.clear();
    MarkSceneChanged();
    return true;
}

// ---------------------------------------------------------------------------
// Scene save / load
// ---------------------------------------------------------------------------

bool Editor::SaveSceneToFile(const std::string& filepath)
{
    Scene scene;
    scene.Entities          = &mEntities;
    scene.CameraPosition    = &mSavedCameraPosition;
    scene.CameraRotation    = &mSavedCameraRotation;
    scene.TimeOfDay         = mTimeOfDaySettings;
    scene.Taa               = mTaaSettings;
    scene.Smaa              = mSmaaSettings;
    scene.Sharpen           = mSharpenSettings;
    scene.Dlss              = mDlssSettings;
    scene.Fsr               = mFsrSettings;
    scene.GlobalIllumination = mGlobalIlluminationMode;
    scene.Rtgi              = mRtgiSettings;
    scene.RadianceCascades  = mRadianceCascadesSettings;
    scene.Rtao              = mRtaoSettings;
    scene.Gtao              = mGtaoSettings;
    scene.Ssr               = mSsrSettings;
    scene.ChromaticAberration = mChromaticAberrationSettings;
    scene.Agx               = mAgxSettings;
    scene.VolumetricFog     = mVolumetricFogSettings;
    scene.VolumetricCloud   = mVolumetricCloudSettings;
    scene.Bloom             = mBloomSettings;

    // Snapshot rather than hand out the editor's copy: the serializer takes a mutable
    // pointer, and nothing outside the Node Graph window may write that document.
    NodeGraphDocument nodeGraph = NodeGraphEditor::Document();
    scene.NodeGraph = &nodeGraph;

    try
    {
        SceneSerializer serializer(&scene);
        serializer.Serialize(filepath);
        mCurrentSceneFilePath = filepath;
        mLastSceneStatusMessage = "Scene saved: " + filepath;
        mSceneDirty = false;
        mNodeGraphRevisionAtSave = NodeGraphEditor::Revision();
        return true;
    }
    catch (const std::exception& e)
    {
        mLastSceneStatusMessage = std::string("Save failed: ") + e.what();
        return false;
    }
}

const NodeGraphDocument& Editor::GetRuntimeNodeGraph() const
{
    return mPlaySceneActive ? mPlayNodeGraph : NodeGraphEditor::Document();
}

void Editor::RestoreEditorAfterPlay()
{
    if (!mPlaySceneActive) return;
    // Game callbacks must finish before replacing the entity vector they address.
    if (mSceneRenderer && mSceneRenderer->IsGameRunning()) mSceneRenderer->StopGame();
    mEntities.swap(mEntitiesBeforePlay);
    mEntitiesBeforePlay.clear();
    for (auto& restore : mRestorePlaySettings) restore();
    mRestorePlaySettings.clear();
    mPlaySceneActive=false;
    if (mSceneRenderer) {
        mSceneRenderer->SetEntities(&mEntities);
        mSceneRenderer->SetNodeGraph(&NodeGraphEditor::Document());
    }
}

void Editor::StopPlaySession()
{
    mPlayStartRequested=false;
    RestoreEditorAfterPlay();
}

bool Editor::IsPlaySessionActive() const
{
    return mPlaySceneActive || (mSceneRenderer && mSceneRenderer->IsGameRunning());
}

void Editor::DrawPlayModeMenuItems()
{
    // The bool* overload is what makes a menu action checkable, so each entry gets a
    // throwaway mirror of the mode and the real assignment happens below - a checked
    // entry that is clicked must stay checked, not toggle itself off.
    //
    // Greyed out while a session runs: the mode is read when the session starts, so
    // offering to change it mid-play would only look like it did something.
    const bool available = !IsPlaySessionActive();
    bool inViewport = !mPlayInNewWindow;
    bool inNewWindow = mPlayInNewWindow;
    if (QtUi::MenuItem("Play In Viewport", nullptr, &inViewport, available))
        mPlayInNewWindow = false;
    if (QtUi::MenuItem("Play In New Window", nullptr, &inNewWindow, available))
        mPlayInNewWindow = true;
}

void Editor::UpdatePlaySession()
{
    if (mPlaySceneActive && mSceneRenderer && !mSceneRenderer->IsGameRunning())
        RestoreEditorAfterPlay();
    if (!mPlayStartRequested || !mSceneRenderer) return;
    mPlayStartRequested=false;
    if (IsSceneLoading()) {
        mGameStartErrorMessage="Wait for the level to finish loading before Play.";
        return;
    }
    try {
        // Copy entity values, retaining shared mesh assets already resident on the GPU.
        mEntitiesBeforePlay=mEntities;
        mPlaySceneActive=true;
        // The viewport stops handling interaction for the session, so a marquee drag
        // that was in progress would otherwise still be latched when it ends.
        mViewportSelection.IsDragging=false;
        mPlayNodeGraph=NodeGraphEditor::Document();
        auto remember=[this](auto* setting) {
            if (setting) mRestorePlaySettings.emplace_back([setting, value=*setting] { *setting=value; });
        };
        remember(mTimeOfDaySettings); remember(mTaaSettings); remember(mSmaaSettings);
        remember(mSharpenSettings); remember(mDlssSettings); remember(mFsrSettings); remember(mGlobalIlluminationMode);
        remember(mRtgiSettings); remember(mRadianceCascadesSettings); remember(mRtaoSettings);
        remember(mGtaoSettings); remember(mSsrSettings); remember(mChromaticAberrationSettings);
        remember(mAgxSettings); remember(mVolumetricFogSettings); remember(mVolumetricCloudSettings);
        remember(mBloomSettings);
        // Playing in the viewport, F11 hides the panels for the session. Stopping puts
        // them back rather than leaving the editor fullscreen with nothing running.
        remember(&mViewportFullscreen);
        const bool grid=mSceneRenderer->IsGridEnabled(), wireframe=mSceneRenderer->IsWireframeEnabled();
        mRestorePlaySettings.emplace_back([this, grid, wireframe] {
            mSceneRenderer->SetGridEnabled(grid); mSceneRenderer->SetWireframeEnabled(wireframe);
        });
        if (_stricmp(std::filesystem::path(mCurrentSceneFilePath).filename().string().c_str(), "Farkle.json")!=0) {
            wchar_t executable[MAX_PATH]{};
            GetModuleFileNameW(nullptr, executable, MAX_PATH);
            auto directory=std::filesystem::path(executable).parent_path();
            std::filesystem::path level;
            for (int i=0; i<6; ++i) {
                auto candidate=directory / "Data" / "Levels" / "Farkle.json";
                if (std::filesystem::exists(candidate)) { level=candidate; break; }
                directory=directory.parent_path();
            }
            if (level.empty()) throw std::runtime_error("Could not find Data/Levels/Farkle.json.");
            Scene scene;
            scene.Entities=&mEntities; scene.NodeGraph=&mPlayNodeGraph;
            scene.TimeOfDay=mTimeOfDaySettings; scene.Taa=mTaaSettings; scene.Smaa=mSmaaSettings;
            scene.Sharpen=mSharpenSettings; scene.Dlss=mDlssSettings; scene.Fsr=mFsrSettings;
            scene.GlobalIllumination=mGlobalIlluminationMode; scene.Rtgi=mRtgiSettings;
            scene.RadianceCascades=mRadianceCascadesSettings; scene.Rtao=mRtaoSettings;
            scene.Gtao=mGtaoSettings; scene.Ssr=mSsrSettings;
            scene.ChromaticAberration=mChromaticAberrationSettings; scene.Agx=mAgxSettings;
            scene.VolumetricFog=mVolumetricFogSettings; scene.VolumetricCloud=mVolumetricCloudSettings;
            scene.Bloom=mBloomSettings;
            mEntities.clear();
            SceneSerializer serializer(&scene);
            if (!serializer.Deserialize(level.string())) throw std::runtime_error("Could not load the Farkle play level.");
        }
        mSceneRenderer->SetEntities(&mEntities);
        mSceneRenderer->SetNodeGraph(&mPlayNodeGraph);
        mSceneRenderer->SetGridEnabled(false); mSceneRenderer->SetWireframeEnabled(false);
        if (!mSceneRenderer->StartGame(mPlayInNewWindow)) throw std::runtime_error(mSceneRenderer->GetGameStartErrorMessage());
        mGameStartErrorMessage.clear();
    } catch (const std::exception& error) {
        mGameStartErrorMessage=error.what();
        RestoreEditorAfterPlay();
    }
}

bool Editor::LoadSceneFromFile(const std::string& filepath)
{
    Scene scene;
    scene.Entities      = &mEntities;
    scene.CameraPosition = &mSavedCameraPosition;
    scene.CameraRotation = &mSavedCameraRotation;
    scene.HasCameraPosition = nullptr;
    scene.HasCameraRotation = nullptr;
    scene.TimeOfDay     = mTimeOfDaySettings;
    scene.Taa           = mTaaSettings;
    scene.Smaa          = mSmaaSettings;
    scene.Sharpen       = mSharpenSettings;
    scene.Dlss          = mDlssSettings;
    scene.Fsr           = mFsrSettings;
    scene.GlobalIllumination = mGlobalIlluminationMode;
    scene.Rtgi          = mRtgiSettings;
    scene.RadianceCascades = mRadianceCascadesSettings;
    scene.Rtao          = mRtaoSettings;
    scene.Gtao          = mGtaoSettings;
    scene.Ssr           = mSsrSettings;
    scene.ChromaticAberration = mChromaticAberrationSettings;
    scene.Agx           = mAgxSettings;
    scene.VolumetricFog = mVolumetricFogSettings;
    scene.VolumetricCloud = mVolumetricCloudSettings;
    scene.Bloom         = mBloomSettings;

    NodeGraphDocument nodeGraph;
    scene.NodeGraph = &nodeGraph;

    try
    {
        SceneSerializer serializer(&scene);
        mEntities.clear();
        mSelectedEntityIndex = -1;
        mSelectedEntityIndices.clear();
        if (serializer.Deserialize(filepath))
        {
            mCurrentSceneFilePath   = filepath;
            mLastSceneStatusMessage = "Scene loaded: " + filepath;
            mNextGeometryInstanceId = static_cast<int>(mEntities.size()) + 1;
            mSceneDirty = false;
            ResetUndoHistory();
            NodeGraphEditor::SetDocument(nodeGraph);
            mNodeGraphRevisionAtSave = NodeGraphEditor::Revision();
            return true;
        }
        mLastSceneStatusMessage = "Failed to deserialize: " + filepath;
        return false;
    }
    catch (const std::exception& e)
    {
        mLastSceneStatusMessage = std::string("Load failed: ") + e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// Async scene loading
// ---------------------------------------------------------------------------

bool Editor::BeginLoadSceneFromFile(const std::string& filepath)
{
    if (IsSceneLoading()) return false;

    if (mSceneLoadWorker.joinable())
    {
        mSceneLoadWorker.join();
    }

    mSceneLoadState.Progress = 0.0f;
    {
        std::lock_guard<std::mutex> lock(mSceneLoadState.Mutex);
        mSceneLoadState.StatusMessage = "Loading...";
    }
    mSceneLoadState.InProgress = true;
    mSceneLoadState.Completed = false;
    mSceneLoadState.CancelRequested = false;
    mSceneLoadState.Result.reset();

    std::string fp = filepath;
    mSceneLoadWorker = std::thread([this, fp]()
    {
        SceneLoadData data;
        Scene scene;
        scene.Entities      = &data.Entities;
        scene.CameraPosition = &data.CameraPosition;
        scene.CameraRotation = &data.CameraRotation;
        scene.HasCameraPosition = &data.HasCameraPosition;
        scene.HasCameraRotation = &data.HasCameraRotation;
        scene.TimeOfDay     = &data.TimeOfDay;
        scene.Taa           = &data.Taa;
        scene.Smaa          = &data.Smaa;
        scene.Sharpen       = &data.Sharpen;
        scene.Dlss          = &data.Dlss;
        scene.Fsr           = &data.Fsr;
        scene.GlobalIllumination = &data.GlobalIlluminationMode;
        scene.Rtgi          = &data.Rtgi;
        scene.RadianceCascades = &data.RadianceCascades;
        scene.Rtao          = &data.Rtao;
        scene.Gtao          = &data.Gtao;
        scene.Ssr           = &data.Ssr;
        scene.ChromaticAberration = &data.ChromaticAberration;
        scene.Agx           = &data.Agx;
        scene.VolumetricFog = &data.VolumetricFog;
        scene.VolumetricCloud = &data.VolumetricCloud;
        scene.Bloom         = &data.Bloom;
        // Parsed on the worker, handed to the Qt window in UpdateSceneLoading: the node
        // editor is a widget and must only ever be touched from the main thread.
        scene.NodeGraph     = &data.NodeGraph;

        try
        {
            SceneSerializer serializer(&scene);
            bool ok = serializer.Deserialize(fp, SceneLoadProgressCallback, this);
            mSceneLoadState.Completed.store(ok);
        }
        catch (...)
        {
            mSceneLoadState.Completed.store(false);
        }

        mSceneLoadState.Result.emplace(std::move(data));
        mSceneLoadState.InProgress.store(false);
    });

    mCurrentSceneFilePath = filepath;
    return true;
}

void Editor::ResetScene()
{
    mEntities.clear();
    mSelectedEntityIndex = -1;
    mSelectedEntityIndices.clear();
    mCurrentSceneFilePath.clear();
    mLastSceneStatusMessage = "New scene created.";
    mNextGeometryInstanceId = 1;
    mSceneDirty = false;
    ResetUndoHistory();

    NodeGraphEditor::SetDocument(NodeGraphDocument{});
    mNodeGraphRevisionAtSave = NodeGraphEditor::Revision();
}

bool Editor::ConfirmDiscardUnsavedScene(HWND ownerWindowHandle)
{
    if (!HasUnsavedChanges())
    {
        return true;
    }

    const int result = MessageBoxA(
        ownerWindowHandle,
        "The current level has unsaved changes. Do you want to save it before continuing?",
        "Unsaved Level",
        MB_YESNOCANCEL | MB_ICONWARNING);

    if (result == IDCANCEL)
    {
        return false;
    }

    if (result == IDYES)
    {
        return SaveScene(ownerWindowHandle);
    }

    return true;
}

bool Editor::SaveScene(HWND ownerWindowHandle)
{
    if (!mCurrentSceneFilePath.empty())
    {
        return SaveSceneToFile(mCurrentSceneFilePath);
    }

    return SaveSceneAs(ownerWindowHandle);
}

bool Editor::SaveSceneAs(HWND ownerWindowHandle)
{
    char sceneFileBuffer[MAX_PATH] = {};
    if (!mCurrentSceneFilePath.empty())
    {
        strcpy_s(sceneFileBuffer, mCurrentSceneFilePath.c_str());
    }

    if (!PromptForSceneSavePath(ownerWindowHandle, sceneFileBuffer, static_cast<DWORD>(std::size(sceneFileBuffer))))
    {
        return false;
    }

    return SaveSceneToFile(sceneFileBuffer);
}

bool Editor::OpenScene(HWND ownerWindowHandle)
{
    if (!ConfirmDiscardUnsavedScene(ownerWindowHandle))
    {
        return false;
    }

    char sceneFileBuffer[MAX_PATH] = {};
    if (!mCurrentSceneFilePath.empty())
    {
        strcpy_s(sceneFileBuffer, mCurrentSceneFilePath.c_str());
    }

    if (!PromptForSceneOpenPath(ownerWindowHandle, sceneFileBuffer, static_cast<DWORD>(std::size(sceneFileBuffer))))
    {
        return false;
    }

    return BeginLoadSceneFromFile(sceneFileBuffer);
}

bool Editor::NewScene(HWND ownerWindowHandle)
{
    if (!ConfirmDiscardUnsavedScene(ownerWindowHandle))
    {
        return false;
    }

    ResetScene();
    return true;
}

void Editor::UpdateSceneLoading()
{
    // InProgress first: the worker fills Result and only then clears it, so the
    // optional must not be looked at while the worker may still be writing it.
    if (mSceneLoadState.InProgress.load()) return;
    if (!mSceneLoadState.Result.has_value()) return;
    if (mSceneLoadWorker.joinable()) mSceneLoadWorker.join();

    if (mSceneLoadState.Completed.load())
    {
        auto& data       = *mSceneLoadState.Result;
        mEntities        = std::move(data.Entities);
        mSelectedEntityIndex = -1;
        mSelectedEntityIndices.clear();
        mNextGeometryInstanceId = static_cast<int>(mEntities.size()) + 1;

        if (data.HasCameraPosition)
            mSavedCameraPosition = data.CameraPosition;
        if (data.HasCameraRotation)
            mSavedCameraRotation = data.CameraRotation;
        mRequestCameraRestore = data.HasCameraPosition || data.HasCameraRotation;

        if (mTimeOfDaySettings)       *mTimeOfDaySettings       = data.TimeOfDay;
        if (mTaaSettings)             *mTaaSettings             = data.Taa;
        if (mSmaaSettings)            *mSmaaSettings            = data.Smaa;
        if (mSharpenSettings)         *mSharpenSettings         = data.Sharpen;
        if (mDlssSettings)            *mDlssSettings            = data.Dlss;
        if (mFsrSettings)             *mFsrSettings             = data.Fsr;
        if (mGlobalIlluminationMode)  *mGlobalIlluminationMode  = data.GlobalIlluminationMode;
        if (mRtgiSettings)            *mRtgiSettings            = data.Rtgi;
        if (mRadianceCascadesSettings) *mRadianceCascadesSettings = data.RadianceCascades;
        if (mRtaoSettings)            *mRtaoSettings            = data.Rtao;
        if (mGtaoSettings)            *mGtaoSettings            = data.Gtao;
        if (mSsrSettings)             *mSsrSettings             = data.Ssr;
        if (mChromaticAberrationSettings) *mChromaticAberrationSettings = data.ChromaticAberration;
        if (mAgxSettings)             *mAgxSettings             = data.Agx;
        if (mVolumetricFogSettings)   *mVolumetricFogSettings   = data.VolumetricFog;
        if (mVolumetricCloudSettings) *mVolumetricCloudSettings = data.VolumetricCloud;
        if (mBloomSettings)           *mBloomSettings           = data.Bloom;

        NodeGraphEditor::SetDocument(data.NodeGraph);
        mNodeGraphRevisionAtSave = NodeGraphEditor::Revision();

        mLastSceneStatusMessage = "Scene loaded: " + mCurrentSceneFilePath;
        mSceneDirty = false;
        ResetUndoHistory();

        // The render loop resolves the meshes from here on and reports back.
        mSceneAssetsStreaming = true;
        mSceneAssetsFinishing = false;
        mSceneAssetsResolved = 0;
        mSceneAssetsTotal = 0;
    }
    else
    {
        mLastSceneStatusMessage = "Failed to load: " + mCurrentSceneFilePath;
    }

    mSceneLoadState.Result.reset();
}

void Editor::SetSceneAssetStreamingProgress(size_t resolvedMeshes, size_t totalMeshes)
{
    if (!mSceneAssetsStreaming) return;
    mSceneAssetsResolved = resolvedMeshes;
    mSceneAssetsTotal = totalMeshes;
    if (resolvedMeshes < totalMeshes)
    {
        mSceneAssetsFinishing = false;
        return;
    }
    if (mSceneAssetsFinishing)
        mSceneAssetsStreaming = false;
    else
        mSceneAssetsFinishing = true;
}

namespace
{
    // Parsing the file is quick next to reading the meshes, so it gets a small
    // slice of the bar and the meshes get the rest.
    constexpr float kSceneFileProgressShare = 0.1f;
}

bool Editor::IsSceneLoading() const
{
    return mSceneLoadState.InProgress.load() || mSceneAssetsStreaming;
}

float Editor::GetSceneLoadProgress() const
{
    if (mSceneLoadState.InProgress.load())
        return kSceneFileProgressShare * mSceneLoadState.Progress.load();
    if (mSceneAssetsStreaming && mSceneAssetsTotal > 0)
        return kSceneFileProgressShare + (1.0f - kSceneFileProgressShare)
            * (static_cast<float>(mSceneAssetsResolved) / static_cast<float>(mSceneAssetsTotal));
    if (mSceneAssetsStreaming)
        return kSceneFileProgressShare;
    return 1.0f;
}

std::string Editor::GetSceneLoadStatusMessage() const
{
    if (!mSceneLoadState.InProgress.load() && mSceneAssetsStreaming)
    {
        if (mSceneAssetsFinishing)
            return "Loading textures...";
        return "Loading meshes (" + std::to_string(mSceneAssetsResolved) + "/"
            + std::to_string(mSceneAssetsTotal) + ")...";
    }
    std::lock_guard<std::mutex> lock(mSceneLoadState.Mutex);
    return mSceneLoadState.StatusMessage;
}

void Editor::SetSceneLoadProgress(float progress, const char* statusMessage)
{
    mSceneLoadState.Progress.store(progress);
    if (statusMessage)
    {
        std::lock_guard<std::mutex> lock(mSceneLoadState.Mutex);
        mSceneLoadState.StatusMessage = statusMessage;
    }
}

bool Editor::SceneLoadProgressCallback(float progress, const char* statusMessage, void* userData)
{
    if (auto* editor = static_cast<Editor*>(userData))
    {
        if (editor->mSceneLoadState.CancelRequested.load())
        {
            return false;
        }

        editor->SetSceneLoadProgress(progress, statusMessage);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Viewport helpers
// ---------------------------------------------------------------------------

bool Editor::TryProjectWorldToViewport(
    const DirectX::XMFLOAT3& worldPosition,
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera,
    UiVec2& outScreenPosition,
    bool clampToViewport) const
{
    using namespace DirectX;
    XMVECTOR pos = XMLoadFloat3(&worldPosition);
    XMMATRIX vp  = camera.GetViewMatrix() * camera.GetProjectionMatrix();
    XMVECTOR clip = XMVector4Transform(XMVectorSetW(pos, 1.0f), vp);

    float w = XMVectorGetW(clip);
    if (w < 0.0001f) return false;

    float ndcX = XMVectorGetX(clip) / w;
    float ndcY = XMVectorGetY(clip) / w;

    if (!clampToViewport && (ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f)) return false;

    if (clampToViewport)
    {
        ndcX = (std::max)(-1.0f, (std::min)(1.0f, ndcX));
        ndcY = (std::max)(-1.0f, (std::min)(1.0f, ndcY));
    }

    outScreenPosition.x = viewportOrigin.x + (ndcX  * 0.5f + 0.5f) * viewportSize.x;
    outScreenPosition.y = viewportOrigin.y + (1.0f - (ndcY * 0.5f + 0.5f)) * viewportSize.y;
    return true;
}

bool Editor::TryGetViewportWorldPositionOnGrid(
    const UiVec2& mousePosition,
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera,
    DirectX::XMFLOAT3& outWorldPosition) const
{
    using namespace DirectX;

    if (viewportSize.x <= 0.0f || viewportSize.y <= 0.0f) return false;

    float ndcX = ((mousePosition.x - viewportOrigin.x) / viewportSize.x) * 2.0f - 1.0f;
    float ndcY = 1.0f - ((mousePosition.y - viewportOrigin.y) / viewportSize.y) * 2.0f;

    XMMATRIX invVP = XMMatrixInverse(nullptr, camera.GetViewMatrix() * camera.GetProjectionMatrix());

    XMVECTOR nearClip = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), invVP);
    XMVECTOR farClip  = XMVector4Transform(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), invVP);

    nearClip = nearClip / XMVectorSplatW(nearClip);
    farClip  = farClip  / XMVectorSplatW(farClip);

    XMVECTOR dir = XMVector3Normalize(farClip - nearClip);
    float dirZ = XMVectorGetZ(dir);
    if (std::abs(dirZ) < 0.0001f) return false;

    float t = -XMVectorGetZ(nearClip) / dirZ;
    if (t < 0.0f) return false;

    XMVECTOR hit = nearClip + dir * t;
    XMStoreFloat3(&outWorldPosition, hit);
    return true;
}

// ---------------------------------------------------------------------------
// Viewport selection / interaction
// ---------------------------------------------------------------------------

void Editor::HandleViewportInteraction(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera,
    bool viewportHovered)
{
    if (!viewportHovered) return;

    if (mBlockViewportSelection)
    {
        if (!QtUi::IsMouseDown(QtUiMouseButton_Left) && !QtUi::IsMouseReleased(QtUiMouseButton_Left))
        {
            mBlockViewportSelection = false;
            mViewportSelection.IsDragging = false;
        }
        return;
    }

    QtUiIO& io = QtUi::GetIO();
    const bool gizmoHovered = mManualGizmo.IsActive;
    const bool gizmoInUse = mManualGizmo.IsActive;

    if (gizmoHovered || gizmoInUse)
    {
        mViewportSelection.IsDragging = false;
        return;
    }

    if (QtUi::IsMouseClicked(QtUiMouseButton_Left))
    {
        mViewportSelection.IsDragging = true;
        mViewportSelection.Start       = io.MousePos;
        mViewportSelection.Current     = io.MousePos;
    }

    if (mViewportSelection.IsDragging && QtUi::IsMouseDown(QtUiMouseButton_Left))
    {
        mViewportSelection.Current = io.MousePos;
    }

    if (mViewportSelection.IsDragging && QtUi::IsMouseReleased(QtUiMouseButton_Left))
    {
        FinishViewportSelection(viewportOrigin, viewportSize, camera);
        mViewportSelection.IsDragging = false;
    }
}

void Editor::FinishViewportSelection(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera)
{
    // Terrain brush mode intercepts the click before any prototype
    // placement so the artist can paint with the existing left-click
    // gesture. The renderer applies the stroke to the terrain patch
    // containing the picked world XY ground-plane position.
    if (mTerrainBrushModeActive && mTerrainRenderer != nullptr)
    {
        DirectX::XMFLOAT3 worldPosition{};
        if (TryGetViewportWorldPositionOnGrid(mViewportSelection.Current, viewportOrigin, viewportSize, camera, worldPosition))
        {
            const DirectX::XMFLOAT2 pickXZ(worldPosition.x, worldPosition.y);
            std::string statusMessage;
            const bool applied = mTerrainRenderer->ApplyBrushAt(pickXZ, &statusMessage);
            if (applied)
            {
                MarkSceneChanged();
                mLastTerrainBrushMessage = statusMessage;
            }
            else if (!statusMessage.empty())
            {
                mLastTerrainBrushMessage = "Brush skipped: " + statusMessage;
            }
        }
        return;
    }

    if (mGeometryPrototypeSelected || mPointLightPrototypeSelected || mSpotLightPrototypeSelected || mRectLightPrototypeSelected || mAudioEmitterPrototypeSelected || mDecalPrototypeSelected || mRainPrototypeSelected || mParticleSystemPrototypeSelected || mTerrainPrototypeSelected || mWaterPrototypeSelected || mVegetationPrototypeSelected)
    {
        if (mTerrainPrototypeSelected)
        {
            DirectX::XMFLOAT3 worldPosition{};
            if (TryGetViewportWorldPositionOnGrid(mViewportSelection.Current, viewportOrigin, viewportSize, camera, worldPosition))
            {
                worldPosition.z = 0.0f;
                CreateTerrainFromRawFile(&worldPosition);
            }
            mTerrainPrototypeSelected = false;
            return;
        }

        DirectX::XMFLOAT3 worldPosition{};
        if (TryGetViewportWorldPositionOnGrid(mViewportSelection.Current, viewportOrigin, viewportSize, camera, worldPosition))
        {
            worldPosition.z = 0.0f;

            if (mGeometryPrototypeSelected)
            {
                CreateGeometryInstanceAt(worldPosition);
            }
            else if (mPointLightPrototypeSelected)
            {
                Entity entity;
                entity.Name = "PointLight_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                entity.AddPointLightComponent();
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mSpotLightPrototypeSelected)
            {
                Entity entity;
                entity.Name = "SpotLight_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                // Placed on the ground plane like every other prototype, but a
                // spot emits along local -Z, so drop it at head height and let
                // it point straight down rather than into the floor it sits on.
                entity.Transform.Position.z += 3.0f;
                PointLightComponent& spotLight = entity.AddPointLightComponent();
                spotLight.Type = LightType::Spot;
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mRectLightPrototypeSelected)
            {
                Entity entity;
                entity.Name = "RectLight_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                entity.Transform.Position.z += 3.0f;
                PointLightComponent& rectLight = entity.AddPointLightComponent();
                rectLight.Type = LightType::Rect;
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mAudioEmitterPrototypeSelected)
            {
                Entity entity;
                entity.Name = "AudioEmitter_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                entity.AddAudioEmitterComponent();
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mDecalPrototypeSelected)
            {
                Entity entity;
                entity.Name = "Decal_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                entity.AddDecalComponent();
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mRainPrototypeSelected)
            {
                Entity entity;
                entity.Name = "Rain_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                entity.AddRainComponent();
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mParticleSystemPrototypeSelected)
            {
                Entity entity;
                entity.Name = "ParticleSystem_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                entity.AddParticleSystemComponent();
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mWaterPrototypeSelected)
            {
                Entity entity;
                entity.Name = "Water_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;
                entity.AddWaterComponent();
                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
            else if (mVegetationPrototypeSelected)
            {
                Entity entity;
                entity.Name = "Vegetation_" + std::to_string(static_cast<int>(mEntities.size()) + 1);
                entity.Transform.Position = worldPosition;

                VegetationAreaComponent& area = entity.AddVegetationAreaComponent();
                // Start with one empty layer so the inspector has something to
                // show; an area with no layers looks broken rather than new.
                area.Layers.emplace_back();
                // Seed from the placement position so two areas dropped in the
                // same session do not produce identical arrangements.
                area.Seed = static_cast<std::uint32_t>(
                    std::hash<float>{}(worldPosition.x) ^ (std::hash<float>{}(worldPosition.y) << 1));

                mEntities.push_back(std::move(entity));
                mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                mSelectedEntityIndices = { mSelectedEntityIndex };
            }
        }
        return;
    }

    const bool isSelectMode = (mActiveGizmo == GizmoType::None);
    UiVec2 selectionMin((std::min)(mViewportSelection.Start.x, mViewportSelection.Current.x), (std::min)(mViewportSelection.Start.y, mViewportSelection.Current.y));
    UiVec2 selectionMax((std::max)(mViewportSelection.Start.x, mViewportSelection.Current.x), (std::max)(mViewportSelection.Start.y, mViewportSelection.Current.y));
    const float selectionWidth = selectionMax.x - selectionMin.x;
    const float selectionHeight = selectionMax.y - selectionMin.y;

    if (isSelectMode && (selectionWidth > 4.0f || selectionHeight > 4.0f))
    {
        mSelectedEntityIndices.clear();
        for (int i = 0; i < static_cast<int>(mEntities.size()); ++i)
        {
            UiVec2 screenPos;
            if (!TryProjectWorldToViewport(mEntities[i].Transform.Position, viewportOrigin, viewportSize, camera, screenPos))
                continue;

            if (screenPos.x >= selectionMin.x && screenPos.x <= selectionMax.x &&
                screenPos.y >= selectionMin.y && screenPos.y <= selectionMax.y)
            {
                mSelectedEntityIndices.push_back(i);
            }
        }

        mSelectedEntityIndex = mSelectedEntityIndices.empty() ? -1 : mSelectedEntityIndices.front();
        return;
    }

    UiVec2 clickPos = mViewportSelection.Current;
    float  bestDist = 20.0f;
    int    bestIdx  = -1;

    for (int i = 0; i < static_cast<int>(mEntities.size()); ++i)
    {
        UiVec2 screenPos;
        if (TryProjectWorldToViewport(mEntities[i].Transform.Position, viewportOrigin, viewportSize, camera, screenPos))
        {
            float dx = screenPos.x - clickPos.x;
            float dy = screenPos.y - clickPos.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < bestDist)
            {
                bestDist = dist;
                bestIdx  = i;
            }
        }
    }

    mSelectedEntityIndex = bestIdx;
    mSelectedEntityIndices.clear();
    if (bestIdx >= 0)
        mSelectedEntityIndices.push_back(bestIdx);
}

// ---------------------------------------------------------------------------
// Gizmo sizing
//
// The gizmo must keep a roughly constant size on screen however far away the
// entity is. It used to do that per axis, by projecting one world unit along
// the axis, measuring the pixels that covered, and extrapolating to the length
// that would cover 72 of them.
//
// Perspective is not linear, so that extrapolation is only valid when the
// measured unit is both small on screen and perpendicular to the view. Point
// the camera down an axis and the projected unit shrinks towards nothing; the
// extrapolation then divides by it and asks for an axis thousands of units
// long, which projects somewhere absurd and gets clamped to the viewport edge.
// That is the gizmo exploding into lines across the screen, and because the
// rotate rings take their radii from the same lengths, the rings explode with
// it.
//
// One length, derived from the depth the perspective divide actually uses,
// replaces all three. It cannot blow up, and axes that point away from the
// camera now foreshorten instead of compensating - which is what they should
// do, and what makes the gizmo readable end-on.
// ---------------------------------------------------------------------------

float Editor::ComputeGizmoWorldScale(
    const DirectX::XMFLOAT3& pivotWorldPosition,
    const UiVec2& viewportSize,
    const EditorCamera& camera) const
{
    using namespace DirectX;

    if (viewportSize.y <= 1.0f)
        return 0.0f;

    // View-space Z, not the euclidean distance: the divide that turns clip
    // space into pixels uses the former, so it alone sets the scale.
    const XMVECTOR viewPosition =
        XMVector3TransformCoord(XMLoadFloat3(&pivotWorldPosition), camera.GetViewMatrix());
    const float viewDepth = XMVectorGetZ(viewPosition);
    if (viewDepth <= camera.GetNearPlane())
        return 0.0f;

    const float worldUnitsPerPixel =
        2.0f * viewDepth * std::tan(camera.GetFovYRadians() * 0.5f) / viewportSize.y;
    return kGizmoScreenLength * worldUnitsPerPixel;
}

void Editor::HandleManualGizmoInteraction(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera,
    bool viewportHovered,
    Entity* selectedEntity)
{
    if (!selectedEntity || !viewportHovered || mActiveGizmo == GizmoType::None)
    {
        mManualGizmo.HoveredHandle = ManualGizmoHandle::None;
        if (!QtUi::IsMouseDown(QtUiMouseButton_Left))
        {
            mManualGizmo.IsActive = false;
            mManualGizmo.ActiveHandle = ManualGizmoHandle::None;
        }
        return;
    }

    QtUiIO& io = QtUi::GetIO();
    using namespace DirectX;

    const XMFLOAT3 origin = selectedEntity->Transform.Position;

    UiVec2 pivotScreen;
    UiVec2 xScreen;
    UiVec2 yScreen;
    UiVec2 zScreen;
    if (!TryProjectWorldToViewport(origin, viewportOrigin, viewportSize, camera, pivotScreen, true))
    {
        mManualGizmo.HoveredHandle = ManualGizmoHandle::None;
        return;
    }

    // One length for every axis: see ComputeGizmoWorldScale. The hit tests and
    // the drawing both call it, so what is clicked is always what is drawn.
    const float axisWorldLength = ComputeGizmoWorldScale(origin, viewportSize, camera);
    if (axisWorldLength <= 0.0f)
    {
        mManualGizmo.HoveredHandle = ManualGizmoHandle::None;
        return;
    }

    auto projectAxisEnd = [&](const XMFLOAT3& axisWorldDirection, UiVec2& axisScreenEnd) -> bool
    {
        return TryProjectWorldToViewport(
            XMFLOAT3(
                origin.x + axisWorldDirection.x * axisWorldLength,
                origin.y + axisWorldDirection.y * axisWorldLength,
                origin.z + axisWorldDirection.z * axisWorldLength),
            viewportOrigin,
            viewportSize,
            camera,
            axisScreenEnd,
            true);
    };

    const float xAxisWorldLength = axisWorldLength;
    const float yAxisWorldLength = axisWorldLength;
    const float zAxisWorldLength = axisWorldLength;
    const bool hasXAxis = projectAxisEnd(XMFLOAT3(1.0f, 0.0f, 0.0f), xScreen);
    const bool hasYAxis = projectAxisEnd(XMFLOAT3(0.0f, 0.0f, 1.0f), yScreen);
    const bool hasZAxis = projectAxisEnd(XMFLOAT3(0.0f, 1.0f, 0.0f), zScreen);

    auto distanceToSegment = [](const UiVec2& point, const UiVec2& segmentStart, const UiVec2& segmentEnd) -> float
    {
        const UiVec2 segment(segmentEnd.x - segmentStart.x, segmentEnd.y - segmentStart.y);
        const UiVec2 toPoint(point.x - segmentStart.x, point.y - segmentStart.y);
        const float segmentLengthSquared = segment.x * segment.x + segment.y * segment.y;
        if (segmentLengthSquared <= 0.0001f)
            return FLT_MAX;

        const float t = (std::max)(0.0f, (std::min)(1.0f, (toPoint.x * segment.x + toPoint.y * segment.y) / segmentLengthSquared));
        const UiVec2 closestPoint(segmentStart.x + segment.x * t, segmentStart.y + segment.y * t);
        const float dx = point.x - closestPoint.x;
        const float dy = point.y - closestPoint.y;
        return std::sqrt(dx * dx + dy * dy);
    };

    auto distanceToProjectedRing = [&](const XMFLOAT3& axisA, float axisALength, const XMFLOAT3& axisB, float axisBLength) -> float
    {
        constexpr int segmentCount = 48;
        float bestDistance = FLT_MAX;
        UiVec2 previousPoint{};
        bool hasPreviousPoint = false;

        for (int segmentIndex = 0; segmentIndex <= segmentCount; ++segmentIndex)
        {
            const float angle = (XM_2PI * static_cast<float>(segmentIndex)) / static_cast<float>(segmentCount);
            const XMFLOAT3 ringPoint(
                origin.x + axisA.x * std::cos(angle) * axisALength + axisB.x * std::sin(angle) * axisBLength,
                origin.y + axisA.y * std::cos(angle) * axisALength + axisB.y * std::sin(angle) * axisBLength,
                origin.z + axisA.z * std::cos(angle) * axisALength + axisB.z * std::sin(angle) * axisBLength);

            UiVec2 projectedPoint;
            if (TryProjectWorldToViewport(ringPoint, viewportOrigin, viewportSize, camera, projectedPoint))
            {
                if (hasPreviousPoint)
                    bestDistance = (std::min)(bestDistance, distanceToSegment(io.MousePos, previousPoint, projectedPoint));

                previousPoint = projectedPoint;
                hasPreviousPoint = true;
            }
            else
            {
                hasPreviousPoint = false;
            }
        }

        return bestDistance;
    };

    auto beginAxisDrag = [&](ManualGizmoHandle handle, const XMFLOAT3& axisWorldDirection, const UiVec2& axisScreenEnd, float axisWorldLength)
    {
        const UiVec2 axisScreenDirection(axisScreenEnd.x - pivotScreen.x, axisScreenEnd.y - pivotScreen.y);
        const float axisScreenLength = std::sqrt(axisScreenDirection.x * axisScreenDirection.x + axisScreenDirection.y * axisScreenDirection.y);
        if (axisScreenLength <= 0.0001f)
            return;

        mManualGizmo.IsActive = true;
        mManualGizmo.ActiveHandle = handle;
        mManualGizmo.StartMouse = io.MousePos;
        mManualGizmo.StartPosition = selectedEntity->Transform.Position;
        mManualGizmo.StartRotation = selectedEntity->Transform.Rotation;
        mManualGizmo.StartScale = selectedEntity->Transform.Scale;
        mManualGizmo.AxisWorldDirection = axisWorldDirection;
        mManualGizmo.SecondaryAxisWorldDirection = XMFLOAT3(0.0f, 0.0f, 0.0f);
        mManualGizmo.AxisScreenDirection = UiVec2(axisScreenDirection.x / axisScreenLength, axisScreenDirection.y / axisScreenLength);
        mManualGizmo.SecondaryAxisScreenDirection = UiVec2(0.0f, 0.0f);
        mManualGizmo.PixelsPerWorldUnit = axisScreenLength / axisWorldLength;
        mManualGizmo.SecondaryPixelsPerWorldUnit = 1.0f;
        mManualGizmo.StartAngle = std::atan2(io.MousePos.y - pivotScreen.y, io.MousePos.x - pivotScreen.x);
        mBlockViewportSelection = true;
    };

    auto beginPlaneDrag = [&](ManualGizmoHandle handle,
                              const XMFLOAT3& primaryAxisWorldDirection,
                              const UiVec2& primaryAxisScreenEnd,
                              float primaryAxisWorldLength,
                              const XMFLOAT3& secondaryAxisWorldDirection,
                              const UiVec2& secondaryAxisScreenEnd,
                              float secondaryAxisWorldLength)
    {
        const UiVec2 primaryAxisScreenDirection(primaryAxisScreenEnd.x - pivotScreen.x, primaryAxisScreenEnd.y - pivotScreen.y);
        const UiVec2 secondaryAxisScreenDirection(secondaryAxisScreenEnd.x - pivotScreen.x, secondaryAxisScreenEnd.y - pivotScreen.y);
        const float primaryAxisScreenLength = std::sqrt(primaryAxisScreenDirection.x * primaryAxisScreenDirection.x + primaryAxisScreenDirection.y * primaryAxisScreenDirection.y);
        const float secondaryAxisScreenLength = std::sqrt(secondaryAxisScreenDirection.x * secondaryAxisScreenDirection.x + secondaryAxisScreenDirection.y * secondaryAxisScreenDirection.y);
        if (primaryAxisScreenLength <= 0.0001f || secondaryAxisScreenLength <= 0.0001f)
            return;

        mManualGizmo.IsActive = true;
        mManualGizmo.ActiveHandle = handle;
        mManualGizmo.StartMouse = io.MousePos;
        mManualGizmo.StartPosition = selectedEntity->Transform.Position;
        mManualGizmo.StartRotation = selectedEntity->Transform.Rotation;
        mManualGizmo.StartScale = selectedEntity->Transform.Scale;
        mManualGizmo.AxisWorldDirection = primaryAxisWorldDirection;
        mManualGizmo.SecondaryAxisWorldDirection = secondaryAxisWorldDirection;
        mManualGizmo.AxisScreenDirection = UiVec2(primaryAxisScreenDirection.x / primaryAxisScreenLength, primaryAxisScreenDirection.y / primaryAxisScreenLength);
        mManualGizmo.SecondaryAxisScreenDirection = UiVec2(secondaryAxisScreenDirection.x / secondaryAxisScreenLength, secondaryAxisScreenDirection.y / secondaryAxisScreenLength);
        mManualGizmo.PixelsPerWorldUnit = primaryAxisScreenLength / primaryAxisWorldLength;
        mManualGizmo.SecondaryPixelsPerWorldUnit = secondaryAxisScreenLength / secondaryAxisWorldLength;
        mManualGizmo.StartAngle = std::atan2(io.MousePos.y - pivotScreen.y, io.MousePos.x - pivotScreen.x);
        mBlockViewportSelection = true;
    };

    auto pointInTriangle = [](const UiVec2& point, const UiVec2& a, const UiVec2& b, const UiVec2& c) -> bool
    {
        const auto sign = [](const UiVec2& p1, const UiVec2& p2, const UiVec2& p3)
        {
            return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
        };

        const float d1 = sign(point, a, b);
        const float d2 = sign(point, b, c);
        const float d3 = sign(point, c, a);
        const bool hasNegative = (d1 < 0.0f) || (d2 < 0.0f) || (d3 < 0.0f);
        const bool hasPositive = (d1 > 0.0f) || (d2 > 0.0f) || (d3 > 0.0f);
        return !(hasNegative && hasPositive);
    };

    // The handle under the cursor is worked out every frame, not only when the
    // mouse goes down: the drawing highlights it, so you can see what you are
    // about to grab before you grab it. Running one chain for both means the
    // highlight can never point at a different handle than the click takes.
    if (!mManualGizmo.IsActive)
    {
        const bool clicked = QtUi::IsMouseClicked(QtUiMouseButton_Left);
        mManualGizmo.HoveredHandle = ManualGizmoHandle::None;
        const float axisHitThreshold = 10.0f;
        if ((mActiveGizmo == GizmoType::Translate || mActiveGizmo == GizmoType::Scale) && hasXAxis && hasYAxis)
        {
            const UiVec2 xyCorner(pivotScreen.x + (xScreen.x - pivotScreen.x) * 0.35f + (yScreen.x - pivotScreen.x) * 0.35f,
                                  pivotScreen.y + (xScreen.y - pivotScreen.y) * 0.35f + (yScreen.y - pivotScreen.y) * 0.35f);
            if (pointInTriangle(io.MousePos, pivotScreen, UiVec2(pivotScreen.x + (xScreen.x - pivotScreen.x) * 0.35f, pivotScreen.y + (xScreen.y - pivotScreen.y) * 0.35f), xyCorner))
            {
                const ManualGizmoHandle handle = mActiveGizmo == GizmoType::Translate ? ManualGizmoHandle::TranslateXYPlane : ManualGizmoHandle::Scale;
                mManualGizmo.HoveredHandle = handle;
                if (clicked)
                {
                    beginPlaneDrag(handle,
                               XMFLOAT3(1.0f, 0.0f, 0.0f), xScreen, xAxisWorldLength,
                               XMFLOAT3(0.0f, 0.0f, 1.0f), yScreen, yAxisWorldLength);
                }
                return;
            }
        }

        if ((mActiveGizmo == GizmoType::Translate || mActiveGizmo == GizmoType::Scale) && hasXAxis && hasZAxis)
        {
            const UiVec2 xzCorner(pivotScreen.x + (xScreen.x - pivotScreen.x) * 0.35f + (zScreen.x - pivotScreen.x) * 0.35f,
                                  pivotScreen.y + (xScreen.y - pivotScreen.y) * 0.35f + (zScreen.y - pivotScreen.y) * 0.35f);
            if (pointInTriangle(io.MousePos, pivotScreen, UiVec2(pivotScreen.x + (xScreen.x - pivotScreen.x) * 0.35f, pivotScreen.y + (xScreen.y - pivotScreen.y) * 0.35f), xzCorner))
            {
                const ManualGizmoHandle handle = mActiveGizmo == GizmoType::Translate ? ManualGizmoHandle::TranslateXZPlane : ManualGizmoHandle::Scale;
                mManualGizmo.HoveredHandle = handle;
                if (clicked)
                {
                    beginPlaneDrag(handle,
                               XMFLOAT3(1.0f, 0.0f, 0.0f), xScreen, xAxisWorldLength,
                               XMFLOAT3(0.0f, 1.0f, 0.0f), zScreen, zAxisWorldLength);
                }
                return;
            }
        }

        if ((mActiveGizmo == GizmoType::Translate || mActiveGizmo == GizmoType::Scale) && hasYAxis && hasZAxis)
        {
            const UiVec2 yzCorner(pivotScreen.x + (yScreen.x - pivotScreen.x) * 0.35f + (zScreen.x - pivotScreen.x) * 0.35f,
                                  pivotScreen.y + (yScreen.y - pivotScreen.y) * 0.35f + (zScreen.y - pivotScreen.y) * 0.35f);
            if (pointInTriangle(io.MousePos, pivotScreen, UiVec2(pivotScreen.x + (yScreen.x - pivotScreen.x) * 0.35f, pivotScreen.y + (yScreen.y - pivotScreen.y) * 0.35f), yzCorner))
            {
                const ManualGizmoHandle handle = mActiveGizmo == GizmoType::Translate ? ManualGizmoHandle::TranslateYZPlane : ManualGizmoHandle::Scale;
                mManualGizmo.HoveredHandle = handle;
                if (clicked)
                {
                    beginPlaneDrag(handle,
                               XMFLOAT3(0.0f, 0.0f, 1.0f), yScreen, yAxisWorldLength,
                               XMFLOAT3(0.0f, 1.0f, 0.0f), zScreen, zAxisWorldLength);
                }
                return;
            }
        }

        if ((mActiveGizmo == GizmoType::Translate || mActiveGizmo == GizmoType::Scale) && hasXAxis && distanceToSegment(io.MousePos, pivotScreen, xScreen) <= axisHitThreshold)
        {
            const ManualGizmoHandle handle = mActiveGizmo == GizmoType::Translate ? ManualGizmoHandle::TranslateXAxis : ManualGizmoHandle::ScaleXAxis;
            mManualGizmo.HoveredHandle = handle;
            if (clicked)
                beginAxisDrag(handle, XMFLOAT3(1.0f, 0.0f, 0.0f), xScreen, xAxisWorldLength);
            return;
        }

        if ((mActiveGizmo == GizmoType::Translate || mActiveGizmo == GizmoType::Scale) && hasYAxis && distanceToSegment(io.MousePos, pivotScreen, yScreen) <= axisHitThreshold)
        {
            const ManualGizmoHandle handle = mActiveGizmo == GizmoType::Translate ? ManualGizmoHandle::TranslateYAxis : ManualGizmoHandle::ScaleYAxis;
            mManualGizmo.HoveredHandle = handle;
            if (clicked)
                beginAxisDrag(handle, XMFLOAT3(0.0f, 0.0f, 1.0f), yScreen, yAxisWorldLength);
            return;
        }

        if ((mActiveGizmo == GizmoType::Translate || mActiveGizmo == GizmoType::Scale) && hasZAxis && distanceToSegment(io.MousePos, pivotScreen, zScreen) <= axisHitThreshold)
        {
            const ManualGizmoHandle handle = mActiveGizmo == GizmoType::Translate ? ManualGizmoHandle::TranslateZAxis : ManualGizmoHandle::ScaleZAxis;
            mManualGizmo.HoveredHandle = handle;
            if (clicked)
                beginAxisDrag(handle, XMFLOAT3(0.0f, 1.0f, 0.0f), zScreen, zAxisWorldLength);
            return;
        }

        if (mActiveGizmo == GizmoType::Rotate)
        {
            const float rotateHitThreshold = 10.0f;
            const float redRingDistance = distanceToProjectedRing(XMFLOAT3(0.0f, 0.0f, 1.0f), yAxisWorldLength, XMFLOAT3(0.0f, 1.0f, 0.0f), zAxisWorldLength);
            const float greenRingDistance = distanceToProjectedRing(XMFLOAT3(1.0f, 0.0f, 0.0f), xAxisWorldLength, XMFLOAT3(0.0f, 0.0f, 1.0f), yAxisWorldLength);
            const float blueRingDistance = distanceToProjectedRing(XMFLOAT3(1.0f, 0.0f, 0.0f), xAxisWorldLength, XMFLOAT3(0.0f, 1.0f, 0.0f), zAxisWorldLength);

            float bestRingDistance = redRingDistance;
            XMFLOAT3 rotationAxis(1.0f, 0.0f, 0.0f);
            if (greenRingDistance < bestRingDistance)
            {
                bestRingDistance = greenRingDistance;
                rotationAxis = XMFLOAT3(0.0f, 0.0f, 1.0f);
            }
            if (blueRingDistance < bestRingDistance)
            {
                bestRingDistance = blueRingDistance;
                rotationAxis = XMFLOAT3(0.0f, 1.0f, 0.0f);
            }

            if (bestRingDistance <= rotateHitThreshold)
            {
                mManualGizmo.HoveredHandle = ManualGizmoHandle::Rotate;
                // Which ring the cursor is nearest is also which ring the
                // drawing lights up, so the pick is stored either way.
                mManualGizmo.HoveredRotationAxis = rotationAxis;
                if (!clicked)
                    return;

                mManualGizmo.IsActive = true;
                mManualGizmo.ActiveHandle = ManualGizmoHandle::Rotate;
                mManualGizmo.StartMouse = io.MousePos;
                mManualGizmo.StartPosition = selectedEntity->Transform.Position;
                mManualGizmo.StartRotation = selectedEntity->Transform.Rotation;
                mManualGizmo.StartScale = selectedEntity->Transform.Scale;
                mManualGizmo.AxisWorldDirection = rotationAxis;
                mManualGizmo.StartAngle = std::atan2(io.MousePos.y - pivotScreen.y, io.MousePos.x - pivotScreen.x);
                mBlockViewportSelection = true;
                return;
            }
        }
    }

    if (!mManualGizmo.IsActive)
        return;

    if (!QtUi::IsMouseDown(QtUiMouseButton_Left))
    {
        mManualGizmo.IsActive = false;
        mManualGizmo.ActiveHandle = ManualGizmoHandle::None;
        mBlockViewportSelection = false;
        return;
    }

    const UiVec2 mouseDelta(io.MousePos.x - mManualGizmo.StartMouse.x, io.MousePos.y - mManualGizmo.StartMouse.y);
    const float projectedPixelDelta = mouseDelta.x * mManualGizmo.AxisScreenDirection.x + mouseDelta.y * mManualGizmo.AxisScreenDirection.y;

    switch (mManualGizmo.ActiveHandle)
    {
    case ManualGizmoHandle::TranslateXAxis:
    case ManualGizmoHandle::TranslateYAxis:
    case ManualGizmoHandle::TranslateZAxis:
    case ManualGizmoHandle::TranslateXYPlane:
    case ManualGizmoHandle::TranslateXZPlane:
    case ManualGizmoHandle::TranslateYZPlane:
    {
        float worldDelta = projectedPixelDelta / (std::max)(mManualGizmo.PixelsPerWorldUnit, 0.0001f);
        const float secondaryProjectedPixelDelta = mouseDelta.x * mManualGizmo.SecondaryAxisScreenDirection.x + mouseDelta.y * mManualGizmo.SecondaryAxisScreenDirection.y;
        float secondaryWorldDelta = secondaryProjectedPixelDelta / (std::max)(mManualGizmo.SecondaryPixelsPerWorldUnit, 0.0001f);
        if (mMovementSnapEnabled)
        {
            worldDelta = SnapToStep(worldDelta, mMovementSnapStep);
            secondaryWorldDelta = SnapToStep(secondaryWorldDelta, mMovementSnapStep);
        }

        const XMFLOAT3 snappedPosition(
            mManualGizmo.StartPosition.x + mManualGizmo.AxisWorldDirection.x * worldDelta + mManualGizmo.SecondaryAxisWorldDirection.x * secondaryWorldDelta,
            mManualGizmo.StartPosition.y + mManualGizmo.AxisWorldDirection.y * worldDelta + mManualGizmo.SecondaryAxisWorldDirection.y * secondaryWorldDelta,
            mManualGizmo.StartPosition.z + mManualGizmo.AxisWorldDirection.z * worldDelta + mManualGizmo.SecondaryAxisWorldDirection.z * secondaryWorldDelta);
        if (!NearlyEqual(selectedEntity->Transform.Position, snappedPosition))
        {
            selectedEntity->Transform.Position = snappedPosition;
            MarkSceneChanged();
        }
        break;
    }
    case ManualGizmoHandle::ScaleXAxis:
    case ManualGizmoHandle::ScaleYAxis:
    case ManualGizmoHandle::ScaleZAxis:
    {
        const float scaleDelta = projectedPixelDelta * 0.01f;
        selectedEntity->Transform.Scale = mManualGizmo.StartScale;
        if (mManualGizmo.ActiveHandle == ManualGizmoHandle::Scale)
        {
            const float secondaryProjectedPixelDelta = mouseDelta.x * mManualGizmo.SecondaryAxisScreenDirection.x + mouseDelta.y * mManualGizmo.SecondaryAxisScreenDirection.y;
            const float secondaryScaleDelta = secondaryProjectedPixelDelta * 0.01f;
            selectedEntity->Transform.Scale.x = (std::max)(0.01f, mManualGizmo.StartScale.x + (std::abs(mManualGizmo.AxisWorldDirection.x) > 0.5f ? scaleDelta : 0.0f) + (std::abs(mManualGizmo.SecondaryAxisWorldDirection.x) > 0.5f ? secondaryScaleDelta : 0.0f));
            selectedEntity->Transform.Scale.y = (std::max)(0.01f, mManualGizmo.StartScale.y + (std::abs(mManualGizmo.AxisWorldDirection.y) > 0.5f ? scaleDelta : 0.0f) + (std::abs(mManualGizmo.SecondaryAxisWorldDirection.y) > 0.5f ? secondaryScaleDelta : 0.0f));
            selectedEntity->Transform.Scale.z = (std::max)(0.01f, mManualGizmo.StartScale.z + (std::abs(mManualGizmo.AxisWorldDirection.z) > 0.5f ? scaleDelta : 0.0f) + (std::abs(mManualGizmo.SecondaryAxisWorldDirection.z) > 0.5f ? secondaryScaleDelta : 0.0f));
        }
        else if (mManualGizmo.ActiveHandle == ManualGizmoHandle::ScaleXAxis)
            selectedEntity->Transform.Scale.x = (std::max)(0.01f, mManualGizmo.StartScale.x + scaleDelta);
        else if (mManualGizmo.ActiveHandle == ManualGizmoHandle::ScaleYAxis)
            selectedEntity->Transform.Scale.y = (std::max)(0.01f, mManualGizmo.StartScale.y + scaleDelta);
        else if (mManualGizmo.ActiveHandle == ManualGizmoHandle::ScaleZAxis)
            selectedEntity->Transform.Scale.z = (std::max)(0.01f, mManualGizmo.StartScale.z + scaleDelta);
        break;
    }
    case ManualGizmoHandle::Rotate:
    {
        const float currentAngle = std::atan2(io.MousePos.y - pivotScreen.y, io.MousePos.x - pivotScreen.x);
        selectedEntity->Transform.Rotation = mManualGizmo.StartRotation;
        float angleDelta = currentAngle - mManualGizmo.StartAngle;
        while (angleDelta > DirectX::XM_PI)
            angleDelta -= DirectX::XM_2PI;
        while (angleDelta < -DirectX::XM_PI)
            angleDelta += DirectX::XM_2PI;
        if (mRotationSnapEnabled)
            angleDelta = SnapToStep(angleDelta, DegreesToRadians(mRotationSnapDegrees));
        if (std::abs(mManualGizmo.AxisWorldDirection.x) > 0.5f)
            selectedEntity->Transform.Rotation.x += angleDelta;
        else if (std::abs(mManualGizmo.AxisWorldDirection.y) > 0.5f)
            selectedEntity->Transform.Rotation.z += angleDelta;
        else
            selectedEntity->Transform.Rotation.y += angleDelta;
        if (!NearlyEqual(selectedEntity->Transform.Rotation, mManualGizmo.StartRotation))
            MarkSceneChanged();
        break;
    }
    default:
        break;
    }
}

void Editor::CreateGeometryInstanceAt(const DirectX::XMFLOAT3& worldPosition)
{
    Entity e;
    e.Name = "Geometry_" + std::to_string(mNextGeometryInstanceId++);
    e.Transform.Position = worldPosition;
    e.Transform.Position.z = 0.0f;
    e.AddMeshComponent();
    mEntities.push_back(std::move(e));
    mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
    mSelectedEntityIndices = { mSelectedEntityIndex };
    MarkSceneChanged();
}

// ---------------------------------------------------------------------------
// Statistics overlay
// ---------------------------------------------------------------------------

void Editor::SetViewportStatisticsText(const char* text)
{
    mViewportStatisticsText = text;
}

void Editor::HandleKeyboardShortcuts()
{
    // Playing inside the viewport, the keyboard belongs to the game - 1 through 6 pick
    // dice, R rolls, W and S walk. Leaving the editor's unmodified shortcuts live would
    // have every one of those also change the gizmo or delete the selected entity. The
    // Play button stays on screen, so there is always a way back out.
    //
    // F11 still switches fullscreen, but the game asks for it through the game API
    // rather than the editor reading the key, so that the request works the same way
    // when the game's own pause menu is what raised it.
    if (QtUi::IsGamePlaying())
    {
        if (QtUi::ConsumeGameFullscreenRequest())
            mViewportFullscreen = !mViewportFullscreen;
        return;
    }

    if (QtUi::GetIO().WantTextInput)
        return;

    QtUiIO& io = QtUi::GetIO();
    HWND windowHandle = DX12Context_GetWindowHandle();

    if (io.KeyCtrl && QtUi::IsKeyPressed(QtUiKey_N, false))
    {
        NewScene(windowHandle);
        return;
    }

    if (io.KeyCtrl && QtUi::IsKeyPressed(QtUiKey_O, false))
    {
        OpenScene(windowHandle);
        return;
    }

    if (io.KeyCtrl && QtUi::IsKeyPressed(QtUiKey_S, false))
    {
        SaveScene(windowHandle);
        return;
    }

    // Undo and redo, before the unmodified single-key shortcuts so a stray Ctrl
    // cannot fall through to a gizmo change.
    //
    // Redo answers to both Ctrl+Y and Ctrl+Shift+Z. Windows editors are split
    // between the two and neither is wrong, so binding both costs nothing and
    // saves the user finding out which one this engine picked.
    if (io.KeyCtrl && !io.KeyShift && QtUi::IsKeyPressed(QtUiKey_Z, false))
    {
        Undo();
        return;
    }

    if (io.KeyCtrl && (QtUi::IsKeyPressed(QtUiKey_Y, false) ||
                       (io.KeyShift && QtUi::IsKeyPressed(QtUiKey_Z, false))))
    {
        Redo();
        return;
    }

    if (QtUi::IsKeyPressed(QtUiKey_1, false))
        mActiveGizmo = GizmoType::None;

    if (QtUi::IsKeyPressed(QtUiKey_2, false))
        mActiveGizmo = GizmoType::Translate;

    if (QtUi::IsKeyPressed(QtUiKey_3, false))
        mActiveGizmo = GizmoType::Rotate;

    if (QtUi::IsKeyPressed(QtUiKey_4, false))
        mActiveGizmo = GizmoType::Scale;

    // Unmodified, and not swallowed by the Ctrl shortcuts above, so it works
    // whichever panel has focus.
    if (QtUi::IsKeyPressed(QtUiKey_F11, false))
        mViewportFullscreen = !mViewportFullscreen;

    if (QtUi::IsKeyPressed(QtUiKey_Delete, false) && CanDeleteSelectedEntity())
        DeleteSelectedEntity();

    if (io.KeyCtrl && QtUi::IsKeyPressed(QtUiKey_C, false) && CanCopySelectedEntity())
        CopySelectedEntity();

    if (io.KeyCtrl && QtUi::IsKeyPressed(QtUiKey_V, false) && CanPasteEntity())
        PasteCopiedEntity();
}

void Editor::DrawViewportStatisticsOverlay() const
{
    if (!mShowViewportStatistics || !mViewportStatisticsText) return;
    if (mLastViewportContentSize.x <= 1.0f || mLastViewportContentSize.y <= 1.0f) return;

    const QtUiWindowFlags overlayFlags =
        QtUiWindowFlags_NoDecoration |
        QtUiWindowFlags_NoNav | QtUiWindowFlags_NoMove | QtUiWindowFlags_NoSavedSettings |
        QtUiWindowFlags_NoDocking |
        QtUiWindowFlags_NoFocusOnAppearing | QtUiWindowFlags_NoMouseInputs;

    QtUi::SetNextWindowBgAlpha(0.45f);
    QtUi::SetNextWindowPos(UiVec2(mLastViewportContentOrigin.x + 10.0f,
                                   mLastViewportContentOrigin.y + 10.0f),
                             QtUiCond_Always);
    QtUi::PushStyleVar(QtUiStyleVar_WindowPadding, UiVec2(6.0f, 4.0f));
    if (QtUi::Begin("##stats_overlay", nullptr, overlayFlags))
        QtUi::TextUnformatted(mViewportStatisticsText);
    QtUi::End();
    QtUi::PopStyleVar();
}

// ---------------------------------------------------------------------------
// Scene loading overlay
// ---------------------------------------------------------------------------

void Editor::DrawSceneLoadingOverlay()
{
    if (!IsSceneLoading()) return;

    UiVec2 overlayCenter;
    if (mLastViewportContentSize.x > 1.0f && mLastViewportContentSize.y > 1.0f)
    {
        overlayCenter = UiVec2(
            mLastViewportContentOrigin.x + mLastViewportContentSize.x * 0.5f,
            mLastViewportContentOrigin.y + mLastViewportContentSize.y * 0.5f);
    }
    else
    {
        QtUiIO& io = QtUi::GetIO();
        overlayCenter = UiVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    }

    QtUi::SetNextWindowPos(overlayCenter, QtUiCond_Always, UiVec2(0.5f, 0.5f));
    QtUi::SetNextWindowSize(UiVec2(360.0f, 80.0f), QtUiCond_Always);
    QtUi::SetNextWindowBgAlpha(0.85f);
    const QtUiWindowFlags flags =
        QtUiWindowFlags_NoDecoration | QtUiWindowFlags_NoNav |
        QtUiWindowFlags_NoMove | QtUiWindowFlags_NoSavedSettings |
        QtUiWindowFlags_NoDocking;

    if (QtUi::Begin("##loading_overlay", nullptr, flags))
    {
        QtUi::Text("Loading scene...");
        QtUi::ProgressBar(GetSceneLoadProgress(), UiVec2(-1.0f, 0.0f));
        QtUi::TextDisabled("%s", GetSceneLoadStatusMessage().c_str());
    }
    QtUi::End();
}

void Editor::DrawResourceDebugWindow()
{
    QtUi::SetNextWindowSize(UiVec2(780.0f, 560.0f), QtUiCond_FirstUseEver);
    if (!QtUi::Begin("Resource Debug", &mShowResourceDebugPanel))
    {
        QtUi::End();
        return;
    }

    QtUi::TextDisabled(
        "Total CPU: %.1f%%  |  Total GPU: %.1f%%  |  Total RAM: %.1f%%",
        mResourceUsageSnapshot.TotalCpuUsagePercent,
        mResourceUsageSnapshot.TotalGpuUsagePercent,
        mResourceUsageSnapshot.TotalRamUsagePercent);
    QtUi::Separator();

    if (mResourceUsageSnapshot.Entries.empty() && mRendererTimingSnapshot.Entries.empty())
    {
        QtUi::TextDisabled("No resource usage or renderer timing data available.");
        QtUi::End();
        return;
    }

    if (!mRendererTimingSnapshot.Entries.empty())
    {
        QtUi::Text("Renderer Elements");
        QtUi::TextDisabled("CPU command-recording time this frame: %.3f ms", mRendererTimingSnapshot.TotalCpuMilliseconds);

        if (QtUi::BeginTable("RendererTimingTable", 4, QtUiTableFlags_Borders | QtUiTableFlags_RowBg | QtUiTableFlags_SizingStretchProp))
        {
            QtUi::TableSetupColumn("Category");
            QtUi::TableSetupColumn("Render Element");
            QtUi::TableSetupColumn("CPU ms", QtUiTableColumnFlags_WidthFixed, 82.0f);
            QtUi::TableSetupColumn("Frame %", QtUiTableColumnFlags_WidthFixed, 82.0f);
            QtUi::TableHeadersRow();

            for (const RendererTimingEntry& entry : mRendererTimingSnapshot.Entries)
            {
                const float framePercent = mRendererTimingSnapshot.TotalCpuMilliseconds > 0.0f
                    ? (entry.CpuMilliseconds / mRendererTimingSnapshot.TotalCpuMilliseconds) * 100.0f
                    : 0.0f;

                QtUi::TableNextRow();
                QtUi::TableSetColumnIndex(0);
                QtUi::TextUnformatted(entry.Category.c_str());
                QtUi::TableSetColumnIndex(1);
                QtUi::TextUnformatted(entry.Name.c_str());
                QtUi::TableSetColumnIndex(2);
                QtUi::Text("%.3f", entry.CpuMilliseconds);
                QtUi::TableSetColumnIndex(3);
                QtUi::Text("%.1f", framePercent);
            }

            QtUi::EndTable();
        }

        QtUi::Separator();
    }

    if (!mResourceUsageSnapshot.Entries.empty()
        && QtUi::BeginTable("ResourceUsageTable", 4, QtUiTableFlags_Borders | QtUiTableFlags_RowBg | QtUiTableFlags_SizingStretchProp))
    {
        QtUi::TableSetupColumn("Engine Element");
        QtUi::TableSetupColumn("CPU %");
        QtUi::TableSetupColumn("GPU %");
        QtUi::TableSetupColumn("RAM %");
        QtUi::TableHeadersRow();

        for (const EngineResourceUsageEntry& entry : mResourceUsageSnapshot.Entries)
        {
            QtUi::TableNextRow();
            QtUi::TableSetColumnIndex(0);
            QtUi::TextUnformatted(entry.Name.c_str());
            QtUi::TableSetColumnIndex(1);
            QtUi::Text("%.1f", entry.CpuUsagePercent);
            QtUi::TableSetColumnIndex(2);
            QtUi::Text("%.1f", entry.GpuUsagePercent);
            QtUi::TableSetColumnIndex(3);
            QtUi::Text("%.1f", entry.RamUsagePercent);
        }

        QtUi::EndTable();
    }

    QtUi::TextDisabled("Percent values are grouped runtime estimates; renderer element ms values are measured while recording the frame.");
    QtUi::End();
}

// ---------------------------------------------------------------------------
// Viewport placement icons
// ---------------------------------------------------------------------------

void Editor::DrawViewportPlacementIcons(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera)
{
    if (!mShowViewportPlacementIcons)
        return;

    UiDrawList* drawList = QtUi::GetWindowDrawList();
    const float iconSize = 54.0f;

    for (size_t i = 0; i < mEntities.size(); ++i)
    {
        UiVec2 screenPosition;
        if (!TryProjectWorldToViewport(mEntities[i].Transform.Position, viewportOrigin, viewportSize, camera, screenPosition))
            continue;

        const bool isSelected = IsEntitySelected(static_cast<int>(i));
        const UiU32 tintColor = isSelected ? UI_COL32(255, 245, 170, 255) : UI_COL32(255, 255, 255, 230);
        const UiVec2 iconMin = UiVec2(screenPosition.x - iconSize * 0.5f, screenPosition.y - iconSize * 0.5f);
        const UiVec2 iconMax = UiVec2(screenPosition.x + iconSize * 0.5f, screenPosition.y + iconSize * 0.5f);
        D3D12_GPU_DESCRIPTOR_HANDLE iconHandle{};

        if (mEntities[i].PointLight.has_value())
            iconHandle = mPointLightIcon.GpuHandle;
        else if (mEntities[i].AudioEmitter.has_value())
            iconHandle = mAudioEmitterIcon.GpuHandle;
        else if (mEntities[i].Decal.has_value())
            iconHandle = mDecalIcon.GpuHandle;
        else if (mEntities[i].Rain.has_value())
            iconHandle = mRainIcon.GpuHandle;
        else if (mEntities[i].ParticleSystem.has_value())
            iconHandle = mParticleSystemIcon.GpuHandle;
        else
            iconHandle = mGeometryIcon.GpuHandle;

        if (iconHandle.ptr != 0)
        {
            drawList->AddImage(TextureIdFromHandle(iconHandle), iconMin, iconMax, UiVec2(0.0f, 0.0f), UiVec2(1.0f, 1.0f), tintColor);
        }
        else
        {
            drawList->AddRectFilled(iconMin, iconMax, UI_COL32(210, 95, 30, 220), 3.0f);
        }

        if (isSelected)
        {
            drawList->AddRect(iconMin, iconMax, UI_COL32(255, 230, 120, 255), 0.0f, 0, 2.0f);
        }
    }
}

void Editor::DrawTerrainViewportOverlay(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera) const
{
    UiDrawList* drawList = QtUi::GetWindowDrawList();
    constexpr int kGridLineCount = 16;
    constexpr int kBrushSegments = 96;

    auto drawProjectedLine = [&](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, UiU32 color, float thickness)
    {
        UiVec2 sa{};
        UiVec2 sb{};
        if (TryProjectWorldToViewport(a, viewportOrigin, viewportSize, camera, sa, false)
            && TryProjectWorldToViewport(b, viewportOrigin, viewportSize, camera, sb, false))
        {
            drawList->AddLine(sa, sb, color, thickness);
        }
    };

    for (int entityIndex = 0; entityIndex < static_cast<int>(mEntities.size()); ++entityIndex)
    {
        const Entity& entity = mEntities[entityIndex];
        if (!entity.HasTerrainComponent() || !entity.Terrain.has_value())
            continue;

        const TerrainComponent& tc = *entity.Terrain;
        if (tc.WorldSize <= 0.0f)
            continue;

        const bool selected = IsEntitySelected(entityIndex);
        const UiU32 edgeColor = selected
            ? UI_COL32(255, 205, 80, 255)
            : UI_COL32(230, 140, 50, 220);
        const UiU32 gridColor = selected
            ? UI_COL32(255, 205, 80, 125)
            : UI_COL32(230, 140, 50, 85);
        const float edgeThickness = selected ? 2.0f : 1.25f;

        const float halfSize = tc.WorldSize * 0.5f;
        const float baseZ = entity.Transform.Position.z + tc.HeightOffset;
        const float minX = entity.Transform.Position.x - halfSize;
        const float maxX = entity.Transform.Position.x + halfSize;
        const float minY = entity.Transform.Position.y - halfSize;
        const float maxY = entity.Transform.Position.y + halfSize;

        drawProjectedLine({ minX, minY, baseZ }, { maxX, minY, baseZ }, edgeColor, edgeThickness);
        drawProjectedLine({ maxX, minY, baseZ }, { maxX, maxY, baseZ }, edgeColor, edgeThickness);
        drawProjectedLine({ maxX, maxY, baseZ }, { minX, maxY, baseZ }, edgeColor, edgeThickness);
        drawProjectedLine({ minX, maxY, baseZ }, { minX, minY, baseZ }, edgeColor, edgeThickness);

        for (int lineIndex = 1; lineIndex < kGridLineCount; ++lineIndex)
        {
            const float t = static_cast<float>(lineIndex) / static_cast<float>(kGridLineCount);
            const float x = minX + (maxX - minX) * t;
            const float y = minY + (maxY - minY) * t;
            drawProjectedLine({ x, minY, baseZ }, { x, maxY, baseZ }, gridColor, 1.0f);
            drawProjectedLine({ minX, y, baseZ }, { maxX, y, baseZ }, gridColor, 1.0f);
        }
    }

    if (!mTerrainBrushModeActive || !QtUi::IsWindowHovered())
        return;

    DirectX::XMFLOAT3 mouseWorld{};
    if (!TryGetViewportWorldPositionOnGrid(
        QtUi::GetIO().MousePos,
        viewportOrigin,
        viewportSize,
        camera,
        mouseWorld))
    {
        return;
    }

    const TerrainComponent* brushTerrain = nullptr;
    const Entity* brushEntity = nullptr;
    for (const Entity& entity : mEntities)
    {
        if (!entity.HasTerrainComponent() || !entity.Terrain.has_value())
            continue;

        const TerrainComponent& tc = *entity.Terrain;
        const float halfSize = tc.WorldSize * 0.5f;
        if (mouseWorld.x < entity.Transform.Position.x - halfSize
            || mouseWorld.x > entity.Transform.Position.x + halfSize
            || mouseWorld.y < entity.Transform.Position.y - halfSize
            || mouseWorld.y > entity.Transform.Position.y + halfSize)
        {
            continue;
        }

        brushTerrain = &tc;
        brushEntity = &entity;
        break;
    }

    if (brushTerrain == nullptr || brushEntity == nullptr)
        return;

    float previewZ = brushEntity->Transform.Position.z + brushTerrain->HeightOffset;
    if (mTerrainRenderer != nullptr)
    {
        float sampledZ = previewZ;
        if (mTerrainRenderer->SampleHeightAt({ mouseWorld.x, mouseWorld.y }, sampledZ))
            previewZ = sampledZ;
    }

    UiU32 brushColor = UI_COL32(255, 215, 80, 245);
    switch (brushTerrain->Brush)
    {
    case TerrainComponent::BrushType::Lower:
        brushColor = UI_COL32(90, 170, 255, 245);
        break;
    case TerrainComponent::BrushType::Flatten:
        brushColor = UI_COL32(255, 160, 70, 245);
        break;
    case TerrainComponent::BrushType::Smooth:
        brushColor = UI_COL32(120, 235, 165, 245);
        break;
    case TerrainComponent::BrushType::Paint:
        brushColor = UI_COL32(230, 120, 235, 245);
        break;
    case TerrainComponent::BrushType::Raise:
    default:
        break;
    }

    std::vector<UiVec2> brushPoints;
    brushPoints.reserve(kBrushSegments + 1);
    for (int segment = 0; segment <= kBrushSegments; ++segment)
    {
        const float angle = (DirectX::XM_2PI * static_cast<float>(segment)) / static_cast<float>(kBrushSegments);
        const DirectX::XMFLOAT3 worldPoint(
            mouseWorld.x + std::cos(angle) * brushTerrain->BrushRadius,
            mouseWorld.y + std::sin(angle) * brushTerrain->BrushRadius,
            previewZ + 0.05f);

        UiVec2 screenPoint{};
        if (TryProjectWorldToViewport(worldPoint, viewportOrigin, viewportSize, camera, screenPoint, false))
        {
            brushPoints.push_back(screenPoint);
        }
        else if (brushPoints.size() > 1)
        {
            drawList->AddPolyline(brushPoints.data(), static_cast<int>(brushPoints.size()), brushColor, 0, 2.0f);
            brushPoints.clear();
        }
    }

    if (brushPoints.size() > 1)
    {
        drawList->AddPolyline(brushPoints.data(), static_cast<int>(brushPoints.size()), brushColor, 0, 2.0f);
    }

    UiVec2 centreScreen{};
    if (TryProjectWorldToViewport({ mouseWorld.x, mouseWorld.y, previewZ + 0.05f }, viewportOrigin, viewportSize, camera, centreScreen, false))
    {
        drawList->AddCircleFilled(centreScreen, 3.5f, brushColor);
        drawList->AddCircle(centreScreen, 5.5f, UI_COL32(20, 20, 20, 210), 0, 1.5f);
    }
}

void Editor::DrawManualGizmoPivot(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera,
    const Entity* selectedEntity) const
{
    if (!selectedEntity || mActiveGizmo == GizmoType::None) return;

    using namespace DirectX;

    auto projectAxisPoint = [&](const XMFLOAT3& point, UiVec2& screenPoint) -> bool
    {
        return TryProjectWorldToViewport(point, viewportOrigin, viewportSize, camera, screenPoint, true);
    };

    UiVec2 screenPos;
    if (TryProjectWorldToViewport(selectedEntity->Transform.Position, viewportOrigin, viewportSize, camera, screenPos, true))
    {
        UiDrawList* dl = QtUi::GetWindowDrawList();
        dl->AddCircleFilled(screenPos, 6.0f, UI_COL32(255, 200, 0, 230));
        dl->AddCircle(screenPos, 6.0f, UI_COL32(0, 0, 0, 220), 0, 2.0f);

        const XMFLOAT3 origin = selectedEntity->Transform.Position;

        // One length for every axis, shared with the hit tests: see
        // ComputeGizmoWorldScale.
        const float axisWorldLength = ComputeGizmoWorldScale(origin, viewportSize, camera);
        if (axisWorldLength <= 0.0f)
            return;

        auto projectAxisEnd = [&](const XMFLOAT3& axisWorldDirection, UiVec2& axisScreenEnd) -> bool
        {
            return projectAxisPoint(
                XMFLOAT3(
                    origin.x + axisWorldDirection.x * axisWorldLength,
                    origin.y + axisWorldDirection.y * axisWorldLength,
                    origin.z + axisWorldDirection.z * axisWorldLength),
                axisScreenEnd);
        };

        UiVec2 xAxisScreen;
        UiVec2 yAxisScreen;
        UiVec2 zAxisScreen;
        const float xAxisWorldLength = axisWorldLength;
        const float yAxisWorldLength = axisWorldLength;
        const float zAxisWorldLength = axisWorldLength;
        const bool hasXAxis = projectAxisEnd(XMFLOAT3(1.0f, 0.0f, 0.0f), xAxisScreen);
        const bool hasYAxis = projectAxisEnd(XMFLOAT3(0.0f, 0.0f, 1.0f), yAxisScreen);
        const bool hasZAxis = projectAxisEnd(XMFLOAT3(0.0f, 1.0f, 0.0f), zAxisScreen);

        // The handle the user is about to grab, or has grabbed. Highlighting it
        // is what tells them which axis they are on before the drag moves
        // anything - the old gizmo drew all three identically and the only way
        // to find out was to drag and watch.
        const ManualGizmoHandle emphasised = mManualGizmo.IsActive
            ? mManualGizmo.ActiveHandle
            : mManualGizmo.HoveredHandle;

        // A highlighted axis is drawn near-white and thicker; the other two dim
        // back so the one in use reads at a glance rather than by comparison.
        const auto axisColor = [&](UiU32 base, std::initializer_list<ManualGizmoHandle> owned)
        {
            const bool anyEmphasis = emphasised != ManualGizmoHandle::None;
            if (!anyEmphasis)
                return base;

            const bool isEmphasised = std::find(owned.begin(), owned.end(), emphasised) != owned.end();
            if (isEmphasised)
                return UI_COL32(255, 248, 190, 255);

            // Dim, not hidden: the other axes still say which way they point.
            // UI_COL32 packs R, G, B, A into bytes 0..3, so the alpha byte is
            // replaced in place and the colour is left alone.
            return (base & 0x00FFFFFFu) | (static_cast<UiU32>(110) << 24);
        };

        const auto axisThickness = [&](std::initializer_list<ManualGizmoHandle> owned)
        {
            return std::find(owned.begin(), owned.end(), emphasised) != owned.end() ? 4.0f : 2.5f;
        };

        if (mActiveGizmo == GizmoType::Translate || mActiveGizmo == GizmoType::Scale)
        {
            auto drawArrowHead = [&](const UiVec2& start, const UiVec2& end, UiU32 color)
            {
                UiVec2 dir(end.x - start.x, end.y - start.y);
                const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (len <= 0.0001f)
                    return;

                dir.x /= len;
                dir.y /= len;
                const UiVec2 perp(-dir.y, dir.x);
                const float arrowSize = 8.0f;
                const UiVec2 left(end.x - dir.x * arrowSize + perp.x * (arrowSize * 0.5f), end.y - dir.y * arrowSize + perp.y * (arrowSize * 0.5f));
                const UiVec2 right(end.x - dir.x * arrowSize - perp.x * (arrowSize * 0.5f), end.y - dir.y * arrowSize - perp.y * (arrowSize * 0.5f));
                dl->AddTriangleFilled(end, left, right, color);
            };

            // Each axis owns two handles - the translate one and the scale one -
            // because the same line is both, depending on the active gizmo.
            if (hasXAxis)
            {
                const auto owned = { ManualGizmoHandle::TranslateXAxis, ManualGizmoHandle::ScaleXAxis };
                const UiU32 color = axisColor(UI_COL32(220, 70, 70, 255), owned);
                dl->AddLine(screenPos, xAxisScreen, color, axisThickness(owned));
                drawArrowHead(screenPos, xAxisScreen, color);
            }
            if (hasYAxis)
            {
                const auto owned = { ManualGizmoHandle::TranslateYAxis, ManualGizmoHandle::ScaleYAxis };
                const UiU32 color = axisColor(UI_COL32(80, 160, 255, 255), owned);
                dl->AddLine(screenPos, yAxisScreen, color, axisThickness(owned));
                drawArrowHead(screenPos, yAxisScreen, color);
            }
            if (hasZAxis)
            {
                const auto owned = { ManualGizmoHandle::TranslateZAxis, ManualGizmoHandle::ScaleZAxis };
                const UiU32 color = axisColor(UI_COL32(70, 220, 120, 255), owned);
                dl->AddLine(screenPos, zAxisScreen, color, axisThickness(owned));
                drawArrowHead(screenPos, zAxisScreen, color);
            }

            const float planeHandleScale = 0.35f;
            if (hasXAxis && hasYAxis)
            {
                const UiVec2 xyA(screenPos.x + (xAxisScreen.x - screenPos.x) * planeHandleScale, screenPos.y + (xAxisScreen.y - screenPos.y) * planeHandleScale);
                const UiVec2 xyB(screenPos.x + (yAxisScreen.x - screenPos.x) * planeHandleScale, screenPos.y + (yAxisScreen.y - screenPos.y) * planeHandleScale);
                const UiVec2 xyC(xyA.x + (xyB.x - screenPos.x) * planeHandleScale, xyA.y + (xyB.y - screenPos.y) * planeHandleScale);
                const auto owned = { ManualGizmoHandle::TranslateXYPlane, ManualGizmoHandle::Scale };
                dl->AddTriangleFilled(screenPos, xyA, xyC, axisColor(UI_COL32(220, 150, 90, 60), owned));
                dl->AddTriangle(screenPos, xyA, xyC, axisColor(UI_COL32(220, 150, 90, 160), owned), 1.5f);
            }

            if (hasXAxis && hasZAxis)
            {
                const UiVec2 xzA(screenPos.x + (xAxisScreen.x - screenPos.x) * planeHandleScale, screenPos.y + (xAxisScreen.y - screenPos.y) * planeHandleScale);
                const UiVec2 xzB(screenPos.x + (zAxisScreen.x - screenPos.x) * planeHandleScale, screenPos.y + (zAxisScreen.y - screenPos.y) * planeHandleScale);
                const UiVec2 xzC(xzA.x + (xzB.x - screenPos.x) * planeHandleScale, xzA.y + (xzB.y - screenPos.y) * planeHandleScale);
                const auto owned = { ManualGizmoHandle::TranslateXZPlane, ManualGizmoHandle::Scale };
                dl->AddTriangleFilled(screenPos, xzA, xzC, axisColor(UI_COL32(180, 120, 220, 60), owned));
                dl->AddTriangle(screenPos, xzA, xzC, axisColor(UI_COL32(180, 120, 220, 160), owned), 1.5f);
            }

            if (hasYAxis && hasZAxis)
            {
                const UiVec2 yzA(screenPos.x + (yAxisScreen.x - screenPos.x) * planeHandleScale, screenPos.y + (yAxisScreen.y - screenPos.y) * planeHandleScale);
                const UiVec2 yzB(screenPos.x + (zAxisScreen.x - screenPos.x) * planeHandleScale, screenPos.y + (zAxisScreen.y - screenPos.y) * planeHandleScale);
                const UiVec2 yzC(yzA.x + (yzB.x - screenPos.x) * planeHandleScale, yzA.y + (yzB.y - screenPos.y) * planeHandleScale);
                const auto owned = { ManualGizmoHandle::TranslateYZPlane, ManualGizmoHandle::Scale };
                dl->AddTriangleFilled(screenPos, yzA, yzC, axisColor(UI_COL32(120, 200, 220, 60), owned));
                dl->AddTriangle(screenPos, yzA, yzC, axisColor(UI_COL32(120, 200, 220, 160), owned), 1.5f);
            }
        }

        if (mActiveGizmo == GizmoType::Rotate)
        {
            auto drawProjectedRing = [&](const XMFLOAT3& axisA, float axisALength, const XMFLOAT3& axisB, float axisBLength, UiU32 color)
            {
                constexpr int segmentCount = 48;
                UiVec2 previousPoint{};
                bool hasPreviousPoint = false;

                for (int segmentIndex = 0; segmentIndex <= segmentCount; ++segmentIndex)
                {
                    const float angle = (XM_2PI * static_cast<float>(segmentIndex)) / static_cast<float>(segmentCount);
                    XMFLOAT3 ringPoint(
                        origin.x + axisA.x * std::cos(angle) * axisALength + axisB.x * std::sin(angle) * axisBLength,
                        origin.y + axisA.y * std::cos(angle) * axisALength + axisB.y * std::sin(angle) * axisBLength,
                        origin.z + axisA.z * std::cos(angle) * axisALength + axisB.z * std::sin(angle) * axisBLength);

                    UiVec2 projectedPoint;
                    if (projectAxisPoint(ringPoint, projectedPoint))
                    {
                        if (hasPreviousPoint)
                            dl->AddLine(previousPoint, projectedPoint, color, 2.0f);

                        previousPoint = projectedPoint;
                        hasPreviousPoint = true;
                    }
                    else
                    {
                        hasPreviousPoint = false;
                    }
                }
            };

            // All three rings share one handle, so which ring is emphasised is
            // decided by the rotation axis the pick chose, not the handle.
            const XMFLOAT3 emphasisedAxis = mManualGizmo.IsActive
                ? mManualGizmo.AxisWorldDirection
                : mManualGizmo.HoveredRotationAxis;
            const bool ringEmphasis = (emphasised == ManualGizmoHandle::Rotate);

            const auto ringColor = [&](UiU32 base, const XMFLOAT3& ringAxis)
            {
                if (!ringEmphasis)
                    return base;
                const bool isThisRing =
                    std::fabs(emphasisedAxis.x - ringAxis.x) < 0.01f &&
                    std::fabs(emphasisedAxis.y - ringAxis.y) < 0.01f &&
                    std::fabs(emphasisedAxis.z - ringAxis.z) < 0.01f;
                if (isThisRing)
                    return UI_COL32(255, 248, 190, 255);
                return (base & 0x00FFFFFFu) | (static_cast<UiU32>(90) << 24);
            };

            drawProjectedRing(XMFLOAT3(0.0f, 0.0f, 1.0f), yAxisWorldLength, XMFLOAT3(0.0f, 1.0f, 0.0f), zAxisWorldLength, ringColor(UI_COL32(220, 70, 70, 220), XMFLOAT3(1.0f, 0.0f, 0.0f)));
            drawProjectedRing(XMFLOAT3(1.0f, 0.0f, 0.0f), xAxisWorldLength, XMFLOAT3(0.0f, 0.0f, 1.0f), yAxisWorldLength, ringColor(UI_COL32(70, 220, 120, 220), XMFLOAT3(0.0f, 0.0f, 1.0f)));
            drawProjectedRing(XMFLOAT3(1.0f, 0.0f, 0.0f), xAxisWorldLength, XMFLOAT3(0.0f, 1.0f, 0.0f), zAxisWorldLength, ringColor(UI_COL32(80, 160, 255, 255), XMFLOAT3(0.0f, 1.0f, 0.0f)));
        }

        if (mActiveGizmo == GizmoType::Scale)
        {
            const float handleSize = 4.0f;
            if (hasXAxis)
                dl->AddRectFilled(UiVec2(xAxisScreen.x - handleSize, xAxisScreen.y - handleSize), UiVec2(xAxisScreen.x + handleSize, xAxisScreen.y + handleSize), UI_COL32(220, 70, 70, 255), 1.0f);
            if (hasYAxis)
                dl->AddRectFilled(UiVec2(yAxisScreen.x - handleSize, yAxisScreen.y - handleSize), UiVec2(yAxisScreen.x + handleSize, yAxisScreen.y + handleSize), UI_COL32(80, 160, 255, 255), 1.0f);
            if (hasZAxis)
                dl->AddRectFilled(UiVec2(zAxisScreen.x - handleSize, zAxisScreen.y - handleSize), UiVec2(zAxisScreen.x + handleSize, zAxisScreen.y + handleSize), UI_COL32(70, 220, 120, 255), 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// DrawGameUiOverlay
//
// Forwards pointer and keyboard input to the game UI. The compositing itself happens in
// the renderer, over whichever surface is presenting; this runs from the viewport draw
// because that is where the viewport rectangle needed to map cursor position into UI
// pixels is known.
// ---------------------------------------------------------------------------

void Editor::DrawGameUiOverlay(const UiVec2& viewportOrigin, const UiVec2& viewportSize)
{
    if (mSceneRenderer == nullptr)
        return;

    RmlUiRenderer& gameUi = mSceneRenderer->GetRmlUiRenderer();
    if (!gameUi.IsInitialized())
        return;

    gameUi.SetVisible(QtUi::IsGamePlaying() || mShowGameUi);

    // The same arithmetic serves both play modes: the rectangle passed in comes from
    // QtUi, which reports the Play window's surface while that window owns presentation
    // and the editor's viewport otherwise.
    //
    // Input goes to the game UI on exactly the frames it is on screen, so a hidden or
    // stopped UI can never swallow clicks meant for the editor viewport.
    if (!mSceneRenderer->IsGameUiActive() || viewportSize.x <= 1.0f || viewportSize.y <= 1.0f)
    {
        // Drop pointer focus as well, so hover and pressed states do not stay latched
        // while the UI is hidden.
        gameUi.SetMouseState(false, 0.0f, 0.0f, false, false, false);
        return;
    }

    // Nothing is drawn here: the UI texture is composited into the presenting surface by
    // QtViewportRenderer, alongside the scene blit. This function only owns input, which
    // is where the rectangle is actually needed.
    const UiVec2 mousePosition = QtUi::GetIO().MousePos;
    const bool insideSurface =
        mousePosition.x >= viewportOrigin.x && mousePosition.x < viewportOrigin.x + viewportSize.x &&
        mousePosition.y >= viewportOrigin.y && mousePosition.y < viewportOrigin.y + viewportSize.y;
    const bool hasPointer = QtUi::IsWindowHovered() && insideSurface;

    // The UI target is sized to the render resolution, which is not necessarily the size
    // the image is displayed at, so pointer coordinates are scaled into UI pixels.
    const float scaleX = static_cast<float>(gameUi.GetWidth()) / viewportSize.x;
    const float scaleY = static_cast<float>(gameUi.GetHeight()) / viewportSize.y;

    gameUi.SetMouseState(
        hasPointer,
        (mousePosition.x - viewportOrigin.x) * scaleX,
        (mousePosition.y - viewportOrigin.y) * scaleY,
        QtUi::IsMouseDown(0),
        QtUi::IsMouseDown(1),
        QtUi::IsMouseDown(2));

    // Keyboard follows focus rather than the pointer: a pause menu is navigated with the
    // arrow keys and Enter, and the cursor is rarely still over the window when it opens.
    // Outside a play session it stays tied to the pointer, so the UI Editor's preview
    // cannot eat keys aimed at the editor.
    if (hasPointer || QtUi::GameWindowHasFocus())
        gameUi.UpdateKeyboardInput();
}

// ---------------------------------------------------------------------------
// DrawToolbar
// ---------------------------------------------------------------------------

void Editor::DrawToolbar(
    D3D12_GPU_DESCRIPTOR_HANDLE selectIcon,
    D3D12_GPU_DESCRIPTOR_HANDLE moveIcon,
    D3D12_GPU_DESCRIPTOR_HANDLE rotateIcon,
    D3D12_GPU_DESCRIPTOR_HANDLE scaleIcon,
    D3D12_GPU_DESCRIPTOR_HANDLE wireframeIcon,
    D3D12_GPU_DESCRIPTOR_HANDLE proxyIcon,
    D3D12_GPU_DESCRIPTOR_HANDLE gameIcon)
{
    QtUi::SetNextWindowSize(UiVec2(320.0f, 56.0f), QtUiCond_FirstUseEver);
    QtUi::PushStyleVar(QtUiStyleVar_WindowPadding, UiVec2(4, 4));
    QtUi::PushStyleVar(QtUiStyleVar_ItemSpacing,   UiVec2(4, 4));

    if (!QtUi::Begin("Toolbar", nullptr, QtUiWindowFlags_NoCollapse))
    {
        QtUi::End();
        QtUi::PopStyleVar(2);
        return;
    }

    const float iconSize = 24.0f;
    const UiVec2 btnSize(iconSize + 8.0f, iconSize + 8.0f);
    const float separatorWidth = 8.0f;

    auto ToolButton = [&](D3D12_GPU_DESCRIPTOR_HANDLE icon, GizmoType type, const char* tooltip) {
        bool active = (mActiveGizmo == type);
        if (active) QtUi::PushStyleColor(QtUiCol_Button, QtUi::GetStyleColorVec4(QtUiCol_ButtonActive));
        bool clicked;
        if (icon.ptr != 0)
            clicked = QtUi::ImageButton(tooltip, TextureIdFromHandle(icon), UiVec2(iconSize, iconSize));
        else
            clicked = QtUi::Button(tooltip, btnSize);
        if (active) QtUi::PopStyleColor();
        if (clicked) mActiveGizmo = active ? GizmoType::None : type;
        if (QtUi::IsItemHovered()) QtUi::SetTooltip("%s", tooltip);
    };

    auto ToggleButton = [&](D3D12_GPU_DESCRIPTOR_HANDLE icon, bool& value, const char* tooltip)
    {
        if (value) QtUi::PushStyleColor(QtUiCol_Button, QtUi::GetStyleColorVec4(QtUiCol_ButtonActive));
        const bool clicked = (icon.ptr != 0)
            ? QtUi::ImageButton(tooltip, TextureIdFromHandle(icon), UiVec2(iconSize, iconSize))
            : QtUi::Button(tooltip, btnSize);
        if (value) QtUi::PopStyleColor();
        if (clicked) value = !value;
        if (QtUi::IsItemHovered()) QtUi::SetTooltip("%s", tooltip);
    };

    // Play transfers the live renderer at the next frame boundary.
    auto PlayButton = [&](D3D12_GPU_DESCRIPTOR_HANDLE icon)
    {
        const bool playing = IsPlaySessionActive();
        if (playing) QtUi::PushStyleColor(QtUiCol_Button, QtUi::GetStyleColorVec4(QtUiCol_ButtonActive));
        const bool clicked = (icon.ptr != 0)
            ? QtUi::ImageButton("Play", TextureIdFromHandle(icon), UiVec2(iconSize, iconSize))
            : QtUi::Button(playing ? "Stop" : "Play", btnSize);
        if (playing) QtUi::PopStyleColor();

        if (clicked && mSceneRenderer != nullptr)
        {
            if (playing) mSceneRenderer->StopGame();
            else RequestPlaySession();
        }

        if (QtUi::IsItemHovered())
        {
            if (!mGameStartErrorMessage.empty())
                QtUi::SetTooltip("Play failed: %s", mGameStartErrorMessage.c_str());
            else if (playing)
                QtUi::SetTooltip("Stop the game\nEscape pauses; F11 switches fullscreen");
            else if (mPlayInNewWindow)
                QtUi::SetTooltip("Play in a new window\nRight-click for the play mode");
            else
                QtUi::SetTooltip("Play in the viewport\nRight-click for the play mode");
        }

        // The mode lives on a right-click menu rather than a second toolbar button: it
        // is picked once and then left alone, so it does not deserve permanent space
        // next to the control you actually press. The same two entries are in the Play
        // menu, which is where anyone who does not think to right-click will look.
        //
        // Last, because the menu items it builds become "the last item" and would take
        // the tooltip and hover queries above with them.
        if (QtUi::BeginPopupContextItem("play-mode"))
        {
            DrawPlayModeMenuItems();
            QtUi::EndPopup();
        }
    };

    const float totalButtonsWidth =
        btnSize.x * 7.0f +
        QtUi::GetStyle().ItemSpacing.x * 6.0f +
        separatorWidth * 2.0f;
    const float availableWidth = QtUi::GetContentRegionAvail().x;
    if (availableWidth > totalButtonsWidth)
    {
        QtUi::SetCursorPosX(QtUi::GetCursorPosX() + (availableWidth - totalButtonsWidth) * 0.5f);
    }

    ToolButton(selectIcon, GizmoType::None,      "Select  (Q)");
    QtUi::SameLine();
    ToolButton(moveIcon,   GizmoType::Translate,  "Move    (W)");
    QtUi::SameLine();
    ToolButton(rotateIcon, GizmoType::Rotate,     "Rotate  (E)");
    QtUi::SameLine();
    ToolButton(scaleIcon,  GizmoType::Scale,      "Scale   (R)");
    QtUi::SameLine();
    QtUi::Dummy(UiVec2(separatorWidth, 0.0f));
    QtUi::SameLine();
    ToggleButton(wireframeIcon, mWireframeEnabled, "Wireframe");
    QtUi::SameLine();
    ToggleButton(proxyIcon, mProxyEnabled, "Proxy");
    QtUi::SameLine();
    QtUi::Dummy(UiVec2(separatorWidth, 0.0f));
    QtUi::SameLine();
    PlayButton(gameIcon);

    QtUi::End();
    QtUi::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
// DrawViewport
// ---------------------------------------------------------------------------

void Editor::DrawViewport(
    D3D12_GPU_DESCRIPTOR_HANDLE sceneTextureHandle,
    const EditorCamera& camera,
    Entity* selectedEntity)
{
    QtUiViewport* mainViewport = QtUi::GetMainViewport();
    QtUi::SetNextWindowViewport(mainViewport->ID);
    QtUi::SetNextWindowSize(mainViewport->WorkSize, QtUiCond_FirstUseEver);
    QtUi::PushStyleVar(QtUiStyleVar_WindowPadding, UiVec2(0, 0));
    QtUi::Begin("Viewport", nullptr,
        QtUiWindowFlags_NoScrollbar | QtUiWindowFlags_NoScrollWithMouse |
        QtUiWindowFlags_NoCollapse | QtUiWindowFlags_NoBringToFrontOnFocus |
        QtUiWindowFlags_MenuBar);

    if (QtUi::BeginMenuBar())
    {
        QtUi::PushStyleVar(QtUiStyleVar_WindowPadding, UiVec2(12.0f, 10.0f));
        QtUi::PushStyleVar(QtUiStyleVar_ItemSpacing, UiVec2(10.0f, 8.0f));
        QtUi::PushStyleVar(QtUiStyleVar_FramePadding, UiVec2(10.0f, 6.0f));

        if (QtUi::BeginMenu("Viewport"))
        {
            if (QtUi::MenuItem("Resolution..."))
                mShowViewportResolutionDialog = true;
            if (QtUi::MenuItem("Screenshot..."))
                mShowScreenshotDialog = true;
            QtUi::EndMenu();
        }

        if (QtUi::BeginMenu("Options"))
        {
            QtUi::MenuItem("Renderer Statistics", nullptr, &mShowViewportStatistics);
            QtUi::MenuItem("Placement Icons & Light Shapes", nullptr, &mShowViewportPlacementIcons);
            QtUi::MenuItem("Grid", nullptr, &mShowViewportGrid);
            QtUi::MenuItem("Game UI", nullptr, &mShowGameUi);
            if (mSceneRenderer != nullptr)
            {
                // Report what the UI is actually doing. Initialization and document
                // loading are both non-fatal, so without this a failure is silent and
                // looks identical to a UI that is simply drawing nothing.
                const RmlUiRenderer& gameUi = mSceneRenderer->GetRmlUiRenderer();
                if (!gameUi.IsInitialized())
                    QtUi::TextDisabled("UI: not initialized - %s",
                        gameUi.GetLastErrorMessage() ? gameUi.GetLastErrorMessage() : "no error reported");
                else if (gameUi.GetLoadedDocumentName().empty())
                    QtUi::TextDisabled("UI: ready, no document (loaded by game code)");
                else
                    QtUi::TextDisabled("UI: %s (%ux%u)%s",
                        gameUi.GetLoadedDocumentName().c_str(), gameUi.GetWidth(), gameUi.GetHeight(),
                        mSceneRenderer->IsGameRunning() ? "" : " - shown in play mode");
            }
            if (mSceneRenderer != nullptr && mSceneRenderer->GetRmlUiRenderer().IsDebuggerAvailable())
            {
                bool debuggerVisible = mSceneRenderer->GetRmlUiRenderer().IsDebuggerVisible();
                if (QtUi::MenuItem("Game UI Inspector", nullptr, &debuggerVisible))
                    mSceneRenderer->GetRmlUiRenderer().SetDebuggerVisible(debuggerVisible);
            }
            if (QtUi::MenuItem("Reload Game UI") && mSceneRenderer != nullptr)
            {
                // Documents are read from disk on load, so this is the authoring loop:
                // edit Data/UI/hud.rml or hud.rcss, then reload without restarting.
                RmlUiRenderer& gameUi = mSceneRenderer->GetRmlUiRenderer();
                if (gameUi.IsInitialized() && !gameUi.ReloadDocument() && gameUi.GetLastErrorMessage())
                    OutputDebugStringA(gameUi.GetLastErrorMessage());
            }
            QtUi::Separator();
            QtUi::MenuItem("Movement Snap", nullptr, &mMovementSnapEnabled);
            QtUi::PushItemWidth(130.0f);
            if (QtUi::DragFloat("Move Step", &mMovementSnapStep, 0.05f, 0.001f, 1000.0f, "%.3f"))
                mMovementSnapStep = (std::max)(0.001f, mMovementSnapStep);
            QtUi::MenuItem("Rotation Snap", nullptr, &mRotationSnapEnabled);
            if (QtUi::DragFloat("Rotate Step", &mRotationSnapDegrees, 0.5f, 0.1f, 180.0f, "%.1f deg"))
                mRotationSnapDegrees = (std::max)(0.1f, mRotationSnapDegrees);
            QtUi::PopItemWidth();
            QtUi::EndMenu();
        }

        // A top-level bar action rather than an entry inside a menu: this is a
        // mode you toggle constantly while framing a shot, so it should be one
        // click, not two. MenuItem adds straight to the QMenuBar when there is
        // no open menu on the scope stack.
        //
        // The name is fixed and the state shown by the check mark, because
        // nodes here are keyed by their label - swapping the text for
        // "Exit Fullscreen" would create a second action and leave both in the
        // bar.
        QtUi::MenuItem("Fullscreen", "F11", &mViewportFullscreen);

        QtUi::PopStyleVar(3);
        QtUi::EndMenuBar();
    }

    const UiVec2 viewportOrigin = QtUi::GetCursorScreenPos();
    const UiVec2 viewportSize   = QtUi::GetContentRegionAvail();
    mLastViewportContentOrigin = viewportOrigin;
    mLastViewportContentSize = viewportSize;

    if (viewportSize.x > 1.0f && viewportSize.y > 1.0f && sceneTextureHandle.ptr != 0)
    {
        QtUi::Image(TextureIdFromHandle(sceneTextureHandle), viewportSize);
    }

    DrawGameUiOverlay(viewportOrigin, viewportSize);

    // Playing in the viewport, the viewport shows the game and nothing else: light
    // gizmos, placement icons and collision hulls are authoring aids drawn over the
    // scene, and a click in there belongs to the game rather than to entity selection.
    // The panels stay live, so the level can still be edited from the Level Explorer and
    // Properties while it runs - it is only the picture that is the game's.
    const bool playing = QtUi::IsGamePlaying();
    const bool viewportHovered = !playing && QtUi::IsWindowHovered();
    if (!playing)
    {
        DrawViewportPlacementIcons(viewportOrigin, viewportSize, camera);
        DrawLightShapeGizmos(viewportOrigin, viewportSize, camera);
        DrawTerrainViewportOverlay(viewportOrigin, viewportSize, camera);
        HandleManualGizmoInteraction(viewportOrigin, viewportSize, camera, viewportHovered, selectedEntity);
        DrawManualGizmoPivot(viewportOrigin, viewportSize, camera, selectedEntity);
    }

    // -----------------------------------------------------------------------
    // Proxy view: draw convex hull wireframes for all entities with collision data.
    // -----------------------------------------------------------------------
    if (mProxyEnabled && !playing)
    {
        using namespace DirectX;
        const XMMATRIX viewProjMatrix = XMMatrixMultiply(
            camera.GetViewMatrix(), camera.GetProjectionMatrix());

        UiDrawList* drawList = QtUi::GetWindowDrawList();
        const UiU32 hullColor = UI_COL32(80, 220, 80, 200);

        for (const Entity& entity : mEntities)
        {
            if (!entity.Mesh.has_value()) continue;
            const MeshComponent& mesh = *entity.Mesh;
            if (!mesh.MeshAsset || !mesh.MeshAsset->HasCollisionHulls()) continue;

            const XMMATRIX worldMatrix = entity.Transform.GetTransform();
            const XMMATRIX worldViewProj = XMMatrixMultiply(worldMatrix, viewProjMatrix);

            for (const CollisionHull& hull : mesh.MeshAsset->GetCollisionHulls())
            {
                for (std::size_t index = 0; index + 2 < hull.Indices.size(); index += 3)
                {
                    const std::uint32_t i0 = hull.Indices[index + 0];
                    const std::uint32_t i1 = hull.Indices[index + 1];
                    const std::uint32_t i2 = hull.Indices[index + 2];
                    if (i0 >= hull.Vertices.size() ||
                        i1 >= hull.Vertices.size() ||
                        i2 >= hull.Vertices.size())
                        continue;

                    const XMFLOAT3& p0 = hull.Vertices[i0];
                    const XMFLOAT3& p1 = hull.Vertices[i1];
                    const XMFLOAT3& p2 = hull.Vertices[i2];

                    UiVec2 s0, s1, s2;
                    const bool ok0 = ProjectPoint(worldViewProj, viewportOrigin, viewportSize, p0, s0);
                    const bool ok1 = ProjectPoint(worldViewProj, viewportOrigin, viewportSize, p1, s1);
                    const bool ok2 = ProjectPoint(worldViewProj, viewportOrigin, viewportSize, p2, s2);

                    if (ok0 && ok1) drawList->AddLine(s0, s1, hullColor, 1.0f);
                    if (ok1 && ok2) drawList->AddLine(s1, s2, hullColor, 1.0f);
                    if (ok0 && ok2) drawList->AddLine(s0, s2, hullColor, 1.0f);
                }
            }
        }
    }

    if (mActiveGizmo == GizmoType::None && mViewportSelection.IsDragging && !playing)
    {
        UiDrawList* drawList = QtUi::GetWindowDrawList();
        UiVec2 selectionMin((std::min)(mViewportSelection.Start.x, mViewportSelection.Current.x), (std::min)(mViewportSelection.Start.y, mViewportSelection.Current.y));
        UiVec2 selectionMax((std::max)(mViewportSelection.Start.x, mViewportSelection.Current.x), (std::max)(mViewportSelection.Start.y, mViewportSelection.Current.y));
        drawList->AddRectFilled(selectionMin, selectionMax, UI_COL32(255, 180, 80, 40));
        drawList->AddRect(selectionMin, selectionMax, UI_COL32(255, 210, 120, 220), 0.0f, 0, 1.5f);
    }

    if (!playing)
        HandleViewportInteraction(viewportOrigin, viewportSize, camera, viewportHovered);

    QtUi::End();
    QtUi::PopStyleVar();
}

// ---------------------------------------------------------------------------
// DrawLevelExplorerPanel
// ---------------------------------------------------------------------------

void Editor::DrawLevelExplorerPanel()
{
    if (!QtUi::Begin("Level Explorer", &mShowLevelExplorerPanel))
    {
        QtUi::End();
        return;
    }

    for (int i = 0; i < static_cast<int>(mEntities.size()); ++i)
    {
        const bool selected = (mSelectedEntityIndex == i);
        QtUiTreeNodeFlags flags = QtUiTreeNodeFlags_Leaf | QtUiTreeNodeFlags_SpanLabelWidth;
        if (selected) flags |= QtUiTreeNodeFlags_Selected;

        QtUi::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(i)), flags,
                          "%s", mEntities[i].Name.c_str());
        if (QtUi::IsItemClicked())
            mSelectedEntityIndex = i;
        QtUi::TreePop();
    }

    QtUi::End();
}

// ---------------------------------------------------------------------------
// Terrain support
// ---------------------------------------------------------------------------

namespace
{
    // A small modal for asking the user to enter width / height of an
    // incoming .raw heightmap.  We use a single static state so the modal
    // survives across frames until the user confirms or cancels.
    struct TerrainImportModalState
    {
        bool   Open         = false;
        int    Width        = 512;
        int    Height       = 512;
        // True when Width/Height were filled in from the source image (PNG,
        // JPG, EXR, etc.).  When true, the modal disables the Width/Height
        // inputs because the image itself defines the resolution.
        bool   DimensionsAutoDetected = false;
        float  WorldSize    = 1024.0f;
        float  HeightScale  = 256.0f;
        float  HeightOffset = 0.0f;
        bool   HasPlacementPosition = false;
        DirectX::XMFLOAT3 PlacementPosition{ 0.0f, 0.0f, 0.0f };
        std::string RawPath;
        std::string DdsPath;
        std::string ErrorMessage;
        // Cached CPU samples from the modal's preview decode, so the actual
        // "Create" click can re-encode the DDS without re-reading the file.
        std::vector<std::uint16_t> CachedSamples;
    };

    TerrainImportModalState& GetTerrainImportModalState()
    {
        static TerrainImportModalState state;
        return state;
    }

    bool PromptForRawHeightmapPath(HWND ownerWindowHandle, std::string& inOutPath)
    {
        // Accept either a flat .raw heightmap or any of the common image
        // formats that DirectXTex can decode (PNG, JPG, BMP, TIFF, TGA,
        // HDR).  DirectXTex doesn't ship with OpenEXR support, so .exr is
        // intentionally absent from the filter -- convert EXR to one of
        // the supported formats in an external tool first.
        return PromptForDataFile(
            ownerWindowHandle,
            "Select Heightmap",
            "Heightmap Image\0*.raw;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.tga;*.hdr\0"
            "All Files\0*.*\0",
            inOutPath);
    }

    // Draws the CryEngine-style material paint-layer editor for one terrain.
    // Returns true when the terrain mesh must be rebuilt (adding/removing a
    // layer changes whether the splat weights are baked into the vertex
    // colour).  Sets `outSceneDirty` for any edit that should flag the level
    // as unsaved.
    bool DrawTerrainLayerControls(TerrainComponent& tc, HWND ownerWindow, bool& outSceneDirty)
    {
        bool needsRebuild = false;

        QtUi::SeparatorText("Material Layers (paint)");
        QtUi::TextDisabled("Add layers, pick a texture, then use the Paint brush.");

        int layerToRemove = -1;
        for (int i = 0; i < static_cast<int>(tc.PaintLayers.size()); ++i)
        {
            QtUi::PushID(i);
            TerrainPaintLayer& layer = tc.PaintLayers[i];

            bool isActive = (tc.ActivePaintLayer == i);
            if (QtUi::RadioButton("##active", isActive))
            {
                tc.ActivePaintLayer = i;
                outSceneDirty = true;
            }
            QtUi::SameLine();
            QtUi::Text("Layer %d%s", i, isActive ? " (active)" : "");

            QtUi::SameLine();
            if (QtUi::SmallButton("Remove"))
                layerToRemove = i;

            QtUi::TextWrapped("Texture: %s",
                layer.DiffuseTexturePath.empty() ? "(none)" : layer.DiffuseTexturePath.c_str());
            if (QtUi::Button("Texture..."))
            {
                std::string texPath = layer.DiffuseTexturePath;
                if (PromptForDataFile(ownerWindow, "Select Layer Texture",
                                      "Texture (DDS)\0*.dds\0All Files\0*.*\0", texPath))
                {
                    layer.DiffuseTexturePath = texPath;
                    outSceneDirty = true;
                }
            }
            QtUi::SameLine();
            if (QtUi::SmallButton("Clear Texture"))
            {
                layer.DiffuseTexturePath.clear();
                outSceneDirty = true;
            }

            if (QtUi::DragFloat("Tile Scale", &layer.TileScale, 0.25f, 0.25f, 512.0f))
                outSceneDirty = true;

            float tint[4] = { layer.TintR, layer.TintG, layer.TintB, layer.TintA };
            if (QtUi::ColorEdit4("Tint", tint))
            {
                layer.TintR = tint[0];
                layer.TintG = tint[1];
                layer.TintB = tint[2];
                layer.TintA = tint[3];
                outSceneDirty = true;
            }

            QtUi::Separator();
            QtUi::PopID();
        }

        if (layerToRemove >= 0)
        {
            tc.PaintLayers.erase(tc.PaintLayers.begin() + layerToRemove);
            if (tc.ActivePaintLayer >= static_cast<int>(tc.PaintLayers.size()))
                tc.ActivePaintLayer = (std::max)(0, static_cast<int>(tc.PaintLayers.size()) - 1);
            outSceneDirty = true;
            needsRebuild  = true;
        }

        if (static_cast<int>(tc.PaintLayers.size()) < kTerrainMaxLayers)
        {
            if (QtUi::Button("Add Layer"))
            {
                tc.PaintLayers.emplace_back();
                tc.ActivePaintLayer = static_cast<int>(tc.PaintLayers.size()) - 1;
                outSceneDirty = true;
                needsRebuild  = true; // baking splat weights into vertex colour
            }
        }
        else
        {
            QtUi::TextDisabled("Maximum of %d layers reached.", kTerrainMaxLayers);
        }

        return needsRebuild;
    }
}

void Editor::CreateTerrainFromRawFile(const DirectX::XMFLOAT3* placementPosition)
{
    TerrainImportModalState& modal = GetTerrainImportModalState();
    modal.ErrorMessage.clear();

    std::string rawPath;
    if (!PromptForRawHeightmapPath(DX12Context_GetWindowHandle(), rawPath))
    {
        return;
    }
    modal.RawPath = rawPath;
    modal.Width  = 512;
    modal.Height = 512;
    modal.DimensionsAutoDetected = false;
    modal.CachedSamples.clear();
    modal.WorldSize = 1024.0f;
    modal.HeightScale = 256.0f;
    modal.HeightOffset = 0.0f;
    modal.HasPlacementPosition = placementPosition != nullptr;
    modal.PlacementPosition = placementPosition != nullptr
        ? *placementPosition
        : DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);

    // For image sources (anything but .raw) the importer auto-detects the
    // resolution, so we peek the file up front to fill in the modal and
    // cache the samples so the actual "Create" click can re-encode the
    // DDS without re-reading the file.
    std::string ext = std::filesystem::path(rawPath).extension().string();
    for (char& c : ext) { c = static_cast<char>(::tolower(static_cast<unsigned char>(c))); }
    if (ext != ".raw")
    {
        std::string peekError;
        int peekedWidth = 0;
        int peekedHeight = 0;
        if (HeightmapImporter::LoadImageToHeightmap(
                rawPath, modal.CachedSamples, peekedWidth, peekedHeight, peekError)
            && peekedWidth > 0 && peekedHeight > 0)
        {
            modal.Width  = peekedWidth;
            modal.Height = peekedHeight;
            modal.DimensionsAutoDetected = true;
        }
        else
        {
            modal.CachedSamples.clear();
            modal.ErrorMessage = "Could not pre-decode the source image: " + peekError;
        }
    }

    modal.Open = true;
    mShowTerrainToolWindow = true;
}

void Editor::DrawTerrainToolWindow(Entity* selectedEntity)
{
    TerrainImportModalState& modal = GetTerrainImportModalState();

    // Import dialog (modal) for an incoming .raw heightmap.
    if (modal.Open)
    {
        QtUi::OpenPopup("Create Terrain");
        if (QtUi::BeginPopupModal("Create Terrain", &modal.Open, QtUiWindowFlags_AlwaysAutoResize))
        {
            QtUi::TextWrapped("Source heightmap: %s", modal.RawPath.c_str());
            if (modal.DimensionsAutoDetected)
            {
                QtUi::TextDisabled("Image format detected -- width/height are read-only.");
            }
            QtUi::BeginDisabled(modal.DimensionsAutoDetected);
            QtUi::InputInt("Width",  &modal.Width);
            QtUi::InputInt("Height", &modal.Height);
            // Clamp the raw .raw input here so the user can't accidentally
            // type a dimension that overruns the renderer cap (the mesh
            // build decimation caps at 2049, but a 100k x 100k heightmap
            // would still consume 20 GB of memory during decode).
            if (modal.Width  > 16384) modal.Width  = 16384;
            if (modal.Height > 16384) modal.Height = 16384;
            if (modal.Width  < 1)     modal.Width  = 1;
            if (modal.Height < 1)     modal.Height = 1;
            QtUi::EndDisabled();
            QtUi::DragFloat("World Size (m)",    &modal.WorldSize,    1.0f, 1.0f, 100000.0f);
            QtUi::DragFloat("Height Scale (m)",  &modal.HeightScale,  1.0f, 0.001f, 100000.0f);
            QtUi::DragFloat("Height Offset (m)", &modal.HeightOffset, 0.1f, -100000.0f, 100000.0f);

            if (!modal.ErrorMessage.empty())
            {
                QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.4f, 0.4f, 1.0f));
                QtUi::TextWrapped("%s", modal.ErrorMessage.c_str());
                QtUi::PopStyleColor();
            }

            if (QtUi::Button("Create"))
            {
                if (modal.Width <= 0 || modal.Height <= 0)
                {
                    modal.ErrorMessage = "Width and height must be positive.";
                }
                else
                {
                    std::string ddsPath;
                    if (!modal.CachedSamples.empty())
                    {
                        // The modal already decoded the image up front, so
                        // we just need to write the DDS from those samples.
                        if (HeightmapImporter::EncodeHeightmapToDds(
                                modal.RawPath, modal.CachedSamples,
                                modal.Width, modal.Height, modal.ErrorMessage))
                        {
                            ddsPath = HeightmapImporter::DerivedDdsPath(modal.RawPath);
                        }
                    }
                    else
                    {
                        // .raw source: ConvertRaw16ToDds re-reads the file
                        // (we have no cached samples).
                        ddsPath = HeightmapImporter::ConvertRaw16ToDds(
                            modal.RawPath, modal.Width, modal.Height, modal.ErrorMessage);
                    }
                    if (ddsPath.empty())
                    {
                        // ConvertRaw16ToDds already populated ErrorMessage.
                    }
                    else
                    {
                        Entity entity;
                        entity.Name = "Terrain_" + std::to_string(mEntities.size() + 1);
                        entity.Transform.Position = modal.HasPlacementPosition
                            ? modal.PlacementPosition
                            : DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
                        TerrainComponent& tc = entity.AddTerrainComponent();
                        tc.HeightmapRawPath = modal.RawPath;
                        tc.HeightmapDdsPath = ddsPath;
                        tc.Width            = modal.Width;
                        tc.Height           = modal.Height;
                        tc.WorldSize        = modal.WorldSize;
                        tc.HeightScale      = modal.HeightScale;
                        tc.HeightOffset     = modal.HeightOffset;
                        tc.Brush            = TerrainComponent::BrushType::Raise;
                        tc.BrushRadius      = 8.0f;
                        tc.BrushStrength    = 0.5f;
                        tc.FlattenHeight    = 0.0f;
                        tc.BrushSmoothingPasses = 1;

                        mEntities.push_back(std::move(entity));
                        mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
                        mSelectedEntityIndices = { mSelectedEntityIndex };
                        MarkSceneChanged();
                        mLastSceneStatusMessage = "Created terrain from '" + modal.RawPath + "'.";
                        modal.Open = false;

                        // Frame the camera on the new terrain patch so the
                        // artist doesn't have to hunt for it.  The
                        // default editor camera is tilted up; without
                        // this the freshly imported terrain sits below
                        // the viewport and looks invisible.
                        if (mSceneRenderer != nullptr)
                        {
                            const float halfSize = modal.WorldSize * 0.5f;
                            const DirectX::XMFLOAT3& terrainPosition = mEntities.back().Transform.Position;
                            const DirectX::XMFLOAT3 aabbMin(
                                terrainPosition.x - halfSize,
                                terrainPosition.y - halfSize,
                                terrainPosition.z - modal.HeightScale);
                            const DirectX::XMFLOAT3 aabbMax(
                                terrainPosition.x + halfSize,
                                terrainPosition.y + halfSize,
                                terrainPosition.z + modal.HeightScale);
                            mSceneRenderer->FrameAabbInView(aabbMin, aabbMax);
                        }
                    }
                }
            }
            QtUi::SameLine();
            if (QtUi::Button("Cancel"))
            {
                modal.Open = false;
            }
            QtUi::EndPopup();
        }
    }

    // Persistent brush tool window.  Mirrors the brush section from the
    // Properties panel so the artist can keep the brush settings visible
    // while flying the camera around the level.
    if (!QtUi::Begin("Terrain Tool", &mShowTerrainToolWindow, QtUiWindowFlags_AlwaysAutoResize))
    {
        QtUi::End();
        return;
    }

    Entity* terrain = nullptr;
    for (Entity& e : mEntities)
    {
        if (e.HasTerrainComponent() && e.Terrain.has_value())
        {
            terrain = &e;
            break;
        }
    }

    if (terrain == nullptr)
    {
        QtUi::TextDisabled("No terrain in the level. Create one from the Components panel.");
    }
    else
    {
        QtUi::Text("Terrain: %s", terrain->Name.c_str());
        QtUi::SeparatorText("Brush");
        TerrainComponent& tc = *terrain->Terrain;
        const char* brushNames[] = { "Raise", "Lower", "Flatten", "Smooth", "Paint" };
        int brushTypeIndex = static_cast<int>(tc.Brush);
        if (QtUi::Combo("Type", &brushTypeIndex, brushNames, std::size(brushNames)))
        {
            tc.Brush = static_cast<TerrainComponent::BrushType>(brushTypeIndex);
            MarkSceneChanged();
        }
        if (QtUi::DragFloat("Radius (m)",  &tc.BrushRadius,   0.1f, 0.1f, 1000.0f)) MarkSceneChanged();
        if (QtUi::DragFloat("Strength (m/s, raise/lower)", &tc.BrushStrength, 0.01f, 0.001f, 100.0f)) MarkSceneChanged();
        if (QtUi::DragFloat("Flatten Height (m)", &tc.FlattenHeight, 0.1f, -10000.0f, 10000.0f)) MarkSceneChanged();
        if (QtUi::DragInt  ("Smooth Passes", &tc.BrushSmoothingPasses, 1, 1, 10)) MarkSceneChanged();

        QtUi::Separator();
        bool brushActive = mTerrainBrushModeActive;
        if (QtUi::Button(brushActive ? "Exit Brush Mode" : "Enter Brush Mode"))
        {
            mTerrainBrushModeActive = !brushActive;
        }
        QtUi::SameLine();
        if (QtUi::Button("Paint at Selected"))
        {
            if (mSelectedEntityIndex >= 0
                && mSelectedEntityIndex < static_cast<int>(mEntities.size())
                && mEntities[mSelectedEntityIndex].HasTerrainComponent())
            {
                Entity& e = mEntities[mSelectedEntityIndex];
                e.Transform.Position; // (no-op, just to silence unused warnings)
            }
        }
        if (selectedEntity && selectedEntity->HasTerrainComponent())
        {
            if (QtUi::Button("Apply Brush at Last Pick"))
            {
                // Will be triggered by a viewport click; the message below
                // just shows the last status from the renderer.
            }
        }

        if (tc.Brush == TerrainComponent::BrushType::Paint && tc.PaintLayers.empty())
        {
            QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.75f, 0.35f, 1.0f));
            QtUi::TextWrapped("Add at least one material layer below to paint.");
            QtUi::PopStyleColor();
        }

        // Material paint-layer editor.  A layer add/remove requires a mesh
        // rebuild because the splat weights are baked into the vertex colour.
        bool layerSceneDirty = false;
        if (DrawTerrainLayerControls(tc, DX12Context_GetWindowHandle(), layerSceneDirty))
        {
            if (mTerrainRenderer != nullptr)
            {
                const std::size_t terrainIndex =
                    static_cast<std::size_t>(terrain - mEntities.data());
                mTerrainRenderer->MarkTerrainDirty(terrainIndex);
            }
        }
        if (layerSceneDirty)
            MarkSceneChanged();

        if (!mLastTerrainBrushMessage.empty())
        {
            QtUi::Separator();
            QtUi::TextWrapped("%s", mLastTerrainBrushMessage.c_str());
        }
    }

    QtUi::End();
}

// ---------------------------------------------------------------------------
// DrawComponentsPanel
// ---------------------------------------------------------------------------

void Editor::DrawLightStyleControls(
    const char*   idSuffix,
    LightStyleId& style,
    float&        styleSpeed,
    float&        styleAmplitude,
    float&        stylePhaseOffset,
    std::string&  customStylePattern)
{
    QtUi::PushID(idSuffix);

    const char* styleNames[kLightStyleCount];
    for (int i = 0; i < kLightStyleCount; ++i)
    {
        styleNames[i] = LightStyles::GetStyleDisplayName(static_cast<LightStyleId>(i));
    }

    int styleIndex = static_cast<int>(style);
    if (QtUi::Combo("Style", &styleIndex, styleNames, kLightStyleCount))
    {
        style = static_cast<LightStyleId>(styleIndex);
        // Adopt the style's natural rate on selection. The classic patterns all
        // want 10 steps per second; the continuous fire curves want about 1, and
        // carrying 10 over to them turns a flame into a buzzing lamp.
        styleSpeed = LightStyles::GetStyleInfo(style).DefaultSpeed;
        MarkSceneChanged();
    }
    QtUi::SetItemTooltip(
        "Fire and Torch are continuous noise curves; the rest are the classic\n"
        "stepped light-style patterns. The multiplier reaches the deferred\n"
        "shading, the ray-traced GI and the volumetric fog together.");

    QtUi::BeginDisabled(style == LightStyleId::None);

    if (QtUi::DragFloat("Speed", &styleSpeed, 0.05f, 0.0f, 60.0f, "%.2f"))
    {
        styleSpeed = (std::max)(styleSpeed, 0.0f);
        MarkSceneChanged();
    }
    QtUi::SetItemTooltip("Pattern steps per second, or the noise rate for Fire and Torch.");

    if (QtUi::SliderFloat("Amount", &styleAmplitude, 0.0f, 2.0f, "%.2f"))
    {
        styleAmplitude = (std::max)(styleAmplitude, 0.0f);
        MarkSceneChanged();
    }
    QtUi::SetItemTooltip("0 holds the light constant, 1 applies the style in full.");

    if (QtUi::DragFloat("Phase Offset", &stylePhaseOffset, 0.05f, -60.0f, 60.0f, "%.2f s"))
        MarkSceneChanged();
    QtUi::SetItemTooltip("Offsets this light into the curve, so two torches in one room do not flicker in lockstep.");

    if (style == LightStyleId::Custom)
    {
        char patternBuffer[128];
        strncpy_s(patternBuffer, customStylePattern.c_str(), _TRUNCATE);
        if (QtUi::InputText("Pattern", patternBuffer, std::size(patternBuffer)))
        {
            customStylePattern = patternBuffer;
            MarkSceneChanged();
        }
        QtUi::SetItemTooltip("Letters 'a' (black) to 'z', where 'm' is the light's authored brightness.");
    }

    QtUi::EndDisabled();
    QtUi::PopID();
}

void Editor::DrawComponentsPanel()
{
    if (!QtUi::Begin("Components", &mShowComponentsPanel,
        QtUiWindowFlags_NoCollapse))
    {
        QtUi::End();
        return;
    }

    // Clears every placement-prototype flag; each Selectable calls this before
    // (re)setting the one it owns so only a single prototype is ever armed.
    const auto clearPrototypes = [this]()
    {
        mGeometryPrototypeSelected = false;
        mPointLightPrototypeSelected = false;
        mSpotLightPrototypeSelected = false;
        mRectLightPrototypeSelected = false;
        mAudioEmitterPrototypeSelected = false;
        mDecalPrototypeSelected = false;
        mRainPrototypeSelected = false;
        mParticleSystemPrototypeSelected = false;
        mTerrainPrototypeSelected = false;
        mWaterPrototypeSelected = false;
        mVegetationPrototypeSelected = false;
    };

    if (QtUi::Selectable("Geometry", mGeometryPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mGeometryPrototypeSelected;
        clearPrototypes();
        mGeometryPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Point Light", mPointLightPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mPointLightPrototypeSelected;
        clearPrototypes();
        mPointLightPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Spot Light", mSpotLightPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mSpotLightPrototypeSelected;
        clearPrototypes();
        mSpotLightPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Rect Light", mRectLightPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mRectLightPrototypeSelected;
        clearPrototypes();
        mRectLightPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Audio Emitter", mAudioEmitterPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mAudioEmitterPrototypeSelected;
        clearPrototypes();
        mAudioEmitterPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Decal", mDecalPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mDecalPrototypeSelected;
        clearPrototypes();
        mDecalPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Rain", mRainPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mRainPrototypeSelected;
        clearPrototypes();
        mRainPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Particle System", mParticleSystemPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mParticleSystemPrototypeSelected;
        clearPrototypes();
        mParticleSystemPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Vegetation Area", mVegetationPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mVegetationPrototypeSelected;
        clearPrototypes();
        mVegetationPrototypeSelected = shouldSelect;
    }

    if (QtUi::Selectable("Terrain", mTerrainPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mTerrainPrototypeSelected;
        clearPrototypes();
        mTerrainPrototypeSelected = shouldSelect;
        if (shouldSelect && QtUi::IsMouseDoubleClicked(QtUiMouseButton_Left))
        {
            CreateTerrainFromRawFile();
            mTerrainPrototypeSelected = false;
        }
    }

    if (QtUi::Selectable("Water", mWaterPrototypeSelected, QtUiSelectableFlags_AllowDoubleClick))
    {
        const bool shouldSelect = !mWaterPrototypeSelected;
        clearPrototypes();
        mWaterPrototypeSelected = shouldSelect;
    }

    QtUi::End();
}

// ---------------------------------------------------------------------------
// DrawPropertiesPanel
// ---------------------------------------------------------------------------

void Editor::DrawPropertiesPanel(Entity* selectedEntity, AudioManager* audioManager)
{
    if (!QtUi::Begin("Properties", &mShowPropertiesPanel,
        QtUiWindowFlags_NoCollapse))
    {
        QtUi::End();
        return;
    }

    if (!selectedEntity)
    {
        QtUi::TextDisabled("No entity selected.");
        QtUi::End();
        return;
    }

    static char entityNameBuffer[256] = {};
    strcpy_s(entityNameBuffer, selectedEntity->Name.c_str());
    if (QtUi::InputText("Name", entityNameBuffer, std::size(entityNameBuffer)))
    {
        selectedEntity->Name = entityNameBuffer;
        MarkSceneChanged();
    }

    QtUi::Separator();

    if (QtUi::CollapsingHeader("Transform", QtUiTreeNodeFlags_DefaultOpen))
    {
        if (QtUi::DragFloat3("Position", &selectedEntity->Transform.Position.x, 0.01f))
            MarkSceneChanged();
        float rotationDegrees[3]
        {
            RadiansToDegrees(selectedEntity->Transform.Rotation.x),
            RadiansToDegrees(selectedEntity->Transform.Rotation.y),
            RadiansToDegrees(selectedEntity->Transform.Rotation.z)
        };
        if (QtUi::DragFloat3("Rotation", rotationDegrees, 0.5f))
        {
            selectedEntity->Transform.Rotation.x = DegreesToRadians(rotationDegrees[0]);
            selectedEntity->Transform.Rotation.y = DegreesToRadians(rotationDegrees[1]);
            selectedEntity->Transform.Rotation.z = DegreesToRadians(rotationDegrees[2]);
            MarkSceneChanged();
        }
        if (QtUi::DragFloat3("Scale",    &selectedEntity->Transform.Scale.x,    0.01f))
            MarkSceneChanged();
    }

    if (selectedEntity->PointLight.has_value())
    {
        if (QtUi::CollapsingHeader("Point Light", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& pl = *selectedEntity->PointLight;

            const char* lightTypeNames[] = { "Point", "Spot", "Rect" };
            int lightTypeIndex = static_cast<int>(pl.Type);
            if (QtUi::Combo("Type##pl", &lightTypeIndex, lightTypeNames, static_cast<int>(std::size(lightTypeNames))))
            {
                pl.Type = static_cast<LightType>(lightTypeIndex);
                MarkSceneChanged();
            }
            if (pl.Type != LightType::Point)
            {
                QtUi::TextDisabled("Emits along the entity's local -Z: unrotated points straight down.");
            }

            if (QtUi::DragFloat("Intensity (lm)", &pl.IntensityLumens, 10.0f, 0.0f, 100000.0f)) MarkSceneChanged();
            if (QtUi::DragFloat("Radius",         &pl.Radius,          0.1f,  0.0f, 1000.0f)) MarkSceneChanged();
            float col[3] = { pl.ColorR, pl.ColorG, pl.ColorB };
            if (QtUi::ColorEdit3("Color##pl", col)) { pl.ColorR = col[0]; pl.ColorG = col[1]; pl.ColorB = col[2]; MarkSceneChanged(); }
            if (QtUi::Checkbox("Cast Shadows##pl",          &pl.CastShadows)) MarkSceneChanged();
            if (QtUi::Checkbox("Affect Volumetric Fog##pl", &pl.AffectVolumetricFog)) MarkSceneChanged();
            if (QtUi::Checkbox("Affect Global Illumination##pl", &pl.AffectGlobalIllumination)) MarkSceneChanged();
            QtUi::SetItemTooltip(
                "Whether this light contributes to the indirect bounce - ray-traced GI,\n"
                "radiance probes and cascades. Turn it off for a cinematic key or rim\n"
                "light that should shape the shot without bouncing colour off the walls.\n"
                "The direct contribution is unchanged either way.");
            QtUi::BeginDisabled(!pl.AffectGlobalIllumination);
            if (QtUi::SliderFloat("GI Contribution##pl", &pl.GiContribution, 0.0f, 4.0f, "%.2f"))
            {
                pl.GiContribution = (std::max)(pl.GiContribution, 0.0f);
                MarkSceneChanged();
            }
            QtUi::SetItemTooltip("Scales the indirect bounce only, leaving the direct light at its authored intensity.");
            QtUi::EndDisabled();
            if (QtUi::DragFloat("Falloff Exponent", &pl.FalloffExponent, 0.01f, 0.1f, 10.0f)) MarkSceneChanged();
            if (QtUi::DragFloat("Source Radius",    &pl.SourceRadius,    0.001f, 0.0f, 1.0f)) MarkSceneChanged();
            if (QtUi::Checkbox("Use Temperature",   &pl.UseTemperature)) MarkSceneChanged();
            if (pl.UseTemperature)
                if (QtUi::DragFloat("Temperature (K)", &pl.TemperatureKelvin, 100.0f, 1000.0f, 20000.0f)) MarkSceneChanged();

            if (pl.Type == LightType::Spot)
            {
                QtUi::SeparatorText("Cone");
                if (QtUi::SliderFloat("Inner Angle##pl", &pl.SpotInnerConeDegrees, 0.0f, 179.0f, "%.1f deg"))
                {
                    pl.SpotInnerConeDegrees = (std::min)(pl.SpotInnerConeDegrees, pl.SpotOuterConeDegrees);
                    MarkSceneChanged();
                }
                QtUi::SetItemTooltip("Full angle of the hotspot, which receives the light's full intensity.");
                if (QtUi::SliderFloat("Outer Angle##pl", &pl.SpotOuterConeDegrees, 0.1f, 179.0f, "%.1f deg"))
                {
                    pl.SpotInnerConeDegrees = (std::min)(pl.SpotInnerConeDegrees, pl.SpotOuterConeDegrees);
                    MarkSceneChanged();
                }
                QtUi::SetItemTooltip("Full angle where the cone reaches zero. Equal to the inner angle gives a hard edge.");
            }
            else if (pl.Type == LightType::Rect)
            {
                QtUi::SeparatorText("Rectangle");
                if (QtUi::DragFloat("Width##pl", &pl.RectWidth, 0.01f, 0.001f, 1000.0f, "%.3f m"))
                    MarkSceneChanged();
                if (QtUi::DragFloat("Height##pl", &pl.RectHeight, 0.01f, 0.001f, 1000.0f, "%.3f m"))
                    MarkSceneChanged();
                QtUi::SetItemTooltip("The panel lies in the entity's local XY plane. A bigger panel gives softer shadows and a longer highlight.");
                if (QtUi::Checkbox("Two Sided##pl", &pl.RectTwoSided))
                    MarkSceneChanged();
                QtUi::SetItemTooltip("Off suits a window or a wall-mounted panel; on suits a floating strip lighting both ways.");
            }

            QtUi::SeparatorText("Light Style");
            DrawLightStyleControls(
                "pl",
                pl.Style,
                pl.StyleSpeed,
                pl.StyleAmplitude,
                pl.StylePhaseOffset,
                pl.CustomStylePattern);
        }
    }

    if (selectedEntity->Mesh.has_value())
    {
        if (QtUi::CollapsingHeader("Mesh", QtUiTreeNodeFlags_DefaultOpen))
        {
            MeshComponent& meshComponent = *selectedEntity->Mesh;

            QtUi::TextWrapped("Geometry: %s", meshComponent.MeshPath.empty() ? "(none)" : meshComponent.MeshPath.c_str());
            if (QtUi::Button("Select Geometry (.ptero)"))
            {
                std::string updatedMeshPath = meshComponent.MeshPath;
                if (PromptForDataFile(DX12Context_GetWindowHandle(), "Select Geometry", "Ptero Geometry\0*.ptero\0All Files\0*.*\0", updatedMeshPath))
                {
                    meshComponent.MeshPath = updatedMeshPath;
                    meshComponent.MeshAsset.reset();
                    MarkSceneChanged();

                    const std::string defaultMaterialPath = FindDefaultMaterialPathForMesh(meshComponent.MeshPath);
                    if (!defaultMaterialPath.empty())
                    {
                        meshComponent.MaterialPath = defaultMaterialPath;
                    }
                }
            }
            QtUi::SameLine();
            if (QtUi::Button("Clear Geometry"))
            {
                meshComponent.MeshPath.clear();
                meshComponent.MeshAsset.reset();
                MarkSceneChanged();
            }

            QtUi::Spacing();
            QtUi::TextWrapped("Material: %s", meshComponent.MaterialPath.empty() ? "(none)" : meshComponent.MaterialPath.c_str());
            if (QtUi::Button("Select Material"))
            {
                std::string updatedMaterialPath = meshComponent.MaterialPath;
                if (PromptForDataFile(DX12Context_GetWindowHandle(), "Select Material", "Material JSON\0*.json\0All Files\0*.*\0", updatedMaterialPath))
                {
                    meshComponent.MaterialPath = updatedMaterialPath;
                    MarkSceneChanged();
                }
            }
            QtUi::SameLine();
            if (QtUi::Button("Clear Material"))
            {
                meshComponent.MaterialPath.clear();
                MarkSceneChanged();
            }

            if (!meshComponent.MeshPath.empty())
            {
                const std::string defaultMaterialPath = FindDefaultMaterialPathForMesh(meshComponent.MeshPath);
                if (!defaultMaterialPath.empty() && meshComponent.MaterialPath != defaultMaterialPath)
                {
                    if (QtUi::Button("Use Geometry Default Material"))
                    {
                        meshComponent.MaterialPath = defaultMaterialPath;
                        MarkSceneChanged();
                    }
                }
            }

            QtUi::Spacing();
            if (QtUi::DragFloat("LOD Usage Scale", &meshComponent.LodUsageScale, 0.01f, 0.1f, 8.0f, "%.2f"))
            {
                meshComponent.LodUsageScale = (std::clamp)(meshComponent.LodUsageScale, 0.1f, 8.0f);
                MarkSceneChanged();
            }

            const char* lodDebugModes[] = { "Automatic", "LOD 0", "LOD 1", "LOD 2", "LOD 3" };
            int lodDebugMode = meshComponent.DebugForcedLod + 1;
            if (QtUi::Combo("LOD Debug View", &lodDebugMode, lodDebugModes, std::size(lodDebugModes)))
            {
                meshComponent.DebugForcedLod = lodDebugMode - 1;
                MarkSceneChanged();
            }

            if (meshComponent.MeshAsset)
            {
                QtUi::Text("Available LODs: %zu", meshComponent.MeshAsset->GetLodCount());
            }
        }
    }

    if (selectedEntity->AudioEmitter.has_value())
    {
        if (QtUi::CollapsingHeader("Audio Emitter", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& ae = *selectedEntity->AudioEmitter;
            static char epBuf[512];
            strncpy_s(epBuf, ae.EventPath.c_str(), sizeof(epBuf) - 1);
            if (QtUi::InputText("Event Path##ae", epBuf, sizeof(epBuf)))
            {
                ae.EventPath = epBuf;
                MarkSceneChanged();
            }

            if (audioManager != nullptr)
            {
                const std::vector<AudioEvent>& availableEvents = audioManager->GetEvents();
                if (!availableEvents.empty())
                {
                    int selectedEventIndex = -1;
                    for (int eventIndex = 0; eventIndex < static_cast<int>(availableEvents.size()); ++eventIndex)
                    {
                        if (availableEvents[eventIndex].Path == ae.EventPath)
                        {
                            selectedEventIndex = eventIndex;
                            break;
                        }
                    }

                    const char* previewValue = selectedEventIndex >= 0
                        ? availableEvents[selectedEventIndex].Path.c_str()
                        : (ae.EventPath.empty() ? "Select Event" : ae.EventPath.c_str());

                    if (QtUi::BeginCombo("Select Event##ae", previewValue))
                    {
                        for (int eventIndex = 0; eventIndex < static_cast<int>(availableEvents.size()); ++eventIndex)
                        {
                            const bool isSelected = (selectedEventIndex == eventIndex);
                            if (QtUi::Selectable(availableEvents[eventIndex].Path.c_str(), isSelected))
                            {
                                ae.EventPath = availableEvents[eventIndex].Path;
                                strcpy_s(epBuf, ae.EventPath.c_str());
                                MarkSceneChanged();
                            }

                            if (isSelected)
                                QtUi::SetItemDefaultFocus();
                        }

                        QtUi::EndCombo();
                    }
                }
                else
                {
                    const std::filesystem::path audioDataPath = audioManager->GetAudioDataPath();
                    if (!audioDataPath.empty())
                    {
                        QtUi::TextWrapped("No audio events loaded from %s", audioDataPath.string().c_str());
                    }
                    else
                    {
                        QtUi::TextDisabled("No audio events loaded.");
                    }
                }
            }

            if (QtUi::Checkbox("Auto Play##ae", &ae.AutoPlay))
                MarkSceneChanged();
        }
    }

    if (selectedEntity->Decal.has_value())
    {
        if (QtUi::CollapsingHeader("Decal", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& dc = *selectedEntity->Decal;

            static char matBuf[512];
            strncpy_s(matBuf, dc.MaterialPath.c_str(), sizeof(matBuf) - 1);
            if (QtUi::InputText("Material##dc", matBuf, sizeof(matBuf)))
            {
                dc.MaterialPath = matBuf;
                MarkSceneChanged();
            }

            if (QtUi::DragFloat("Size X##dc", &dc.SizeX, 0.01f, 0.001f, 1000.0f))
                MarkSceneChanged();
            if (QtUi::DragFloat("Size Y##dc", &dc.SizeY, 0.01f, 0.001f, 1000.0f))
                MarkSceneChanged();
            if (QtUi::DragFloat("Size Z##dc", &dc.SizeZ, 0.01f, 0.001f, 1000.0f))
                MarkSceneChanged();
        }
    }

    if (selectedEntity->VegetationArea.has_value())
    {
        if (QtUi::CollapsingHeader("Vegetation Area", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& va = *selectedEntity->VegetationArea;

            // --- Live scatter feedback ---------------------------------------
            // "Nothing is spawning" is by far the most common problem with
            // area-based scattering, and without the rejection breakdown the
            // artist has no way to tell a too-steep slope from an unsatisfied
            // paint mask.  So it goes at the top, not buried at the bottom.
            if (mSceneRenderer != nullptr && mSelectedEntityIndex >= 0)
            {
                std::uint32_t instanceCount = 0;
                VegetationScatterStats stats{};
                bool scatterInProgress = false;

                if (mSceneRenderer->GetVegetationRenderer().GetAreaStats(
                        static_cast<std::size_t>(mSelectedEntityIndex),
                        instanceCount, stats, scatterInProgress))
                {
                    if (scatterInProgress)
                    {
                        QtUi::TextColored(UiVec4(1.0f, 0.8f, 0.2f, 1.0f), "Scattering...");
                    }
                    else
                    {
                        QtUi::Text("Instances: %u", instanceCount);
                    }

                    if (instanceCount == 0 && !scatterInProgress && stats.CandidatesConsidered > 0)
                    {
                        QtUi::TextColored(UiVec4(1.0f, 0.5f, 0.5f, 1.0f),
                            "No instances placed - see rejections below.");
                    }

                    if (QtUi::TreeNode("Scatter diagnostics"))
                    {
                        QtUi::Text("Candidates considered: %u", stats.CandidatesConsidered);
                        QtUi::Separator();
                        QtUi::Text("Outside shape:  %u", stats.RejectedOutsideShape);
                        QtUi::Text("No surface hit: %u", stats.RejectedNoSurface);
                        QtUi::Text("Slope too steep:%u", stats.RejectedSlope);
                        QtUi::Text("Altitude band:  %u", stats.RejectedAltitude);
                        QtUi::Text("Layer mask:     %u", stats.RejectedLayerMask);
                        QtUi::Text("Spacing:        %u", stats.RejectedSpacing);
                        QtUi::Text("Exclusion:      %u", stats.RejectedExclusion);
                        QtUi::Text("Artist override:%u", stats.RejectedOverride);
                        if (stats.ClampedByInstanceCap > 0)
                        {
                            QtUi::TextColored(UiVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                "Clamped by instance cap: %u", stats.ClampedByInstanceCap);
                        }
                        QtUi::TreePop();
                    }
                }
                else
                {
                    QtUi::TextDisabled("No scatter yet.");
                }

                if (QtUi::Button("Regenerate##veg"))
                {
                    mSceneRenderer->GetVegetationRenderer().RequestRegenerate(
                        static_cast<std::size_t>(mSelectedEntityIndex));
                }
                QtUi::SameLine();
                QtUi::TextDisabled("(auto-regenerates when rules change)");
            }

            QtUi::SeparatorText("Volume");

            const char* shapeNames[] = { "Box", "Sphere", "Polygon" };
            int shapeIndex = static_cast<int>(va.Shape);
            if (QtUi::Combo("Shape##veg", &shapeIndex, shapeNames, std::size(shapeNames)))
            {
                va.Shape = static_cast<VegetationAreaShape>(shapeIndex);
                MarkSceneChanged();
            }

            if (va.Shape == VegetationAreaShape::Sphere)
            {
                if (QtUi::DragFloat("Radius##veg", &va.ExtentX, 0.25f, 0.01f, 5000.0f))
                    MarkSceneChanged();
            }
            else
            {
                if (va.Shape == VegetationAreaShape::Box)
                {
                    if (QtUi::DragFloat("Half Extent X##veg", &va.ExtentX, 0.25f, 0.01f, 5000.0f))
                        MarkSceneChanged();
                    if (QtUi::DragFloat("Half Extent Y##veg", &va.ExtentY, 0.25f, 0.01f, 5000.0f))
                        MarkSceneChanged();
                }
                else
                {
                    QtUi::TextDisabled("Polygon footprint: %zu points", va.PolygonPoints.size());
                    QtUi::TextDisabled("(edit points in the viewport)");
                }

                if (QtUi::DragFloat("Half Height Z##veg", &va.ExtentZ, 0.25f, 0.01f, 5000.0f))
                    MarkSceneChanged();
            }

            int seed = static_cast<int>(va.Seed);
            if (QtUi::DragInt("Seed##veg", &seed, 1.0f, 0, 1000000))
            {
                va.Seed = static_cast<std::uint32_t>((std::max)(seed, 0));
                MarkSceneChanged();
            }
            QtUi::SameLine();
            if (QtUi::Button("Reroll##veg"))
            {
                // Reshuffles the whole distribution without touching any other
                // rule -- the fastest way out of an arrangement that looks
                // wrong for reasons the sliders cannot express.
                va.Seed = va.Seed * 1664525u + 1013904223u;
                MarkSceneChanged();
            }

            QtUi::SeparatorText("Snapping");
            if (QtUi::Checkbox("Snap to terrain##veg", &va.SnapToTerrain))
                MarkSceneChanged();
            if (QtUi::Checkbox("Snap to geometry##veg", &va.SnapToGeometry))
                MarkSceneChanged();
            if (QtUi::DragFloat("Max snap distance##veg", &va.SnapMaxDistance, 1.0f, 0.01f, 10000.0f))
                MarkSceneChanged();

            if (QtUi::Checkbox("Exclusion volume##veg", &va.IsExclusionVolume))
                MarkSceneChanged();
            if (QtUi::IsItemHovered())
            {
                QtUi::SetTooltip(
                    "Removes vegetation from other areas instead of adding any.\n"
                    "Use to carve roads, clearings and building footprints.");
            }

            if (QtUi::Checkbox("Show bounds##veg", &va.ShowBounds))
                MarkSceneChanged();

            // --- Layers --------------------------------------------------------
            QtUi::SeparatorText("Layers");

            if (QtUi::Button("Add layer##veg")
                && va.Layers.size() < static_cast<std::size_t>(kVegetationMaxLayers))
            {
                va.Layers.emplace_back();
                va.ActiveLayer = static_cast<int>(va.Layers.size()) - 1;
                MarkSceneChanged();
            }

            if (va.Layers.size() >= static_cast<std::size_t>(kVegetationMaxLayers))
            {
                QtUi::SameLine();
                QtUi::TextDisabled("(max %d)", kVegetationMaxLayers);
            }

            int layerToRemove = -1;

            for (int layerIndex = 0; layerIndex < static_cast<int>(va.Layers.size()); ++layerIndex)
            {
                VegetationLayer& layer = va.Layers[layerIndex];

                QtUi::PushID(layerIndex);

                const std::string label = layer.Name.empty()
                    ? ("Layer " + std::to_string(layerIndex))
                    : layer.Name;

                if (QtUi::TreeNodeEx(label.c_str(),
                        layerIndex == va.ActiveLayer ? QtUiTreeNodeFlags_DefaultOpen : 0))
                {
                    va.ActiveLayer = layerIndex;

                    if (QtUi::Checkbox("Enabled", &layer.Enabled))
                        MarkSceneChanged();

                    char nameBuf[128];
                    strncpy_s(nameBuf, layer.Name.c_str(), sizeof(nameBuf) - 1);
                    if (QtUi::InputText("Name", nameBuf, sizeof(nameBuf)))
                    {
                        layer.Name = nameBuf;
                        MarkSceneChanged();
                    }

                    char meshBuf[512];
                    strncpy_s(meshBuf, layer.MeshPath.c_str(), sizeof(meshBuf) - 1);
                    if (QtUi::InputText("Mesh", meshBuf, sizeof(meshBuf)))
                    {
                        layer.MeshPath = meshBuf;
                        MarkSceneChanged();
                    }

                    char matBuf[512];
                    strncpy_s(matBuf, layer.MaterialPath.c_str(), sizeof(matBuf) - 1);
                    if (QtUi::InputText("Material", matBuf, sizeof(matBuf)))
                    {
                        layer.MaterialPath = matBuf;
                        MarkSceneChanged();
                    }
                    if (QtUi::IsItemHovered())
                    {
                        QtUi::SetTooltip(
                            "Foliage materials should set alphaCutoff so leaf\n"
                            "cards clip to their texture instead of drawing as\n"
                            "opaque quads.");
                    }

                    QtUi::SeparatorText("Density");
                    if (QtUi::DragFloat("Per m2", &layer.Density, 0.01f, 0.0f, 200.0f, "%.3f"))
                        MarkSceneChanged();
                    if (QtUi::DragFloat("Spacing radius", &layer.CollisionRadius, 0.01f, 0.0f, 50.0f))
                        MarkSceneChanged();
                    if (QtUi::IsItemHovered())
                        QtUi::SetTooltip("Discards instances closer than this to one already placed. 0 disables.");

                    QtUi::SeparatorText("Variation");
                    if (QtUi::DragFloatRange2("Scale", &layer.MinScale, &layer.MaxScale, 0.01f, 0.01f, 20.0f))
                        MarkSceneChanged();
                    if (QtUi::Checkbox("Random yaw", &layer.RandomYaw))
                        MarkSceneChanged();
                    if (QtUi::DragFloat("Max tilt (deg)", &layer.MaxTiltDegrees, 0.1f, 0.0f, 45.0f))
                        MarkSceneChanged();

                    QtUi::SeparatorText("Surface filtering");
                    if (QtUi::SliderFloat("Align to normal", &layer.AlignToNormal, 0.0f, 1.0f))
                        MarkSceneChanged();
                    if (QtUi::IsItemHovered())
                        QtUi::SetTooltip("0 keeps instances upright, 1 follows the slope. Trees want 0, ground cover ~0.3.");
                    if (QtUi::DragFloat("Max slope (deg)", &layer.MaxSlopeDegrees, 0.5f, 0.0f, 90.0f))
                        MarkSceneChanged();
                    if (QtUi::DragFloat("Min altitude", &layer.MinAltitude, 0.5f, -100000.0f, 100000.0f))
                        MarkSceneChanged();
                    if (QtUi::DragFloat("Max altitude", &layer.MaxAltitude, 0.5f, -100000.0f, 100000.0f))
                        MarkSceneChanged();
                    if (QtUi::DragFloat("Sink offset", &layer.SinkOffset, 0.005f, 0.0f, 5.0f))
                        MarkSceneChanged();

                    QtUi::SeparatorText("Terrain layer mask");
                    int mask = layer.TerrainLayerMask;
                    if (QtUi::SliderInt("Paint layer", &mask, -1, kTerrainMaxLayers - 1,
                            mask < 0 ? "disabled" : "%d"))
                    {
                        layer.TerrainLayerMask = mask;
                        MarkSceneChanged();
                    }
                    if (QtUi::IsItemHovered())
                    {
                        QtUi::SetTooltip(
                            "Only spawn where this terrain paint layer is painted.\n"
                            "Lets a grass layer follow the painted grass automatically.\n"
                            "Has no effect on instances landing on mesh geometry.");
                    }
                    if (layer.TerrainLayerMask >= 0)
                    {
                        if (QtUi::SliderFloat("Mask threshold", &layer.TerrainLayerThreshold, 0.0f, 1.0f))
                            MarkSceneChanged();
                    }

                    QtUi::SeparatorText("Rendering");
                    if (QtUi::DragFloat("Cull distance", &layer.CullDistance, 1.0f, 1.0f, 20000.0f))
                        MarkSceneChanged();
                    if (QtUi::SliderFloat("Fade fraction", &layer.FadeFraction, 0.0f, 1.0f))
                        MarkSceneChanged();
                    if (QtUi::Checkbox("Cast shadows", &layer.CastShadows))
                        MarkSceneChanged();

                    char billboardBuf[512];
                    strncpy_s(billboardBuf, layer.BillboardTexturePath.c_str(), sizeof(billboardBuf) - 1);
                    if (QtUi::InputText("Billboard card", billboardBuf, sizeof(billboardBuf)))
                    {
                        layer.BillboardTexturePath = billboardBuf;
                        MarkSceneChanged();
                    }
                    if (QtUi::IsItemHovered())
                    {
                        QtUi::SetTooltip(
                            "Optional .dds drawn as a camera-facing card at the\n"
                            "farthest LOD, instead of the mesh.  Worth it for trees;\n"
                            "leave empty for grass, where a card costs as much as\n"
                            "the tuft it replaces.");
                    }
                    if (!layer.BillboardTexturePath.empty())
                    {
                        if (QtUi::DragFloat("Billboard scale", &layer.BillboardScale, 0.01f, 0.01f, 10.0f))
                            MarkSceneChanged();
                    }
                    if (QtUi::Checkbox("Contribute to ray tracing", &layer.ContributeToRayTracing))
                        MarkSceneChanged();
                    if (QtUi::IsItemHovered())
                    {
                        QtUi::SetTooltip(
                            "Adds this layer's nearest LOD to the ray tracing scene.\n"
                            "Alpha-tested foliage is expensive to trace - leave this\n"
                            "off for grass and enable it only for hero trees.");
                    }

                    QtUi::SeparatorText("Wind and interaction");
                    const char* bendNames[] = { "None", "Tree", "Grass" };
                    int bendIndex = static_cast<int>(layer.BendModel);
                    if (QtUi::Combo("Bend model", &bendIndex, bendNames, std::size(bendNames)))
                    {
                        layer.BendModel = static_cast<VegetationBendModel>(bendIndex);
                        MarkSceneChanged();
                    }
                    if (QtUi::IsItemHovered())
                    {
                        QtUi::SetTooltip(
                            "Tree: trunk sway + branch sway + leaf flutter, wind only.\n"
                            "Grass: single-hinge blade bend that also reacts to the\n"
                            "runtime interaction map (players, props).");
                    }

                    if (QtUi::SliderFloat("Wind influence", &layer.WindInfluence, 0.0f, 4.0f))
                        MarkSceneChanged();
                    if (QtUi::SliderFloat("Stiffness", &layer.Stiffness, 0.05f, 8.0f))
                        MarkSceneChanged();
                    if (QtUi::SliderFloat("Flutter amount", &layer.FlutterAmount, 0.0f, 4.0f))
                        MarkSceneChanged();
                    if (layer.BendModel == VegetationBendModel::Grass)
                    {
                        if (QtUi::SliderFloat("Interaction influence", &layer.InteractionInfluence, 0.0f, 4.0f))
                            MarkSceneChanged();
                    }

                    QtUi::Separator();
                    if (QtUi::Button("Remove layer"))
                        layerToRemove = layerIndex;

                    QtUi::TreePop();
                }

                QtUi::PopID();
            }

            if (layerToRemove >= 0)
            {
                va.Layers.erase(va.Layers.begin() + layerToRemove);
                if (va.ActiveLayer >= static_cast<int>(va.Layers.size()))
                    va.ActiveLayer = (std::max)(0, static_cast<int>(va.Layers.size()) - 1);
                MarkSceneChanged();
            }
        }
    }

    if (selectedEntity->Rain.has_value())
    {
        if (QtUi::CollapsingHeader("Rain", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& rc = *selectedEntity->Rain;

            if (QtUi::Checkbox("Enabled##rain", &rc.Enabled))
                MarkSceneChanged();

            QtUi::SeparatorText("Physics");
            float wind[3] = { rc.WindX, rc.WindY, rc.WindZ };
            if (QtUi::DragFloat3("Wind##rain", wind, 0.05f, -20.0f, 20.0f))
            {
                rc.WindX = wind[0]; rc.WindY = wind[1]; rc.WindZ = wind[2];
                MarkSceneChanged();
            }
            if (QtUi::DragFloat("Gravity##rain", &rc.Gravity, 0.1f, 0.0f, 50.0f))
                MarkSceneChanged();

            QtUi::SeparatorText("Bounding Box");
            if (QtUi::DragFloat("Extent X##rain", &rc.BoxExtentX, 0.5f, 1.0f, 200.0f))
                MarkSceneChanged();
            if (QtUi::DragFloat("Extent Y##rain", &rc.BoxExtentY, 0.5f, 1.0f, 200.0f))
                MarkSceneChanged();
            if (QtUi::DragFloat("Extent Z##rain", &rc.BoxExtentZ, 0.5f, 1.0f, 200.0f))
                MarkSceneChanged();

            QtUi::SeparatorText("Visual");
            if (QtUi::DragFloat("Intensity##rain", &rc.Intensity, 0.01f, 0.0f, 8.0f))
                MarkSceneChanged();
            if (QtUi::DragFloat("Streak Length##rain", &rc.StreakLength, 0.005f, 0.01f, 2.0f))
                MarkSceneChanged();
            float col[4] = { rc.ColorR, rc.ColorG, rc.ColorB, rc.ColorA };
            if (QtUi::ColorEdit4("Color##rain", col))
            {
                rc.ColorR = col[0]; rc.ColorG = col[1];
                rc.ColorB = col[2]; rc.ColorA = col[3];
                MarkSceneChanged();
            }
            if (QtUi::DragFloat("Wetness##rain", &rc.WetnessIntensity, 0.01f, 0.0f, 1.0f))
                MarkSceneChanged();
        }
    }

    if (selectedEntity->ParticleSystem.has_value())
    {
        if (QtUi::CollapsingHeader("Particle System", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& ps = *selectedEntity->ParticleSystem;

            if (QtUi::Checkbox("Enabled##ps", &ps.Enabled))
                MarkSceneChanged();

            QtUi::SeparatorText("Material");
            QtUi::TextWrapped("Material: %s", ps.MaterialPath.empty() ? "(none)" : ps.MaterialPath.c_str());
            QtUi::TextDisabled("Base Color is the sprite, Emissive Color the glow. Set \"Particle Material\" in the Material Editor.");
            if (QtUi::Button("Select...##psmat"))
            {
                std::string updatedMaterialPath = ps.MaterialPath;
                if (PromptForDataFile(
                        DX12Context_GetWindowHandle(),
                        "Select Particle Material",
                        "Material JSON\0*.json\0All Files\0*.*\0",
                        updatedMaterialPath))
                {
                    ps.MaterialPath = updatedMaterialPath;
                    MarkSceneChanged();
                }
            }
            QtUi::SameLine();
            if (QtUi::Button("Clear##psmat"))
            {
                ps.MaterialPath.clear();
                MarkSceneChanged();
            }

            QtUi::SeparatorText("Emission");
            if (QtUi::Checkbox("Burst##ps", &ps.Burst))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Off: emit continuously at Spawn Rate. On: emit Burst Count particles every Burst Interval.");

            if (ps.Burst)
            {
                if (QtUi::DragInt("Burst Count##ps", &ps.BurstCount, 1.0f, 0, 4096))
                    MarkSceneChanged();
                if (QtUi::DragFloat("Burst Interval##ps", &ps.BurstInterval, 0.01f, 0.01f, 60.0f, "%.2f s"))
                    MarkSceneChanged();
            }
            else
            {
                if (QtUi::DragFloat("Spawn Rate##ps", &ps.SpawnRate, 1.0f, 0.0f, 20000.0f, "%.0f /s"))
                    MarkSceneChanged();
            }

            if (QtUi::DragFloat("Lifetime##ps", &ps.Lifetime, 0.01f, 0.01f, 60.0f, "%.2f s"))
                MarkSceneChanged();
            if (QtUi::SliderFloat("Lifetime Variance##ps", &ps.LifetimeVariance, 0.0f, 0.95f, "%.2f"))
                MarkSceneChanged();
            if (QtUi::DragInt("Max Particles##ps", &ps.MaxParticles, 16.0f, 1, kParticleMaxPerSystem))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Ceiling on the buffer. The system only allocates what Spawn Rate x Lifetime actually needs.");
            if (QtUi::Checkbox("Prewarm##ps", &ps.Prewarm))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Start already at steady state instead of building up from empty on load.");

            QtUi::SeparatorText("Shape");
            const char* shapeNames[] = { "Point", "Sphere", "Box", "Cone", "Disc", "Edge" };
            int shapeIndex = static_cast<int>(ps.Shape);
            if (QtUi::Combo("Shape##ps", &shapeIndex, shapeNames, static_cast<int>(std::size(shapeNames))))
            {
                ps.Shape = static_cast<ParticleEmitterShape>(shapeIndex);
                MarkSceneChanged();
            }
            QtUi::TextDisabled("The emitter faces the entity's local +Z (world up when unrotated).");

            if (ps.Shape == ParticleEmitterShape::Box)
            {
                float extents[3] = { ps.ShapeExtents.x, ps.ShapeExtents.y, ps.ShapeExtents.z };
                if (QtUi::DragFloat3("Extents##ps", extents, 0.01f, 0.0f, 100.0f))
                {
                    ps.ShapeExtents = { extents[0], extents[1], extents[2] };
                    MarkSceneChanged();
                }
            }
            else if (ps.Shape != ParticleEmitterShape::Point)
            {
                if (QtUi::DragFloat("Radius##ps", &ps.ShapeRadius, 0.01f, 0.0f, 100.0f, "%.3f m"))
                    MarkSceneChanged();
            }

            if (ps.Shape == ParticleEmitterShape::Cone)
            {
                if (QtUi::SliderFloat("Cone Angle##ps", &ps.ConeAngleDegrees, 0.0f, 180.0f, "%.1f deg"))
                    MarkSceneChanged();
            }

            if (ps.Shape == ParticleEmitterShape::Sphere ||
                ps.Shape == ParticleEmitterShape::Cone ||
                ps.Shape == ParticleEmitterShape::Disc)
            {
                if (QtUi::SliderFloat("Shell Bias##ps", &ps.ShapeShellBias, 0.0f, 1.0f, "%.2f"))
                    MarkSceneChanged();
                QtUi::SetItemTooltip("0 fills the shape, 1 puts every particle on its surface. A ring of flame around a log is a Disc at 1.");
            }

            QtUi::SeparatorText("Motion");
            if (QtUi::DragFloat("Initial Speed##ps", &ps.InitialSpeed, 0.01f, 0.0f, 100.0f, "%.2f m/s"))
                MarkSceneChanged();
            if (QtUi::SliderFloat("Speed Variance##ps", &ps.SpeedVariance, 0.0f, 1.0f, "%.2f"))
                MarkSceneChanged();

            float acceleration[3] = { ps.Acceleration.x, ps.Acceleration.y, ps.Acceleration.z };
            if (QtUi::DragFloat3("Acceleration##ps", acceleration, 0.05f, -50.0f, 50.0f))
            {
                ps.Acceleration = { acceleration[0], acceleration[1], acceleration[2] };
                MarkSceneChanged();
            }
            QtUi::SetItemTooltip("Positive Z for fire (hot gas rising); -9.8 Z for debris that falls.");

            if (QtUi::DragFloat("Drag##ps", &ps.Drag, 0.01f, 0.0f, 20.0f, "%.2f"))
                MarkSceneChanged();
            if (QtUi::SliderFloat("Wind Influence##ps", &ps.WindInfluence, 0.0f, 4.0f, "%.2f"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("How strongly the scene-wide wind pushes this system.");

            if (QtUi::DragFloat("Turbulence##ps", &ps.TurbulenceStrength, 0.01f, 0.0f, 20.0f, "%.2f"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("What turns a cone of sprites into something that licks and curls. The main knob for fire.");
            if (QtUi::DragFloat("Turbulence Scale##ps", &ps.TurbulenceFrequency, 0.01f, 0.01f, 10.0f, "%.2f"))
                MarkSceneChanged();
            if (QtUi::DragFloat("Turbulence Speed##ps", &ps.TurbulenceSpeed, 0.01f, 0.0f, 10.0f, "%.2f"))
                MarkSceneChanged();
            if (QtUi::DragFloat("Vortex##ps", &ps.VortexStrength, 0.01f, -20.0f, 20.0f, "%.2f"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Swirl about the emitter's up axis, for a flame that twists as it rises.");

            QtUi::SeparatorText("Size and Rotation");
            if (QtUi::DragFloat("Start Size##ps", &ps.StartSize, 0.005f, 0.0f, 50.0f, "%.3f m"))
                MarkSceneChanged();
            if (QtUi::DragFloat("End Size##ps", &ps.EndSize, 0.005f, 0.0f, 50.0f, "%.3f m"))
                MarkSceneChanged();
            if (QtUi::SliderFloat("Size Variance##ps", &ps.SizeVariance, 0.0f, 1.0f, "%.2f"))
                MarkSceneChanged();
            if (QtUi::DragFloat("Rotation Speed##ps", &ps.RotationSpeedDegrees, 1.0f, -720.0f, 720.0f, "%.0f deg/s"))
                MarkSceneChanged();
            if (QtUi::SliderFloat("Rotation Variance##ps", &ps.RotationSpeedVariance, 0.0f, 1.0f, "%.2f"))
                MarkSceneChanged();
            if (QtUi::SliderFloat("Random Start Rotation##ps", &ps.RandomStartRotation, 0.0f, 1.0f, "%.2f"))
                MarkSceneChanged();

            QtUi::SeparatorText("Color Over Life");
            float colorStart[4] = { ps.ColorStart.x, ps.ColorStart.y, ps.ColorStart.z, ps.ColorStart.w };
            if (QtUi::ColorEdit4("Start##pscol", colorStart))
            {
                ps.ColorStart = { colorStart[0], colorStart[1], colorStart[2], colorStart[3] };
                MarkSceneChanged();
            }
            float colorMid[4] = { ps.ColorMid.x, ps.ColorMid.y, ps.ColorMid.z, ps.ColorMid.w };
            if (QtUi::ColorEdit4("Mid##pscol", colorMid))
            {
                ps.ColorMid = { colorMid[0], colorMid[1], colorMid[2], colorMid[3] };
                MarkSceneChanged();
            }
            float colorEnd[4] = { ps.ColorEnd.x, ps.ColorEnd.y, ps.ColorEnd.z, ps.ColorEnd.w };
            if (QtUi::ColorEdit4("End##pscol", colorEnd))
            {
                ps.ColorEnd = { colorEnd[0], colorEnd[1], colorEnd[2], colorEnd[3] };
                MarkSceneChanged();
            }
            if (QtUi::SliderFloat("Mid Point##pscol", &ps.ColorMidPoint, 0.01f, 0.99f, "%.2f"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Where the middle key sits along the particle's life. Low values hold a flame's hot core longer.");
            if (QtUi::DragFloat("Emissive Intensity##ps", &ps.EmissiveIntensity, 0.05f, 0.0f, 200.0f, "%.2f"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Multiplies the material's emissive colour. The main brightness control for fire.");

            QtUi::SeparatorText("Flipbook");
            QtUi::TextDisabled("Leave at 1x1 to inherit the material's own atlas layout.");
            if (QtUi::SliderInt("Columns##ps", &ps.FlipbookColumns, 1, 16))
                MarkSceneChanged();
            if (QtUi::SliderInt("Rows##ps", &ps.FlipbookRows, 1, 16))
                MarkSceneChanged();
            if (QtUi::DragFloat("Frames Per Second##ps", &ps.FlipbookFps, 0.5f, 0.0f, 120.0f, "%.1f"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("0 spreads the whole atlas across the particle's lifetime, which is what a hand-authored flame sheet wants.");
            if (QtUi::Checkbox("Blend Frames##ps", &ps.FlipbookBlendFrames))
                MarkSceneChanged();
            if (QtUi::Checkbox("Random Start Frame##ps", &ps.FlipbookRandomStartFrame))
                MarkSceneChanged();

            QtUi::SeparatorText("Rendering");
            const char* facingNames[] = { "Billboard", "Velocity Stretched", "Horizontal", "Vertical" };
            int facingIndex = static_cast<int>(ps.Facing);
            if (QtUi::Combo("Facing##ps", &facingIndex, facingNames, static_cast<int>(std::size(facingNames))))
            {
                ps.Facing = static_cast<ParticleFacingMode>(facingIndex);
                MarkSceneChanged();
            }
            if (ps.Facing == ParticleFacingMode::VelocityStretched)
            {
                if (QtUi::DragFloat("Stretch##ps", &ps.StretchFactor, 0.005f, 0.0f, 4.0f, "%.3f"))
                    MarkSceneChanged();
            }
            if (QtUi::Checkbox("Soft Particles##ps", &ps.SoftParticles))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Fades sprites where they meet geometry, so a flame does not cut a hard line into the floor.");
            if (ps.SoftParticles)
            {
                if (QtUi::DragFloat("Soft Fade##ps", &ps.SoftFadeDistance, 0.01f, 0.001f, 10.0f, "%.3f m"))
                    MarkSceneChanged();
            }
            if (QtUi::DragFloat("Cull Distance##ps", &ps.CullDistance, 1.0f, 1.0f, 10000.0f, "%.0f m"))
                MarkSceneChanged();

            QtUi::SeparatorText("Light and Global Illumination");
            QtUi::TextWrapped(
                "An emissive system registers an analytic light standing in for the flame. "
                "That one light drives the deferred shading, the ray-traced GI bounce and "
                "the volumetric fog together.");
            if (QtUi::Checkbox("Emit Light##ps", &ps.EmitLight))
                MarkSceneChanged();

            QtUi::BeginDisabled(!ps.EmitLight);
            if (QtUi::DragFloat("Intensity (lm)##ps", &ps.LightIntensityLumens, 10.0f, 0.0f, 100000.0f, "%.0f"))
                MarkSceneChanged();
            if (QtUi::DragFloat("Light Radius##ps", &ps.LightRadius, 0.1f, 0.001f, 1000.0f, "%.2f m"))
                MarkSceneChanged();
            if (QtUi::DragFloat("Height Offset##ps", &ps.LightHeightOffset, 0.01f, -50.0f, 50.0f, "%.2f m"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("A fire's apparent light source sits inside the flame, not at its base.");
            if (QtUi::Checkbox("Color From Particles##ps", &ps.UseParticleColorForLight))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Take the light's colour from the particle gradient, so recolouring the fire recolours the light.");
            if (!ps.UseParticleColorForLight)
            {
                float lightColor[3] = { ps.LightColorR, ps.LightColorG, ps.LightColorB };
                if (QtUi::ColorEdit3("Light Color##ps", lightColor))
                {
                    ps.LightColorR = lightColor[0];
                    ps.LightColorG = lightColor[1];
                    ps.LightColorB = lightColor[2];
                    MarkSceneChanged();
                }
            }
            if (QtUi::SliderFloat("GI Contribution##ps", &ps.GiContribution, 0.0f, 4.0f, "%.2f"))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Scales the indirect bounce only. Lower it when a fire is washing out a small room's GI.");
            if (QtUi::Checkbox("Cast Shadows##pslight", &ps.LightCastShadows))
                MarkSceneChanged();
            if (QtUi::Checkbox("Affect Volumetric Fog##pslight", &ps.LightAffectVolumetricFog))
                MarkSceneChanged();

            QtUi::SeparatorText("Flicker");
            DrawLightStyleControls(
                "pslightstyle",
                ps.LightStyle,
                ps.LightStyleSpeed,
                ps.LightStyleAmplitude,
                ps.LightStylePhaseOffset,
                ps.LightCustomStylePattern);
            if (QtUi::Checkbox("Flicker The Sprites Too##ps", &ps.StyleDrivesParticleEmissive))
                MarkSceneChanged();
            QtUi::SetItemTooltip("Applies the same curve to the sprites' brightness, so the flame dims with the light it casts.");
            QtUi::EndDisabled();
        }
    }

    if (selectedEntity->Water.has_value())
    {
        if (QtUi::CollapsingHeader("Water", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& wc = *selectedEntity->Water;

            QtUi::SeparatorText("Area");
            QtUi::TextDisabled("Surface centred on the entity's position (Z = water level).");
            if (QtUi::DragFloat("Size X##water", &wc.SizeX, 0.5f, 1.0f, 100000.0f, "%.1f m"))
                MarkSceneChanged();
            if (QtUi::DragFloat("Size Y##water", &wc.SizeY, 0.5f, 1.0f, 100000.0f, "%.1f m"))
                MarkSceneChanged();
            if (QtUi::SliderInt("Resolution##water", &wc.Resolution, 2, 512))
                MarkSceneChanged();
            QtUi::TextDisabled("Grid vertices per axis (%d triangles).",
                (wc.Resolution - 1) * (wc.Resolution - 1) * 2);

            QtUi::SeparatorText("Material");
            QtUi::TextWrapped("Material: %s",
                wc.MaterialPath.empty() ? "(built-in ocean)" : wc.MaterialPath.c_str());
            if (QtUi::Button("Ocean##water"))
            {
                wc.MaterialPath = "Materials/Ocean.json";
                MarkSceneChanged();
            }
            QtUi::SameLine();
            if (QtUi::Button("Water##waterpreset"))
            {
                wc.MaterialPath = "Materials/Water.json";
                MarkSceneChanged();
            }
            QtUi::SameLine();
            if (QtUi::Button("Select...##water"))
            {
                std::string updatedMaterialPath = wc.MaterialPath;
                if (PromptForDataFile(
                        DX12Context_GetWindowHandle(),
                        "Select Water Material",
                        "Material JSON\0*.json\0All Files\0*.*\0",
                        updatedMaterialPath))
                {
                    wc.MaterialPath = updatedMaterialPath;
                    MarkSceneChanged();
                }
            }

            QtUi::SeparatorText("Waves");
            if (QtUi::DragFloat("Wave Scale##water", &wc.WaveScale, 0.01f, 0.0f, 5.0f))
                MarkSceneChanged();
            if (QtUi::DragFloat("Detail Tiling##water", &wc.DetailTiling, 0.25f, 0.0f, 200.0f))
                MarkSceneChanged();

            QtUi::TextDisabled("Wave model is material-driven: set \"useFFT\": true");
            QtUi::TextDisabled("in the material's water block for the cascaded FFT ocean.");

            QtUi::SeparatorText("Diagnostics");
            // Isolating one shading term at a time separates an optics bug from
            // a wave bug: if flat water already blows out in the Fresnel view,
            // the fault is exposure/reflection, not the wave simulation.
            const char* debugModes[] = {
                "Off", "Fresnel", "N dot V", "Normals",
                "Thickness", "Reflection (environment)", "Transmission (volume)", "Foam",
                "Neutral 0.18 (exposure test)", "Sun specular" };
            if (QtUi::Combo("Debug View##water", &wc.DebugMode, debugModes, std::size(debugModes)))
                MarkSceneChanged();
            if (wc.DebugMode != 0)
                QtUi::TextDisabled("Debug view active - shading is overridden.");
        }
    }

    if (selectedEntity->Terrain.has_value())
    {
        if (QtUi::CollapsingHeader("Terrain", QtUiTreeNodeFlags_DefaultOpen))
        {
            auto& tc = *selectedEntity->Terrain;

            QtUi::TextWrapped("Heightmap: %s",
                tc.HeightmapRawPath.empty() ? "(none)" : tc.HeightmapRawPath.c_str());
            QtUi::TextWrapped("DDS: %s",
                tc.HeightmapDdsPath.empty() ? "(none)" : tc.HeightmapDdsPath.c_str());
            QtUi::TextWrapped("Material: %s",
                tc.MaterialPath.empty() ? "(none)" : tc.MaterialPath.c_str());

            if (QtUi::Button("Select Terrain Material"))
            {
                std::string updatedMaterialPath = tc.MaterialPath;
                if (PromptForDataFile(
                    DX12Context_GetWindowHandle(),
                    "Select Terrain Material",
                    "Material JSON\0*.json\0All Files\0*.*\0",
                    updatedMaterialPath))
                {
                    tc.MaterialPath = updatedMaterialPath;
                    MarkSceneChanged();
                }
            }
            QtUi::SameLine();
            if (QtUi::Button("Clear Terrain Material"))
            {
                tc.MaterialPath.clear();
                MarkSceneChanged();
            }

            if (mTerrainRenderer != nullptr)
            {
                std::uint32_t terrainIndexCount = 0;
                int builtWidth = 0;
                int builtHeight = 0;
                bool terrainDirty = false;
                if (mSelectedEntityIndex >= 0
                    && mTerrainRenderer->GetTerrainDebugInfo(
                        static_cast<std::size_t>(mSelectedEntityIndex),
                        terrainIndexCount,
                        builtWidth,
                        builtHeight,
                        terrainDirty))
                {
                    QtUi::Text("Renderer: %ux indices, built %dx%d%s",
                        terrainIndexCount,
                        builtWidth,
                        builtHeight,
                        terrainDirty ? " (dirty)" : "");
                }
                else
                {
                    QtUi::TextDisabled("Renderer: waiting for terrain mesh upload.");
                }

                if (const char* terrainError = mTerrainRenderer->GetLastErrorMessage())
                {
                    QtUi::PushStyleColor(QtUiCol_Text, UiVec4(1.0f, 0.45f, 0.28f, 1.0f));
                    QtUi::TextWrapped("Renderer error: %s", terrainError);
                    QtUi::PopStyleColor();
                }
            }

            if (QtUi::Button("Select Heightmap..."))
            {
                std::string updatedPath = tc.HeightmapRawPath;
                if (PromptForDataFile(DX12Context_GetWindowHandle(),
                                      "Select Heightmap",
                                      "Heightmap Image\0*.raw;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.tga;*.hdr\0"
                                      "All Files\0*.*\0",
                                      updatedPath))
                {
                    tc.HeightmapRawPath = updatedPath;

                    // For image sources the importer auto-detects the
                    // resolution.  If we keep the previous terrain's Width/
                    // Height the renderer's dimension check in
                    // LoadTerrainSamples rejects the load and nothing
                    // appears, so re-detect the dimensions here and update
                    // the component to match the new image.  .raw sources
                    // carry no header, so their dimensions are left for the
                    // artist to edit in the Geometry section below.
                    std::string ext = std::filesystem::path(updatedPath).extension().string();
                    for (char& c : ext) { c = static_cast<char>(::tolower(static_cast<unsigned char>(c))); }
                    if (ext != ".raw")
                    {
                        std::vector<std::uint16_t> peekSamples;
                        int peekedWidth = 0;
                        int peekedHeight = 0;
                        std::string peekError;
                        if (HeightmapImporter::LoadImageToHeightmap(
                                updatedPath, peekSamples, peekedWidth, peekedHeight, peekError)
                            && peekedWidth > 0 && peekedHeight > 0)
                        {
                            tc.Width  = peekedWidth;
                            tc.Height = peekedHeight;
                        }
                    }

                    // Refresh the sibling DDS so downstream tools (and the
                    // Properties readout) reflect the newly selected source.
                    std::string convertError;
                    const std::string ddsPath = HeightmapImporter::ConvertRaw16ToDds(
                        updatedPath, tc.Width, tc.Height, convertError);
                    if (!ddsPath.empty())
                        tc.HeightmapDdsPath = ddsPath;

                    MarkSceneChanged();
                    if (mTerrainRenderer != nullptr)
                    {
                        mTerrainRenderer->MarkTerrainDirty(
                            static_cast<std::size_t>(mSelectedEntityIndex));
                    }
                }
            }

            QtUi::SeparatorText("Geometry");
            // We only mark the GPU mesh dirty when the user *finishes*
            // editing a slider (mouse release).  Rebuilding the
            // vertex/index buffers every frame while the user is dragging
            // stalls the GPU and can crash on allocations near the VRAM
            // ceiling.
            if (QtUi::DragInt("Width (samples)",  &tc.Width,  1.0f, 1, 8192))
                MarkSceneChanged();
            if (QtUi::IsItemDeactivatedAfterEdit() && mTerrainRenderer != nullptr)
                mTerrainRenderer->MarkTerrainDirty(static_cast<std::size_t>(mSelectedEntityIndex));

            if (QtUi::DragInt("Height (samples)", &tc.Height, 1.0f, 1, 8192))
                MarkSceneChanged();
            if (QtUi::IsItemDeactivatedAfterEdit() && mTerrainRenderer != nullptr)
                mTerrainRenderer->MarkTerrainDirty(static_cast<std::size_t>(mSelectedEntityIndex));

            if (QtUi::DragFloat("World Size (m)",   &tc.WorldSize,    1.0f, 1.0f, 100000.0f))
                MarkSceneChanged();
            if (QtUi::IsItemDeactivatedAfterEdit() && mTerrainRenderer != nullptr)
                mTerrainRenderer->MarkTerrainDirty(static_cast<std::size_t>(mSelectedEntityIndex));

            if (QtUi::DragFloat("Height Scale (m)",  &tc.HeightScale,  1.0f, 0.001f, 100000.0f))
                MarkSceneChanged();
            if (QtUi::IsItemDeactivatedAfterEdit() && mTerrainRenderer != nullptr)
                mTerrainRenderer->MarkTerrainDirty(static_cast<std::size_t>(mSelectedEntityIndex));

            if (QtUi::DragFloat("Height Offset (m)", &tc.HeightOffset, 0.1f, -100000.0f, 100000.0f))
                MarkSceneChanged();
            if (QtUi::IsItemDeactivatedAfterEdit() && mTerrainRenderer != nullptr)
                mTerrainRenderer->MarkTerrainDirty(static_cast<std::size_t>(mSelectedEntityIndex));

            QtUi::SeparatorText("Brush");
            const char* brushNames[] = { "Raise", "Lower", "Flatten", "Smooth", "Paint" };
            int brushTypeIndex = static_cast<int>(tc.Brush);
            if (QtUi::Combo("Type##terrainbrush", &brushTypeIndex,
                             brushNames, std::size(brushNames)))
            {
                tc.Brush = static_cast<TerrainComponent::BrushType>(brushTypeIndex);
                MarkSceneChanged();
            }
            if (QtUi::DragFloat("Radius (m)",  &tc.BrushRadius,  0.1f, 0.1f, 1000.0f)) MarkSceneChanged();
            if (QtUi::DragFloat("Strength (m/s)", &tc.BrushStrength, 0.01f, 0.001f, 100.0f)) MarkSceneChanged();
            if (QtUi::DragFloat("Flatten Height (m)", &tc.FlattenHeight, 0.1f, -10000.0f, 10000.0f)) MarkSceneChanged();
            if (QtUi::DragInt  ("Smooth Passes", &tc.BrushSmoothingPasses, 1, 1, 10)) MarkSceneChanged();

            QtUi::Separator();
            bool brushActive = mTerrainBrushModeActive;
            if (QtUi::Button(brushActive ? "Exit Brush Mode" : "Enter Brush Mode"))
            {
                mTerrainBrushModeActive = !brushActive;
            }
            QtUi::SameLine();
            QtUi::Checkbox("Show Tool Window", &mShowTerrainToolWindow);

            // Material paint-layer editor (mirrors the Terrain Tool window).
            bool layerSceneDirty = false;
            if (DrawTerrainLayerControls(tc, DX12Context_GetWindowHandle(), layerSceneDirty))
            {
                if (mTerrainRenderer != nullptr && mSelectedEntityIndex >= 0)
                    mTerrainRenderer->MarkTerrainDirty(static_cast<std::size_t>(mSelectedEntityIndex));
            }
            if (layerSceneDirty)
                MarkSceneChanged();

            if (!mLastTerrainBrushMessage.empty())
            {
                QtUi::Separator();
                QtUi::TextWrapped("Last brush: %s", mLastTerrainBrushMessage.c_str());
            }
        }
    }

    QtUi::End();
}

void Editor::DrawViewportResolutionWindow()
{
    if (!mShowViewportResolutionDialog)
        return;

    QtUi::SetNextWindowSize(UiVec2(320.0f, 0.0f), QtUiCond_FirstUseEver);
    if (!QtUi::Begin("Viewport Resolution", &mShowViewportResolutionDialog, QtUiWindowFlags_AlwaysAutoResize))
    {
        QtUi::End();
        return;
    }

    QtUi::InputInt("Width", &mViewportResolutionWidth);
    QtUi::InputInt("Height", &mViewportResolutionHeight);
    mViewportResolutionWidth = (std::max)(1, mViewportResolutionWidth);
    mViewportResolutionHeight = (std::max)(1, mViewportResolutionHeight);

    QtUi::SeparatorText("Presets");
    struct ResolutionPreset
    {
        const char* Label;
        int Width;
        int Height;
    };

    static constexpr ResolutionPreset presets[] =
    {
        { "1280 x 720", 1280, 720 },
        { "1600 x 900", 1600, 900 },
        { "1920 x 1080", 1920, 1080 },
        { "2560 x 1440", 2560, 1440 },
        { "3840 x 2160", 3840, 2160 }
    };

    for (int presetIndex = 0; presetIndex < static_cast<int>(std::size(presets)); ++presetIndex)
    {
        if (QtUi::Button(presets[presetIndex].Label, UiVec2(140.0f, 0.0f)))
        {
            mViewportResolutionWidth = presets[presetIndex].Width;
            mViewportResolutionHeight = presets[presetIndex].Height;
        }

        if ((presetIndex & 1) == 0 && presetIndex + 1 < static_cast<int>(std::size(presets)))
            QtUi::SameLine();
    }

    if (QtUi::Button("Window Resolution"))
    {
        UINT windowWidth = 0;
        UINT windowHeight = 0;
        if (DX12Context_GetRenderSize(&windowWidth, &windowHeight) && windowWidth > 0 && windowHeight > 0)
        {
            mViewportResolutionWidth = static_cast<int>(windowWidth);
            mViewportResolutionHeight = static_cast<int>(windowHeight);
        }
    }

    QtUi::Spacing();
    if (mViewportResolutionFixed)
        QtUi::TextDisabled("Fixed resolution in use; the viewport preview is scaled to fit.");
    else
        QtUi::TextDisabled("Following the viewport panel size.");

    if (QtUi::Button("Apply"))
    {
        mViewportResolutionFixed = true;
        mRequestViewportResolutionChange = true;
        mShowViewportResolutionDialog = false;
    }

    QtUi::SameLine();
    if (QtUi::Button("Fit to Viewport"))
    {
        // Release the fixed size; the auto-fit in the frame loop picks the
        // panel size up again on the next frame.
        mViewportResolutionFixed = false;
        mShowViewportResolutionDialog = false;
    }

    QtUi::SameLine();
    if (QtUi::Button("Cancel"))
    {
        mShowViewportResolutionDialog = false;
    }

    QtUi::End();
}

void Editor::DrawScreenshotWindow()
{
    if (!mShowScreenshotDialog)
        return;

    QtUi::SetNextWindowSize(UiVec2(460.0f, 0.0f), QtUiCond_FirstUseEver);
    if (!QtUi::Begin("Screenshot", &mShowScreenshotDialog, QtUiWindowFlags_AlwaysAutoResize))
    {
        QtUi::End();
        return;
    }

    // Screenshots always go to <project root>/Screenshots, never relative to the
    // working directory (which is Binaries/ when launched from the IDE).
    if (mScreenshotOutputFolder.empty())
    {
        const std::filesystem::path dataDirectory = FindProjectDataDirectory();
        const std::filesystem::path folder = dataDirectory.empty()
            ? std::filesystem::path(L"K:\\Ptero-Engine\\Screenshots")
            : dataDirectory.parent_path() / "Screenshots";
        mScreenshotOutputFolder = folder.lexically_normal().make_preferred().string();
    }

    QtUi::Text("Output Folder: %s", mScreenshotOutputFolder.c_str());
    if (mSceneRenderer != nullptr)
        QtUi::TextDisabled("Resolution: %u x %u", mSceneRenderer->GetSceneWidth(), mSceneRenderer->GetSceneHeight());

    if (QtUi::Button("Capture"))
    {
        mRequestScreenshot = true;
        mShowScreenshotDialog = false;
    }

    QtUi::SameLine();
    if (QtUi::Button("Cancel"))
    {
        mShowScreenshotDialog = false;
    }

    QtUi::End();
}

// ---------------------------------------------------------------------------
// DrawAudioManagerWindow
// ---------------------------------------------------------------------------

void Editor::DrawAudioManagerWindow(AudioManager* audioManager)
{
    if (!QtUi::Begin("Audio Manager", &mShowAudioManagerPanel))
    {
        QtUi::End();
        return;
    }

    if (!audioManager || !audioManager->IsInitialized())
    {
        QtUi::TextColored(UiVec4(1.0f, 0.4f, 0.4f, 1.0f), "Audio system not initialised.");
        QtUi::End();
        return;
    }

    // Summary line
    const auto& loadedBanks   = audioManager->GetLoadedBanks();
    const auto& bankFailures  = audioManager->GetBankLoadFailures();
    const auto& events        = audioManager->GetEvents();
    QtUi::TextDisabled("Loaded Banks: %d  |  Failed: %d  |  Events: %d",
                        static_cast<int>(loadedBanks.size()),
                        static_cast<int>(bankFailures.size()),
                        static_cast<int>(events.size()));
    QtUi::TextDisabled("Path: %s", audioManager->GetAudioDataPath().string().c_str());
    QtUi::Separator();

    // ---- Resonance Audio ------------------------------------------------------
    if (QtUi::CollapsingHeader("Resonance Audio", QtUiTreeNodeFlags_DefaultOpen))
    {
        const bool resonanceAvailable = audioManager->IsResonanceAudioAvailable();
        bool resonanceEnabled = audioManager->IsResonanceAudioEnabled();

        // Greyed out on purpose: there is no engine-owned Resonance DSP left to bypass,
        // so a live checkbox here would be a switch wired to nothing.
        QtUi::BeginDisabled(true);
        QtUi::Checkbox("Enable Resonance Audio", &resonanceEnabled);
        QtUi::EndDisabled();

        QtUi::SameLine();
        if (resonanceAvailable)
            QtUi::TextColored(UiVec4(0.3f, 1.0f, 0.3f, 1.0f), "FMOD DSP loaded");
        else
            QtUi::TextColored(UiVec4(1.0f, 0.4f, 0.4f, 1.0f), "Unavailable");

        QtUi::TextWrapped("%s", audioManager->GetResonanceAudioStatus().c_str());
        QtUi::Spacing();
        QtUi::TextDisabled(
            "Spatialisation lives in the FMOD project, not here: each event carries its "
            "own Resonance Source and Listener effect. The engine only loads the plugin, "
            "which the banks need in order to load at all.");
    }

    QtUi::Separator();

    // ---- Loaded banks ---------------------------------------------------------
    if (QtUi::CollapsingHeader("Loaded Banks", QtUiTreeNodeFlags_DefaultOpen))
    {
        if (loadedBanks.empty())
        {
            QtUi::TextDisabled("No banks loaded.");
        }
        else
        {
            for (const auto& bank : loadedBanks)
                QtUi::BulletText("%s", bank.c_str());
        }
    }

    // ---- Bank load failures ---------------------------------------------------
    if (!bankFailures.empty() && QtUi::CollapsingHeader("Bank Load Failures"))
    {
        for (const auto& fail : bankFailures)
            QtUi::TextColored(UiVec4(1, 0.4f, 0.4f, 1), "%s", fail.c_str());
    }

    // ---- Event list -----------------------------------------------------------
    if (QtUi::CollapsingHeader("Events", QtUiTreeNodeFlags_DefaultOpen))
    {
        if (events.empty())
        {
            QtUi::TextDisabled("No FMOD events were discovered in the loaded banks.");
        }
        else
        {
            for (int i = 0; i < static_cast<int>(events.size()); ++i)
            {
                QtUi::PushID(i);
                QtUi::TextUnformatted(events[i].Path.c_str());
                QtUi::SameLine();
                if (QtUi::SmallButton("Play"))  audioManager->PlayEvent(i);
                QtUi::SameLine();
                if (QtUi::SmallButton("Stop"))  audioManager->StopEvent(i);
                QtUi::PopID();
            }
        }

        QtUi::Spacing();
        if (QtUi::Button("Stop All"))
            audioManager->StopAll();
    }

    QtUi::End();
}

// ---------------------------------------------------------------------------
// Draw (main entry called each frame by DX12RendererAPI)
// ---------------------------------------------------------------------------

void Editor::Draw(
    D3D12_GPU_DESCRIPTOR_HANDLE sceneTextureHandle,
    const EditorCamera& camera,
    const char* sceneStatusMessage,
    const char* statisticsText,
    bool showStatistics,
    AudioManager* audioManager)
{
    if (QtUi::IsStandaloneGame() || mPlaySceneActive)
    {
        QtUi::Begin("Viewport", nullptr, QtUiWindowFlags_NoDecoration);
        const UiVec2 origin=QtUi::GetCursorScreenPos();
        const UiVec2 size=QtUi::GetContentRegionAvail();
        mLastViewportContentOrigin=origin;
        mLastViewportContentSize=size;
        DrawGameUiOverlay(origin, size);
        QtUi::End();
        return;
    }
    mSceneStatusMessage     = sceneStatusMessage;
    mViewportStatisticsText = statisticsText;
    mSavedCameraPosition    = camera.GetPosition();
    mSavedCameraRotation    = camera.GetRotation();
    if (!mViewportStatisticsInitialized)
    {
        mShowViewportStatistics = showStatistics;
        mViewportStatisticsInitialized = true;
    }

    UpdateSceneLoading();
    HandleKeyboardShortcuts();

    Entity* selectedEntity = GetSelectedEntity();

    DrawViewport(sceneTextureHandle, camera, selectedEntity);
    selectedEntity = GetSelectedEntity();
    DrawToolbar(
        mSelectIcon.GpuHandle,
        mMoveIcon.GpuHandle,
        mRotateIcon.GpuHandle,
        mScaleIcon.GpuHandle,
        mWireframeIcon.GpuHandle,
        mProxyIcon.GpuHandle,
        mGameIcon.GpuHandle);

    // The RmlUi pass only runs while something needs its output. Set every
    // frame rather than from inside the panel, which would never turn it off
    // again once the panel stopped drawing.
    if (mSceneRenderer != nullptr)
        mSceneRenderer->SetUiPreviewActive(mShowUiEditorPanel && !mViewportFullscreen);

    // Fullscreen viewport simply skips the panels for the frame rather than
    // clearing their visibility flags, so whatever was open comes back exactly
    // as it was on the way out - and the Windows menu still reflects the user's
    // real choices rather than the temporary mode.
    if (!mViewportFullscreen)
    {
        if (mShowComponentsPanel)   DrawComponentsPanel();
        if (mShowConsolePanel)       DrawConsolePanel();
        if (mShowUiEditorPanel)      DrawUiEditorPanel();
        if (mShowLevelExplorerPanel) DrawLevelExplorerPanel();
        QtUi::PushID(mSelectedEntityIndex);
        if (mShowPropertiesPanel)    DrawPropertiesPanel(selectedEntity, audioManager);
        QtUi::PopID();
        if (mShowAudioManagerPanel)  DrawAudioManagerWindow(audioManager);
        if (mShowResourceDebugPanel) DrawResourceDebugWindow();
        if (mShowTerrainToolWindow)  DrawTerrainToolWindow(selectedEntity);
        DrawViewportResolutionWindow();
        DrawScreenshotWindow();
    }

    DrawViewportStatisticsOverlay();
    DrawSceneLoadingOverlay();
}
