#include "pch.h"
#include "DX12SceneRenderer.h"
#include "../QtUi/QtUi.h"

#include "..\System\include\System\AssetManager.h"
#include "System/PteroLog.h"
#include "AudioManager.h"
#include "EntityIds.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_GetRenderSize(UINT* width, UINT* height);
    double __stdcall DX12Context_GetRenderLatencyMilliseconds();
    HWND __stdcall DX12Context_GetWindowHandle();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    bool __stdcall DX12Context_WaitForGPU();
    bool __stdcall DX12Context_StreamlineInitialize();
}

    namespace
{
    constexpr DXGI_FORMAT SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // The depth resource must be TYPELESS so we can create both a DSV (D32_FLOAT)
    // and an SRV (R32_FLOAT) for the deferred lighting pass world-position reconstruction.
    constexpr DXGI_FORMAT SceneDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
    constexpr DXGI_FORMAT SceneDepthFormat         = DXGI_FORMAT_D32_FLOAT;
    constexpr DXGI_FORMAT SceneDepthSrvFormat      = DXGI_FORMAT_R32_FLOAT;

    std::vector<DX12SceneRenderer::Vertex> GenerateGridVertices()
    {
        constexpr float gridExtent = 50.0f;
        constexpr float lineSpacing = 1.0f;
        constexpr XMFLOAT4 gridColor(0.2f, 0.2f, 0.2f, 1.0f);

        std::vector<DX12SceneRenderer::Vertex> vertices;
        vertices.reserve(404);

        // Emit one pair of vertices per grid line so the geometry can be drawn as a line list.
        for (float position = -gridExtent; position <= gridExtent; position += lineSpacing)
        {
            vertices.push_back({ XMFLOAT3(position, -gridExtent, 0.0f), gridColor });
            vertices.push_back({ XMFLOAT3(position,  gridExtent, 0.0f), gridColor });
            vertices.push_back({ XMFLOAT3(-gridExtent, position, 0.0f), gridColor });
            vertices.push_back({ XMFLOAT3( gridExtent, position, 0.0f), gridColor });
        }

        return vertices;
    }

    DirectX::XMFLOAT3 KelvinToLinearRgb(float temperatureKelvin)
    {
        const float temperature = (std::clamp)(temperatureKelvin, 1000.0f, 40000.0f) / 100.0f;

        float red = 255.0f;
        float green = 0.0f;
        float blue = 0.0f;

        if (temperature <= 66.0f)
        {
            green = 99.4708025861f * std::log(temperature) - 161.1195681661f;
            blue = temperature <= 19.0f
                ? 0.0f
                : 138.5177312231f * std::log(temperature - 10.0f) - 305.0447927307f;
        }
        else
        {
            red = 329.698727446f * std::pow(temperature - 60.0f, -0.1332047592f);
            green = 288.1221695283f * std::pow(temperature - 60.0f, -0.0755148492f);
            blue = 255.0f;
        }

        red = (std::clamp)(red, 0.0f, 255.0f) / 255.0f;
        green = (std::clamp)(green, 0.0f, 255.0f) / 255.0f;
        blue = (std::clamp)(blue, 0.0f, 255.0f) / 255.0f;

        return DirectX::XMFLOAT3(red, green, blue);
    }
}

float DX12SceneRenderer::GetMeshLocalRadius(const Mesh* mesh) const
{
    if (mesh == nullptr)
    {
        return 0.0f;
    }

    const auto cached = mMeshLocalRadiusCache.find(mesh);
    if (cached != mMeshLocalRadiusCache.end())
    {
        return cached->second;
    }

    float localRadius = 0.0f;
    for (const ::Vertex& vertex : mesh->GetVertices())
    {
        const float distance = std::sqrt(
            vertex.Position.x * vertex.Position.x +
            vertex.Position.y * vertex.Position.y +
            vertex.Position.z * vertex.Position.z);
        localRadius = (distance > localRadius) ? distance : localRadius;
    }

    mMeshLocalRadiusCache.emplace(mesh, localRadius);
    return localRadius;
}

float DX12SceneRenderer::ComputeSceneBoundRadius() const
{
    float sceneRadius = 50.0f;

    if (mEntities == nullptr)
    {
        return sceneRadius;
    }

    for (const Entity& entity : *mEntities)
    {
        if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset)
        {
            continue;
        }

        const Mesh* mesh = entity.Mesh->MeshAsset.get();
        const float localRadius = GetMeshLocalRadius(mesh);
        const float scaleX = std::fabs(entity.Transform.Scale.x * TransformComponent::MeshWorldScale);
        const float scaleY = std::fabs(entity.Transform.Scale.y * TransformComponent::MeshWorldScale);
        const float scaleZ = std::fabs(entity.Transform.Scale.z * TransformComponent::MeshWorldScale);
        const float maxScale = (scaleX > scaleY)
            ? ((scaleX > scaleZ) ? scaleX : scaleZ)
            : ((scaleY > scaleZ) ? scaleY : scaleZ);
        const float worldOffset = std::sqrt(
            entity.Transform.Position.x * entity.Transform.Position.x +
            entity.Transform.Position.y * entity.Transform.Position.y +
            entity.Transform.Position.z * entity.Transform.Position.z);

        const float worldRadius = worldOffset + (localRadius * maxScale);
        sceneRadius = (worldRadius > sceneRadius) ? worldRadius : sceneRadius;
    }

    // Add a little slack so the orthographic shadow camera does not clip near the scene edge.
    return sceneRadius * 1.1f;
}

bool DX12SceneRenderer::IsSceneContentDirtyForTemporal()
{
    return mEntityMeshRenderer.ConsumeSceneContentChangedFlag();
}

void DX12SceneRenderer::ReportProgress(const wchar_t* message) const
{
    if (mProgressCallback)
        mProgressCallback(message);
}

bool DX12SceneRenderer::Initialize(ID3D12GraphicsCommandList* commandList)
{
    try
    {
        if (mIsInitialized)
        {
            return true;
        }

        if (commandList == nullptr)
        {
            return false;
        }


        ReportProgress(L"Preparing depth debug resources...");
        if (!CreateDepthDebugResources())
        {
            mLastErrorMessage = "Failed to create the depth debug preview resources.";
            Shutdown();
            return false;
        }

        mLastErrorMessage.clear();

        mCamera.SetPosition(0.0f, -6.0f, 1.5f);
        mCamera.LookAt(0.0f, 0.0f, 0.0f);

        ReportProgress(L"Compiling scene shaders and pipeline...");
        if (!CreatePipeline())
        {
            mLastErrorMessage = "Failed to create the DX12 scene pipeline or compile the cube shaders.";
            Shutdown();
            return false;
        }

        ReportProgress(L"Creating scene render targets...");
        if (!EnsureSceneTargetMatchesWindowSize())
        {
            if (mLastErrorMessage.empty())
            {
                mLastErrorMessage = "Failed to create the off-screen scene render target.";
            }
            Shutdown();
            return false;
        }

        ReportProgress(L"Uploading startup geometry buffers...");
        if (!CreateGeometry(commandList))
        {
            mLastErrorMessage = "Failed to upload cube vertex or index buffers to the GPU.";
            Shutdown();
            return false;
        }

        ReportProgress(L"Creating camera constant buffers...");
        if (!CreateConstantBuffer())
        {
            mLastErrorMessage = "Failed to create the camera constant buffer.";
            Shutdown();
            return false;
        }

        ReportProgress(L"Initializing motion vector renderer...");
        if (!mMotionVectorRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: Motion vector renderer initialization failed.\n");
            if (mMotionVectorRenderer.GetLastErrorMessage())
                OutputDebugStringA(mMotionVectorRenderer.GetLastErrorMessage());
        }

        ReportProgress(L"Initializing DLSS and Streamline...");
        if (DX12Context_StreamlineInitialize())
        {
            if (!mDlssRenderer.Initialize(mSceneWidth, mSceneHeight))
            {
                OutputDebugStringA("DX12SceneRenderer: DLSS initialization failed – DLSS disabled.\n");
                if (mDlssRenderer.GetLastErrorMessage())
                    OutputDebugStringA(mDlssRenderer.GetLastErrorMessage());
            }
        }
        else
        {
            OutputDebugStringA("DX12SceneRenderer: Streamline initialization failed; DLSS unavailable.\n");
        }

        // Non-fatal: without the FidelityFX DLLs FSR reports itself unavailable and
        // the settings panel says why. (It was once skipped for standalone on the belief
        // that it crashed there; that crash was QtUi touching a nonexistent Qt window.)
        ReportProgress(L"Initializing AMD FSR...");
        if (!mFsrRenderer.Initialize() && mFsrRenderer.GetLastErrorMessage())
        {
            PteroLog::Write(PteroLog::Level::Warning, "FSR", mFsrRenderer.GetLastErrorMessage());
        }

        UpdateSceneConstants();

        // Initialize the entity mesh renderer so it is ready to receive entities each frame.
        ReportProgress(L"Initializing entity mesh renderer...");
        if (!mEntityMeshRenderer.Initialize(commandList))
        {
            mLastErrorMessage = "Failed to initialize the entity mesh renderer.";
            Shutdown();
            return false;
        }

        // Initialize the terrain renderer.  Non-fatal if it fails -- the
        // engine still renders the rest of the scene without terrain, and the
        // Terrain panel surfaces the error string to the artist.
        ReportProgress(L"Initializing terrain renderer...");
        if (!mTerrainRenderer.Initialize(commandList))
        {
            OutputDebugStringA("DX12SceneRenderer: Terrain renderer initialization failed.\n");
            if (const char* terrainInitError = mTerrainRenderer.GetLastErrorMessage())
                OutputDebugStringA(terrainInitError);
        }

        // Initialize the vegetation renderer.  Non-fatal if it fails -- areas
        // simply produce nothing and the Vegetation panel shows the error.
        // Terrain must already be initialized, since vegetation snaps onto it
        // and polls its revision counter to know when to re-scatter.
        ReportProgress(L"Initializing vegetation renderer...");
        mVegetationRenderer.SetTerrainRenderer(&mTerrainRenderer);
        mVegetationRenderer.SetWindSettings(&mWindSettings);
        if (!mVegetationRenderer.Initialize(commandList))
        {
            OutputDebugStringA("DX12SceneRenderer: Vegetation renderer initialization failed.\n");
            if (const char* vegetationInitError = mVegetationRenderer.GetLastErrorMessage())
                OutputDebugStringA(vegetationInitError);
        }

        // Initialize the water renderer.  Non-fatal if it fails -- the rest of
        // the scene still renders; the water PSO is created lazily on the first
        // frame a WaterComponent is present.
        ReportProgress(L"Initializing water renderer...");
        if (!mWaterRenderer.Initialize())
        {
            OutputDebugStringA("DX12SceneRenderer: Water renderer initialization failed.\n");
            if (const char* waterInitError = mWaterRenderer.GetLastErrorMessage())
                OutputDebugStringA(waterInitError);
        }

        // Initialize the TAA renderer.  Non-fatal if it fails – TAA is a quality feature
        // and the engine still renders correctly without it.
        ReportProgress(L"Initializing temporal anti-aliasing...");
        if (!mTaaRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: TAA initialization failed – TAA disabled.\n");
            if (mTaaRenderer.GetLastErrorMessage())
                OutputDebugStringA(mTaaRenderer.GetLastErrorMessage());
            mTaaSettings.Enabled = false;
        }
        else
        {
            // The history texture starts undefined on first init, so force the
            // first resolve to use only the current frame instead of blending
            // against uninitialized history data.
            mTaaSettings.ResetHistory = true;
        }

        ReportProgress(L"Initializing subpixel morphological anti-aliasing...");
        if (!mSmaaRenderer.Initialize(mSceneWidth, mSceneHeight, commandList))
        {
            OutputDebugStringA("DX12SceneRenderer: SMAA initialization failed - SMAA disabled.\n");
            if (mSmaaRenderer.GetLastErrorMessage())
                OutputDebugStringA(mSmaaRenderer.GetLastErrorMessage());
            mSmaaSettings.Enabled = false;
        }

        ReportProgress(L"Initializing image sharpening...");
        if (!mImageSharpenRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: image sharpening initialization failed - image sharpening disabled.\n");
            if (mImageSharpenRenderer.GetLastErrorMessage())
                OutputDebugStringA(mImageSharpenRenderer.GetLastErrorMessage());
            mSharpenSettings.ImageSharpeningEnabled = false;
        }

        // Bring the RmlUi subsystem up with the rest of the renderer - shaders, pipeline
        // and context - but deliberately load no document. What the UI shows is the game's
        // decision, made from game code once a play session starts; the editor only
        // provides the machinery. Non-fatal: a renderer without a UI is still a usable
        // editor, and the failure reason is reported on the log.
        ReportProgress(L"Initializing RmlUi user interface...");
        if (!mRmlUiRenderer.Initialize(mSceneWidth, mSceneHeight, commandList))
        {
            OutputDebugStringA("DX12SceneRenderer: RmlUi initialization failed - the game UI is disabled.\n");
            if (mRmlUiRenderer.GetLastErrorMessage())
                OutputDebugStringA(mRmlUiRenderer.GetLastErrorMessage());
        }

        // Initialize the AgX tonemapper.  Non-fatal if it fails.
        ReportProgress(L"Initializing AgX tonemapper...");
        if (!mAgxTonemapper.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: AgX tonemapper initialization failed – tonemapping disabled.\n");
            if (mAgxTonemapper.GetLastErrorMessage())
                OutputDebugStringA(mAgxTonemapper.GetLastErrorMessage());
            mAgxSettings.Enabled = false;
        }

        // Initialize histogram auto-exposure. Non-fatal: the tonemapper falls
        // back to the manual exposure if the meter is unavailable.
        ReportProgress(L"Initializing auto exposure...");
        if (!mAutoExposure.Initialize())
        {
            OutputDebugStringA("DX12SceneRenderer: auto exposure initialization failed – manual exposure only.\n");
            if (mAutoExposure.GetLastErrorMessage())
                OutputDebugStringA(mAutoExposure.GetLastErrorMessage());
        }

        // Initialize the sky renderer.  Non-fatal if it fails.
        ReportProgress(L"Initializing sky renderer...");
        if (!mSkyRenderer.Initialize(SceneColorFormat, SceneDepthFormat))
        {
            OutputDebugStringA("DX12SceneRenderer: Sky renderer initialization failed.\n");
            if (mSkyRenderer.GetLastError())
                OutputDebugStringA(mSkyRenderer.GetLastError());
        }

        // Initialize the shadow map renderer.  Non-fatal if it fails — meshes
        // will render without shadows until the next successful initialization.
        ReportProgress(L"Initializing directional shadows...");
        if (!mShadowMapRenderer.Initialize())
        {
            OutputDebugStringA("DX12SceneRenderer: Shadow map renderer initialization failed.\n");
            if (mShadowMapRenderer.GetLastError())
                OutputDebugStringA(mShadowMapRenderer.GetLastError());
        }

        ReportProgress(L"Initializing point light shadows...");
        mPointShadowMapRenderer.SetMapSize(static_cast<UINT>(mPointShadowSettings.MapSize));
        mPointShadowMapRenderer.SetShadowBias(mPointShadowSettings.Bias);
        mPointShadowMapRenderer.SetSlopeScaledDepthBias(mPointShadowSettings.SlopeScaledDepthBias);
        if (!mPointShadowMapRenderer.Initialize())
        {
            OutputDebugStringA("DX12SceneRenderer: Point shadow map renderer initialization failed.\n");
            if (mPointShadowMapRenderer.GetLastError())
                OutputDebugStringA(mPointShadowMapRenderer.GetLastError());
        }

        // Initialize the point light renderer (wireframe gizmo spheres).  Non-fatal.
        ReportProgress(L"Initializing point light renderer...");
        if (!mPointLightRenderer.Initialize(commandList, SceneColorFormat, SceneDepthFormat))
        {
            OutputDebugStringA("DX12SceneRenderer: Point light renderer initialization failed.\n");
            if (mPointLightRenderer.GetLastError())
                OutputDebugStringA(mPointLightRenderer.GetLastError());
        }

        // Initialize the decal renderer (wireframe box + arrow gizmos).  Non-fatal.
        ReportProgress(L"Initializing decal renderer...");
        if (!mDecalRenderer.Initialize(commandList, SceneColorFormat, SceneDepthFormat))
        {
            OutputDebugStringA("DX12SceneRenderer: Decal renderer initialization failed.\n");
            if (mDecalRenderer.GetLastError())
                OutputDebugStringA(mDecalRenderer.GetLastError());
        }

        // Initialize the deferred lighting pass (G-Buffer RTs + fullscreen lighting resolve).
        // Non-fatal — the scene will be black if this fails but won't crash.
        ReportProgress(L"Initializing deferred lighting...");
        if (!mDeferredLightingPass.Initialize(mSceneWidth, mSceneHeight, SceneDepthFormat, mMsaaSettings))
        {
            OutputDebugStringA("DX12SceneRenderer: Deferred lighting pass initialization failed.\n");
            if (mDeferredLightingPass.GetLastError())
                OutputDebugStringA(mDeferredLightingPass.GetLastError());
        }

        ReportProgress(L"Initializing volumetric fog...");
        if (!mVolumetricFogRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: Volumetric fog initialization failed.\n");
            if (mVolumetricFogRenderer.GetLastError())
                OutputDebugStringA(mVolumetricFogRenderer.GetLastError());
        }

        ReportProgress(L"Initializing volumetric clouds...");
        if (!mVolumetricCloudRenderer.Initialize(mSceneWidth, mSceneHeight, SceneColorFormat))
        {
            OutputDebugStringA("DX12SceneRenderer: Volumetric cloud initialization failed.\n");
            if (mVolumetricCloudRenderer.GetLastError())
                OutputDebugStringA(mVolumetricCloudRenderer.GetLastError());
        }

        ReportProgress(L"Initializing rain renderer...");
        if (!mRainRenderer.Initialize())
        {
            OutputDebugStringA("DX12SceneRenderer: Rain renderer initialization failed.\n");
            if (mRainRenderer.GetLastError())
                OutputDebugStringA(mRainRenderer.GetLastError());
        }

        ReportProgress(L"Initializing particle systems...");
        if (!mParticleRenderer.Initialize())
        {
            OutputDebugStringA("DX12SceneRenderer: Particle renderer initialization failed.\n");
            if (mParticleRenderer.GetLastError())
                OutputDebugStringA(mParticleRenderer.GetLastError());
        }

        // Chromatic aberration. Non-fatal if it fails.
        ReportProgress(L"Initializing chromatic aberration...");
        if (!mChromaticAberrationRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: chromatic aberration initialization failed - effect disabled.\n");
            if (mChromaticAberrationRenderer.GetLastErrorMessage())
                OutputDebugStringA(mChromaticAberrationRenderer.GetLastErrorMessage());
            mChromaticAberrationSettings.Enabled = false;
        }

        // Screen-space reflections. Non-fatal if it fails.
        ReportProgress(L"Initializing screen-space reflections...");
        if (!mSsrRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: SSR initialization failed - reflections disabled.\n");
            if (mSsrRenderer.GetLastErrorMessage())
                OutputDebugStringA(mSsrRenderer.GetLastErrorMessage());
            mSsrSettings.Enabled = false;
        }

        // Initialize the bloom renderer. Non-fatal if it fails.
        ReportProgress(L"Initializing bloom...");
        if (!mBloomRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: Bloom initialization failed – bloom disabled.\n");
            if (mBloomRenderer.GetLastErrorMessage())
                OutputDebugStringA(mBloomRenderer.GetLastErrorMessage());
            mBloomSettings.Enabled = false;
        }

        // RTGI is lazily initialized on first use (waits for a valid render size).
        // We do not pre-initialize here so startup is fast even on non-DXR GPUs.

        ReportProgress(L"Scene renderer ready.");
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        mLastErrorMessage = exception.what();
        Shutdown();
        return false;
    }
    catch (...)
    {
        mLastErrorMessage = "The DX12 scene renderer failed with an unknown exception during initialization.";
        Shutdown();
        return false;
    }
}

void DX12SceneRenderer::Render(ID3D12GraphicsCommandList* commandList)
{
    if (!(mIsInitialized && commandList && mPipeline.GetRootSignature() && mPipeline.GetPipelineState()))
    {
        return;
    }

    if (!EnsureSceneTargetMatchesWindowSize())
    {
        return;
    }

    BeginTimingFrame();
    const auto frameTimingStart = std::chrono::steady_clock::now();

    mMarkerCommandList = commandList;
    mCurrentPassOrder.clear();
    if (!mGpuProgressBuffer)
    {
        if (ID3D12Device* device = DX12Context_GetDevice())
        {
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
            const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(256);
            if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mGpuProgressBuffer))))
            {
                mGpuProgressBuffer->SetName(L"GPU progress breadcrumbs");
                void* mapped = nullptr;
                if (SUCCEEDED(mGpuProgressBuffer->Map(0, nullptr, &mapped)))
                    mGpuProgressMapped = static_cast<const UINT32*>(mapped);
            }
        }
    }
    commandList->QueryInterface(IID_PPV_ARGS(&mMarkerCommandList2));
    ++mGpuProgressFrame;
    WriteGpuProgress(0, 0, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN);
    struct MarkerListReset
    {
        DX12SceneRenderer& Renderer;
        ~MarkerListReset()
        {
            Renderer.WriteGpuProgress(3, 0, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT);
            Renderer.mMarkerCommandList2.Reset();
            Renderer.mMarkerCommandList = nullptr;
            Renderer.mLastPassOrder.swap(Renderer.mCurrentPassOrder);
        }
    } markerListReset{ *this };

    // Advance the mesh renderer's constant-buffer ring before any of its passes
    // record. The shadow, depth and G-Buffer passes must all land in the same
    // frame's copy, and none of them may write over constants an earlier
    // in-flight frame is still drawing from.
    mEntityMeshRenderer.BeginFrame();

    mMsaaSettings.Validate();
    const UINT desiredMsaaSampleCount = mMsaaSettings.GetEffectiveSampleCount();
    const UINT desiredMsaaQuality = mMsaaSettings.GetEffectiveQuality();
    if (mSceneColorTarget
        && (desiredMsaaSampleCount != mSceneTargetMsaaSampleCount
            || desiredMsaaQuality != mSceneTargetMsaaQuality))
    {
        // MSAA resource changes must be applied before DX12Context_BeginFrame.
        // Recording a frame while replacing depth/G-buffer sample layouts can
        // leave the shared command list in an invalid state.
        return;
    }

    const uint32_t renderFrameIndex = mRenderFrameIndex++;

    // Drive the editor camera once per frame so the constant buffer reflects live input.
    UpdateCamera();
    UpdateSceneConstants();

    mSharpenSettings.Validate();
    // Under DLSS/FSR the texture mip bias is the upscalers' own recommendation,
    // log2(render / display), not the level's texture sharpening: a level authored for
    // native TAA can carry a bias like -3, which at a reduced render resolution samples
    // textures far too fine - sparkle the upscaler reads as detail and cannot resolve,
    // worse the lower the quality mode.
    float textureMipLODBias = mSharpenSettings.GetEffectiveTextureMipLODBias() + mTextureQualityMipBias;
    if (UpscalerOwnsPostAaOutput() && mSceneWidth > 0)
    {
        UINT displayWidth = mSceneWidth;
        UINT displayHeight = mSceneHeight;
        DX12Context_GetRenderSize(&displayWidth, &displayHeight);
        const float renderScale = static_cast<float>(mSceneWidth) / static_cast<float>((std::max)(displayWidth, 1u));
        textureMipLODBias = std::log2((std::min)(renderScale, 1.0f)) + mTextureQualityMipBias;
    }
    mEntityMeshRenderer.SetTextureMipLODBias(textureMipLODBias);
    mTerrainRenderer.SetTextureMipLODBias(textureMipLODBias);

    const bool rtaoDebugViewActive = (mRtaoSettings.DebugView > 0);
    if (rtaoDebugViewActive != mRtaoDebugViewWasActive)
    {
        mTaaSettings.ResetHistory = true;
        mRtaoDebugViewWasActive = rtaoDebugViewActive;
    }

    // Evaluate the Hosek-Wilkie sky model from the current time-of-day settings.
    mHosekResult = EvaluateHosekWilkie(mTimeOfDaySettings);

    // ---- Scale normalised Hosek colours by user-specified lux values ----
    constexpr float kRefSkyLux = 20000.0f;
    constexpr float kRefSunLux = 100000.0f;

    // Disabling time of day is expressed as zero sun and sky intensity. Every consumer of
    // these six values - deferred lighting, volumetric fog and clouds, and the forward
    // water pass through mFrameSunColor - then contributes nothing, so the switch does not
    // have to be threaded through each of those passes separately.
    const float sunLuxScale = mTimeOfDaySettings.Enabled
        ? (mTimeOfDaySettings.SunIntensityLux / kRefSunLux) : 0.0f;
    const float skyLuxScale = mTimeOfDaySettings.Enabled
        ? (mTimeOfDaySettings.SkyIntensityLux / kRefSkyLux) : 0.0f;

    const float sunR = (mTimeOfDaySettings.OverrideSunColor ? mTimeOfDaySettings.SunColorR : mHosekResult.SunR) * sunLuxScale;
    const float sunG = (mTimeOfDaySettings.OverrideSunColor ? mTimeOfDaySettings.SunColorG : mHosekResult.SunG) * sunLuxScale;
    const float sunB = (mTimeOfDaySettings.OverrideSunColor ? mTimeOfDaySettings.SunColorB : mHosekResult.SunB) * sunLuxScale;
    const float skyR = (mTimeOfDaySettings.OverrideSkyColor ? mTimeOfDaySettings.SkyColorR : mHosekResult.SkyR) * skyLuxScale;
    const float skyG = (mTimeOfDaySettings.OverrideSkyColor ? mTimeOfDaySettings.SkyColorG : mHosekResult.SkyG) * skyLuxScale;
    const float skyB = (mTimeOfDaySettings.OverrideSkyColor ? mTimeOfDaySettings.SkyColorB : mHosekResult.SkyB) * skyLuxScale;

    // Cache lux-scaled sun/sky colour for the forward water pass (runs later in
    // Dispatch, after these locals are gone).
    mFrameSunColor = XMFLOAT3(sunR, sunG, sunB);
    mFrameSkyColor = XMFLOAT3(skyR, skyG, skyB);

    // ---- Upload per-frame lighting data to the deferred lighting pass ----
    mDeferredLightingPass.SetSceneLighting(
        { mHosekResult.SunDirX, mHosekResult.SunDirY, mHosekResult.SunDirZ },
        { sunR, sunG, sunB },
        { skyR, skyG, skyB });
    mDeferredLightingPass.SetPointShadowDebug(
        mPointShadowSettings.DebugView,
        mPointShadowSettings.FilterRadius,
        mPointShadowSettings.SeamBlendDistance,
        mPointShadowSettings.NormalOffset);

    // Upload inverse view-projection for world-position reconstruction in the lighting pass.
    // mJitteredViewProjection = view * proj (not transposed).
    // HLSL uses mul(ndc, M) which expects M = Transpose(inv(VP)).
    {
        const XMMATRIX vp = XMLoadFloat4x4(&mJitteredViewProjection);
        XMFLOAT4X4 invVP;
        XMStoreFloat4x4(&invVP, XMMatrixTranspose(XMMatrixInverse(nullptr, vp)));
        mInvViewProjection = invVP;
        const XMFLOAT3 camPos = mCamera.GetPosition();
        mDeferredLightingPass.SetCameraData(invVP, camPos);
    }

    // -----------------------------------------------------------------------
    // Particle systems.
    //
    // Synced before the light array is assembled, because an emissive system
    // contributes an analytic proxy light that has to be in that array: one
    // array feeds the deferred shading, the RTGI point-light set and the
    // volumetric fog, so registering the fire's light here is what makes the
    // flame light the room, bounce off its walls and glow through smoke without
    // any of those passes knowing a particle system exists.
    // -----------------------------------------------------------------------
    {
        PTERO_SCOPED_PASS_TIMER("Scene", "Particle system sync");
        const float deltaSeconds = mFrameDeltaTimeMs * 0.001f;

        mParticleSystems.clear();
        if (mEntities != nullptr)
        {
            for (const Entity& entity : *mEntities)
            {
                if (!entity.HasParticleSystemComponent())
                    continue;
                if (mParticleSystems.size() >= static_cast<size_t>(kParticleMaxSystems))
                    break;

                ParticleSystemInstance instance;
                instance.Settings = *entity.ParticleSystem;
                instance.EmitterPosition = entity.Transform.Position;
                instance.DebugName = entity.Name;

                // Rotation and translation only. The global mesh scale that
                // TransformComponent bakes in exists for imported geometry; a
                // particle system's sizes are authored in metres and must not
                // be divided by ten behind the artist's back.
                instance.EmitterToWorld =
                    PteroTransform::ComposeRotation(entity.Transform.Rotation) *
                    XMMatrixTranslation(
                        entity.Transform.Position.x,
                        entity.Transform.Position.y,
                        entity.Transform.Position.z);

                mParticleSystems.push_back(std::move(instance));
            }
        }

        if (mParticleRenderer.IsInitialized())
        {
            mParticleRenderer.SetSystems(mParticleSystems, deltaSeconds);
        }
    }

    // Gather point lights from the entity list and upload to the deferred lighting pass.
    if (mEntities != nullptr)
    {
        PTERO_SCOPED_PASS_TIMER("Scene", "Light gather");

        // Normalise against 800 lm (standard 60W-equivalent LED) so default lights = 1.0 brightness.
        constexpr float kRefLumens = 800.0f;

        DeferredLightingPass::PointLightGpu gpuLights[DeferredLightingPass::kMaxPointLights]{};
        int numLights = 0;

        // Per-light weight applied to indirect lighting only. Entity lights set
        // it from their own GI settings, particle proxies from theirs; 1 leaves
        // a light's bounce matching its direct contribution.
        float lightGiScale[DeferredLightingPass::kMaxPointLights];
        for (int i = 0; i < DeferredLightingPass::kMaxPointLights; ++i)
            lightGiScale[i] = 1.0f;

        for (const Entity& entity : *mEntities)
        {
            if (!entity.HasPointLightComponent() || numLights >= DeferredLightingPass::kMaxPointLights)
                continue;

            const PointLightComponent& pl = *entity.PointLight;

            // The light style multiplier is folded into the colour here, once,
            // rather than being threaded through each consumer: the deferred
            // shading, the RTGI bounce and the fog scattering all read the same
            // array, so a flickering torch flickers in all three together.
            const float styleMultiplier = LightStyles::Evaluate(
                pl.Style,
                mSceneTimeSeconds,
                pl.StyleSpeed,
                pl.StyleAmplitude,
                pl.StylePhaseOffset,
                pl.CustomStylePattern);

            const float brightness = (pl.IntensityLumens / kRefLumens) * styleMultiplier;
            const DirectX::XMFLOAT3 lightColor = pl.UseTemperature
                ? KelvinToLinearRgb(pl.TemperatureKelvin)
                : DirectX::XMFLOAT3(pl.ColorR, pl.ColorG, pl.ColorB);

            const int entityLightIndex = numLights++;
            lightGiScale[entityLightIndex] =
                pl.AffectGlobalIllumination ? (std::max)(pl.GiContribution, 0.0f) : 0.0f;

            DeferredLightingPass::PointLightGpu& gpu = gpuLights[entityLightIndex];
            gpu.Position    = entity.Transform.Position;
            gpu.Radius      = pl.Radius > 0.0f ? pl.Radius : 0.001f;
            gpu.Color       = { lightColor.x * brightness, lightColor.y * brightness, lightColor.z * brightness };
            gpu.InvRadiusSq = 1.0f / (gpu.Radius * gpu.Radius);
            gpu.FalloffExponent = pl.FalloffExponent;
            gpu.SourceRadius = pl.SourceRadius;
            gpu.CastShadows = pl.CastShadows ? 1.0f : 0.0f;
            gpu._Pad0 = 0.0f;

            // Spot and rect lights take their orientation from the entity's
            // rotation: they emit along local -Z and the rectangle lies in the
            // local XY plane, so an unrotated light points straight down.
            {
                const XMMATRIX lightRotation = PteroTransform::ComposeRotation(entity.Transform.Rotation);
                XMStoreFloat3(
                    &gpu.Direction,
                    XMVector3Normalize(XMVector3TransformNormal(g_XMNegIdentityR2, lightRotation)));
                XMStoreFloat3(
                    &gpu.RectRight,
                    XMVector3Normalize(XMVector3TransformNormal(g_XMIdentityR0, lightRotation)));
            }

            gpu.LightType = static_cast<float>(static_cast<int>(pl.Type));

            // Half-angle cosines: the component stores full cone angles, which
            // is what an artist measures, but the shader compares a dot product.
            gpu.SpotCosInner = std::cos(XMConvertToRadians(pl.SpotInnerConeDegrees * 0.5f));
            gpu.SpotCosOuter = std::cos(XMConvertToRadians(pl.SpotOuterConeDegrees * 0.5f));
            gpu.RectHalfWidth = pl.RectWidth * 0.5f;
            gpu.RectHalfHeight = pl.RectHeight * 0.5f;
            gpu.RectTwoSided = pl.RectTwoSided ? 1.0f : 0.0f;

            // The fog reads this very record later, so all it needs here is
            // whether the artist let this light into the medium.
            mCachedPointLightAffectsFog[entityLightIndex] = pl.AffectVolumetricFog;
        }

        // Append the particle systems' emissive proxy lights. They are ordinary
        // point lights from here on, so nothing downstream needs to special-case
        // them - but each carries its own GI weight, applied to the RTGI copy
        // below so a fire can be toned down in the bounce without dimming the
        // pool of light it throws on the floor.
        for (const ParticleProxyLight& proxy : mParticleRenderer.GetProxyLights())
        {
            if (numLights >= DeferredLightingPass::kMaxPointLights)
                break;

            const int lightIndex = numLights++;
            DeferredLightingPass::PointLightGpu& gpu = gpuLights[lightIndex];
            gpu.Position = proxy.Position;
            gpu.Radius = proxy.Radius;
            gpu.Color = proxy.Color;
            gpu.InvRadiusSq = proxy.InvRadiusSq;
            // A flame is a volume, not a point: inverse-square from its centre
            // is far too sharp up close, and it has a real source radius that
            // softens the shadows it casts.
            gpu.FalloffExponent = 2.0f;
            gpu.SourceRadius = 0.15f;
            gpu.CastShadows = proxy.CastShadows ? 1.0f : 0.0f;
            gpu._Pad0 = 0.0f;

            // A flame radiates in every direction, so the proxy is always a
            // point light and the shape fields go unread. Set them anyway so a
            // reused array slot cannot leave a stale cone behind.
            gpu.Direction = { 0.0f, 0.0f, -1.0f };
            gpu.RectRight = { 1.0f, 0.0f, 0.0f };
            gpu.LightType = static_cast<float>(static_cast<int>(LightType::Point));
            gpu.SpotCosInner = 1.0f;
            gpu.SpotCosOuter = -1.0f;
            gpu.RectHalfWidth = 0.0f;
            gpu.RectHalfHeight = 0.0f;
            gpu.RectTwoSided = 0.0f;

            lightGiScale[lightIndex] = proxy.GiContribution;

            mCachedPointLightAffectsFog[lightIndex] = proxy.AffectVolumetricFog;
        }

        mDeferredLightingPass.SetPointLights(gpuLights, numLights);

        // Cache for the RT GI pass (which runs later in the same frame).
        mNumCachedPointLights = numLights;
        memcpy(mCachedPointLights, gpuLights, numLights * sizeof(DeferredLightingPass::PointLightGpu));

        // Keep the indirect-only weights rather than applying them here: the
        // point shadow pass writes shadow indices into mCachedPointLights and
        // re-uploads that same array to the deferred pass, so scaling it in
        // place would dim the direct lighting too. BuildGiPointLights() applies
        // them to a separate copy once the shadow indices are in.
        memcpy(mPointLightGiScale, lightGiScale, sizeof(lightGiScale));

        mRainSettings = RainSettings{};
        mRainSettings.Enabled = false;
        bool foundRainComponent = false;

        // Sync the first RainComponent found in the entity list to the rain renderer settings.
        for (const Entity& entity : *mEntities)
        {
            if (!entity.HasRainComponent())
                continue;
            const RainComponent& rc = *entity.Rain;
            foundRainComponent = true;
            mRainSettings.Enabled           = rc.Enabled;
            // Wind now comes from the scene-wide WindSettings rather than the
            // rain component's own vector, so rain and vegetation cannot end up
            // blowing in different directions.  The component's WindX/Y/Z are
            // still serialized for older levels but no longer drive anything.
            mRainSettings.WindVector        = mWindSettings.GetVelocityVector();
            mRainSettings.Gravity           = rc.Gravity;
            mRainSettings.BoundingBoxExtents = { rc.BoxExtentX, rc.BoxExtentY, rc.BoxExtentZ };
            mRainSettings.Intensity          = rc.Intensity;
            mRainSettings.StreakLength       = rc.StreakLength;
            mRainSettings.RainColorR         = rc.ColorR;
            mRainSettings.RainColorG         = rc.ColorG;
            mRainSettings.RainColorB         = rc.ColorB;
            mRainSettings.RainColorA         = rc.ColorA;
            mRainSettings.WetnessIntensity   = rc.WetnessIntensity;
            break;
        }

        if (!foundRainComponent)
        {
            mRainRenderer.ResetSimulation();
        }
    }
    else
    {
        mRainSettings = RainSettings{};
        mRainSettings.Enabled = false;
        mRainRenderer.ResetSimulation();
    }

    mEntityMeshRenderer.SetRainSurfaceState(mRainSettings.Enabled, mRainSettings.WetnessIntensity);

    // -----------------------------------------------------------------------
    // PASS 0 – Vegetation update, interaction map and GPU culling.
    //
    // All three must run before the shadow pass, because the shadow draws
    // consume the same compacted visible-index list and indirect arguments the
    // cull pass produces.  Culling uses the non-jittered view-projection so the
    // set of visible instances does not flicker with the TAA jitter.
    // -----------------------------------------------------------------------
    {
        PTERO_SCOPED_PASS_TIMER("Scene", "Vegetation update + cull");
        const float deltaSeconds = mFrameDeltaTimeMs * 0.001f;

        mVegetationRenderer.Update(commandList, mCamera.GetPosition(), deltaSeconds);
        mVegetationRenderer.DispatchInteraction(commandList, mCamera.GetPosition(), deltaSeconds);
        mVegetationRenderer.DispatchCull(
            commandList,
            XMLoadFloat4x4(&mNonJitteredViewProjection),
            mCamera.GetPosition());
    }

    // -----------------------------------------------------------------------
    // PASS 1 – Shadow pass (unchanged from forward renderer)
    // Render scene depth from the sun's perspective before touching the scene RT.
    // -----------------------------------------------------------------------
    // A black sun casts no visible shadow, so with time of day off this whole pass is
    // cost for nothing. Point lights keep their own shadow pass further down.
    if (mShadowMapRenderer.IsInitialized() && mTimeOfDaySettings.Enabled)
    {
        PTERO_SCOPED_PASS_TIMER("Shadows", "Sun shadow map");
        const XMFLOAT3 sunDir(mHosekResult.SunDirX, mHosekResult.SunDirY, mHosekResult.SunDirZ);
        const float kSceneBoundRadius = ComputeSceneBoundRadius();
        mShadowMapRenderer.BeginShadowPass(commandList, sunDir, kSceneBoundRadius);

        // Shadows off: the pass still clears the map, so the lighting reads "nothing
        // occludes" rather than a stale map from before the switch.
        if (mShadowsEnabled)
        {
            mEntityMeshRenderer.RenderDepthOnly(
                commandList,
                mShadowMapRenderer.GetRootSignature(),
                mShadowMapRenderer.GetPipelineState(),
                mShadowMapRenderer.GetLightViewProjection());

            // Vegetation casts through its own alpha-tested depth pipeline rather
            // than the shared one, because a leaf card has to clip to its texture
            // or it would cast the shadow of a solid rectangle.
            mVegetationRenderer.RenderShadowDepth(
                commandList,
                mShadowMapRenderer.GetLightViewProjection(),
                kShadowDepthFormat);
        }

        mShadowMapRenderer.EndShadowPass(commandList);

        // Forward shadow data to the deferred lighting pass (used in the lighting resolve).
        mDeferredLightingPass.SetShadowData(
            mShadowMapRenderer.GetLightViewProjection(),
            static_cast<float>(kShadowMapSize),
            mShadowMapRenderer.GetShadowSrvGpuHandle());
    }

    if (mPointShadowMapRenderer.IsInitialized() && mEntities != nullptr)
    {
       const UINT desiredPointShadowMapSize = static_cast<UINT>((mPointShadowSettings.MapSize > 0) ? mPointShadowSettings.MapSize : 1);
        if (mPointShadowMapRenderer.GetMapSize() != desiredPointShadowMapSize)
        {
            if (!DX12Context_WaitForGPU())
            {
                mLastErrorMessage = "Timed out while waiting to recreate point-shadow resources.";
            }
            else
            {
            mPointShadowMapRenderer.Shutdown();
            mPointShadowMapRenderer.SetMapSize(desiredPointShadowMapSize);
            mPointShadowMapRenderer.SetShadowBias(mPointShadowSettings.Bias);
            mPointShadowMapRenderer.SetSlopeScaledDepthBias(mPointShadowSettings.SlopeScaledDepthBias);
            if (!mPointShadowMapRenderer.Initialize())
            {
                OutputDebugStringA("DX12SceneRenderer: Point shadow map renderer reinitialization failed.\n");
                if (mPointShadowMapRenderer.GetLastError())
                    OutputDebugStringA(mPointShadowMapRenderer.GetLastError());
            }
            }
        }

        mPointShadowMapRenderer.SetShadowBias(mPointShadowSettings.Bias);
        mPointShadowMapRenderer.SetSlopeScaledDepthBias(mPointShadowSettings.SlopeScaledDepthBias);
        std::vector<PointShadowMapRenderer::ShadowedPointLight> shadowLights;
        shadowLights.reserve(kMaxShadowCastingPointLights);

        int pointLightIndex = 0;
        for (Entity& entity : *mEntities)
        {
            if (!entity.HasPointLightComponent() || pointLightIndex >= DeferredLightingPass::kMaxPointLights)
                continue;

            PointLightComponent& pl = *entity.PointLight;
            if (mShadowsEnabled && pl.CastShadows && shadowLights.size() < kMaxShadowCastingPointLights)
            {
                shadowLights.push_back({ entity.Transform.Position, (std::max)(pl.Radius, 0.05f) });
                mCachedPointLights[pointLightIndex]._Pad0 = static_cast<float>(shadowLights.size() - 1);
            }
            else
            {
                mCachedPointLights[pointLightIndex]._Pad0 = -1.0f;
            }

            ++pointLightIndex;
        }

        PTERO_SCOPED_PASS_TIMER("Shadows", "Point shadow cubemaps");
        mPointShadowMapRenderer.BeginFrame(shadowLights);
        for (int lightIndex = 0; lightIndex < mPointShadowMapRenderer.GetActiveLightCount(); ++lightIndex)
        {
            for (int faceIndex = 0; faceIndex < 6; ++faceIndex)
            {
                mPointShadowMapRenderer.BeginShadowFacePass(commandList, lightIndex, faceIndex);
                mEntityMeshRenderer.RenderPointLightShadowDepth(
                    commandList,
                    mPointShadowMapRenderer.GetRootSignature(),
                    mPointShadowMapRenderer.GetPipelineState(),
                    mPointShadowMapRenderer.GetFaceViewProjection(lightIndex, faceIndex),
                    mPointShadowMapRenderer.GetLightPosition(lightIndex),
                    mPointShadowMapRenderer.GetLightFarPlane(lightIndex));
                mPointShadowMapRenderer.EndShadowFacePass(commandList, lightIndex, faceIndex);
            }
        }

        mDeferredLightingPass.SetPointShadowSrv(
            mPointShadowMapRenderer.GetShadowTextureArraySrvGpuHandle(),
            mPointShadowMapRenderer.GetActiveLightCount(),
            static_cast<float>(mPointShadowMapRenderer.GetMapSize()),
            mPointShadowMapRenderer.GetShadowBias());
        mDeferredLightingPass.SetPointShadowMatrices(
            mPointShadowMapRenderer.GetAllFaceViewProjections(),
            mPointShadowMapRenderer.GetActiveLightCount());
        mDeferredLightingPass.SetPointLights(mCachedPointLights, mNumCachedPointLights);
    }
    else
    {
        mDeferredLightingPass.SetPointShadowSrv({}, 0, 0.0f, 0.0f);
    }

    // The shadow indices are final now, so derive the indirect-lighting copy the
    // GI passes use. Doing it here rather than at collection time is what keeps
    // a particle system's GI Contribution out of the direct lighting.
    BuildGiPointLights();

    // Bind the shared shader-visible SRV heap for all subsequent passes.
    ID3D12DescriptorHeap* shaderVisibleHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, shaderVisibleHeaps);

    // -----------------------------------------------------------------------
    // PASS 2 – Geometry pass (G-Buffer fill)
    // All mesh entities write albedo, normal, and material into three MRT
    // outputs.  The scene depth buffer is used for depth testing/writing.
    // The scene colour RT is NOT bound here; it will be used in the sky and
    // lighting passes.
    // -----------------------------------------------------------------------

    // Ensure the G-Buffer agrees with the current MSAA configuration.
    mDeferredLightingPass.EnsureSize(mSceneWidth, mSceneHeight, mMsaaSettings);

    ID3D12Resource* geometryDepthResource = mMsaaSettings.Enabled
        ? mMsaaSceneDepthTarget.Get()
        : mSceneDepthTarget.Get();
    D3D12_RESOURCE_STATES& geometryDepthState = mMsaaSettings.Enabled
        ? mMsaaDepthBufferState
        : mDepthBufferState;

    // Transition the writable geometry depth target to DEPTH_WRITE if it was
    // left in a shader-readable state by the previous frame.
    if (geometryDepthResource && geometryDepthState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        const auto toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            geometryDepthResource,
            geometryDepthState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commandList->ResourceBarrier(1, &toDepthWrite);
        geometryDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    // Subsurface materials register their profiles as the G-Buffer is drawn.
    SubsurfaceProfiles::BeginFrame();

    // Clear scene depth and begin the G-Buffer geometry pass.
    commandList->ClearDepthStencilView(mSceneDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    mDeferredLightingPass.BeginGeometryPass(commandList, mSceneDsvHandle, mSceneWidth, mSceneHeight);

    // G-Buffer formats: albedo (RGBA8), normal (RGBA16F), material (RGBA8).
    constexpr DXGI_FORMAT kAlbedoFmt   = DXGI_FORMAT_R8G8B8A8_UNORM;
    // Must match kGBufferFormats[1] in DeferredLightingPass.cpp, which explains
    // why the normal target is 32-bit: it also carries device depth, and fp16
    // has nowhere near the precision that needs.
    constexpr DXGI_FORMAT kNormalFmt   = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr DXGI_FORMAT kMaterialFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    {
        PTERO_SCOPED_PASS_TIMER("Scene", "Entity meshes (G-Buffer)");
        mEntityMeshRenderer.Render(
            commandList,
            XMLoadFloat4x4(&mJitteredViewProjection),
            mCamera.GetPosition(),
            kAlbedoFmt,
            kNormalFmt,
            kMaterialFmt,
            SceneDepthFormat,
            mDeferredLightingPass.GetMsaaSampleCount());
    }

    // Terrain patches write into the same G-Buffer MRTs, sharing the depth
    // buffer with the entity meshes.  Render() internally syncs from the
    // entity list and rebuilds any dirty terrain mesh before drawing, so it
    // must run inside the geometry pass while the MRTs are still bound.
    {
        PTERO_SCOPED_PASS_TIMER("Scene", "Terrain");
        mTerrainRenderer.Render(
            commandList,
            XMLoadFloat4x4(&mJitteredViewProjection),
            kAlbedoFmt,
            kNormalFmt,
            kMaterialFmt,
            SceneDepthFormat,
            mDeferredLightingPass.GetMsaaSampleCount());
    }

    // Vegetation draws last in the geometry pass.  It is alpha-tested and
    // two-sided, so letting the opaque terrain and meshes lay down depth first
    // lets early-Z reject most of the foliage pixels that would be hidden
    // anyway -- which matters, because foliage is the heaviest overdraw in the
    // scene.
    mVegetationRenderer.Render(
        commandList,
        XMLoadFloat4x4(&mJitteredViewProjection),
        mCamera.GetPosition(),
        kAlbedoFmt,
        kNormalFmt,
        kMaterialFmt,
        SceneDepthFormat,
        mDeferredLightingPass.GetMsaaSampleCount());

    // Close the G-Buffer pass: transition G-Buffer RTs to SRV state.
    mDeferredLightingPass.EndGeometryPass(commandList);
    {
        PTERO_SCOPED_PASS_TIMER("Scene", "G-Buffer MSAA resolve");
        mDeferredLightingPass.ResolveGBuffer(commandList);
    }
    ResolveMsaaDepth(commandList);
    mMsaaResolveTimeMs = mDeferredLightingPass.GetLastResolveTimeMs();

    // Reset TAA history on scene content changes.
    const bool sceneContentChanged = IsSceneContentDirtyForTemporal();
    if (sceneContentChanged)
        mTaaSettings.ResetHistory = true;

    // -----------------------------------------------------------------------
    // PASS 2b – Custom RTGI (ReSTIR GI)
    // Dispatches four compute passes: RayGen → Temporal → Spatial → Composite.
    // The output is a RGBA16F GI radiance buffer bound as t5 in the deferred
    // lighting pass below.
    // -----------------------------------------------------------------------

    // The G-Buffer was left in PIXEL_SHADER_RESOURCE by EndGeometryPass, but compute
    // shaders require ALL_SHADER_RESOURCE state.  Promote before RTGI dispatch.
    mProbeSettings.GridX = (std::clamp)(mProbeSettings.GridX, 1, 64);
    mProbeSettings.GridY = (std::clamp)(mProbeSettings.GridY, 1, 64);
    mProbeSettings.GridZ = (std::clamp)(mProbeSettings.GridZ, 1, 64);

    const bool probesEnabled = mProbeSettings.Enabled && mEntities != nullptr;
    const bool probesDebugEnabled = probesEnabled && mProbeSettings.DebugShowProbes;
    const bool rtgiWillRun = mRtgiSettings.Enabled && mEntities != nullptr && !probesEnabled;
    const bool rtaoWillRun = mRtaoSettings.Enabled && mEntities != nullptr;
    // Known only now: the G-Buffer pass above is what registers subsurface materials.
    const bool subsurfaceWillRun = mSubsurfaceSettings.Enabled && SubsurfaceProfiles::AnyActiveThisFrame();
    const bool subsurfaceRayTracedWillRun = subsurfaceWillRun
        && mSubsurfaceSettings.Mode == 1
        && mEntities != nullptr;
    const bool gtaoWillRun = mGtaoSettings.Enabled;
    const bool volumetricFogWillRun = mVolumetricFogSettings.Enabled && mVolumetricFogRenderer.IsInitialized();

    // The fog takes its indirect light from the radiance probe grid, which is
    // the engine's only world-space irradiance field - the RTGI accumulation
    // buffer is screen-space and has nothing to say about a froxel behind a
    // wall. So the grid may need to run for the fog alone, without probes
    // taking over surface GI from RTGI the way mProbeSettings.Enabled does.
    const bool fogWantsProbeGi = volumetricFogWillRun
        && mVolumetricFogSettings.GiIntensity > 0.0f
        && mEntities != nullptr;
    const bool probeGridWillRun = probesEnabled || fogWantsProbeGi;
    // Clouds are part of the sky, so they go away with it rather than being left to
    // composite an unlit black layer over the scene.
    const bool volumetricCloudsWillRun = mVolumetricCloudSettings.Enabled
        && mVolumetricCloudRenderer.IsInitialized()
        && mTimeOfDaySettings.Enabled;
    if (rtgiWillRun || rtaoWillRun || gtaoWillRun || volumetricFogWillRun || volumetricCloudsWillRun || probeGridWillRun || probesDebugEnabled)
    {
        D3D12_RESOURCE_BARRIER toNonPixel[3];
        for (UINT i = 0; i < 3; ++i)
        {
            toNonPixel[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mDeferredLightingPass.GetGBufferResource(i),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        commandList->ResourceBarrier(3, toNonPixel);

        if (mDepthBufferState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
        {
            auto toDepthSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                mSceneDepthTarget.Get(),
                mDepthBufferState,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &toDepthSrv);
            mDepthBufferState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        }
    }

    // Filled in by the radiance probe block further down, which runs before the
    // fog is dispatched. Left null when no grid is available, in which case the
    // fog simply injects without indirect light.
    D3D12_GPU_DESCRIPTOR_HANDLE fogProbeSrv{};
    XMFLOAT3 fogProbeOrigin{};

    const auto dispatchVolumetricFog = [&]()
    {
        if (!volumetricFogWillRun)
        {
            mDeferredLightingPass.SetVolumetricFogSrv({}, nullptr);
            return;
        }

        mVolumetricFogRenderer.EnsureSize(mSceneWidth, mSceneHeight, mVolumetricFogSettings);

        const XMMATRIX fogViewProjection = XMLoadFloat4x4(&mJitteredViewProjection);
        XMFLOAT4X4 invVP;
        XMStoreFloat4x4(&invVP, XMMatrixTranspose(XMMatrixInverse(nullptr, fogViewProjection)));

        XMFLOAT4X4 currJitteredVP;
        XMStoreFloat4x4(&currJitteredVP, XMMatrixTranspose(fogViewProjection));

        const XMFLOAT3 camPos = mCamera.GetPosition();

        // Compact the scene's lights down to the ones the artist let into the
        // medium. mCachedPointLights is read rather than a parallel array built
        // at gather time so the fog gets the shadow indices the point shadow
        // pass wrote into it, and so its falloff is driven by the same record
        // the walls are lit from. There is a fog slot per scene light, so this
        // can no longer run out and quietly drop one.
        VolumetricFogRenderer::FogPointLight fogLights[VolumetricFogRenderer::kMaxPointLights]{};
        uint32_t numFogLights = 0;
        for (int i = 0; i < mNumCachedPointLights; ++i)
        {
            if (mCachedPointLightAffectsFog[i])
                fogLights[numFogLights++] = mCachedPointLights[i];
        }

        VolumetricFogRenderer::FrameInputs fogInputs{};
        fogInputs.ViewProjInv = invVP.m[0];
        fogInputs.CurrViewProj = currJitteredVP.m[0];
        fogInputs.CameraPos = &camPos.x;
        fogInputs.NearPlane = 0.1f;
        fogInputs.FarPlane = mVolumetricFogSettings.MaxDistance;
        fogInputs.SunDir[0] = mHosekResult.SunDirX;
        fogInputs.SunDir[1] = mHosekResult.SunDirY;
        fogInputs.SunDir[2] = mHosekResult.SunDirZ;
        fogInputs.SunColor[0] = sunR;
        fogInputs.SunColor[1] = sunG;
        fogInputs.SunColor[2] = sunB;
        fogInputs.SkyColor[0] = skyR;
        fogInputs.SkyColor[1] = skyG;
        fogInputs.SkyColor[2] = skyB;
        fogInputs.PointLights = fogLights;
        fogInputs.NumPointLights = numFogLights;

        if (fogProbeSrv.ptr != 0)
        {
            fogInputs.ProbeSrv = fogProbeSrv;
            fogInputs.ProbeGridX = static_cast<uint32_t>((std::max)(mProbeSettings.GridX, 0));
            fogInputs.ProbeGridY = static_cast<uint32_t>((std::max)(mProbeSettings.GridY, 0));
            fogInputs.ProbeGridZ = static_cast<uint32_t>((std::max)(mProbeSettings.GridZ, 0));
            fogInputs.ProbeSpacing = mProbeSettings.Spacing;
            fogInputs.ProbeOrigin[0] = fogProbeOrigin.x;
            fogInputs.ProbeOrigin[1] = fogProbeOrigin.y;
            fogInputs.ProbeOrigin[2] = fogProbeOrigin.z;
        }

        // Shadowed shafts. The cubemaps were rendered long before this point in
        // the frame and are left in ALL_SHADER_RESOURCE precisely so a compute
        // pass can read them.
        if (mPointShadowMapRenderer.IsInitialized() && mPointShadowMapRenderer.GetActiveLightCount() > 0)
        {
            fogInputs.PointShadowSrv = mPointShadowMapRenderer.GetShadowTextureArraySrvGpuHandle();
            fogInputs.PointShadowFaceViewProj = mPointShadowMapRenderer.GetAllFaceViewProjections();
            fogInputs.PointShadowLightCount = mPointShadowMapRenderer.GetActiveLightCount();
            fogInputs.PointShadowMapSize = static_cast<float>(mPointShadowMapRenderer.GetMapSize());
            fogInputs.PointShadowBias = mPointShadowMapRenderer.GetShadowBias();
        }

        // No ray tracing anywhere in this pass, so no TLAS gate and no
        // ID3D12GraphicsCommandList4. Requiring one used to make the fog vanish
        // with no diagnostic whenever RTGI was off or its TLAS was not ready.
        mVolumetricFogRenderer.Dispatch(
            commandList,
            mDepthSrvGpuHandle,
            mVolumetricFogSettings,
            fogInputs);

        mDeferredLightingPass.SetVolumetricFogSrv(
            mVolumetricFogRenderer.GetIntegratedFogSrv(),
            &mVolumetricFogSettings);
    };

    if (mRtgiSettings.Enabled && mEntities != nullptr)
    {
        // Lazy-initialize once; stop retrying after a permanent failure.
        if (!mRtgiRenderer.IsInitialized() && !mRtgiRenderer.HasInitFailed())
        {
            if (!mRtgiRenderer.Initialize(mSceneWidth, mSceneHeight))
            {
                std::string rtgiErr = "RTGI init failed";
                if (mRtgiRenderer.GetLastError()) rtgiErr += std::string(": ") + mRtgiRenderer.GetLastError();
                OutputDebugStringA((rtgiErr + "\n").c_str());
                mLastErrorMessage = rtgiErr;
                // Write error to a log file so it is visible without a debugger attached.
                {
                    wchar_t exePath[MAX_PATH]{};
                    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                    namespace fs = std::filesystem;
                    fs::path logPath = fs::path(exePath).parent_path() / L"rtgi_init_error.txt";
                    std::ofstream log(logPath, std::ios::out | std::ios::trunc);
                    if (log.is_open()) log << rtgiErr;
                }
            }
        }
    }

    // Build the shared raytracing scene once per frame and reuse it across RTGI,
    // RTAO, volumetric fog, and probe updates. Rebuilding multiple times in the
    // same frame can retire BLAS upload buffers too early when new geometry is
    // added, which can hang the GPU.
    // The fog pass itself traces nothing; it is in this list only through the
    // probe grid it may ask for.
    const bool sharedTlasNeeded = mEntities != nullptr
        && (rtgiWillRun || rtaoWillRun || probeGridWillRun || subsurfaceRayTracedWillRun);
    if (sharedTlasNeeded)
    {
        PTERO_SCOPED_PASS_TIMER("GI", "TLAS build");
        if (!mRtgiRenderer.IsInitialized() && !mRtgiRenderer.HasInitFailed())
        {
            mRtgiRenderer.Initialize(mSceneWidth, mSceneHeight);
        }

        if (mRtgiRenderer.IsInitialized())
        {
            mRtgiRenderer.EnsureSize(mSceneWidth, mSceneHeight);

            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> sharedRtCmdList;
            if (SUCCEEDED(commandList->QueryInterface(IID_PPV_ARGS(&sharedRtCmdList))))
            {
                // Only layers explicitly marked ContributeToRayTracing enter the
                // acceleration structure, and only near the camera.  Tracing a
                // whole field of alpha-tested grass would cost far more than the
                // bounce light it contributes; distant and unmarked vegetation
                // is lit by the radiance probe grid instead.
                std::vector<VegetationRenderer::RayTracingBatch> vegetationBatches;
                if (mVegetationRenderer.HasRayTracedLayers())
                {
                    constexpr float kVegetationRayTracingRadius = 60.0f;
                    mVegetationRenderer.CollectRayTracingBatches(
                        mCamera.GetPosition(), kVegetationRayTracingRadius, vegetationBatches);
                }

                mRtgiRenderer.BuildTlas(
                    sharedRtCmdList.Get(),
                    *mEntities,
                    vegetationBatches.empty() ? nullptr : &vegetationBatches);
            }
        }
    }

    if (rtgiWillRun && mRtgiRenderer.IsInitialized() && mEntities != nullptr)
    {
        PTERO_SCOPED_PASS_TIMER("GI", "RTGI");
        mRtgiRenderer.EnsureSize(mSceneWidth, mSceneHeight);

        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        if (SUCCEEDED(commandList->QueryInterface(IID_PPV_ARGS(&cmdList4))))
        {
            // Reconstructing current-frame world positions must use the same jittered
            // view-projection that wrote the G-Buffer depth this frame.  History
            // reprojection stays non-jittered separately via mPreviousViewProjectionForRtgi.
            const XMMATRIX rtgiViewProjection = XMLoadFloat4x4(&mJitteredViewProjection);
            XMFLOAT4X4 invVP;
            XMStoreFloat4x4(&invVP, XMMatrixTranspose(XMMatrixInverse(nullptr, rtgiViewProjection)));

            // Previous frame VP is used by the temporal pass to reproject reservoir samples.
            XMFLOAT4X4 prevVP;
            XMStoreFloat4x4(&prevVP, XMMatrixTranspose(XMLoadFloat4x4(&mPreviousViewProjectionForRtgi)));

            XMFLOAT4X4 currNonJitteredVP;
            XMStoreFloat4x4(&currNonJitteredVP, XMMatrixTranspose(XMLoadFloat4x4(&mNonJitteredViewProjection)));

            const XMFLOAT3 camPos = mCamera.GetPosition();

            // Dispatch the RTGI passes. NRD expects the original non-jittered camera matrices,
            // not the transposed row-vector versions used by the engine's own HLSL shaders.
            const XMFLOAT4X4& worldToView     = mNonJitteredViewMatrix;
            const XMFLOAT4X4& prevWorldToView = mPrevNonJitteredViewMatrix;
            const XMFLOAT4X4& viewToClip      = mNonJitteredProjectionMatrix;
            const XMFLOAT4X4& prevViewToClip  = mPrevNonJitteredProjectionMatrix;

            mRtgiRenderer.Dispatch(
                cmdList4.Get(),
                mDeferredLightingPass.GetSrvs().Albedo,   // t0: albedo G-buffer
                mDeferredLightingPass.GetSrvs().Normal,   // t1: normal+depth G-buffer
                mDeferredLightingPass.GetSrvs().Material, // t2: roughness/metallic/AO G-buffer
                mRtgiSettings,
                invVP.m[0],
                currNonJitteredVP.m[0],
                prevVP.m[0],
                &camPos.x,
                // Sun direction and colours from the Hosek-Wilkie sky model.
                mHosekResult.SunDirX, mHosekResult.SunDirY, mHosekResult.SunDirZ,
                sunR, sunG, sunB,
                skyR, skyG, skyB,
                mGiPointLights,
                static_cast<uint32_t>(mNumCachedPointLights),
                worldToView.m[0],
                viewToClip.m[0],
                prevWorldToView.m[0],
                prevViewToClip.m[0],
                mCurrentCameraJitter,
                mPrevCameraJitter,
                mFrameDeltaTimeMs);

            // Bind the RTGI output as the GI radiance source in the deferred lighting pass.
            mDeferredLightingPass.SetGiSrv(mRtgiRenderer.GetOutputSrv(), mRtgiSettings.GiIntensity);
            mDeferredLightingPass.SetProbeSrv({}, nullptr, mCamera.GetPosition());
            mDeferredLightingPass.SetRtgiDebugView(mRtgiSettings.DebugView);

            // Bind the RTGI specular reflections if enabled.
            if (mRtgiSettings.SpecularEnabled)
                mDeferredLightingPass.SetSpecularSrv(mRtgiRenderer.GetSpecularSrv(), mRtgiSettings.SpecularIntensity);
            else
                mDeferredLightingPass.SetSpecularSrv({}, 0.0f);

            // Save the non-jittered VP for next frame's temporal reprojection.
            mPreviousViewProjectionForRtgi = mNonJitteredViewProjection;
            mPrevCameraPositionForRtgi     = camPos;
        }
    }
    else
    {
        // RTGI disabled – clear the GI and specular contributions in the deferred lighting pass.
        mDeferredLightingPass.SetGiSrv({}, 0.0f);
        mDeferredLightingPass.SetSpecularSrv({}, 0.0f);
        mDeferredLightingPass.SetRtgiDebugView(0);
    }

    // -----------------------------------------------------------------------
    // PASS 2b – Radiance Probe Update
    // Updates the world-space SH irradiance probe grid from TLAS ray samples.
    // -----------------------------------------------------------------------
    if (probeGridWillRun)
    {
        PTERO_SCOPED_PASS_TIMER("GI", "Radiance probes");

        // Probes only take over surface GI when they are the chosen GI mode.
        // Running the grid purely to give the fog something to scatter must not
        // change how the walls are lit, and leaving last frame's SRV bound
        // would do exactly that.
        if (!probesEnabled)
            mDeferredLightingPass.SetProbeSrv({}, nullptr, mCamera.GetPosition());

        if (!mProbeRenderer.IsInitialized() && !mProbeRenderer.HasInitFailed())
        {
            if (!mProbeRenderer.Initialize(mProbeSettings))
            {
                OutputDebugStringA("RadianceProbeRenderer init failed\n");
                if (mProbeRenderer.GetLastError())
                {
                    OutputDebugStringA(mProbeRenderer.GetLastError());
                    OutputDebugStringA("\n");
                }
            }
        }

        if (mProbeRenderer.IsInitialized())
        {
            const XMFLOAT3 camPos = mCamera.GetPosition();
            const uint32_t totalProbes = static_cast<uint32_t>(
                mProbeSettings.GridX * mProbeSettings.GridY * mProbeSettings.GridZ);
            if (totalProbes != mProbeRenderer.GetTotalProbes())
            {
                if (!mProbeRenderer.Initialize(mProbeSettings) && mProbeRenderer.GetLastError())
                {
                    OutputDebugStringA(mProbeRenderer.GetLastError());
                    OutputDebugStringA("\n");
                }
                mProbeFrameIndex = 0;
            }

            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> probeCmdList4;
            if (SUCCEEDED(commandList->QueryInterface(IID_PPV_ARGS(&probeCmdList4))))
            {
                ID3D12DescriptorHeap* sharedSrvHeap = DX12Context_GetSrvDescriptorHeap();
                if (sharedSrvHeap)
                    probeCmdList4->SetDescriptorHeaps(1, &sharedSrvHeap);

                if (!mRtgiRenderer.IsInitialized() && !mRtgiRenderer.HasInitFailed())
                    mRtgiRenderer.Initialize(mSceneWidth, mSceneHeight);

                if (mRtgiRenderer.IsInitialized() && mRtgiRenderer.IsTlasReady())
                {
                    mProbeRenderer.Update(
                        probeCmdList4.Get(),
                        mRtgiRenderer.GetTlasSrv(),
                        mRtgiRenderer.GetVertexSrv(),
                        mRtgiRenderer.GetIndexSrv(),
                        mRtgiRenderer.GetInstanceInfoSrv(),
                        mRtgiRenderer.GetMaterialRangeSrv(),
                        mRtgiRenderer.GetBaseTextureTableSrv(),
                        mProbeSettings,
                        mRtgiSettings.ColorLeakIntensity,
                        mRtgiSettings.MaxBounces,
                        mGiPointLights,
                        static_cast<uint32_t>(mNumCachedPointLights),
                        &camPos.x,
                        mHosekResult.SunDirX, mHosekResult.SunDirY, mHosekResult.SunDirZ,
                        sunR, sunG, sunB,
                        skyR, skyG, skyB,
                        mProbeFrameIndex++);

                    // The fog samples the grid whether or not probes are also
                    // driving surface GI, so it takes the SRV here rather than
                    // from the deferred pass, which only gets it when probes
                    // are the chosen GI mode.
                    fogProbeSrv = mProbeRenderer.GetProbeSHSrv();
                    ResolveProbeGridOrigin(
                        mProbeSettings,
                        camPos.x, camPos.y, camPos.z,
                        fogProbeOrigin.x, fogProbeOrigin.y, fogProbeOrigin.z);

                    if (probesEnabled)
                    {
                        mDeferredLightingPass.SetGiSrv({}, mRtgiSettings.GiIntensity);
                        mDeferredLightingPass.SetProbeSrv(mProbeRenderer.GetProbeSHSrv(), &mProbeSettings, camPos);
                        mDeferredLightingPass.SetRtgiDebugView(mRtgiSettings.DebugView);
                    }
                }
                else if (probesEnabled)
                {
                    mDeferredLightingPass.SetGiSrv({}, 0.0f);
                    mDeferredLightingPass.SetProbeSrv({}, nullptr, camPos);
                    mDeferredLightingPass.SetRtgiDebugView(0);
                }
            }
        }
    }
    else
    {
        mDeferredLightingPass.SetProbeSrv({}, nullptr, mCamera.GetPosition());
    }
    // -----------------------------------------------------------------------
    // PASS 2c – Ray Traced Ambient Occlusion (RTAO)
    // Dispatches three compute passes: RayGen → Temporal → Spatial.
    // Shares the TLAS already built by RTGI (if RTGI ran this frame).
    // If RTGI did not run, RTAO builds its own TLAS via a QueryInterface.
    // -----------------------------------------------------------------------
    if (rtaoWillRun && mEntities != nullptr)
    {
        PTERO_SCOPED_PASS_TIMER("AO", "RTAO");
        // Lazy-initialize once; stop retrying after a permanent failure.
        if (!mRtaoRenderer.IsInitialized() && !mRtaoRenderer.HasInitFailed())
        {
            if (!mRtaoRenderer.Initialize(mSceneWidth, mSceneHeight))
            {
                std::string aoErr = "RTAO init failed";
                if (mRtaoRenderer.GetLastError()) aoErr += std::string(": ") + mRtaoRenderer.GetLastError();
                OutputDebugStringA((aoErr + "\n").c_str());
                mLastErrorMessage = aoErr;
            }
        }
    }

    if (rtaoWillRun && mRtaoRenderer.IsInitialized() && mEntities != nullptr)
    {
        mRtaoRenderer.EnsureSize(mSceneWidth, mSceneHeight);

        if (sceneContentChanged)
        {
            mRtaoRenderer.ResetHistory();
            mPreviousViewProjectionForRtao = mJitteredViewProjection;
        }

        // If RTGI did not run this frame we need to build the TLAS ourselves.
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmdList4;
        if (SUCCEEDED(commandList->QueryInterface(IID_PPV_ARGS(&cmdList4))))
        {
            ID3D12DescriptorHeap* sharedSrvHeap = DX12Context_GetSrvDescriptorHeap();
            if (sharedSrvHeap)
                cmdList4->SetDescriptorHeaps(1, &sharedSrvHeap);

            // Build TLAS if RTGI didn't (avoids a duplicate build when both are enabled).
            if (mRtgiRenderer.IsInitialized() && mRtgiRenderer.IsTlasReady())
            {
                XMFLOAT4X4 invVP;
                const XMMATRIX nonJitteredVpMatrix = XMLoadFloat4x4(&mNonJitteredViewProjection);
                XMStoreFloat4x4(&invVP, XMMatrixTranspose(XMMatrixInverse(nullptr, nonJitteredVpMatrix)));

                XMFLOAT4X4 currNonJitteredVP;
                XMStoreFloat4x4(&currNonJitteredVP, XMMatrixTranspose(nonJitteredVpMatrix));

                XMFLOAT4X4 prevRtaoSource = mPreviousViewProjectionForRtao;
                if (prevRtaoSource._44 == 0.0f)
                {
                    prevRtaoSource = mNonJitteredViewProjection;
                }

                XMFLOAT4X4 prevRtaoVP;
                XMStoreFloat4x4(&prevRtaoVP, XMMatrixTranspose(XMLoadFloat4x4(&prevRtaoSource)));

                const XMFLOAT3 camPos = mCamera.GetPosition();

                mRtaoRenderer.Dispatch(
                    cmdList4.Get(),
                    mDeferredLightingPass.GetSrvs().Normal,  // t0: G-Buffer normal+depth
                    mDepthSrvGpuHandle,                      // t2: scene depth SRV
                    mRtgiRenderer.GetTlasSrv(),              // t1: shared TLAS
                    mRtaoSettings,
                    invVP.m[0],
                    currNonJitteredVP.m[0],
                    prevRtaoVP.m[0],
                    &camPos.x,
                    mNonJitteredViewMatrix.m[0],
                    mNonJitteredProjectionMatrix.m[0],
                    mPrevNonJitteredViewMatrix.m[0],
                    mPrevNonJitteredProjectionMatrix.m[0],
                    mCurrentCameraJitter,
                    mPrevCameraJitter,
                    mFrameDeltaTimeMs);

                mPreviousViewProjectionForRtao = mNonJitteredViewProjection;

                // Bind the RTAO output only when XeGTAO is not selected as the active AO source.
                if (!gtaoWillRun)
                {
                    const D3D12_GPU_DESCRIPTOR_HANDLE aoDebugSrv = (mRtaoSettings.DebugView == 1)
                        ? mRtaoRenderer.GetRawAOSrv()
                        : mRtaoRenderer.GetOutputSrv();
                    mDeferredLightingPass.SetAoSrv(aoDebugSrv, mRtaoSettings.Intensity, mRtaoSettings.DebugView);
                }
            }
        }
    }
    else if (!gtaoWillRun)
    {
        mDeferredLightingPass.SetAoSrv({}, 0.0f, 0);
    }

    // -----------------------------------------------------------------------
    // PASS 2d – XeGTAO (Screen-Space Ground Truth Ambient Occlusion)
    // Only runs when GTAO is enabled and RTAO is not already providing AO.
    // -----------------------------------------------------------------------
    if (gtaoWillRun)
    {
        PTERO_SCOPED_PASS_TIMER("AO", "XeGTAO");
        if (!mGtaoRenderer.IsInitialized() && !mGtaoRenderer.HasInitFailed())
        {
            if (!mGtaoRenderer.Initialize(mSceneWidth, mSceneHeight))
            {
                std::string err = "GTAO init failed";
                if (mGtaoRenderer.GetLastError()) err += std::string(": ") + mGtaoRenderer.GetLastError();
                OutputDebugStringA((err + "\n").c_str());
                mLastErrorMessage = err;
            }
        }

        if (mGtaoRenderer.IsInitialized())
        {
            mGtaoRenderer.EnsureSize(mSceneWidth, mSceneHeight);

            // Depth must be in ALL_SHADER_RESOURCE for compute (may already be there from RTAO/RTGI).
            if (mDepthBufferState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
            {
                auto b = CD3DX12_RESOURCE_BARRIER::Transition(
                    mSceneDepthTarget.Get(),
                    mDepthBufferState,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                commandList->ResourceBarrier(1, &b);
                mDepthBufferState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
            }

            // XeGTAO reconstructs view-space positions from the depth buffer, and that
            // buffer was rasterised with the TAA jitter applied - so it has to be given the
            // jittered projection, not the clean one. Handing it the non-jittered matrix is
            // what forced the jitter to be switched off whenever AO was on, which in turn
            // left TAA with nothing to accumulate. The view matrix carries no jitter.
            const float* projMat    = mJitteredProjection.m[0];
            const float* worldToView = mNonJitteredViewMatrix.m[0];

            // XeGTAO's raw output is per-pixel noise meant to be blurred by its own denoise
            // passes. Without one (the level may author 0), an upscaler magnifies that
            // grain from the lower render resolution straight onto the screen.
            GtaoSettings gtaoSettings = mGtaoSettings;
            if (UpscalerOwnsPostAaOutput())
                gtaoSettings.DenoisePasses = (std::max)(gtaoSettings.DenoisePasses, 1);

            mGtaoRenderer.Dispatch(
                commandList,
                mDeferredLightingPass.GetSrvs().Normal,
                mDepthSrvGpuHandle,
                gtaoSettings,
                projMat,
                worldToView,
                0); // fixed noise index - no temporal accumulation for GTAO

            // XeGTAO is the active AO source whenever it is enabled.
            mDeferredLightingPass.SetAoSrv(mGtaoRenderer.GetOutputSrv(), mGtaoSettings.Intensity, mGtaoSettings.DebugView);
        }
    }

    // Consume the scene-content changed flag once at the end of temporal pass setup.
    mEntityMeshRenderer.ConsumeSceneContentChangedFlag();

    {
        PTERO_SCOPED_PASS_TIMER("Volumetrics", "Fog inject");
        dispatchVolumetricFog();
    }

    // -----------------------------------------------------------------------
    // PASS 2f – Volumetric clouds
    // Raymarches the cloud shell at reduced resolution against the scene depth
    // buffer and reconstructs a full resolution result.  Runs here while depth
    // is still readable from compute; the composite happens after the deferred
    // lighting resolve, once the scene colour target holds the lit scene.
    // -----------------------------------------------------------------------
    if (volumetricCloudsWillRun)
    {
        PTERO_SCOPED_PASS_TIMER("Volumetrics", "Clouds");
        mVolumetricCloudRenderer.EnsureSize(mSceneWidth, mSceneHeight, mVolumetricCloudSettings);

        const XMMATRIX cloudViewProjection = XMLoadFloat4x4(&mNonJitteredViewProjection);
        XMFLOAT4X4 cloudInvViewProjection;
        XMStoreFloat4x4(
            &cloudInvViewProjection,
            XMMatrixTranspose(XMMatrixInverse(nullptr, cloudViewProjection)));

        XMFLOAT4X4 cloudPrevViewProjection;
        XMStoreFloat4x4(
            &cloudPrevViewProjection,
            XMMatrixTranspose(XMLoadFloat4x4(
                mHasPreviousCloudViewProjection ? &mPreviousViewProjectionForClouds : &mNonJitteredViewProjection)));

        const XMFLOAT3 cloudCameraPosition = mCamera.GetPosition();
        const float cloudSunDirection[3] = { mHosekResult.SunDirX, mHosekResult.SunDirY, mHosekResult.SunDirZ };
        const float cloudSunColor[3] = { sunR, sunG, sunB };
        const float cloudSkyColor[3] = { skyR, skyG, skyB };

        mVolumetricCloudRenderer.Dispatch(
            commandList,
            mDepthSrvGpuHandle,
            mVolumetricCloudSettings,
            cloudInvViewProjection.m[0],
            cloudPrevViewProjection.m[0],
            &cloudCameraPosition.x,
            cloudSunDirection,
            cloudSunColor,
            cloudSkyColor,
            mFrameDeltaTimeMs * 0.001f,
            !mHasPreviousCloudViewProjection || mTaaSettings.ResetHistory);

        if (mVolumetricCloudRenderer.GetLastError())
        {
            OutputDebugStringA(mVolumetricCloudRenderer.GetLastError());
            OutputDebugStringA("\n");
        }
    }

    // Dispatch the rain particle physics compute shader.
    if (mRainRenderer.IsInitialized() && mRainSettings.Enabled)
    {
        ID3D12DescriptorHeap* computeHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, computeHeaps);
        const XMFLOAT3 camPos = mCamera.GetPosition();
        const XMFLOAT3 rawCamForward = mCamera.GetForwardVector();
        const XMFLOAT3 camUp = { 0.0f, 0.0f, 1.0f };

        XMFLOAT3 camForward = { rawCamForward.x, rawCamForward.y, 0.0f };
        const float flatForwardLengthSq = camForward.x * camForward.x + camForward.y * camForward.y;
        if (flatForwardLengthSq > 1.0e-6f)
        {
            const float invLength = 1.0f / std::sqrt(flatForwardLengthSq);
            camForward.x *= invLength;
            camForward.y *= invLength;
        }
        else
        {
            camForward = { 0.0f, 1.0f, 0.0f };
        }

        XMFLOAT3 camRight;
        XMStoreFloat3(
            &camRight,
            XMVector3Normalize(
                XMVector3Cross(
                    XMLoadFloat3(&camUp),
                    XMLoadFloat3(&camForward))));
        const float deltaTimeSec = mFrameDeltaTimeMs * 0.001f;
        PTERO_SCOPED_PASS_TIMER("Scene", "Rain (simulate)");
        mRainRenderer.Dispatch(commandList, mRainSettings, camPos, camRight, camForward, camUp, deltaTimeSec);
    }

    // Simulate the authored particle systems. Runs alongside the rain, before
    // the G-Buffer goes back to being a pixel-shader resource, so the compute
    // work overlaps the rest of the frame's shading rather than stalling the
    // transparent pass that draws it.
    if (mParticleRenderer.IsInitialized() && !mParticleSystems.empty())
    {
        PTERO_SCOPED_PASS_TIMER("Scene", "Particles (simulate)");
        mParticleRenderer.Dispatch(
            commandList,
            mWindSettings.GetVelocityVector(),
            mFrameDeltaTimeMs * 0.001f);

        if (mParticleRenderer.GetLastError())
        {
            OutputDebugStringA(mParticleRenderer.GetLastError());
            OutputDebugStringA("\n");
        }
    }

    // Restore G-Buffer and depth back to PIXEL_SHADER_RESOURCE for the deferred lighting pass.
    if (rtgiWillRun || rtaoWillRun || volumetricFogWillRun || volumetricCloudsWillRun || gtaoWillRun || probeGridWillRun || probesDebugEnabled)
    {
        D3D12_RESOURCE_BARRIER toPixel[3];
        for (UINT i = 0; i < 3; ++i)
        {
            toPixel[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mDeferredLightingPass.GetGBufferResource(i),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        commandList->ResourceBarrier(3, toPixel);

        if (mDepthBufferState == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
        {
            const D3D12_RESOURCE_STATES restoredDepthState = mMsaaSettings.Enabled
                ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                : D3D12_RESOURCE_STATE_DEPTH_WRITE;
            auto restoreDepth = CD3DX12_RESOURCE_BARRIER::Transition(
                mSceneDepthTarget.Get(),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                restoredDepthState);
            commandList->ResourceBarrier(1, &restoreDepth);
            mDepthBufferState = restoredDepthState;
        }
    }

    // -----------------------------------------------------------------------
    // PASS 3 – Sky pass
    // Render the sky into the scene colour RT before the lighting pass so that
    // pixels with depth == 1.0 (no geometry) keep the sky colour.
    // -----------------------------------------------------------------------
    const auto toSceneRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        mSceneColorTarget.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &toSceneRenderTarget);

    const float sceneClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(mSceneRtvHandle, sceneClearColor, 0, nullptr);

    // The sky renderer writes directly into the scene colour RT (no DSV needed).
    commandList->OMSetRenderTargets(1, &mSceneRtvHandle, FALSE, nullptr);
    const D3D12_VIEWPORT vpFull = { 0, 0, static_cast<float>(mSceneWidth), static_cast<float>(mSceneHeight), 0, 1 };
    const D3D12_RECT     srFull = { 0, 0, static_cast<LONG>(mSceneWidth), static_cast<LONG>(mSceneHeight) };
    commandList->RSSetViewports(1, &vpFull);
    commandList->RSSetScissorRects(1, &srFull);

    // With time of day off there is no sky to draw; the scene target's black clear stands
    // in as the background.
    if (mSkyRenderer.IsInitialized() && mTimeOfDaySettings.Enabled)
    {
        PTERO_SCOPED_PASS_TIMER("Scene", "Sky");
        mSkyRenderer.Render(
            commandList,
            mHosekResult,
            mTimeOfDaySettings,
            XMLoadFloat4x4(&mJitteredProjection),
            mCamera.GetViewMatrix(),
            mSceneWidth,
            mSceneHeight);
    }

    // -----------------------------------------------------------------------
    // PASS 4 – Deferred lighting resolve
    // Transition scene depth to SRV state, then run the fullscreen lighting
    // quad that samples the G-Buffer + depth + shadow map.
    // -----------------------------------------------------------------------
    if (mDepthBufferState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        const auto toDepthSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            mSceneDepthTarget.Get(),
            mDepthBufferState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toDepthSrv);
        mDepthBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // Subsurface scattering: upload this frame's profiles and give the lighting resolve
    // somewhere to write the diffuse light it will scatter.
    bool subsurfaceActive = false;
    if (subsurfaceWillRun && mDeferredLightingPass.IsInitialized())
    {
        if (!mSubsurfaceRenderer.IsInitialized() && !mSubsurfaceRenderer.HasInitFailed())
        {
            if (!mSubsurfaceRenderer.Initialize(mSceneWidth, mSceneHeight, SceneColorFormat))
            {
                std::string sssError = "Subsurface scattering init failed";
                if (mSubsurfaceRenderer.GetLastError()) sssError += std::string(": ") + mSubsurfaceRenderer.GetLastError();
                PTERO_LOG_ERROR("Renderer", "%s", sssError.c_str());
                mLastErrorMessage = sssError;
            }
        }

        if (mSubsurfaceRenderer.IsInitialized() && mSubsurfaceRenderer.EnsureSize(mSceneWidth, mSceneHeight))
        {
            SubsurfaceScatteringRenderer::FrameInputs sssInputs;
            sssInputs.Settings = &mSubsurfaceSettings;
            sssInputs.ViewProjection = mJitteredViewProjection;
            sssInputs.Projection = mJitteredProjection;
            sssInputs.CameraPosition = mCamera.GetPosition();
            sssInputs.SunDirection = mDeferredLightingPass.GetSunDirection();
            sssInputs.SunColor = mDeferredLightingPass.GetSunColor();
            sssInputs.SkyAmbient = mDeferredLightingPass.GetSkyAmbient();
            sssInputs.Lights = mDeferredLightingPass.GetPointLights();
            sssInputs.NumLights = mDeferredLightingPass.GetPointLightCount();
            sssInputs.RayTracingAvailable = subsurfaceRayTracedWillRun
                && mRtgiRenderer.IsInitialized() && mRtgiRenderer.IsTlasReady();
            sssInputs.Shadows = &mDeferredLightingPass.GetShadowSnapshot();

            const D3D12_GPU_VIRTUAL_ADDRESS sssConstants = mSubsurfaceRenderer.PrepareFrame(sssInputs);
            if (sssConstants != 0)
            {
                mSubsurfaceRenderer.BeginLightingOutput(commandList);
                mDeferredLightingPass.SetSubsurface(sssConstants, mSubsurfaceRenderer.GetDiffuseRtv());
                subsurfaceActive = true;
            }
        }
    }
    if (!subsurfaceActive)
        mDeferredLightingPass.SetSubsurface(0, {});

    if (mDeferredLightingPass.IsInitialized())
    {
        ID3D12DescriptorHeap* shaderVisibleHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, shaderVisibleHeaps);

        PTERO_SCOPED_PASS_TIMER("Lighting", "Deferred resolve");
        mDeferredLightingPass.ResolveLight(
            commandList,
            mSceneRtvHandle,
            mDepthSrvGpuHandle,
            SceneColorFormat,
            mSceneWidth,
            mSceneHeight);
    }

    // -----------------------------------------------------------------------
    // PASS 4-SSS – Subsurface scattering
    // Scatters the diffuse light the resolve just split out of every subsurface pixel
    // (screen-space separable blur, or ray-traced surface probes) and swaps it back into
    // the scene colour. Runs before anything is layered over the opaque surfaces.
    // -----------------------------------------------------------------------
    if (subsurfaceActive)
    {
        PTERO_SCOPED_PASS_TIMER("Lighting", mSubsurfaceRenderer.IsRayTracedThisFrame()
            ? "Subsurface scattering (ray traced)" : "Subsurface scattering");

        // Read from compute, which needs a state covering non-pixel stages.
        D3D12_RESOURCE_BARRIER sssToCompute[3];
        for (UINT i = 0; i < 3; ++i)
        {
            sssToCompute[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mDeferredLightingPass.GetGBufferResource(i),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        commandList->ResourceBarrier(3, sssToCompute);

        const GBufferSrvs& gbufferSrvs = mDeferredLightingPass.GetSrvs();
        SubsurfaceScatteringRenderer::RayTracedSceneSrvs rtScene;
        // The ray-traced pass shadows its samples with the lighting pass's shadow maps,
        // from compute; the sun map rests in PIXEL_SHADER_RESOURCE, so widen it for the
        // dispatch and put it back afterwards. (The point shadow array already rests in
        // ALL_SHADER_RESOURCE.)
        ID3D12Resource* sunShadowTexture = nullptr;
        if (mSubsurfaceRenderer.IsRayTracedThisFrame())
        {
            const DeferredLightingPass::ShadowSnapshot& shadows = mDeferredLightingPass.GetShadowSnapshot();
            rtScene.Tlas = mRtgiRenderer.GetTlasSrv();
            rtScene.Vertices = mRtgiRenderer.GetVertexSrv();
            rtScene.Indices = mRtgiRenderer.GetIndexSrv();
            rtScene.InstanceInfo = mRtgiRenderer.GetInstanceInfoSrv();
            rtScene.SunShadow = shadows.SunShadowSrv;
            rtScene.PointShadows = shadows.PointShadowSrv;

            sunShadowTexture = shadows.SunShadowSrv.ptr != 0 ? mShadowMapRenderer.GetShadowTexture() : nullptr;
            if (sunShadowTexture)
            {
                const auto toAll = CD3DX12_RESOURCE_BARRIER::Transition(
                    sunShadowTexture,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                commandList->ResourceBarrier(1, &toAll);
            }
        }
        mSubsurfaceRenderer.Apply(commandList, mSceneRtvHandle, gbufferSrvs.Albedo, gbufferSrvs.Normal, rtScene);
        if (sunShadowTexture)
        {
            const auto toPixel = CD3DX12_RESOURCE_BARRIER::Transition(
                sunShadowTexture,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &toPixel);
        }

        D3D12_RESOURCE_BARRIER sssToPixel[3];
        for (UINT i = 0; i < 3; ++i)
        {
            sssToPixel[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mDeferredLightingPass.GetGBufferResource(i),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        commandList->ResourceBarrier(3, sssToPixel);

        // Later passes bind their own render targets, but some expect the scene target
        // and full viewport to still be current, as the lighting resolve left them.
        commandList->OMSetRenderTargets(1, &mSceneRtvHandle, FALSE, nullptr);
        commandList->RSSetViewports(1, &vpFull);
        commandList->RSSetScissorRects(1, &srFull);
    }

    // -----------------------------------------------------------------------
    // PASS 4a – Volumetric cloud composite
    // Blends the resolved cloud layer over the lit scene with
    // (ONE, SRC_ALPHA), i.e. scene * transmittance + in-scattered luminance.
    // The raymarch already clipped against opaque depth, so nearby geometry
    // correctly occludes the layer without a depth test here.
    // -----------------------------------------------------------------------
    if (volumetricCloudsWillRun && mVolumetricCloudRenderer.DidDispatchThisFrame())
    {
        mVolumetricCloudRenderer.Composite(commandList, mSceneRtvHandle, mSceneWidth, mSceneHeight);
    }

    // Build a remapped depth preview texture for the editor debug window.
    RenderDepthDebugPreview(commandList);

    // -----------------------------------------------------------------------
    // PASS 4a – Refractive water
    // Forward pass over the lit opaque scene.  Reads a copy of the scene colour
    // (refraction) and the opaque depth (occlusion + absorption thickness), and
    // composites water with wave animation, Fresnel sky/sun reflection and foam
    // straight into the scene-colour RT.  Runs here while the scene colour is a
    // render target and the depth is still readable as an SRV.
    // -----------------------------------------------------------------------
    {
        mWaterTimeSeconds += mFrameDeltaTimeMs * 0.001f;
        const XMFLOAT3 waterSunDir(mHosekResult.SunDirX, mHosekResult.SunDirY, mHosekResult.SunDirZ);
        PTERO_SCOPED_PASS_TIMER("Scene", "Water");
        mWaterRenderer.Render(
            commandList,
            mSceneRtvHandle,
            mSceneColorTarget.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            mDepthSrvGpuHandle,
            XMLoadFloat4x4(&mJitteredViewProjection),
            mInvViewProjection,
            mCamera.GetPosition(),
            waterSunDir,
            mFrameSunColor,
            mFrameSkyColor,
            mWaterTimeSeconds,
            mSceneWidth,
            mSceneHeight,
            SceneColorFormat);
    }

    // -----------------------------------------------------------------------
    // PASS 4b – Radiance Probe Debug Overlay
    // Draws coloured spheres at each probe position if debug visualisation is on.
    // -----------------------------------------------------------------------
    if (!mMsaaSettings.Enabled
        && mProbeSettings.Enabled && mProbeSettings.DebugShowProbes
        && mProbeRenderer.IsInitialized())
    {
        const auto toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            mSceneDepthTarget.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commandList->ResourceBarrier(1, &toDepthWrite);
        mDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        ID3D12DescriptorHeap* shaderVisibleHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, shaderVisibleHeaps);

        const XMMATRIX probeDebugViewProjection = XMLoadFloat4x4(&mNonJitteredViewProjection);
        XMFLOAT4X4 probeDebugVP;
        XMFLOAT4X4 probeDebugInvVP;
        XMStoreFloat4x4(&probeDebugVP, XMMatrixTranspose(probeDebugViewProjection));
        XMStoreFloat4x4(&probeDebugInvVP, XMMatrixTranspose(XMMatrixInverse(nullptr, probeDebugViewProjection)));

        mProbeRenderer.DrawDebug(
            commandList,
            mSceneRtvHandle,
            mSceneDsvHandle,
            SceneColorFormat,
            mSceneWidth,
            mSceneHeight,
            probeDebugVP.m[0],
            probeDebugInvVP.m[0]);

        if (mProbeRenderer.GetLastError())
        {
            OutputDebugStringA(mProbeRenderer.GetLastError());
            OutputDebugStringA("\n");
        }
    }

    // -----------------------------------------------------------------------
    // PASS 5 – Grid overlay
    // Transition depth back to DEPTH_WRITE and bind it so the grid and gizmos
    // correctly depth-test against the geometry already rendered.
    // -----------------------------------------------------------------------
    const bool depthTestedOverlaysAvailable = !mMsaaSettings.Enabled;
    if (depthTestedOverlaysAvailable && mDepthBufferState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        const auto toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
            mSceneDepthTarget.Get(),
            mDepthBufferState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commandList->ResourceBarrier(1, &toDepthWrite);
        mDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    if (depthTestedOverlaysAvailable)
    {
        commandList->SetGraphicsRootSignature(mPipeline.GetRootSignature());
        commandList->SetPipelineState(mPipeline.GetPipelineState());
        // Bind the scene colour RTV AND the depth buffer so depth-testing works.
        commandList->OMSetRenderTargets(1, &mSceneRtvHandle, FALSE, &mSceneDsvHandle);
        commandList->RSSetViewports(1, &vpFull);
        commandList->RSSetScissorRects(1, &srFull);

        if (mGridEnabled)
        {
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            commandList->IASetVertexBuffers(0, 1, &mVertexBufferView);
            commandList->SetGraphicsRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
            commandList->DrawInstanced(mVertexCount, 1, 0, 0);
        }

        // -----------------------------------------------------------------------
        // PASS 6 – Point light gizmos (editor wireframe spheres)
        // -----------------------------------------------------------------------
        if (mPointLightRenderer.IsInitialized() && mEntities != nullptr)
        {
            mPointLightRenderer.Render(commandList, *mEntities, XMLoadFloat4x4(&mJitteredViewProjection));
        }

        // -----------------------------------------------------------------------
        // PASS 7 – Decal gizmos (wireframe box + facing arrow)
        // -----------------------------------------------------------------------
        if (mDecalRenderer.IsInitialized() && mEntities != nullptr)
        {
            mDecalRenderer.Render(commandList, *mEntities, XMLoadFloat4x4(&mJitteredViewProjection));
            mDecalRenderer.RenderVegetationAreas(commandList, *mEntities, XMLoadFloat4x4(&mJitteredViewProjection));
        }
    }

    // -----------------------------------------------------------------------
    // Rain streak draw pass – additive transparent pass over the full scene.
    // -----------------------------------------------------------------------
    if (depthTestedOverlaysAvailable && mRainRenderer.IsInitialized() && mRainSettings.Enabled)
    {
        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
        commandList->OMSetRenderTargets(1, &mSceneRtvHandle, FALSE, &mSceneDsvHandle);
        commandList->RSSetViewports(1, &vpFull);
        commandList->RSSetScissorRects(1, &srFull);
        XMFLOAT4X4 rainViewProjection;
        XMStoreFloat4x4(&rainViewProjection, XMMatrixTranspose(XMLoadFloat4x4(&mJitteredViewProjection)));
        const XMFLOAT3 rainCameraPosition = mCamera.GetPosition();
        const XMFLOAT3 rainCameraUp = mCamera.GetUpVector();
        const XMFLOAT3 rainCameraForward = mCamera.GetForwardVector();
        XMFLOAT3 rainCameraRight{};
        XMStoreFloat3(
            &rainCameraRight,
            XMVector3Normalize(
                XMVector3Cross(
                    XMLoadFloat3(&rainCameraUp),
                    XMLoadFloat3(&rainCameraForward))));

        XMFLOAT3 primaryLightPosition = { rainCameraPosition.x, rainCameraPosition.y, rainCameraPosition.z + 4.0f };
        XMFLOAT3 primaryLightColor = { 0.7f, 0.75f, 0.85f };
        float primaryLightIntensity = 0.6f;

        if (mEntities != nullptr)
        {
            constexpr float kRefLumens = 800.0f;
            float brightestLight = 0.0f;
            for (const Entity& entity : *mEntities)
            {
                if (!entity.HasPointLightComponent())
                    continue;

                const PointLightComponent& pointLight = *entity.PointLight;
                const float normalizedIntensity = pointLight.IntensityLumens / kRefLumens;
                if (normalizedIntensity <= brightestLight)
                    continue;

                brightestLight = normalizedIntensity;
                primaryLightPosition = entity.Transform.Position;
                primaryLightColor = pointLight.UseTemperature
                    ? KelvinToLinearRgb(pointLight.TemperatureKelvin)
                    : XMFLOAT3(pointLight.ColorR, pointLight.ColorG, pointLight.ColorB);
                primaryLightIntensity = normalizedIntensity;
            }
        }

        PTERO_SCOPED_PASS_TIMER("Scene", "Rain (draw)");
        mRainRenderer.Draw(
            commandList,
            mRainSettings,
            rainViewProjection,
            rainCameraPosition,
            rainCameraRight,
            rainCameraUp,
            primaryLightPosition,
            primaryLightColor,
            primaryLightIntensity);
    }

    // -----------------------------------------------------------------------
    // Particle sprite draw - additive/alpha transparent pass over the lit scene.
    //
    // Two depth strategies, because MSAA changes what can be bound:
    //
    //   No MSAA - depth moves to a read-only state and the pass binds the
    //     read-only DSV, so sprites depth-test in hardware while the pixel
    //     shader samples that same depth for the soft fade. Without the
    //     read-only view the two uses would conflict, and without the fade a
    //     flame cuts a hard line where it meets the floor.
    //
    //   MSAA - the depth-stencil is the multisampled target while the scene
    //     colour target stays single-sample, so no pipeline state can be bound
    //     against both, and the resolved depth everything else reads has no
    //     depth-stencil view (D3D12 forbids ALLOW_DEPTH_STENCIL together with
    //     the ALLOW_UNORDERED_ACCESS the resolve needs). So the pass binds no
    //     depth at all and the shader tests the resolved depth itself.
    // -----------------------------------------------------------------------
    if (mParticleRenderer.IsInitialized() && !mParticleSystems.empty())
    {
        const bool manualDepthTest = mMsaaSettings.Enabled;

        if (manualDepthTest)
        {
            // Only the resolved depth is read, and as an ordinary texture.
            if (mDepthBufferState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
                mDepthBufferState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
            {
                const auto toPixelShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
                    mSceneDepthTarget.Get(),
                    mDepthBufferState,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                commandList->ResourceBarrier(1, &toPixelShaderResource);
                mDepthBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
        }
        else
        {
            constexpr D3D12_RESOURCE_STATES kDepthReadState =
                D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

            if (mDepthBufferState != kDepthReadState)
            {
                const auto toDepthRead = CD3DX12_RESOURCE_BARRIER::Transition(
                    mSceneDepthTarget.Get(),
                    mDepthBufferState,
                    kDepthReadState);
                commandList->ResourceBarrier(1, &toDepthRead);
                mDepthBufferState = kDepthReadState;
            }
        }

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
        if (manualDepthTest)
        {
            commandList->OMSetRenderTargets(1, &mSceneRtvHandle, FALSE, nullptr);
        }
        else
        {
            commandList->OMSetRenderTargets(1, &mSceneRtvHandle, FALSE, &mSceneReadOnlyDsvHandle);
        }
        commandList->RSSetViewports(1, &vpFull);
        commandList->RSSetScissorRects(1, &srFull);

        ParticleDrawContext particleContext;
        XMStoreFloat4x4(
            &particleContext.ViewProjection,
            XMMatrixTranspose(XMLoadFloat4x4(&mJitteredViewProjection)));

        particleContext.CameraPosition = mCamera.GetPosition();
        particleContext.CameraUp = mCamera.GetUpVector();
        particleContext.CameraForward = mCamera.GetForwardVector();
        XMStoreFloat3(
            &particleContext.CameraRight,
            XMVector3Normalize(
                XMVector3Cross(
                    XMLoadFloat3(&particleContext.CameraUp),
                    XMLoadFloat3(&particleContext.CameraForward))));

        particleContext.NearPlane = mCamera.GetNearPlane();
        particleContext.FarPlane = mCamera.GetFarPlane();
        particleContext.ScreenWidth = static_cast<float>(mSceneWidth);
        particleContext.ScreenHeight = static_cast<float>(mSceneHeight);
        particleContext.SceneDepthSrv = mDepthSrvGpuHandle;
        particleContext.ManualDepthTest = manualDepthTest;

        particleContext.SunDirection = { mHosekResult.SunDirX, mHosekResult.SunDirY, mHosekResult.SunDirZ };
        particleContext.SunColor = mFrameSunColor;
        particleContext.SkyColor = mFrameSkyColor;

        // Scene lights that may illuminate the sprites. This is what lets smoke
        // rising off a fire glow orange from underneath instead of staying a
        // flat grey plume - the fire's own proxy light is in this array too.
        const int sceneLightCount =
            (std::min)(mNumCachedPointLights, ParticleDrawContext::kMaxSceneLights);
        for (int i = 0; i < sceneLightCount; ++i)
        {
            particleContext.SceneLights[i].Position = mCachedPointLights[i].Position;
            particleContext.SceneLights[i].Radius = mCachedPointLights[i].Radius;
            particleContext.SceneLights[i].Color = mCachedPointLights[i].Color;
            particleContext.SceneLights[i].InvRadiusSq = mCachedPointLights[i].InvRadiusSq;
        }
        particleContext.NumSceneLights = sceneLightCount;

        PTERO_SCOPED_PASS_TIMER("Scene", "Particles (draw)");
        mParticleRenderer.Draw(commandList, particleContext);
    }

    // -----------------------------------------------------------------------
    // Transition scene colour to SRV for Ui display (and TAA input).
    // -----------------------------------------------------------------------
    const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        mSceneColorTarget.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShaderResource);

    // -----------------------------------------------------------------------
    // PASS 6.5 – Screen-space reflections (optional)
    // Runs last of the shading passes so what it reflects is the finished image -
    // lighting, sky, clouds and water included - and before anti-aliasing so the
    // reflections get temporally resolved along with everything else. The pass
    // composites in place, leaving the scene colour target the current image as before.
    // -----------------------------------------------------------------------
    const bool sssrRequested = mSsrSettings.Enabled && mSsrSettings.Technique == 1;
    if (sssrRequested && !mSssrRenderer.IsInitialized() && !mSssrInitFailed)
    {
        if (!mSssrRenderer.Initialize(mSceneWidth, mSceneHeight))
        {
            OutputDebugStringA("DX12SceneRenderer: FidelityFX SSSR initialization failed - using the ray-march SSR instead.\n");
            if (mSssrRenderer.GetLastErrorMessage())
            {
                OutputDebugStringA(mSssrRenderer.GetLastErrorMessage());
                OutputDebugStringA("\n");
            }
            mSssrInitFailed = true;
        }
    }
    const bool runSssr = sssrRequested && mSssrRenderer.IsInitialized();
    // A failed SSSR falls back to the ray march rather than leaving reflections off.
    const bool runSsr = mSsrSettings.Enabled && !runSssr && mSsrRenderer.IsInitialized();
    if (!runSssr)
    {
        mSssrRenderer.InvalidateHistory();
    }

    if ((runSsr || runSssr) && mDeferredLightingPass.IsInitialized())
    {
        PTERO_SCOPED_PASS_TIMER("Post", runSssr ? "SSR (FidelityFX SSSR)" : "SSR");
        // The lighting resolve leaves the G-Buffer and depth as pixel-shader resources.
        // This pass reads them from compute, which needs a state covering non-pixel
        // stages, so move them across and put them back afterwards.
        D3D12_RESOURCE_BARRIER ssrToCompute[3];
        for (UINT i = 0; i < 3; ++i)
        {
            ssrToCompute[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mDeferredLightingPass.GetGBufferResource(i),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        commandList->ResourceBarrier(3, ssrToCompute);

        const D3D12_RESOURCE_STATES depthStateBeforeSsr = mDepthBufferState;
        if (mDepthBufferState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
        {
            const auto toAllShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
                mSceneDepthTarget.Get(),
                mDepthBufferState,
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &toAllShaderResource);
            mDepthBufferState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        }

        // Same jittered view-projection the frame was rendered with, so the reprojection
        // of marched points lands on the pixels that actually hold that geometry.
        const XMMATRIX ssrViewProjection = XMLoadFloat4x4(&mJitteredViewProjection);
        XMFLOAT4X4 ssrViewProj;
        XMFLOAT4X4 ssrInvViewProj;
        XMStoreFloat4x4(&ssrViewProj, XMMatrixTranspose(ssrViewProjection));
        XMStoreFloat4x4(&ssrInvViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, ssrViewProjection)));

        const GBufferSrvs& gbufferSrvs = mDeferredLightingPass.GetSrvs();
        if (runSssr)
        {
            XMFLOAT3 cameraForward;
            const XMFLOAT3 rawForward = mCamera.GetForwardVector();
            XMStoreFloat3(&cameraForward, XMVector3Normalize(XMLoadFloat3(&rawForward)));

            mSssrRenderer.Dispatch(
                commandList,
                mSceneColorTarget.Get(),
                mSceneSrvGpuHandle,
                mDepthSrvGpuHandle,
                gbufferSrvs.Normal,
                gbufferSrvs.Material,
                gbufferSrvs.Albedo,
                mSsrSettings,
                ssrViewProj,
                ssrInvViewProj,
                mCamera.GetPosition(),
                cameraForward);
        }
        else
        {
            mSsrRenderer.Dispatch(
                commandList,
                mSceneColorTarget.Get(),
                mSceneSrvGpuHandle,
                mDepthSrvGpuHandle,
                gbufferSrvs.Normal,
                gbufferSrvs.Material,
                gbufferSrvs.Albedo,
                mSsrSettings,
                ssrViewProj,
                ssrInvViewProj,
                mCamera.GetPosition());
        }

        D3D12_RESOURCE_BARRIER ssrToPixel[3];
        for (UINT i = 0; i < 3; ++i)
        {
            ssrToPixel[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mDeferredLightingPass.GetGBufferResource(i),
                D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        commandList->ResourceBarrier(3, ssrToPixel);

        if (mDepthBufferState != depthStateBeforeSsr)
        {
            const auto restoreDepth = CD3DX12_RESOURCE_BARRIER::Transition(
                mSceneDepthTarget.Get(),
                mDepthBufferState,
                depthStateBeforeSsr);
            commandList->ResourceBarrier(1, &restoreDepth);
            mDepthBufferState = depthStateBeforeSsr;
        }

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    // -----------------------------------------------------------------------
    // PASS 7 – TAA resolve (optional)
    // -----------------------------------------------------------------------
    const bool motionVectorsAvailable = mMotionVectorRenderer.GetOutputResource() != nullptr;
    const bool dlssWillEvaluate = !rtaoDebugViewActive
        && IsDlssUpscalerActive()
        && motionVectorsAvailable;
    const bool fsrWillEvaluate = !rtaoDebugViewActive
        && IsFsrUpscalerActive()
        && mFsrRenderer.HasOutput()
        && motionVectorsAvailable;
    const bool upscalerWillEvaluate = dlssWillEvaluate || fsrWillEvaluate;
    // Frame generation reads the same depth and motion vectors as the upscalers, so
    // it keeps them being produced even when no upscaler is on.
    const bool frameGenerationWillPrepare = !rtaoDebugViewActive
        && mFrameGeneration.IsActive()
        && motionVectorsAvailable;

    if (!upscalerWillEvaluate && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
    {
        PTERO_SCOPED_PASS_TIMER("Post", "TAA resolve");
        mTaaRenderer.Resolve(
            commandList,
            mSceneColorTarget.Get(),
            mSceneSrvCpuHandle,
            mTaaSettings);

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    if (!upscalerWillEvaluate && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
    {
        ID3D12Resource*             smaaInputResource = mSceneColorTarget.Get();
        D3D12_CPU_DESCRIPTOR_HANDLE smaaInputSrv      = mSceneSrvCpuHandle;

        if (mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
        {
            smaaInputResource = mTaaRenderer.GetOutputResource();
            smaaInputSrv      = mTaaRenderer.GetOutputCpuSrv();
        }

        PTERO_SCOPED_PASS_TIMER("Post", "SMAA");
        mSmaaRenderer.Apply(commandList, smaaInputResource, smaaInputSrv, mSmaaSettings);

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    // -----------------------------------------------------------------------
    // PASS 7.5 – Motion vectors + DLSS / FSR SR + FSR frame generation inputs (optional)
    // Runs after TAA so the temporal resolve can remain available when no upscaler
    // is on. Bloom and AgX then consume the upscaled output when there is one.
    // -----------------------------------------------------------------------
    if (upscalerWillEvaluate || frameGenerationWillPrepare)
    {
        // Captured before either upscaler consumes (and clears) its reset flag, so
        // frame generation sees the same camera cut the upscaler did.
        const bool historyReset = mDlssSettings.ResetHistory || mFsrSettings.ResetHistory || mTaaSettings.ResetHistory;

        if (mEntities != nullptr)
        {
            mMotionVectorRenderer.SetEntities(mEntities);
            mMotionVectorRenderer.Render(
                commandList,
                mPreviousEntityTransforms,
                mNonJitteredViewProjection,
                mPreviousViewProjectionForRtgi,
                mDlssSettings.ResetHistory || mFsrSettings.ResetHistory);

            // Vegetation contributes its own motion vectors, because its
            // movement comes from the wind bend rather than from an entity
            // transform.  Skipping this would leave the upscaler reprojecting
            // foliage by camera motion alone, smearing the canopy whenever wind blows.
            if (mVegetationRenderer.GetTotalInstanceCount() > 0
                && mMotionVectorRenderer.BeginExternalPass(commandList))
            {
                mVegetationRenderer.RenderMotionVectors(
                    commandList,
                    XMLoadFloat4x4(&mNonJitteredViewProjection),
                    XMLoadFloat4x4(&mPreviousViewProjectionForRtgi),
                    mCamera.GetPosition(),
                    MotionVectorRenderer::OutputFormat,
                    // The motion vector target is bound without a depth buffer,
                    // matching MotionVectorRenderer's own pass.
                    DXGI_FORMAT_UNKNOWN);

                mMotionVectorRenderer.EndExternalPass(commandList);
            }
        }

        const XMFLOAT3 cameraRight = { mNonJitteredViewMatrix._11, mNonJitteredViewMatrix._21, mNonJitteredViewMatrix._31 };
        // mCurrentCameraJitter is the offset whose NDC shift is (2x/w, 2y/h). FSR and DLSS
        // count pixels with +Y down, which flips the vertical component.
        const float upscalerJitterX = mCurrentCameraJitter[0];
        const float upscalerJitterY = -mCurrentCameraJitter[1];

        ID3D12Resource* colorInputResource = mSceneColorTarget.Get();
        ID3D12Resource* motionVectorResource = mMotionVectorRenderer.GetOutputResource();
        const D3D12_RESOURCE_STATES upscalerReadState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

        // Scene colour is only an input to the upscalers; frame generation works
        // from the presented back buffer instead.
        if (upscalerWillEvaluate)
        {
            const auto colorToAllShaderRead = CD3DX12_RESOURCE_BARRIER::Transition(
                colorInputResource,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                upscalerReadState);
            commandList->ResourceBarrier(1, &colorToAllShaderRead);
        }

        if (mDepthBufferState != upscalerReadState)
        {
            const auto depthToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                mSceneDepthTarget.Get(),
                mDepthBufferState,
                upscalerReadState);
            commandList->ResourceBarrier(1, &depthToSrv);
            mDepthBufferState = upscalerReadState;
        }

        {
            const auto motionVectorsToAllShaderRead = CD3DX12_RESOURCE_BARRIER::Transition(
                motionVectorResource,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                upscalerReadState);
            commandList->ResourceBarrier(1, &motionVectorsToAllShaderRead);
        }

        if (dlssWillEvaluate)
        {
            PTERO_SCOPED_PASS_TIMER("Post", "DLSS Upscaling");
            DlssRenderer::CameraFrameData cameraData{};
            cameraData.View = mNonJitteredViewMatrix;
            cameraData.Projection = mNonJitteredProjectionMatrix;
            cameraData.PrevViewProjection = mPreviousViewProjectionForRtgi;
            cameraData.CameraPosition = mCamera.GetPosition();
            cameraData.CameraUp = mCamera.GetUpVector();
            cameraData.CameraRight = cameraRight;
            cameraData.CameraForward = mCamera.GetForwardVector();
            // mCurrentCameraJitter shifts NDC with +Y up; DLSS, like FSR, counts pixels with
            // +Y down. Passing it unflipped made DLSS undo the jitter vertically in the
            // wrong direction, so even a still image wobbled, more at lower render scales.
            cameraData.JitterX = upscalerJitterX;
            cameraData.JitterY = upscalerJitterY;
            cameraData.Reset = mDlssSettings.ResetHistory || mTaaSettings.ResetHistory;
            cameraData.NearPlane = 0.1f;
            cameraData.FarPlane = mViewDistanceMeters;
            cameraData.FovY = XM_PIDIV4;
            cameraData.AspectRatio = static_cast<float>(mSceneWidth) / static_cast<float>((std::max)(mSceneHeight, 1u));

            mDlssRenderer.Evaluate(
                commandList,
                colorInputResource,
                mSceneDepthTarget.Get(),
                motionVectorResource,
                cameraData,
                mDlssSettings,
                renderFrameIndex);
        }
        else if (fsrWillEvaluate)
        {
            PTERO_SCOPED_PASS_TIMER("Post", "FSR Upscaling");
            FsrRenderer::FrameData frameData{};
            frameData.JitterX = upscalerJitterX;
            frameData.JitterY = upscalerJitterY;
            frameData.FrameTimeDeltaMs = (std::max)(mFrameDeltaTimeMs, 0.1f);
            frameData.NearPlane = 0.1f;
            frameData.FarPlane = mViewDistanceMeters;
            frameData.FovY = XM_PIDIV4;
            frameData.Reset = mFsrSettings.ResetHistory || mTaaSettings.ResetHistory;

            mFsrRenderer.Evaluate(
                commandList,
                colorInputResource,
                mSceneDepthTarget.Get(),
                motionVectorResource,
                frameData,
                mFsrSettings);
        }

        if (frameGenerationWillPrepare)
        {
            PTERO_SCOPED_PASS_TIMER("Post", "FSR Frame Generation Prepare");
            FsrFrameGeneration::PrepareData prepareData{};
            prepareData.RenderWidth = mSceneWidth;
            prepareData.RenderHeight = mSceneHeight;
            prepareData.JitterX = upscalerJitterX;
            prepareData.JitterY = upscalerJitterY;
            prepareData.FrameTimeDeltaMs = (std::max)(mFrameDeltaTimeMs, 0.1f);
            prepareData.NearPlane = 0.1f;
            prepareData.FarPlane = mViewDistanceMeters;
            prepareData.FovY = XM_PIDIV4;
            prepareData.Reset = historyReset;
            prepareData.CameraPosition = mCamera.GetPosition();
            prepareData.CameraUp = mCamera.GetUpVector();
            prepareData.CameraRight = cameraRight;
            prepareData.CameraForward = mCamera.GetForwardVector();

            mFrameGeneration.Prepare(
                commandList,
                mSceneDepthTarget.Get(),
                motionVectorResource,
                prepareData,
                mFsrSettings);
        }

        if (upscalerWillEvaluate)
        {
            const auto colorBackToPixelShaderRead = CD3DX12_RESOURCE_BARRIER::Transition(
                colorInputResource,
                upscalerReadState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &colorBackToPixelShaderRead);
        }

        {
            const auto motionVectorsBackToPixelShaderRead = CD3DX12_RESOURCE_BARRIER::Transition(
                motionVectorResource,
                upscalerReadState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &motionVectorsBackToPixelShaderRead);
        }

        if (mDepthBufferState == upscalerReadState)
        {
            const auto depthBackToPixelShaderRead = CD3DX12_RESOURCE_BARRIER::Transition(
                mSceneDepthTarget.Get(),
                upscalerReadState,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1, &depthBackToPixelShaderRead);
            mDepthBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    // -----------------------------------------------------------------------
    // PASS 7.75 - Image sharpening (optional)
    // Runs after the selected AA/upscaling pass and before bloom/tonemapping.
    // -----------------------------------------------------------------------
    const bool upscalerOutputAvailable = IsUpscalerOutputAvailable();
    // Sharpening an upscaled image sharpens whatever noise survived the upscale too. FSR
    // already runs its own RCAS, so the engine's pass would be a second one on top; after
    // DLSS it is kept, but no stronger than 0.5.
    //
    // A player-chosen upscaler sharpness (the Image settings page) replaces all of that:
    // FSR applies it through RCAS (set in GameSettings.cpp), so the engine pass stays off;
    // DLSS has no sharpening of its own any more, so the engine pass carries it.
    SharpenSettings sharpenSettings = mSharpenSettings;
    if (upscalerWillEvaluate && mUpscalerSharpness >= 0.0f)
    {
        sharpenSettings.ImageSharpeningEnabled = dlssWillEvaluate && mUpscalerSharpness > 0.0f;
        sharpenSettings.ImageSharpeningStrength = mUpscalerSharpness;
    }
    else if (upscalerWillEvaluate)
    {
        if (fsrWillEvaluate && mFsrSettings.Sharpening)
            sharpenSettings.ImageSharpeningEnabled = false;
        sharpenSettings.ImageSharpeningStrength = (std::min)(sharpenSettings.ImageSharpeningStrength, 0.5f);
    }
    const bool imageSharpenWillApply = !rtaoDebugViewActive
        && sharpenSettings.ImageSharpeningEnabled
        && mImageSharpenRenderer.IsInitialized();

    if (imageSharpenWillApply)
    {
        ID3D12Resource* sharpenInputResource = mSceneColorTarget.Get();
        D3D12_CPU_DESCRIPTOR_HANDLE sharpenInputSrv = mSceneSrvCpuHandle;

        if (upscalerOutputAvailable)
        {
            sharpenInputResource = GetUpscalerOutputResource();
            sharpenInputSrv = GetUpscalerOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
        {
            sharpenInputResource = mSmaaRenderer.GetOutputResource();
            sharpenInputSrv = mSmaaRenderer.GetOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
        {
            sharpenInputResource = mTaaRenderer.GetOutputResource();
            sharpenInputSrv = mTaaRenderer.GetOutputCpuSrv();
        }

        PTERO_SCOPED_PASS_TIMER("Post", "Image sharpen");
        mImageSharpenRenderer.Apply(commandList, sharpenInputResource, sharpenInputSrv, sharpenSettings);

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    // -----------------------------------------------------------------------
    // PASS 8 – Bloom (optional, physical mip-chain)
    // Runs after AA/upscaling/sharpening so bloom operates on the current image.
    // -----------------------------------------------------------------------
    if (!rtaoDebugViewActive && mBloomSettings.Enabled && mBloomRenderer.IsInitialized())
    {
        ID3D12Resource*             bloomInputResource = mSceneColorTarget.Get();
        D3D12_CPU_DESCRIPTOR_HANDLE bloomInputSrv      = mSceneSrvCpuHandle;

        if (imageSharpenWillApply)
        {
            bloomInputResource = mImageSharpenRenderer.GetOutputResource();
            bloomInputSrv      = mImageSharpenRenderer.GetOutputCpuSrv();
        }
        else if (upscalerOutputAvailable)
        {
            bloomInputResource = GetUpscalerOutputResource();
            bloomInputSrv = GetUpscalerOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
        {
            bloomInputResource = mSmaaRenderer.GetOutputResource();
            bloomInputSrv      = mSmaaRenderer.GetOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
        {
            bloomInputResource = mTaaRenderer.GetOutputResource();
            bloomInputSrv      = mTaaRenderer.GetOutputCpuSrv();
        }

        PTERO_SCOPED_PASS_TIMER("Post", "Bloom");
        mBloomRenderer.Apply(commandList, bloomInputResource, bloomInputSrv, mBloomSettings);

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    // -----------------------------------------------------------------------
    // PASS 9 – AgX tonemap (optional)
    // Runs after Bloom so the tonemapper operates on the bloom-composited image.
    // -----------------------------------------------------------------------
    if (!rtaoDebugViewActive && mAgxSettings.Enabled && mAgxTonemapper.IsInitialized())
    {
        // Determine the source for the tonemapper: Bloom output → TAA output → raw scene colour.
        ID3D12Resource*             agxInputResource = mSceneColorTarget.Get();
        D3D12_CPU_DESCRIPTOR_HANDLE agxInputSrv      = mSceneSrvCpuHandle;

        if (mBloomSettings.Enabled && mBloomRenderer.IsInitialized())
        {
            agxInputResource = mBloomRenderer.GetOutputResource();
            agxInputSrv      = mBloomRenderer.GetOutputCpuSrv();
        }
        else if (imageSharpenWillApply)
        {
            agxInputResource = mImageSharpenRenderer.GetOutputResource();
            agxInputSrv = mImageSharpenRenderer.GetOutputCpuSrv();
        }
        else if (upscalerOutputAvailable)
        {
            agxInputResource = GetUpscalerOutputResource();
            agxInputSrv = GetUpscalerOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
        {
            agxInputResource = mSmaaRenderer.GetOutputResource();
            agxInputSrv      = mSmaaRenderer.GetOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
        {
            agxInputResource = mTaaRenderer.GetOutputResource();
            agxInputSrv      = mTaaRenderer.GetOutputCpuSrv();
        }

        // Meter the same HDR image the tonemapper is about to consume, before
        // it is tonemapped. The exposure the meter produces stays on the GPU;
        // the tonemapper reads it through this SRV.
        D3D12_CPU_DESCRIPTOR_HANDLE autoExposureSrv{};
        if (mAgxSettings.ExposureMode == AgxExposureMode::AutoHistogram &&
            mAutoExposure.IsInitialized())
        {
            PTERO_SCOPED_PASS_TIMER("Post", "Auto exposure");
            const bool metered = mAutoExposure.Apply(
                commandList,
                agxInputResource,
                agxInputSrv,
                mAgxSettings,
                mFrameDeltaTimeMs * 0.001f);

            if (metered)
                autoExposureSrv = mAutoExposure.GetExposureCpuSrv();
        }

        PTERO_SCOPED_PASS_TIMER("Post", "AgX tonemap");
        mAgxTonemapper.Apply(commandList, agxInputResource, agxInputSrv, mAgxSettings, autoExposureSrv);

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    // -----------------------------------------------------------------------
    // PASS 9.5 – Chromatic aberration (optional)
    // Last in the chain, on the tonemapped image: it is a lens artefact, so it belongs
    // after everything that models light reaching the lens. Splitting channels in HDR
    // instead would let a highlight's fringe survive tone mapping as a saturated band
    // rather than the subtle edge colouring a real lens gives.
    // -----------------------------------------------------------------------
    if (!rtaoDebugViewActive
        && mChromaticAberrationSettings.Enabled
        && mChromaticAberrationRenderer.IsInitialized())
    {
        // Same precedence the tonemapper uses, with AgX added on top since it now runs
        // before this stage.
        ID3D12Resource*             caInputResource = mSceneColorTarget.Get();
        D3D12_CPU_DESCRIPTOR_HANDLE caInputSrv      = mSceneSrvCpuHandle;

        if (mAgxSettings.Enabled && mAgxTonemapper.IsInitialized())
        {
            caInputResource = mAgxTonemapper.GetOutputResource();
            caInputSrv      = mAgxTonemapper.GetOutputCpuSrv();
        }
        else if (mBloomSettings.Enabled && mBloomRenderer.IsInitialized())
        {
            caInputResource = mBloomRenderer.GetOutputResource();
            caInputSrv      = mBloomRenderer.GetOutputCpuSrv();
        }
        else if (imageSharpenWillApply)
        {
            caInputResource = mImageSharpenRenderer.GetOutputResource();
            caInputSrv      = mImageSharpenRenderer.GetOutputCpuSrv();
        }
        else if (upscalerOutputAvailable)
        {
            caInputResource = GetUpscalerOutputResource();
            caInputSrv      = GetUpscalerOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mSmaaSettings.Enabled && mSmaaRenderer.IsInitialized())
        {
            caInputResource = mSmaaRenderer.GetOutputResource();
            caInputSrv      = mSmaaRenderer.GetOutputCpuSrv();
        }
        else if (!upscalerWillEvaluate && mTaaSettings.Enabled && mTaaRenderer.IsInitialized())
        {
            caInputResource = mTaaRenderer.GetOutputResource();
            caInputSrv      = mTaaRenderer.GetOutputCpuSrv();
        }

        PTERO_SCOPED_PASS_TIMER("Post", "Chromatic aberration");
        mChromaticAberrationRenderer.Apply(
            commandList, caInputResource, caInputSrv, mChromaticAberrationSettings);

        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    // -----------------------------------------------------------------------
    // PASS 10 - RmlUi
    // The UI draws into its own target rather than the scene image, so it runs
    // after the post chain and leaves every earlier pass untouched. The editor
    // composites the result over the viewport; a standalone game would blit it
    // onto the swap chain instead.
    //
    // Only while a play session is running: the UI belongs to the game, so it has no
    // business painting over the viewport while the editor is what is being used.
    // -----------------------------------------------------------------------
    if (IsGameUiActive() || IsUiPreviewActive())
    {
        PTERO_SCOPED_PASS_TIMER("Post", "RmlUi");
        UpdateStatisticsOverlay();
        mRmlUiRenderer.Render(commandList);

        // The UI pass rebinds the render target, viewport and scissor; put the shared
        // descriptor heap back so anything recorded afterwards sees the state it expects.
        ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
        commandList->SetDescriptorHeaps(1, sharedHeaps);
    }

    if (mEntities != nullptr)
    {
        mPreviousEntityTransforms.clear();
        for (std::size_t i = 0; i < mEntities->size(); ++i)
        {
            const Entity& entity = (*mEntities)[i];
            if (!entity.HasMeshComponent() || !entity.Mesh.has_value() || !entity.Mesh->MeshAsset)
            {
                continue;
            }

            XMFLOAT4X4 model{};
            XMStoreFloat4x4(&model, entity.Transform.GetTransform());
            mPreviousEntityTransforms[i] = model;
        }
    }

    mPreviousViewProjectionForRtgi = mNonJitteredViewProjection;
    mPreviousViewProjectionForClouds = mNonJitteredViewProjection;
    mHasPreviousCloudViewProjection = true;

    EndTimingFrame(
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - frameTimingStart).count());
}

void DX12SceneRenderer::Shutdown()
{
    mVideoLayer.Stop();
    mVideoLayer.ShutdownGpu();
    mEntityMeshRenderer.Shutdown();
    mPointLightRenderer.Shutdown();
    mDecalRenderer.Shutdown();
    mPointShadowMapRenderer.Shutdown();
    mDeferredLightingPass.Shutdown();
    mRtgiRenderer.Shutdown();
    mProbeRenderer.Shutdown();
    mVolumetricFogRenderer.Shutdown();
    mVolumetricCloudRenderer.Shutdown();
    mRainRenderer.Shutdown();
    mParticleRenderer.Shutdown();
    mWaterRenderer.Shutdown();
    mVegetationRenderer.Shutdown();
    mDlssRenderer.Shutdown();
    // Before DX12Context shuts down: the frame generation context was configured
    // with the proxy swap chain, which must still exist when it is released.
    mFrameGeneration.Release();
    mFsrRenderer.Shutdown();
    mFsrWasActive = false;
    mChromaticAberrationRenderer.Shutdown();
    mSsrRenderer.Shutdown();
    mSssrRenderer.Shutdown();
    mSubsurfaceRenderer.Shutdown();
    mTaaRenderer.Shutdown();
    mSmaaRenderer.Shutdown();
    mImageSharpenRenderer.Shutdown();
    mBloomRenderer.Shutdown();
    mRmlUiRenderer.Shutdown();
    mAgxTonemapper.Shutdown();
    mAutoExposure.Shutdown();
    mSkyRenderer.Shutdown();
    mShadowMapRenderer.Shutdown();

    if (mConstantBuffer && mMappedConstants != nullptr)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedConstants = nullptr;
    }
    if (mDepthDebugConstantBuffer && mMappedDepthDebugConstants != nullptr)
    {
        mDepthDebugConstantBuffer->Unmap(0, nullptr);
        mMappedDepthDebugConstants = nullptr;
    }
    if (mResolveMsaaDepthConstantBuffer && mMappedResolveMsaaDepthConstants != nullptr)
    {
        mResolveMsaaDepthConstantBuffer->Unmap(0, nullptr);
        mMappedResolveMsaaDepthConstants = nullptr;
    }

    mConstantBuffer.Reset();
    mResolveMsaaDepthConstantBuffer.Reset();
    mResolveMsaaDepthPipelineState.Reset();
    mResolveMsaaDepthRootSignature.Reset();
    mResolveMsaaDepthHeap.Reset();
    mVertexBuffer = {};
    mIndexBuffer = {};
    ReleaseSceneTargetResources();
    mPipeline.mPipelineState.Reset();
    mPipeline.mRootSignature.Reset();
    mDepthDebugPipelineState.Reset();
    mDepthDebugRootSignature.Reset();
    mSceneSrvCpuHandle = {};
    mSceneSrvGpuHandle = {};
    mDepthDebugSrvCpuHandle = {};
    mDepthDebugSrvGpuHandle = {};
    mSceneTextureId = UiTextureID_Invalid;
    mEntities = nullptr;
    mMeshLocalRadiusCache.clear();
    mIsInitialized = false;
}

bool DX12SceneRenderer::EnsureSceneTargetMatchesWindowSize()
{
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    if (!DX12Context_GetRenderSize(&outputWidth, &outputHeight))
    {
        mLastErrorMessage = "Failed to query the DX12 back-buffer size.";
        return false;
    }

    UINT renderWidth = outputWidth;
    UINT renderHeight = outputHeight;
    const UINT previousSceneWidth = mSceneWidth;
    const UINT previousSceneHeight = mSceneHeight;
    const bool fsrActive = IsFsrUpscalerActive();
    if (mDlssSettings.Enabled && mDlssRenderer.IsAvailable())
    {
        mDlssRenderer.QueryOptimalRenderSize(outputWidth, outputHeight, mDlssSettings, renderWidth, renderHeight);
    }
    else if (fsrActive)
    {
        mFsrRenderer.QueryRenderSize(outputWidth, outputHeight, mFsrSettings, renderWidth, renderHeight);
    }
    else if (mHasCustomSceneResolution)
    {
        renderWidth = mCustomSceneWidth;
        renderHeight = mCustomSceneHeight;
    }

    if (renderWidth == 0 || renderHeight == 0)
    {
        return true;
    }

    // FSR owns a context and an output-size texture only while it is in use. The
    // edge that turns it off also changes the render size back, so the resize below
    // rebuilds the post chain at the right size; here only its memory is returned.
    if (!fsrActive && mFsrWasActive)
    {
        mFsrRenderer.ReleaseResources();
    }
    mFsrWasActive = fsrActive;

    // Only counted where DLSS exists: on a GPU without it the renderer is never
    // initialised, and treating that as a pending resize would retry it - and
    // rebuild the post chain - every frame.
    const bool dlssOutputSizeChanged = mDlssRenderer.IsAvailable()
        && (!mDlssRenderer.IsInitialized()
            || outputWidth != mDlssRenderer.GetOutputWidth()
            || outputHeight != mDlssRenderer.GetOutputHeight());
    const bool fsrOutputSizeChanged = fsrActive
        && (!mFsrRenderer.HasOutput()
            || outputWidth != mFsrRenderer.GetOutputWidth()
            || outputHeight != mFsrRenderer.GetOutputHeight());
    const bool outputSizeChanged = dlssOutputSizeChanged || fsrOutputSizeChanged;
    const bool renderSizeChanged = !mSceneColorTarget
        || renderWidth != previousSceneWidth
        || renderHeight != previousSceneHeight;

    if (!renderSizeChanged && !outputSizeChanged)
    {
        return true;
    }

    if (!renderSizeChanged && outputSizeChanged)
    {
        if (dlssOutputSizeChanged
            && !mDlssRenderer.EnsureSize(renderWidth, renderHeight, outputWidth, outputHeight))
        {
            mLastErrorMessage = "Failed to resize the DLSS output resources.";
            return false;
        }

        if (fsrOutputSizeChanged && !mFsrRenderer.EnsureSize(renderWidth, renderHeight, outputWidth, outputHeight))
        {
            // Not fatal to the frame: FSR has no output, so the chain falls back
            // to the raw scene colour and the panel shows why.
            mFsrSettings.Enabled = false;
            PteroLog::Writef(PteroLog::Level::Error, "FSR", "FSR switched off: %s",
                mFsrRenderer.GetLastErrorMessage() ? mFsrRenderer.GetLastErrorMessage() : "could not create its resources.");
            return true;
        }

        // The post chain runs at output size only while an upscaler feeds it.
        if (!UpscalerOwnsPostAaOutput())
        {
            return true;
        }

        if (mBloomRenderer.IsInitialized())
        {
            mBloomRenderer.Initialize(outputWidth, outputHeight);
        }

        if (mImageSharpenRenderer.IsInitialized())
        {
            mImageSharpenRenderer.Initialize(outputWidth, outputHeight);
        }

        if (mAgxTonemapper.IsInitialized())
        {
            mAgxTonemapper.Initialize(outputWidth, outputHeight);
        }

        // The next histogram is built over a different sample grid, so take its
        // result directly instead of easing the old value towards it.
        mAutoExposure.ResetHistory();

        if (mChromaticAberrationRenderer.IsInitialized())
        {
            mChromaticAberrationRenderer.Initialize(outputWidth, outputHeight);
        }

        if (mRmlUiRenderer.IsInitialized())
        {
            mRmlUiRenderer.Resize(outputWidth, outputHeight);
        }

        mDlssSettings.ResetHistory = true;
        mFsrSettings.ResetHistory = true;
        return true;
    }

    mLastErrorMessage.clear();
    return ResizeSceneTargetsTo(renderWidth, renderHeight);
}

bool DX12SceneRenderer::ResizeSceneTarget(UINT width, UINT height)
{
    // An upscaler picks the render size from the window, so a requested viewport
    // resolution does not apply while one is on.
    if (mDlssSettings.Enabled || IsFsrUpscalerActive())
    {
        mHasCustomSceneResolution = false;
        return EnsureSceneTargetMatchesWindowSize();
    }

    if (width == 0 || height == 0)
    {
        return false;
    }

    mHasCustomSceneResolution = true;
    mCustomSceneWidth = width;
    mCustomSceneHeight = height;

    if (mSceneColorTarget && width == mSceneWidth && height == mSceneHeight)
    {
        return true;
    }

    mLastErrorMessage.clear();
    return ResizeSceneTargetsTo(width, height);
}

bool DX12SceneRenderer::ApplyPendingMsaaSettings()
{
    if (!mIsInitialized || !mSceneColorTarget)
    {
        return true;
    }

    mMsaaSettings.Validate();
    const UINT desiredMsaaSampleCount = mMsaaSettings.GetEffectiveSampleCount();
    const UINT desiredMsaaQuality = mMsaaSettings.GetEffectiveQuality();
    if (desiredMsaaSampleCount == mSceneTargetMsaaSampleCount
        && desiredMsaaQuality == mSceneTargetMsaaQuality)
    {
        return true;
    }

    try
    {
        ReleaseSceneTargetResources(true);
        if (!CreateSceneTarget())
        {
            if (mLastErrorMessage.empty())
                mLastErrorMessage = "Failed to apply the pending MSAA setting change.";
            return false;
        }

        if (!mDeferredLightingPass.EnsureSize(mSceneWidth, mSceneHeight, mMsaaSettings))
        {
            mLastErrorMessage = mDeferredLightingPass.GetLastError()
                ? std::string("Failed to resize G-buffer for the pending MSAA setting change: ") + mDeferredLightingPass.GetLastError()
                : "Failed to resize G-buffer for the pending MSAA setting change.";
            return false;
        }

        mTaaSettings.ResetHistory = true;
        mDlssSettings.ResetHistory = true;
        mFsrSettings.ResetHistory = true;
        mMsaaResolveTimeMs = 0.0f;
        return true;
    }
    catch (const std::exception& exception)
    {
        mLastErrorMessage = std::string("Failed to apply the pending MSAA setting change: ") + exception.what();
        OutputDebugStringA((mLastErrorMessage + "\n").c_str());
        return false;
    }
    catch (...)
    {
        mLastErrorMessage = "Failed to apply the pending MSAA setting change: unknown exception.";
        OutputDebugStringA((mLastErrorMessage + "\n").c_str());
        return false;
    }
}

void DX12SceneRenderer::ClearCustomSceneResolution()
{
    mHasCustomSceneResolution = false;
}

FsrRuntimeStatus DX12SceneRenderer::GetFsrRuntimeStatus()
{
    FsrRuntimeStatus status;
    status.ApiAvailable = mFsrRenderer.IsAvailable() || mFrameGeneration.IsApiAvailable();
    status.UpscalerActive = IsFsrUpscalerActive() && mFsrRenderer.HasOutput();
    status.OverriddenByDlss = mFsrSettings.Enabled && IsDlssUpscalerActive();
    status.FrameGenerationActive = mFrameGeneration.IsActive();
    status.RenderWidth = mSceneWidth;
    status.RenderHeight = mSceneHeight;
    status.OutputWidth = status.UpscalerActive ? mFsrRenderer.GetOutputWidth() : mSceneWidth;
    status.OutputHeight = status.UpscalerActive ? mFsrRenderer.GetOutputHeight() : mSceneHeight;
    status.UpscalerVersion = mFsrRenderer.GetVersionName();
    status.FrameGenerationVersion = mFrameGeneration.GetVersionName();
    if (const char* error = mFsrRenderer.GetLastErrorMessage())
        status.LastError = error;
    else if (const char* frameGenerationError = mFrameGeneration.GetLastErrorMessage())
        status.LastError = frameGenerationError;
    return status;
}

bool DX12SceneRenderer::ResizeSceneTargetsTo(UINT width, UINT height)
{
    ReleaseSceneTargetResources();
    mSceneWidth = width;
    mSceneHeight = height;
    mCamera.SetLens(
        XM_PIDIV4,
        static_cast<float>(mSceneWidth) / static_cast<float>(mSceneHeight),
        0.1f,
        mViewDistanceMeters);

    if (!CreateSceneTarget())
    {
        mLastErrorMessage = "Failed to resize the off-screen scene render target.";
        return false;
    }

    if (mTaaRenderer.IsInitialized())
    {
        mTaaRenderer.Initialize(mSceneWidth, mSceneHeight);
        mTaaSettings.ResetHistory = true;
    }

    if (mSmaaRenderer.IsInitialized())
    {
        mSmaaRenderer.Initialize(mSceneWidth, mSceneHeight);
    }

    // SSR works on the render-resolution scene colour, not the post-process size, since
    // it composites back into that target before any upscale.
    if (mSsrRenderer.IsInitialized())
    {
        mSsrRenderer.Initialize(mSceneWidth, mSceneHeight);
    }
    if (mSssrRenderer.IsInitialized())
    {
        mSssrRenderer.Initialize(mSceneWidth, mSceneHeight);
    }

    if (!mMotionVectorRenderer.Initialize(mSceneWidth, mSceneHeight))
    {
        OutputDebugStringA("DX12SceneRenderer: motion vector resize failed.\n");
    }

    UINT outputWidth = mSceneWidth;
    UINT outputHeight = mSceneHeight;
    const bool haveOutputSize = DX12Context_GetRenderSize(&outputWidth, &outputHeight);
    if (haveOutputSize && mDlssRenderer.IsAvailable())
    {
        mDlssRenderer.EnsureSize(mSceneWidth, mSceneHeight, outputWidth, outputHeight);
    }

    if (haveOutputSize && IsFsrUpscalerActive()
        && !mFsrRenderer.EnsureSize(mSceneWidth, mSceneHeight, outputWidth, outputHeight))
    {
        mFsrSettings.Enabled = false;
        PteroLog::Writef(PteroLog::Level::Error, "FSR", "FSR switched off: %s",
            mFsrRenderer.GetLastErrorMessage() ? mFsrRenderer.GetLastErrorMessage() : "could not create its resources.");
    }

    const bool upscaling = UpscalerOwnsPostAaOutput();
    const UINT postProcessWidth = upscaling ? outputWidth : mSceneWidth;
    const UINT postProcessHeight = upscaling ? outputHeight : mSceneHeight;

    if (mAgxTonemapper.IsInitialized())
    {
        mAgxTonemapper.Initialize(postProcessWidth, postProcessHeight);
    }

    // As above: re-meter from scratch rather than easing across the change.
    mAutoExposure.ResetHistory();

    // Runs on the tonemapper's output, so it follows the post-process size too.
    if (mChromaticAberrationRenderer.IsInitialized())
    {
        mChromaticAberrationRenderer.Initialize(postProcessWidth, postProcessHeight);
    }

    if (mImageSharpenRenderer.IsInitialized())
    {
        mImageSharpenRenderer.Initialize(postProcessWidth, postProcessHeight);
    }

    // The UI is authored in the viewport's own pixels, so it follows the post-process
    // size rather than the (possibly upscaled-from) render size.
    if (mRmlUiRenderer.IsInitialized())
    {
        mRmlUiRenderer.Resize(postProcessWidth, postProcessHeight);
    }

    if (mBloomRenderer.IsInitialized())
    {
        mBloomRenderer.Initialize(postProcessWidth, postProcessHeight);
    }

    if (mVolumetricFogRenderer.IsInitialized())
    {
        mVolumetricFogRenderer.Initialize(mSceneWidth, mSceneHeight);
    }

    if (mVolumetricCloudRenderer.IsInitialized())
    {
        mVolumetricCloudRenderer.EnsureSize(mSceneWidth, mSceneHeight, mVolumetricCloudSettings);
        mVolumetricCloudRenderer.ResetHistory();
    }

    mDlssSettings.ResetHistory = true;
    mFsrSettings.ResetHistory = true;

    return true;
}

bool DX12SceneRenderer::RecreateSceneTargetsForMsaaChange()
{
    try
    {
        ReleaseSceneTargetResources(false);
        if (!CreateSceneTarget())
        {
            if (mLastErrorMessage.empty())
                mLastErrorMessage = "Failed to recreate scene targets for the MSAA setting change.";
            return false;
        }

        mTaaSettings.ResetHistory = true;
        mDlssSettings.ResetHistory = true;
        mFsrSettings.ResetHistory = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        mLastErrorMessage = std::string("Failed to recreate scene targets for the MSAA setting change: ") + exception.what();
        OutputDebugStringA((mLastErrorMessage + "\n").c_str());
        return false;
    }
    catch (...)
    {
        mLastErrorMessage = "Failed to recreate scene targets for the MSAA setting change: unknown exception.";
        OutputDebugStringA((mLastErrorMessage + "\n").c_str());
        return false;
    }
}

void DX12SceneRenderer::WriteGpuProgress(UINT slot, UINT ordinal, D3D12_WRITEBUFFERIMMEDIATE_MODE mode)
{
    if (!mMarkerCommandList2 || !mGpuProgressBuffer)
        return;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter{};
    parameter.Dest = mGpuProgressBuffer->GetGPUVirtualAddress() + slot * sizeof(UINT32);
    parameter.Value = ((mGpuProgressFrame & 0xFFFFu) << 16) | (ordinal & 0xFFFFu);
    mMarkerCommandList2->WriteBufferImmediate(1, &parameter, &mode);
}

void DX12SceneRenderer::LogGpuProgress() const
{
    if (mGpuProgressMapped == nullptr)
        return;

    const UINT32 frameBegun = mGpuProgressMapped[0] >> 16;
    const UINT32 started = mGpuProgressMapped[1];
    const UINT32 finished = mGpuProgressMapped[2];
    const UINT32 frameDone = mGpuProgressMapped[3] >> 16;
    const auto passName = [this](UINT32 ordinal) -> std::string
    {
        if (ordinal == 0 || ordinal > mLastPassOrder.size())
            return "(unknown pass)";
        return std::string(mLastPassOrder[ordinal - 1].first) + ": " + mLastPassOrder[ordinal - 1].second;
    };

    PTERO_LOG_ERROR("Renderer",
        "GPU progress: CPU recorded scene frame %u; GPU began scene frame %u and finished scene frame %u.",
        mGpuProgressFrame & 0xFFFFu, frameBegun, frameDone);
    PTERO_LOG_ERROR("Renderer", "  last pass started:  #%u %s (frame %u)",
        started & 0xFFFFu, passName(started & 0xFFFFu).c_str(), started >> 16);
    PTERO_LOG_ERROR("Renderer", "  last pass finished: #%u %s (frame %u)",
        finished & 0xFFFFu, passName(finished & 0xFFFFu).c_str(), finished >> 16);
    if (started != finished)
    {
        PTERO_LOG_ERROR("Renderer", "  => the GPU is stuck inside #%u %s",
            started & 0xFFFFu, passName(started & 0xFFFFu).c_str());
    }
    else if (frameBegun != frameDone)
    {
        PTERO_LOG_ERROR("Renderer", "  => the GPU is stuck between passes after #%u, outside any timed pass.",
            finished & 0xFFFFu);
    }
    else
    {
        PTERO_LOG_ERROR("Renderer", "  => the scene work of that frame completed; the GPU is stuck after the scene (UI, upscaler outside a timed pass, or present).");
    }
}

void DX12SceneRenderer::LogPassOrder() const
{
    PTERO_LOG_ERROR("Renderer", "GPU events opened by the last recorded frame (BeginEvent #N = pass):");
    for (std::size_t i = 0; i < mLastPassOrder.size(); ++i)
    {
        PTERO_LOG_ERROR("Renderer", "  #%zu  %s: %s", i + 1, mLastPassOrder[i].first, mLastPassOrder[i].second);
    }
}

void DX12SceneRenderer::LogLiveGpuBufferRanges() const
{
    mEntityMeshRenderer.LogLiveGpuBufferRanges();

    const auto logResource = [](const char* name, const Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
    {
        if (!resource) return;
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        // Textures have no GPU virtual address of their own, so the size is what
        // identifies them here; buffers get the range.
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            const D3D12_GPU_VIRTUAL_ADDRESS start = resource->GetGPUVirtualAddress();
            PTERO_LOG_ERROR("Renderer", "  %-22s buffer 0x%llx..0x%llx (%llu B)", name,
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(start + desc.Width),
                static_cast<unsigned long long>(desc.Width));
        }
        else
        {
            PTERO_LOG_ERROR("Renderer", "  %-22s texture %llux%u fmt=%d ptr=%p", name,
                static_cast<unsigned long long>(desc.Width), desc.Height,
                static_cast<int>(desc.Format),
                static_cast<const void*>(resource.Get()));
        }
    };

    PTERO_LOG_ERROR("Renderer", "Live scene targets at the time of the fault (%ux%u), "
        "%llu resources and %llu descriptor heaps parked awaiting release:",
        mSceneWidth, mSceneHeight,
        static_cast<unsigned long long>(mRetiredSceneResources.size()),
        static_cast<unsigned long long>(mRetiredSceneDescriptorHeaps.size()));
    logResource("scene colour", mSceneColorTarget);
    logResource("scene depth", mSceneDepthTarget);
    logResource("MSAA scene depth", mMsaaSceneDepthTarget);
    logResource("depth debug", mDepthDebugTarget);

    // Anything still parked was freed late on purpose; a fault inside one of these
    // would mean the parking is not lasting long enough.
    for (const auto& retired : mRetiredSceneResources)
        logResource("parked scene target", retired);
}

void DX12SceneRenderer::ReleaseSceneTargetResources(bool waitForGpu)
{
    // These must not be released while the GPU is still writing through them. A flush
    // is the strong guarantee - but it has a two second deadline, and the render thread
    // has been seen stalled far longer than that while a level loads. Freeing anyway on
    // a flush that gave up releases render targets and descriptor heaps that in-flight
    // command lists still name, which the GPU reports as a page fault inside whichever
    // pass happens to be running - nowhere near the resize that caused it.
    //
    // So: free immediately only when the GPU has actually confirmed it is idle. In every
    // other case park the resources on the retired lists, where they outlive the frames
    // that could still reference them and are dropped by the next confirmed flush.
    bool gpuIsIdle = false;
    if (waitForGpu)
    {
        gpuIsIdle = DX12Context_WaitForGPU();
        if (gpuIsIdle)
        {
            mRetiredSceneResources.clear();
            mRetiredSceneDescriptorHeaps.clear();
        }
        else
        {
            PTERO_LOG_WARNING("Renderer",
                "GPU flush timed out before releasing the scene targets; parking them "
                "instead of freeing them (%llu resources and %llu descriptor heaps already held).",
                static_cast<unsigned long long>(mRetiredSceneResources.size()),
                static_cast<unsigned long long>(mRetiredSceneDescriptorHeaps.size()));
        }
    }

    if (!gpuIsIdle)
    {
        if (mSceneColorTarget) mRetiredSceneResources.push_back(mSceneColorTarget);
        if (mSceneDepthTarget) mRetiredSceneResources.push_back(mSceneDepthTarget);
        if (mMsaaSceneDepthTarget) mRetiredSceneResources.push_back(mMsaaSceneDepthTarget);
        if (mDepthDebugTarget) mRetiredSceneResources.push_back(mDepthDebugTarget);
        if (mSceneRtvHeap) mRetiredSceneDescriptorHeaps.push_back(mSceneRtvHeap);
        if (mSceneDsvHeap) mRetiredSceneDescriptorHeaps.push_back(mSceneDsvHeap);
        if (mDepthDebugRtvHeap) mRetiredSceneDescriptorHeaps.push_back(mDepthDebugRtvHeap);
    }

    mSceneColorTarget.Reset();
    mSceneDepthTarget.Reset();
    mMsaaSceneDepthTarget.Reset();
    mDepthDebugTarget.Reset();
    mSceneRtvHeap.Reset();
    mSceneDsvHeap.Reset();
    mDepthDebugRtvHeap.Reset();
    mSceneRtvHandle = {};
    mSceneDsvHandle = {};
    mSceneReadOnlyDsvHandle = {};
    mDepthDebugRtvHandle = {};
    mDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    mMsaaDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

bool DX12SceneRenderer::CreatePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    const ShaderCompileRequest vertexShaderRequest
    {
        L"Shaders\\HelloCube.hlsl",
        L"VSMain",
        L"vs_5_0",
        ShaderStage::Vertex
    };

    const ShaderCompileRequest pixelShaderRequest
    {
        L"Shaders\\HelloCube.hlsl",
        L"PSMain",
        L"ps_5_0",
        ShaderStage::Pixel
    };

    if (!mVertexShader.Compile(vertexShaderRequest))
    {
        mLastErrorMessage = std::string("Vertex shader compilation failed: ")
            + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "Unknown shader compiler error.");
        return false;
    }

    if (!mPixelShader.Compile(pixelShaderRequest))
    {
        mLastErrorMessage = std::string("Pixel shader compilation failed: ")
            + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "Unknown shader compiler error.");
        return false;
    }

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.Descriptor.RegisterSpace = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.NumStaticSamplers = 0;
    rootSignatureDesc.pStaticSamplers = nullptr;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> rootSignatureErrors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serializedRootSignature,
        &rootSignatureErrors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(
        0,
        serializedRootSignature->GetBufferPointer(),
        serializedRootSignature->GetBufferSize(),
        IID_PPV_ARGS(&mPipeline.mRootSignature)));

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mPipeline.GetRootSignature();
    psoDesc.VS = mVertexShader.GetBytecode();
    psoDesc.PS = mPixelShader.GetBytecode();
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlendDesc{};
    renderTargetBlendDesc.BlendEnable = FALSE;
    renderTargetBlendDesc.LogicOpEnable = FALSE;
    renderTargetBlendDesc.SrcBlend = D3D12_BLEND_ONE;
    renderTargetBlendDesc.DestBlend = D3D12_BLEND_ZERO;
    renderTargetBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    renderTargetBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    renderTargetBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    renderTargetBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    renderTargetBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    renderTargetBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = renderTargetBlendDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.RasterizerState.MultisampleEnable = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    psoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC defaultStencilOp =
    {
        D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP,
        D3D12_COMPARISON_FUNC_ALWAYS
    };
    psoDesc.DepthStencilState.FrontFace = defaultStencilOp;
    psoDesc.DepthStencilState.BackFace = defaultStencilOp;
    psoDesc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = SceneColorFormat;
    psoDesc.DSVFormat = SceneDepthFormat;
    psoDesc.SampleDesc.Count = 1;

    DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipeline.mPipelineState)));
    mPipeline.SetDebugName(L"Floor Grid Pipeline");
    return true;
}

bool DX12SceneRenderer::CreateSceneTarget()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    mMsaaSettings.Validate();

    if ((mSceneSrvCpuHandle.ptr == 0 || mSceneSrvGpuHandle.ptr == 0) &&
        !DX12Context_AllocateSrvDescriptor(&mSceneSrvCpuHandle, &mSceneSrvGpuHandle))
    {
        return false;
    }

    mSceneTextureId = static_cast<UiTextureID>(mSceneSrvGpuHandle.ptr);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mSceneRtvHeap)));
    mSceneRtvHandle = mSceneRtvHeap->GetCPUDescriptorHandleForHeapStart();

    // Two DSVs over the same depth resource: the writable one the geometry pass
    // uses, and a read-only one for transparent passes that need to sample depth
    // while still depth-testing against it.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 2;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mSceneDsvHeap)));
    mSceneDsvHandle = mSceneDsvHeap->GetCPUDescriptorHandleForHeapStart();
    mSceneReadOnlyDsvHandle = mSceneDsvHandle;
    mSceneReadOnlyDsvHandle.ptr +=
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_RESOURCE_DESC colorTargetDesc{};
    colorTargetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorTargetDesc.Alignment = 0;
    colorTargetDesc.Width = mSceneWidth;
    colorTargetDesc.Height = mSceneHeight;
    colorTargetDesc.DepthOrArraySize = 1;
    colorTargetDesc.MipLevels = 1;
    colorTargetDesc.Format = SceneColorFormat;
    colorTargetDesc.SampleDesc.Count = 1;
    colorTargetDesc.SampleDesc.Quality = 0;
    colorTargetDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colorTargetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE colorClearValue{};
    colorClearValue.Format = SceneColorFormat;
    colorClearValue.Color[0] = 0.12f;
    colorClearValue.Color[1] = 0.14f;
    colorClearValue.Color[2] = 0.18f;
    colorClearValue.Color[3] = 1.0f;

    D3D12_HEAP_PROPERTIES defaultHeapProperties{};
    defaultHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeapProperties.CreationNodeMask = 1;
    defaultHeapProperties.VisibleNodeMask = 1;

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &colorTargetDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &colorClearValue,
        IID_PPV_ARGS(&mSceneColorTarget)));

    device->CreateRenderTargetView(mSceneColorTarget.Get(), nullptr, mSceneRtvHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = SceneColorFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mSceneColorTarget.Get(), &srvDesc, mSceneSrvCpuHandle);

    D3D12_CLEAR_VALUE depthClearValue{};
    depthClearValue.Format = SceneDepthFormat;
    depthClearValue.DepthStencil.Depth = 1.0f;
    depthClearValue.DepthStencil.Stencil = 0;

    const UINT msaaSampleCount = mMsaaSettings.GetEffectiveSampleCount();
    const UINT msaaQuality = mMsaaSettings.GetEffectiveQuality();
    const bool msaaEnabled = mMsaaSettings.Enabled && msaaSampleCount > 1;
    mSceneTargetMsaaSampleCount = msaaSampleCount;
    mSceneTargetMsaaQuality = msaaQuality;

    D3D12_RESOURCE_DESC depthTargetDesc{};
    depthTargetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthTargetDesc.Alignment = 0;
    depthTargetDesc.Width = mSceneWidth;
    depthTargetDesc.Height = mSceneHeight;
    depthTargetDesc.DepthOrArraySize = 1;
    depthTargetDesc.MipLevels = 1;
    depthTargetDesc.SampleDesc.Count = 1;
    depthTargetDesc.SampleDesc.Quality = 0;
    depthTargetDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    if (msaaEnabled)
    {
        depthTargetDesc.Format = SceneDepthSrvFormat;
        depthTargetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &depthTargetDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&mSceneDepthTarget)));
        mDepthBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        D3D12_RESOURCE_DESC msaaDepthDesc = depthTargetDesc;
        msaaDepthDesc.Format = SceneDepthResourceFormat;
        msaaDepthDesc.SampleDesc.Count = msaaSampleCount;
        msaaDepthDesc.SampleDesc.Quality = msaaQuality;
        msaaDepthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &msaaDepthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClearValue,
            IID_PPV_ARGS(&mMsaaSceneDepthTarget)));
        mMsaaDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = SceneDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device->CreateDepthStencilView(mMsaaSceneDepthTarget.Get(), &dsvDesc, mSceneDsvHandle);

        dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device->CreateDepthStencilView(mMsaaSceneDepthTarget.Get(), &dsvDesc, mSceneReadOnlyDsvHandle);
    }
    else
    {
        // Use TYPELESS so we can create both a D32_FLOAT DSV and an R32_FLOAT SRV
        // for the deferred lighting pass to reconstruct world-space positions.
        depthTargetDesc.Format = SceneDepthResourceFormat;
        depthTargetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &depthTargetDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depthClearValue,
            IID_PPV_ARGS(&mSceneDepthTarget)));

        // Track the current state so Render() issues the correct barriers.
        mDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        mMsaaDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = SceneDepthFormat;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(mSceneDepthTarget.Get(), &dsvDesc, mSceneDsvHandle);

        dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device->CreateDepthStencilView(mSceneDepthTarget.Get(), &dsvDesc, mSceneReadOnlyDsvHandle);
    }

    // Create an R32_FLOAT SRV for the depth buffer so the deferred lighting pass can
    // sample it to reconstruct world-space positions.  Allocate from the shared
    // shader-visible heap so it is accessible during the lighting resolve draw.
    if (mDepthSrvCpuHandle.ptr == 0 || mDepthSrvGpuHandle.ptr == 0)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mDepthSrvCpuHandle, &mDepthSrvGpuHandle))
        {
            // Non-fatal: deferred lighting world-pos reconstruction will be unavailable,
            // but the rest of the renderer continues to work.
            OutputDebugStringA("DX12SceneRenderer: Failed to allocate depth SRV descriptor.\n");
        }
    }
    if (mDepthDebugSrvCpuHandle.ptr == 0 || mDepthDebugSrvGpuHandle.ptr == 0)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mDepthDebugSrvCpuHandle, &mDepthDebugSrvGpuHandle))
        {
            OutputDebugStringA("DX12SceneRenderer: Failed to allocate debug depth SRV descriptor.\n");
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC depthDebugRtvHeapDesc{};
    depthDebugRtvHeapDesc.NumDescriptors = 1;
    depthDebugRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&depthDebugRtvHeapDesc, IID_PPV_ARGS(&mDepthDebugRtvHeap)));
    mDepthDebugRtvHandle = mDepthDebugRtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (mDepthSrvCpuHandle.ptr != 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
        depthSrvDesc.Format                    = SceneDepthSrvFormat; // R32_FLOAT
        depthSrvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels       = 1;
        device->CreateShaderResourceView(mSceneDepthTarget.Get(), &depthSrvDesc, mDepthSrvCpuHandle);

        if (mDepthDebugSrvCpuHandle.ptr != 0)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC depthDebugSrvDesc{};
            depthDebugSrvDesc.Format                  = SceneDepthSrvFormat; // R32_FLOAT
            depthDebugSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            // Preview the inverse depth in grayscale so surfaces look dark
            // instead of nearly white from raw perspective depth near 1.0.
            depthDebugSrvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
            depthDebugSrvDesc.Texture2D.MipLevels     = 1;
            device->CreateShaderResourceView(mSceneDepthTarget.Get(), &depthDebugSrvDesc, mDepthDebugSrvCpuHandle);
        }
    }

    if (msaaEnabled && !CreateResolveMsaaDepthResources())
    {
        mLastErrorMessage = "Failed to create the MSAA depth resolve resources.";
        return false;
    }

    D3D12_RESOURCE_DESC depthDebugTargetDesc{};
    depthDebugTargetDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDebugTargetDesc.Width = mSceneWidth;
    depthDebugTargetDesc.Height = mSceneHeight;
    depthDebugTargetDesc.DepthOrArraySize = 1;
    depthDebugTargetDesc.MipLevels = 1;
    depthDebugTargetDesc.Format = SceneColorFormat;
    depthDebugTargetDesc.SampleDesc.Count = 1;
    depthDebugTargetDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDebugTargetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE depthDebugClearValue{};
    depthDebugClearValue.Format = SceneColorFormat;
    depthDebugClearValue.Color[0] = 0.0f;
    depthDebugClearValue.Color[1] = 0.0f;
    depthDebugClearValue.Color[2] = 0.0f;
    depthDebugClearValue.Color[3] = 1.0f;

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &depthDebugTargetDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &depthDebugClearValue,
        IID_PPV_ARGS(&mDepthDebugTarget)));

    device->CreateRenderTargetView(mDepthDebugTarget.Get(), nullptr, mDepthDebugRtvHandle);

    if (mDepthDebugSrvCpuHandle.ptr != 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC depthDebugOutputSrvDesc{};
        depthDebugOutputSrvDesc.Format = SceneColorFormat;
        depthDebugOutputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthDebugOutputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthDebugOutputSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(mDepthDebugTarget.Get(), &depthDebugOutputSrvDesc, mDepthDebugSrvCpuHandle);
    }

    return true;
}

bool DX12SceneRenderer::CreateDepthDebugResources()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    const ShaderCompileRequest vsRequest
    {
        L"Shaders\\DepthDebug.hlsl",
        L"VSMain",
        L"vs_5_0",
        ShaderStage::Vertex
    };
    const ShaderCompileRequest psRequest
    {
        L"Shaders\\DepthDebug.hlsl",
        L"PSMain",
        L"ps_5_0",
        ShaderStage::Pixel
    };

    if (!mDepthDebugVertexShader.Compile(vsRequest) || !mDepthDebugPixelShader.Compile(psRequest))
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors = 1;
    depthRange.BaseShaderRegister = 0;
    depthRange.RegisterSpace = 0;
    depthRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &depthRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> rootSignatureErrors;
    DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSignature, &rootSignatureErrors));
    DX12_THROW_IF_FAILED(device->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(), serializedRootSignature->GetBufferSize(), IID_PPV_ARGS(&mDepthDebugRootSignature)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mDepthDebugRootSignature.Get();
    psoDesc.VS = mDepthDebugVertexShader.GetBytecode();
    psoDesc.PS = mDepthDebugPixelShader.GetBytecode();
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    psoDesc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable = FALSE;
    rtBlend.LogicOpEnable = FALSE;
    rtBlend.SrcBlend = D3D12_BLEND_ONE;
    rtBlend.DestBlend = D3D12_BLEND_ZERO;
    rtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = rtBlend;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = SceneColorFormat;
    psoDesc.SampleDesc.Count = 1;
    DX12_THROW_IF_FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mDepthDebugPipelineState)));

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = sizeof(DepthDebugConstants);
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.Format = DXGI_FORMAT_UNKNOWN;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mDepthDebugConstantBuffer))))
    {
        return false;
    }
    if (FAILED(mDepthDebugConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedDepthDebugConstants))))
    {
        return false;
    }
    // Raw perspective depth is already non-linear and tends toward 1.0 for most
    // visible surfaces, so a large scale factor just saturates the preview to
    // white. Keep the scale at 1 so the debug view stays dark/gray instead.
    mMappedDepthDebugConstants->DepthScale = 1.0f;
    mMappedDepthDebugConstants->NearPlane  = mCamera.GetNearPlane();
    mMappedDepthDebugConstants->FarPlane   = mCamera.GetFarPlane();
    return true;
}

void DX12SceneRenderer::RenderDepthDebugPreview(ID3D12GraphicsCommandList* commandList)
{
    if (!commandList || !mDepthDebugPipelineState || !mDepthDebugRootSignature || !mDepthDebugTarget || mDepthSrvGpuHandle.ptr == 0)
    {
        return;
    }

    const auto toDepthDebugRt = CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthDebugTarget.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &toDepthDebugRt);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    commandList->ClearRenderTargetView(mDepthDebugRtvHandle, clearColor, 0, nullptr);
    commandList->OMSetRenderTargets(1, &mDepthDebugRtvHandle, FALSE, nullptr);

    const D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(mSceneWidth), static_cast<float>(mSceneHeight), 0, 1 };
    const D3D12_RECT sr = { 0, 0, static_cast<LONG>(mSceneWidth), static_cast<LONG>(mSceneHeight) };
    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sr);
    commandList->SetGraphicsRootSignature(mDepthDebugRootSignature.Get());
    commandList->SetPipelineState(mDepthDebugPipelineState.Get());
    commandList->SetGraphicsRootConstantBufferView(0, mDepthDebugConstantBuffer->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(1, mDepthSrvGpuHandle);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);

    const auto toDepthDebugSrv = CD3DX12_RESOURCE_BARRIER::Transition(
        mDepthDebugTarget.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toDepthDebugSrv);
}

bool DX12SceneRenderer::CreateResolveMsaaDepthResources()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || mMsaaSceneDepthTarget == nullptr || mSceneDepthTarget == nullptr)
    {
        return false;
    }

    if (mResolveMsaaDepthPipelineState == nullptr)
    {
        const ShaderCompileRequest csRequest
        {
            L"Shaders\\ResolveMsaaDepth.hlsl",
            L"CSMain",
            L"cs_5_0",
            ShaderStage::Compute
        };
        if (!mResolveMsaaDepthShader.Compile(csRequest))
        {
            mLastErrorMessage = std::string("MSAA depth resolve shader compilation failed: ")
                + (mResolveMsaaDepthShader.GetLastErrorMessage() ? mResolveMsaaDepthShader.GetLastErrorMessage() : "Unknown shader compiler error.");
            return false;
        }

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = static_cast<UINT>(std::size(params));
        rsDesc.pParameters = params;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> serializedRootSignature;
        ComPtr<ID3DBlob> rootSignatureErrors;
        DX12_THROW_IF_FAILED(D3D12SerializeRootSignature(
            &rsDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serializedRootSignature,
            &rootSignatureErrors));
        DX12_THROW_IF_FAILED(device->CreateRootSignature(
            0,
            serializedRootSignature->GetBufferPointer(),
            serializedRootSignature->GetBufferSize(),
            IID_PPV_ARGS(&mResolveMsaaDepthRootSignature)));

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = mResolveMsaaDepthRootSignature.Get();
        psoDesc.CS = mResolveMsaaDepthShader.GetBytecode();
        DX12_THROW_IF_FAILED(device->CreateComputePipelineState(
            &psoDesc,
            IID_PPV_ARGS(&mResolveMsaaDepthPipelineState)));

        const UINT64 cbSize = (sizeof(ResolveMsaaDepthConstants) + 255ull) & ~255ull;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width = cbSize;
        cbDesc.Height = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels = 1;
        cbDesc.Format = DXGI_FORMAT_UNKNOWN;
        cbDesc.SampleDesc.Count = 1;
        cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&mResolveMsaaDepthConstantBuffer)));
        DX12_THROW_IF_FAILED(mResolveMsaaDepthConstantBuffer->Map(
            0,
            nullptr,
            reinterpret_cast<void**>(&mMappedResolveMsaaDepthConstants)));
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mResolveMsaaDepthHeap)));
    mResolveMsaaDepthHeapStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = mResolveMsaaDepthHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = srvCpu;
    uavCpu.ptr += mResolveMsaaDepthHeapStride;

    D3D12_SHADER_RESOURCE_VIEW_DESC msaaDepthSrvDesc{};
    msaaDepthSrvDesc.Format = SceneDepthSrvFormat;
    msaaDepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    msaaDepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(mMsaaSceneDepthTarget.Get(), &msaaDepthSrvDesc, srvCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC resolvedDepthUavDesc{};
    resolvedDepthUavDesc.Format = SceneDepthSrvFormat;
    resolvedDepthUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(mSceneDepthTarget.Get(), nullptr, &resolvedDepthUavDesc, uavCpu);

    return true;
}

void DX12SceneRenderer::ResolveMsaaDepth(ID3D12GraphicsCommandList* commandList)
{
    if (!mMsaaSettings.Enabled
        || commandList == nullptr
        || mMsaaSceneDepthTarget == nullptr
        || mSceneDepthTarget == nullptr
        || mResolveMsaaDepthPipelineState == nullptr
        || mResolveMsaaDepthRootSignature == nullptr
        || mResolveMsaaDepthHeap == nullptr
        || mMappedResolveMsaaDepthConstants == nullptr)
    {
        return;
    }

    if (mMsaaDepthBufferState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        const auto msaaDepthToRead = CD3DX12_RESOURCE_BARRIER::Transition(
            mMsaaSceneDepthTarget.Get(),
            mMsaaDepthBufferState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &msaaDepthToRead);
        mMsaaDepthBufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    if (mDepthBufferState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto resolvedDepthToUav = CD3DX12_RESOURCE_BARRIER::Transition(
            mSceneDepthTarget.Get(),
            mDepthBufferState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &resolvedDepthToUav);
        mDepthBufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    mMappedResolveMsaaDepthConstants->SampleCount = mMsaaSettings.GetEffectiveSampleCount();

    ID3D12DescriptorHeap* resolveHeaps[] = { mResolveMsaaDepthHeap.Get() };
    commandList->SetDescriptorHeaps(1, resolveHeaps);
    commandList->SetComputeRootSignature(mResolveMsaaDepthRootSignature.Get());
    commandList->SetPipelineState(mResolveMsaaDepthPipelineState.Get());
    commandList->SetComputeRootConstantBufferView(0, mResolveMsaaDepthConstantBuffer->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = mResolveMsaaDepthHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = srvGpu;
    uavGpu.ptr += mResolveMsaaDepthHeapStride;
    commandList->SetComputeRootDescriptorTable(1, srvGpu);
    commandList->SetComputeRootDescriptorTable(2, uavGpu);
    commandList->Dispatch((mSceneWidth + 7) / 8, (mSceneHeight + 7) / 8, 1);

    const auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(mSceneDepthTarget.Get());
    commandList->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER postResolveBarriers[2]{};
    postResolveBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(
        mSceneDepthTarget.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    postResolveBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(
        mMsaaSceneDepthTarget.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    commandList->ResourceBarrier(2, postResolveBarriers);
    mDepthBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    mMsaaDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    ID3D12DescriptorHeap* sharedHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, sharedHeaps);
}

void DX12SceneRenderer::TransitionDepthForRead(ID3D12GraphicsCommandList* commandList)
{
    // Only transition if depth is not already in PIXEL_SHADER_RESOURCE state.
    if (mDepthBufferState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE && mSceneDepthTarget)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            mSceneDepthTarget.Get(),
            mDepthBufferState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &barrier);
        mDepthBufferState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

void DX12SceneRenderer::TransitionDepthAfterRead(ID3D12GraphicsCommandList* commandList)
{
    if (mSceneTargetMsaaSampleCount > 1)
    {
        // In MSAA mode mSceneDepthTarget is the resolved single-sample depth SRV,
        // while the writable DSV is mMsaaSceneDepthTarget and is restored after
        // ResolveMsaaDepth(). Keep the resolved texture readable for Ui/post.
        return;
    }

    // Restore depth to DEPTH_WRITE for the next frame's geometry pass.
    if (mDepthBufferState != D3D12_RESOURCE_STATE_DEPTH_WRITE && mSceneDepthTarget)
    {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            mSceneDepthTarget.Get(),
            mDepthBufferState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        commandList->ResourceBarrier(1, &barrier);
        mDepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
}

bool DX12SceneRenderer::CreateGeometry(ID3D12GraphicsCommandList* commandList)
{
    const std::vector<Vertex> gridVertices = GenerateGridVertices();
    if (!CreateBufferWithUpload(
        commandList,
        gridVertices.data(),
        static_cast<UINT64>(gridVertices.size() * sizeof(Vertex)),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        mVertexBuffer))
    {
        return false;
    }

    mVertexBufferView.BufferLocation = mVertexBuffer.DefaultBuffer->GetGPUVirtualAddress();
    mVertexBufferView.StrideInBytes = sizeof(Vertex);
    mVertexBufferView.SizeInBytes = static_cast<UINT>(gridVertices.size() * sizeof(Vertex));

    // The floor grid is drawn as a non-indexed line list, so vertex count is all that is needed at draw time.
    mVertexCount = static_cast<UINT>(gridVertices.size());
    return true;
}

bool DX12SceneRenderer::CreateConstantBuffer()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        return false;
    }

    const UINT64 constantBufferSize = (sizeof(SceneConstants) + 255ull) & ~255ull;
    D3D12_HEAP_PROPERTIES uploadHeapProperties{};
    uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProperties.CreationNodeMask = 1;
    uploadHeapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC constantBufferDesc{};
    constantBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    constantBufferDesc.Alignment = 0;
    constantBufferDesc.Width = constantBufferSize;
    constantBufferDesc.Height = 1;
    constantBufferDesc.DepthOrArraySize = 1;
    constantBufferDesc.MipLevels = 1;
    constantBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    constantBufferDesc.SampleDesc.Count = 1;
    constantBufferDesc.SampleDesc.Quality = 0;
    constantBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    constantBufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &constantBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mConstantBuffer)));

    DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedConstants)));
    return true;
}

bool DX12SceneRenderer::CreateBufferWithUpload(
    ID3D12GraphicsCommandList* commandList,
    const void* initialData,
    UINT64 dataSize,
    D3D12_RESOURCE_STATES finalState,
    BufferResource& outBuffer) const
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!(device && commandList && initialData && dataSize > 0))
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES defaultHeapProperties{};
    defaultHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeapProperties.CreationNodeMask = 1;
    defaultHeapProperties.VisibleNodeMask = 1;

    D3D12_HEAP_PROPERTIES uploadHeapProperties{};
    uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeapProperties.CreationNodeMask = 1;
    uploadHeapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Alignment = 0;
    bufferDesc.Width = dataSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.SampleDesc.Quality = 0;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&outBuffer.DefaultBuffer)));

    DX12_THROW_IF_FAILED(device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&outBuffer.UploadBuffer)));

    void* mappedData = nullptr;
    DX12_THROW_IF_FAILED(outBuffer.UploadBuffer->Map(0, nullptr, &mappedData));
    std::memcpy(mappedData, initialData, static_cast<size_t>(dataSize));
    outBuffer.UploadBuffer->Unmap(0, nullptr);

    // Geometry upload follows the standard DX12 staging path: CPU writes to the upload heap,
    // then the command list copies into the default heap before the buffer is used for drawing.
    commandList->CopyBufferRegion(outBuffer.DefaultBuffer.Get(), 0, outBuffer.UploadBuffer.Get(), 0, dataSize);
    const auto transitionBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        outBuffer.DefaultBuffer.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        finalState);
    commandList->ResourceBarrier(1, &transitionBarrier);
    return true;
}

bool DX12SceneRenderer::StartGame(bool separateWindow)
{
    if (mGameHost.IsRunning() || IsGameIntroPlaying())
        return true;

    if (mSkipGameIntro || !mVideoLayer.IsGpuInitialized())
        return BeginGameAfterIntro(separateWindow);

    mGameIntroSeparateWindow = separateWindow;
    mGameIntroSkipKeyWasDown = false;
    if (!mVideoLayer.Play("Videos/logo_pterosoft", false, "Letterbox"))
    {
        PTERO_LOG_WARNING("Game", "Intro video Videos/logo_pterosoft could not be played; starting the game directly.");
        return BeginGameAfterIntro(separateWindow);
    }

    PTERO_LOG_INFO("Game", "Intro: playing Videos/logo_pterosoft.");
    mGameIntroStage = GameIntroStage::Pterosoft;
    return true;
}

void DX12SceneRenderer::UpdateGameIntro(float deltaTime)
{
    if (mGameIntroStage == GameIntroStage::None)
        return;

    mVideoLayer.Update(deltaTime);

    // Any of these skips straight to the next stage (or into the game, from the second
    // clip); edge-detected so holding the key through one skip doesn't eat the next.
    const bool skipKeyDown = QtUi::GameWindowHasFocus() &&
        ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 || (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 ||
         (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    const bool skipRequested = skipKeyDown && !mGameIntroSkipKeyWasDown;
    mGameIntroSkipKeyWasDown = skipKeyDown;

    if (!mVideoLayer.ConsumeFinished() && !skipRequested)
        return;

    mVideoLayer.Stop();
    mGameIntroSkipKeyWasDown = false;

    if (mGameIntroStage == GameIntroStage::Pterosoft && mVideoLayer.Play("Videos/logo_engine", false, "Letterbox"))
    {
        PTERO_LOG_INFO("Game", "Intro: playing Videos/logo_engine.");
        mGameIntroStage = GameIntroStage::Engine;
        return;
    }

    PTERO_LOG_INFO("Game", "Intro finished; starting the game.");
    mGameIntroStage = GameIntroStage::None;
    BeginGameAfterIntro(mGameIntroSeparateWindow);
}

// FPS and frame time are averaged over a quarter second so the numbers can be read;
// latency is the most recent frame's render latency, measured by DX12Context from the
// frame starting (input sampled) to the GPU finishing it.
void DX12SceneRenderer::UpdateStatisticsOverlay()
{
    if (!mShowStatisticsOverlay || !IsGameUiActive())
    {
        mRmlUiRenderer.SetStatisticsOverlay(false, {});
        mStatisticsText.clear();
        mStatisticsAccumulatedMs = 0.0f;
        mStatisticsAccumulatedFrames = 0;
        return;
    }

    mStatisticsAccumulatedMs += mFrameDeltaTimeMs;
    ++mStatisticsAccumulatedFrames;
    if (mStatisticsText.empty() || mStatisticsAccumulatedMs >= 250.0f)
    {
        mStatisticsFrameMs = mStatisticsAccumulatedMs / static_cast<float>((std::max)(mStatisticsAccumulatedFrames, 1));
        mStatisticsFps = mStatisticsFrameMs > 0.0f ? 1000.0f / mStatisticsFrameMs : 0.0f;
        mStatisticsAccumulatedMs = 0.0f;
        mStatisticsAccumulatedFrames = 0;

        char text[160] = {};
        const double latency = DX12Context_GetRenderLatencyMilliseconds();
        // With frame generation on, twice as many frames reach the screen as are rendered.
        std::snprintf(text, sizeof(text), "%.0f FPS%s\n%.2f ms frame\n%.1f ms latency",
            mStatisticsFps, mFrameGeneration.IsActive() ? " (x2 with frame generation)" : "",
            mStatisticsFrameMs, latency);
        mStatisticsText = text;
    }
    mRmlUiRenderer.SetStatisticsOverlay(true, mStatisticsText);
}

void DX12SceneRenderer::PrepareStandaloneGameSettings()
{
    if (mGameSettingsActive)
        return;
    BeginGameSettings(true);
    ApplySavedGameSettings();
}

bool DX12SceneRenderer::BeginGameAfterIntro(bool separateWindow)
{
    mGameInSeparateWindow = separateWindow;

    // Remember the editor pose so stopping the session restores the viewport exactly.
    mEditorCameraPositionBeforePlay = mCamera.GetPosition();
    mEditorCameraRotationBeforePlay = mCamera.GetRotation();

    GameCameraState initialCamera{};
    initialCamera.PositionX = mEditorCameraPositionBeforePlay.x;
    initialCamera.PositionY = mEditorCameraPositionBeforePlay.y;
    initialCamera.PositionZ = mEditorCameraPositionBeforePlay.z;
    initialCamera.Pitch = mEditorCameraRotationBeforePlay.x;
    initialCamera.Yaw = mEditorCameraRotationBeforePlay.y;

    mGameStopRequested = false;
    mFarkleKeys.fill(false);
    GameServices services{};
    services.User = this;
    services.FindEntity = [](void* user, const char* name) -> int {
        auto* self = static_cast<DX12SceneRenderer*>(user);
        if (self->mEntities) for (size_t i=0;i<self->mEntities->size();++i)
            if ((*self->mEntities)[i].Name == name) return static_cast<int>(i);
        return -1;
    };
    services.GetTransform = [](void* user, int index, GameTransform* out) -> bool {
        auto* self = static_cast<DX12SceneRenderer*>(user);
        if (!out || !self->mEntities || index<0 || size_t(index)>=self->mEntities->size()) return false;
        const auto& t=(*self->mEntities)[index].Transform;
        *out={{t.Position.x,t.Position.y,t.Position.z},{t.Rotation.x,t.Rotation.y,t.Rotation.z},{t.Scale.x,t.Scale.y,t.Scale.z}};
        return true;
    };
    services.SetTransform = [](void* user, int index, const GameTransform* pose) {
        auto* self = static_cast<DX12SceneRenderer*>(user);
        if (!pose || !self->mEntities || index<0 || size_t(index)>=self->mEntities->size()) return;
        auto& t=(*self->mEntities)[index].Transform;
        t.Position={pose->Position[0],pose->Position[1],pose->Position[2]};
        t.Rotation={pose->Rotation[0],pose->Rotation[1],pose->Rotation[2]};
        t.Scale={pose->Scale[0],pose->Scale[1],pose->Scale[2]};
    };
    services.LoadUi = [](void* user, const char* path) -> bool {
        auto& ui=static_cast<DX12SceneRenderer*>(user)->mRmlUiRenderer;
        ui.SetVisible(true); return ui.LoadDocument(path);
    };
    services.Ui = [](void* user,int op,const char* id,const char* value) {
        static_cast<DX12SceneRenderer*>(user)->mRmlUiRenderer.ApplyGameUiCommand(op,id,value);
    };
    services.RequestStop = [](void* user) {static_cast<DX12SceneRenderer*>(user)->mGameStopRequested=true;};
    services.ToggleFullscreen = [](void*) {QtUi::ToggleGameFullscreen();};
    services.PlaySound = [](void* user, const char* name) {
        auto* audio=static_cast<DX12SceneRenderer*>(user)->mAudioManager;
        if (audio && name) audio->PlayOneShotByName(name);
    };
    services.PlayMusic = [](void* user, const char* name) -> bool {
        auto* audio=static_cast<DX12SceneRenderer*>(user)->mAudioManager;
        return audio && name && audio->PlayMusicByName(name);
    };
    services.IsMusicPlaying = [](void* user) -> bool {
        auto* audio=static_cast<DX12SceneRenderer*>(user)->mAudioManager;
        return audio && audio->IsMusicPlaying();
    };
    services.PollAction = [](void* user) -> int {
        auto* self=static_cast<DX12SceneRenderer*>(user);
        const auto id=self->mRmlUiRenderer.PollGameAction();
        if (!id.empty()) {
            const std::pair<const char*,int> actions[]={{"roll-button",Roll},{"bank-button",Bank},{"clear-button",Clear},{"help-button",Help},{"pause-button",Pause},{"close-button",Close},{"rematch-button",Rematch},{"main-menu-button",MainMenu},{"continue-button",Continue},{"fullscreen-button",Fullscreen},{"start-button",StartMatch},{"exit-button",ExitGame}};
            for (const auto& action:actions) if(id==action.first)return action.second;
            for(int i=0;i<6;++i) {if(id=="die-"+std::to_string(i+1))return SelectDie+i;if(id=="slot-"+std::to_string(i+1))return RemoveDie+i;}
        }
        const std::pair<int,int> keys[]={{'R',Roll},{'B',Bank},{'C',Clear},{VK_F1,Help},{VK_ESCAPE,Pause},{VK_F11,Fullscreen},{'1',SelectDie},{'2',SelectDie+1},{'3',SelectDie+2},{'4',SelectDie+3},{'5',SelectDie+4},{'6',SelectDie+5}};
        int result=NoAction;
        for(const auto& key:keys) {
            bool down=QtUi::GameWindowHasFocus() && (GetAsyncKeyState(key.first)&0x8000)!=0;
            if(down&&!self->mFarkleKeys[key.first]&&!result)result=key.second;
            self->mFarkleKeys[key.first]=down;
        }
        return result;
    };
    services.GetWinningScore = [](void* user) -> int {
        return static_cast<DX12SceneRenderer*>(user)->mRmlUiRenderer.GetFarkleWinningScore();
    };
    // The editor tests with its own HUD and menu; the shipped menus are the standalone's.
    services.Standalone = QtUi::IsStandaloneGame();
    // Before Start: the game loads its first menu there, and the menu needs its options.
    // A standalone game has had its settings applied since the level loaded (see
    // PrepareStandaloneGameSettings); beginning again would take those applied values as
    // the level's baseline and scale them a second time.
    const bool settingsAlreadyApplied = mGameSettingsActive;
    if (!settingsAlreadyApplied)
        BeginGameSettings(services.Standalone);
    if (!mGameHost.Start(initialCamera, services))
    {
        EndGameSettings();
        return false;
    }

    // Hover and click are the host's to make: the game module never sees them.
    mRmlUiRenderer.SetUiSoundCallback([this](bool click) {
        if (!mAudioManager) return;
        // Clicks everywhere, but hover only on the menu and result screens. The play HUD
        // puts six dice and three actions under a pointer that is moving constantly, and
        // a tick for each one is chatter rather than feedback.
        if (!click && (mRmlUiRenderer.GetLoadedDocumentName() == "Farkle/farkle.rml" ||
                       mRmlUiRenderer.GetLoadedDocumentName() == "Farkle/game.rml"))
            return;
        mAudioManager->PlayOneShotByName(click ? "Click" : "Hover");
    });

    // The window is titled after whatever the module reports, so a different game does
    // not put up a window that still says Farkle.
    const std::string windowTitle =
        (mGameHost.GetGameName().empty() ? std::string("Play") : mGameHost.GetGameName())
        + " - F11 Fullscreen";
    QtUi::OpenGameWindow(separateWindow, windowTitle.c_str());
    // After the window exists, since the display mode is one of the settings.
    if (services.Standalone && !settingsAlreadyApplied)
        ApplySavedGameSettings();

    // The graph runs on a copy taken here, so On Game Start sees the level exactly as it
    // was when play was pressed even if the artist keeps editing the graph afterwards.
    mNodeGraphHost.SetUiRenderer(&mRmlUiRenderer);
    mNodeGraphHost.SetVideoLayer(&mVideoLayer);
    if (mNodeGraphSource != nullptr)
    {
        // Entities added since the level was loaded have no id yet. Nothing in the graph
        // can point at them, but Find Entity can still hand one out.
        if (mEntities != nullptr)
            EnsureEntityIds(*mEntities);
        mNodeGraphRuntime.SetHost(&mNodeGraphHost);
        mNodeGraphRuntime.Start(*mNodeGraphSource);
    }

    mGameHasLastMousePosition = false;
    return true;
}

void DX12SceneRenderer::StopGame()
{
    if (IsGameIntroPlaying())
    {
        mGameIntroStage = GameIntroStage::None;
        mVideoLayer.Stop();
        mVideoLayer.ConsumeFinished();
        EndGameSettings();
        return;
    }

    if (!mGameHost.IsRunning())
        return;

    // A packaged game stopping is the process quitting. The teardown below exists to hand
    // the editor back its own state - baseline graphics settings, editor camera - and in a
    // standalone game that only rebuilds upscaler targets in the very last frame before
    // exit, which is how quitting ended in a GPU hang. Just close the window; the host
    // waits for the GPU and ends the process.
    if (QtUi::IsStandaloneGame())
    {
        if (mAudioManager) mAudioManager->StopAll();
        mGameStopRequested = false;
        QtUi::CloseGameWindow();
        return;
    }

    // Stop the graph first: On Game Stop is still allowed to touch the UI, and the UI
    // outlives the game module.
    mNodeGraphRuntime.Stop();
    mGameHost.Stop();
    EndGameSettings();
    mRmlUiRenderer.SetUiSoundCallback({});
    mRmlUiRenderer.CloseDocument();
    // Like the soundtrack, a video belongs to the session.
    mVideoLayer.Stop();
    mVideoLayer.ConsumeFinished();
    // The soundtrack belongs to the session, not the level: it has to end here even if
    // the game module died without getting the chance to stop it itself.
    if (mAudioManager) mAudioManager->StopMusic();
    QtUi::CloseGameWindow();
    mGameStopRequested = false;
    mGameHasLastMousePosition = false;
    SetCameraTransform(mEditorCameraPositionBeforePlay, mEditorCameraRotationBeforePlay);
}

void DX12SceneRenderer::ToggleGame(bool separateWindow)
{
    if (mGameHost.IsRunning())
        StopGame();
    else
        StartGame(separateWindow);
}

void DX12SceneRenderer::RecordPassTiming(const char* category, const char* name, float milliseconds)
{
    // Merge repeats of the same pass rather than appending duplicates: a few
    // passes record from more than one scope in a frame, and the artist wants
    // the pass's total, not a list of fragments.
    for (RendererTimingEntry& entry : mPendingTimingSnapshot.Entries)
    {
        if (entry.Name == name && entry.Category == category)
        {
            entry.CpuMilliseconds += milliseconds;
            return;
        }
    }

    RendererTimingEntry entry;
    entry.Category = category;
    entry.Name = name;
    entry.CpuMilliseconds = milliseconds;
    mPendingTimingSnapshot.Entries.push_back(std::move(entry));
}

void DX12SceneRenderer::BeginTimingFrame()
{
    // Keep the vector's capacity: the pass set barely changes between frames,
    // so this should not allocate after the first few.
    mPendingTimingSnapshot.Entries.clear();
    mPendingTimingSnapshot.TotalCpuMilliseconds = 0.0f;
}

void DX12SceneRenderer::EndTimingFrame(float totalMilliseconds)
{
    mPendingTimingSnapshot.TotalCpuMilliseconds = totalMilliseconds;

    // Most expensive first - that is the only order worth reading when the
    // question is "where did the frame go".
    std::sort(
        mPendingTimingSnapshot.Entries.begin(),
        mPendingTimingSnapshot.Entries.end(),
        [](const RendererTimingEntry& left, const RendererTimingEntry& right)
        {
            return left.CpuMilliseconds > right.CpuMilliseconds;
        });

    mRendererTimingSnapshot = mPendingTimingSnapshot;
}

void DX12SceneRenderer::BuildGiPointLights()
{
    const int lightCount = (std::min)(mNumCachedPointLights, DeferredLightingPass::kMaxPointLights);
    if (lightCount <= 0)
    {
        return;
    }

    std::memcpy(
        mGiPointLights,
        mCachedPointLights,
        static_cast<size_t>(lightCount) * sizeof(DeferredLightingPass::PointLightGpu));

    for (int i = 0; i < lightCount; ++i)
    {
        const float scale = mPointLightGiScale[i];
        if (scale == 1.0f)
        {
            continue;
        }

        mGiPointLights[i].Color.x *= scale;
        mGiPointLights[i].Color.y *= scale;
        mGiPointLights[i].Color.z *= scale;
    }
}

void DX12SceneRenderer::UpdateCamera()
{
    static auto previousFrameTime = std::chrono::steady_clock::now();

    const auto currentFrameTime = std::chrono::steady_clock::now();
    const float deltaTime = std::chrono::duration<float>(currentFrameTime - previousFrameTime).count();
    previousFrameTime = currentFrameTime;
    // Store in ms for NRD's timing expectations.
    mFrameDeltaTimeMs = deltaTime * 1000.0f;

    // Clamped so a level load or a debugger pause does not jump every animated
    // light style forward by several seconds in one frame.
    mSceneTimeSeconds += (std::min)(deltaTime, 0.1f);

    HWND windowHandle = DX12Context_GetWindowHandle();
    if (windowHandle == nullptr)
    {
        return;
    }

    POINT mousePosition{};
    if (!GetCursorPos(&mousePosition))
    {
        return;
    }

    ScreenToClient(windowHandle, &mousePosition);

    // GetAsyncKeyState reports the physical key no matter which window has focus, so every
    // movement key has to be gated on the editor actually owning the keyboard. Without it
    // the camera drifts while the user types in another application.
    const bool keyboardOwned = QtUi::KeyboardCameraInputAllowed();
    const bool moveForward = keyboardOwned && (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool moveBackward = keyboardOwned && (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool moveLeft = keyboardOwned && (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool moveRight = keyboardOwned && (GetAsyncKeyState('D') & 0x8000) != 0;
    const bool moveUp = keyboardOwned && (GetAsyncKeyState('E') & 0x8000) != 0;
    const bool moveDown = keyboardOwned && (GetAsyncKeyState('Q') & 0x8000) != 0;
    const bool rightMouseButtonDown = QtUi::CameraInputAllowed() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    // While right-dragging to fly the camera, the wheel trims how fast it flies. The step
    // is multiplicative so one notch means the same relative change whether the camera is
    // crawling around a prop or crossing a terrain. Drain the wheel either way, otherwise
    // scrolling outside a drag would bank up and fire the moment one starts.
    const float cameraSpeedWheelNotches = QtUi::ConsumeViewportWheelDelta();
    if (rightMouseButtonDown && cameraSpeedWheelNotches != 0.0f)
    {
        constexpr float speedStepPerNotch = 1.15f;
        // Matches the Camera Speed slider's range so the two never disagree.
        constexpr float minimumSpeed = 0.05f;
        constexpr float maximumSpeed = 200.0f;
        const float adjustedSpeed =
            mCamera.GetMovementSpeed() * std::pow(speedStepPerNotch, cameraSpeedWheelNotches);
        mCamera.SetMovementSpeed(std::clamp(adjustedSpeed, minimumSpeed, maximumSpeed));
    }

    if (mGameHost.IsRunning())
    {
        UpdateGameCamera(deltaTime, mousePosition, rightMouseButtonDown);
        return;
    }

    if (IsGameIntroPlaying())
    {
        UpdateGameIntro(deltaTime);
        return;
    }

    // Feed the current keyboard state and mouse position into the camera every frame.
    mCamera.Update(
        deltaTime,
        moveForward,
        moveBackward,
        moveLeft,
        moveRight,
        moveUp,
        moveDown,
        rightMouseButtonDown,
        static_cast<float>(mousePosition.x),
        static_cast<float>(mousePosition.y));
}

void DX12SceneRenderer::UpdateGameCamera(float deltaTime, const POINT& mousePosition, bool lookActive)
{
    // Escape belongs to Farkle's pause menu. Closing its window ends the session.
    if (mGameStopRequested || QtUi::ConsumeGameCloseRequest())
    {
        StopGame();
        return;
    }

    GameFrameContext frame{};
    frame.DeltaSeconds = deltaTime;

    // Same reasoning as the editor camera: polled keys must not drive the player while the
    // editor is in the background or a panel is taking text.
    const bool keyboardOwned = QtUi::KeyboardCameraInputAllowed();
    GameInputState& input = frame.Input;
    input.MoveForward = keyboardOwned && (GetAsyncKeyState('W') & 0x8000) != 0;
    input.MoveBackward = keyboardOwned && (GetAsyncKeyState('S') & 0x8000) != 0;
    input.MoveLeft = keyboardOwned && (GetAsyncKeyState('A') & 0x8000) != 0;
    input.MoveRight = keyboardOwned && (GetAsyncKeyState('D') & 0x8000) != 0;
    input.MoveUp = keyboardOwned && ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 || (GetAsyncKeyState('E') & 0x8000) != 0);
    input.MoveDown = keyboardOwned && ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 || (GetAsyncKeyState('Q') & 0x8000) != 0);
    input.Sprint = keyboardOwned && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    input.LookActive = lookActive;

    const float mouseX = static_cast<float>(mousePosition.x);
    const float mouseY = static_cast<float>(mousePosition.y);
    if (lookActive)
    {
        // Only report a delta once a starting position has been captured, otherwise the
        // first frame of a drag would snap the view by the whole screen offset.
        if (mGameHasLastMousePosition)
        {
            input.LookDeltaX = mouseX - mGameLastMouseX;
            input.LookDeltaY = mouseY - mGameLastMouseY;
        }

        mGameLastMouseX = mouseX;
        mGameLastMouseY = mouseY;
        mGameHasLastMousePosition = true;
    }
    else
    {
        mGameHasLastMousePosition = false;
    }

    GameCameraState cameraState{};
    const DirectX::XMFLOAT3 position = mCamera.GetPosition();
    const DirectX::XMFLOAT3 rotation = mCamera.GetRotation();
    cameraState.PositionX = position.x;
    cameraState.PositionY = position.y;
    cameraState.PositionZ = position.z;
    cameraState.Pitch = rotation.x;
    cameraState.Yaw = rotation.y;

    mGameHost.Update(frame, cameraState);

    mCamera.SetPosition(cameraState.PositionX, cameraState.PositionY, cameraState.PositionZ);
    mCamera.SetRotation(cameraState.Pitch, cameraState.Yaw);

    // Before the graph ticks, so On Video Finished fires in the frame the video ended.
    mVideoLayer.Update(deltaTime);
    mNodeGraphRuntime.Tick(deltaTime);

    // A Stop Game node only raises a flag; tearing the runtime down from inside its own
    // execution would destroy the state the current step is still walking.
    if (mGameStopRequested || mNodeGraphRuntime.ConsumeStopRequest())
    {
        StopGame();
    }
}

void DX12SceneRenderer::FrameAabbInView(
    const DirectX::XMFLOAT3& aabbMin,
    const DirectX::XMFLOAT3& aabbMax)
{
    const DirectX::XMFLOAT3 center(
        (aabbMin.x + aabbMax.x) * 0.5f,
        (aabbMin.y + aabbMax.y) * 0.5f,
        (aabbMin.z + aabbMax.z) * 0.5f);

    const float extentX = aabbMax.x - aabbMin.x;
    const float extentY = aabbMax.y - aabbMin.y;
    const float extentZ = aabbMax.z - aabbMin.z;
    const float maxExtent = (std::max)((std::max)(extentX, extentY), extentZ);
    const float distance = (std::max)(maxExtent * 1.25f, 4.0f);

    const DirectX::XMFLOAT3 position(
        center.x,
        center.y - distance,
        center.z + distance * 0.5f);

    mCamera.SetPosition(position);
    mCamera.LookAt(center.x, center.y, center.z);
    mTaaSettings.ResetHistory = true;
}

void DX12SceneRenderer::UpdateSceneConstants()
{
    if (mMappedConstants == nullptr)
    {
        return;
    }

    const XMMATRIX model      = XMMatrixIdentity();
    const XMMATRIX view       = mCamera.GetViewMatrix();
    XMMATRIX       projection = mCamera.GetProjectionMatrix();
    mPrevCameraJitter[0] = mCurrentCameraJitter[0];
    mPrevCameraJitter[1] = mCurrentCameraJitter[1];
    mCurrentCameraJitter[0] = 0.0f;
    mCurrentCameraJitter[1] = 0.0f;

    // Detect meaningful camera movement with a tolerance instead of exact float equality.
    // Exact memcmp on matrices can spuriously differ frame-to-frame and continuously
    // reset TAA history, which prevents temporal stabilization.
    XMFLOAT4X4 viewF;
    XMStoreFloat4x4(&viewF, view);
    float maxViewDiff = 0.0f;
    const float* currentViewElems = &viewF.m[0][0];
    const float* previousViewElems = &mPreviousViewMatrix.m[0][0];
    for (int i = 0; i < 16; ++i)
    {
        maxViewDiff = (std::max)(maxViewDiff, std::fabs(currentViewElems[i] - previousViewElems[i]));
    }
    mCameraMovedThisFrame = maxViewDiff > 1e-4f;
    if (mTaaSettings.Enabled && maxViewDiff > 0.25f)
    {
        // Treat only large view jumps as cuts. Resetting on every camera update
        // prevents the TAA resolve from accumulating any history at all.
        mTaaSettings.ResetHistory = true;
    }
    mPreviousViewMatrix = viewF;

    // Cache the non-jittered VP before jitter is applied - used by RT GI to avoid
    // triggering a false accumulation reset every frame from the TAA sub-pixel shift.
    XMStoreFloat4x4(&mNonJitteredViewProjection, view * projection);

    // Cache the individual view and projection matrices (non-jittered) so that NRD
    // can receive worldToView and viewToClip separately each frame.
    mPrevNonJitteredViewMatrix       = mNonJitteredViewMatrix;
    mPrevNonJitteredProjectionMatrix = mNonJitteredProjectionMatrix;
    XMStoreFloat4x4(&mNonJitteredViewMatrix,       view);
    XMStoreFloat4x4(&mNonJitteredProjectionMatrix, projection);

    // Turning either AO pass on or off changes the image enough that the accumulated
    // history no longer describes it, so drop the history. The jitter itself is no longer
    // suppressed: XeGTAO is now handed the jittered projection that matches the depth it
    // samples, and RTAO already passes its jitter through to NRD explicitly.
    const bool rtaoEnabled = mRtaoSettings.Enabled;
    const bool gtaoEnabled = mGtaoSettings.Enabled;
    if (rtaoEnabled != mRtaoEnabledLastFrame)
    {
        mTaaSettings.ResetHistory = true;
        mRtaoEnabledLastFrame = rtaoEnabled;
    }
    if (gtaoEnabled != mGtaoEnabledLastFrame)
    {
        mTaaSettings.ResetHistory = true;
        mGtaoEnabledLastFrame = gtaoEnabled;
    }

    // Apply a sub-pixel Halton jitter to the projection matrix when TAA is enabled.
    // Jittering causes each frame to sample a slightly different sub-pixel location;
    // the TAA resolve pass then accumulates these into a stable anti-aliased image.
    //
    // FSR needs jitter whether or not TAA is on, and uses its own sequence: its
    // length grows with the upscale ratio so every output pixel is covered.
    if (IsFsrUpscalerActive() && mSceneWidth > 0 && mSceneHeight > 0)
    {
        UINT outputWidth = mSceneWidth;
        UINT outputHeight = mSceneHeight;
        DX12Context_GetRenderSize(&outputWidth, &outputHeight);

        float fsrJitterX = 0.0f;
        float fsrJitterY = 0.0f;
        mFsrRenderer.GetJitterOffset(mFsrJitterIndex++, mSceneWidth, outputWidth, fsrJitterX, fsrJitterY);

        // FSR reports pixels with +Y down; the engine's offset shifts NDC by
        // (2x/w, 2y/h), and NDC +Y is up.
        mCurrentCameraJitter[0] = fsrJitterX;
        mCurrentCameraJitter[1] = -fsrJitterY;

        XMFLOAT4X4 projF;
        XMStoreFloat4x4(&projF, projection);
        projF._31 += mCurrentCameraJitter[0] * 2.0f / static_cast<float>(mSceneWidth);
        projF._32 += mCurrentCameraJitter[1] * 2.0f / static_cast<float>(mSceneHeight);
        projection = XMLoadFloat4x4(&projF);
    }
    else if (IsDlssUpscalerActive() && mSceneWidth > 0 && mSceneHeight > 0)
    {
        // DLSS reconstructs from sub-pixel jitter just like FSR, but it used to get jitter
        // only from the TAA branch below - so with TAA off (as it must be under an upscaler)
        // DLSS saw the same samples every frame and could not resolve anything, leaving
        // aliasing that crawls like noise, worse the lower the quality mode.
        //
        // NVIDIA's guidance: Halton(2,3), with 8 * (display / render)^2 phases so the
        // sequence covers every output pixel at the current scale.
        UINT outputWidth = mSceneWidth;
        UINT outputHeight = mSceneHeight;
        DX12Context_GetRenderSize(&outputWidth, &outputHeight);
        const float scale = static_cast<float>((std::max)(outputWidth, mSceneWidth)) / static_cast<float>(mSceneWidth);
        const UINT phaseCount = (std::max)(8u, static_cast<UINT>(std::ceil(8.0f * scale * scale)));

        auto halton = [](UINT index, UINT base)
        {
            float fraction = 1.0f;
            float result = 0.0f;
            for (; index > 0; index /= base)
            {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(index % base);
            }
            return result;
        };
        mDlssJitterIndex = (mDlssJitterIndex % phaseCount) + 1; // Halton index 0 is (0,0)
        mCurrentCameraJitter[0] = halton(mDlssJitterIndex, 2) - 0.5f;
        mCurrentCameraJitter[1] = halton(mDlssJitterIndex, 3) - 0.5f;

        XMFLOAT4X4 projF;
        XMStoreFloat4x4(&projF, projection);
        projF._31 += mCurrentCameraJitter[0] * 2.0f / static_cast<float>(mSceneWidth);
        projF._32 += mCurrentCameraJitter[1] * 2.0f / static_cast<float>(mSceneHeight);
        projection = XMLoadFloat4x4(&projF);
    }
    else if (mTaaSettings.Enabled && mTaaSettings.JitterScale > 0.0f && mSceneWidth > 0 && mSceneHeight > 0)
    {
        // Halton(2,3) sequence gives a well-distributed low-discrepancy pattern.
        constexpr UINT HaltonSequenceLength = 16;
        static const float kHaltonX[HaltonSequenceLength] =
        {
             0.5f,   0.25f,  0.75f,  0.125f,  0.625f,  0.375f,  0.875f,  0.0625f,
             0.5625f,0.3125f,0.8125f, 0.1875f, 0.6875f, 0.4375f, 0.9375f, 0.03125f
        };
        static const float kHaltonY[HaltonSequenceLength] =
        {
             1.f/3,  2.f/3, 1.f/9, 4.f/9, 7.f/9, 2.f/9, 5.f/9, 8.f/9,
             1.f/27, 10.f/27,19.f/27,4.f/27,13.f/27,22.f/27,7.f/27,16.f/27
        };

        mJitterIndex = (mJitterIndex + 1) % HaltonSequenceLength;

        // Keep the original sub-pixel jitter in pixel units for NRD.
        const float jitterPixelX = (kHaltonX[mJitterIndex] - 0.5f) * mTaaSettings.JitterScale;
        const float jitterPixelY = (kHaltonY[mJitterIndex] - 0.5f) * mTaaSettings.JitterScale;
        mCurrentCameraJitter[0] = jitterPixelX;
        mCurrentCameraJitter[1] = jitterPixelY;

        // Convert jitter from pixel units to NDC (range [-1,1]) for the engine projection matrix.
        const float jitterX = jitterPixelX * 2.0f / static_cast<float>(mSceneWidth);
        const float jitterY = jitterPixelY * 2.0f / static_cast<float>(mSceneHeight);

        // Add the jitter directly to the last column of the projection matrix (NDC shift).
        XMFLOAT4X4 projF;
        XMStoreFloat4x4(&projF, projection);
        projF._31 += jitterX;
        projF._32 += jitterY;
        projection = XMLoadFloat4x4(&projF);
    }

    // The floor grid lives on the Z-up ground plane so the camera transform is the only motion applied here.
    const XMMATRIX mvp = XMMatrixTranspose(model * view * projection);
    XMStoreFloat4x4(&mMappedConstants->ModelViewProjection, mvp);

    // Cache the jittered VP so Render() can pass it to the entity renderer.
    XMStoreFloat4x4(&mJitteredViewProjection, view * projection);

    // Cache the jittered projection alone so the sky pass uses the same per-frame
    // jitter as geometry, avoiding sky fringing at mesh edges during TAA accumulation.
    XMStoreFloat4x4(&mJitteredProjection, projection);
}

// ---------------------------------------------------------------------------
// Node graph host
// ---------------------------------------------------------------------------
//
// Every UI node in a graph funnels through here. The adapter is intentionally thin: it
// checks that a UI exists and forwards, so a graph that runs before the UI initialised
// reports a clean failure on its Success pin instead of crashing the play session.

bool DX12SceneRenderer::NodeGraphUiHost::ShowUiDocument(const std::string& fileName)
{
    if (mUiRenderer == nullptr || !mUiRenderer->IsInitialized())
        return false;

    // Showing a document implies the layer is meant to be seen; a graph that hid the UI
    // earlier should not have to remember to unhide it here.
    mUiRenderer->SetVisible(true);
    return mUiRenderer->LoadDocument(fileName);
}

void DX12SceneRenderer::NodeGraphUiHost::CloseUiDocument()
{
    if (mUiRenderer != nullptr && mUiRenderer->IsInitialized())
        mUiRenderer->CloseDocument();
}

bool DX12SceneRenderer::NodeGraphUiHost::ReloadUiDocument()
{
    return mUiRenderer != nullptr && mUiRenderer->IsInitialized() && mUiRenderer->ReloadDocument();
}

void DX12SceneRenderer::NodeGraphUiHost::SetUiVisible(bool visible)
{
    if (mUiRenderer != nullptr)
        mUiRenderer->SetVisible(visible);
}

void DX12SceneRenderer::NodeGraphUiHost::SetUiInputEnabled(bool enabled)
{
    if (mUiRenderer != nullptr)
        mUiRenderer->SetInputEnabled(enabled);
}

bool DX12SceneRenderer::NodeGraphUiHost::SetUiElementText(const std::string& elementId, const std::string& text)
{
    return mUiRenderer != nullptr && mUiRenderer->SetElementText(elementId, text);
}

bool DX12SceneRenderer::NodeGraphUiHost::SetUiElementProperty(
    const std::string& elementId,
    const std::string& property,
    const std::string& value)
{
    return mUiRenderer != nullptr && mUiRenderer->SetElementProperty(elementId, property, value);
}

bool DX12SceneRenderer::NodeGraphUiHost::SetUiElementClass(
    const std::string& elementId,
    const std::string& className,
    bool enabled)
{
    return mUiRenderer != nullptr && mUiRenderer->SetElementClass(elementId, className, enabled);
}

bool DX12SceneRenderer::NodeGraphUiHost::SetUiElementVisible(const std::string& elementId, bool visible)
{
    return mUiRenderer != nullptr && mUiRenderer->SetElementVisible(elementId, visible);
}

// The video nodes forward to the video layer the same way; it lives in Video.dll and
// reports its own failures, which the log picks up here.

bool DX12SceneRenderer::NodeGraphUiHost::PlayVideo(const std::string& fileName, bool loop, const std::string& fit)
{
    if (mVideoLayer == nullptr)
        return false;

    if (!mVideoLayer->Play(fileName, loop, fit))
    {
        PTERO_LOG_WARNING("Video", "Play Video '%s' failed: %s", fileName.c_str(), mVideoLayer->GetLastError().c_str());
        return false;
    }

    return true;
}

void DX12SceneRenderer::NodeGraphUiHost::PauseVideo()
{
    if (mVideoLayer != nullptr)
        mVideoLayer->Pause();
}

void DX12SceneRenderer::NodeGraphUiHost::ResumeVideo()
{
    if (mVideoLayer != nullptr)
        mVideoLayer->Resume();
}

void DX12SceneRenderer::NodeGraphUiHost::StopVideo()
{
    if (mVideoLayer != nullptr)
        mVideoLayer->Stop();
}

void DX12SceneRenderer::NodeGraphUiHost::SeekVideo(double seconds)
{
    if (mVideoLayer != nullptr)
        mVideoLayer->Seek(seconds);
}

void DX12SceneRenderer::NodeGraphUiHost::SetVideoLooping(bool loop)
{
    if (mVideoLayer != nullptr)
        mVideoLayer->SetLooping(loop);
}

void DX12SceneRenderer::NodeGraphUiHost::SetVideoVolume(double volume)
{
    if (mVideoLayer != nullptr)
        mVideoLayer->SetVolume(static_cast<float>(volume));
}

bool DX12SceneRenderer::NodeGraphUiHost::IsVideoPlaying()
{
    return mVideoLayer != nullptr && mVideoLayer->IsPlaying();
}

double DX12SceneRenderer::NodeGraphUiHost::GetVideoTime()
{
    return mVideoLayer != nullptr ? mVideoLayer->GetTime() : 0.0;
}

double DX12SceneRenderer::NodeGraphUiHost::GetVideoDuration()
{
    return mVideoLayer != nullptr ? mVideoLayer->GetDuration() : 0.0;
}

bool DX12SceneRenderer::NodeGraphUiHost::ConsumeVideoFinished()
{
    return mVideoLayer != nullptr && mVideoLayer->ConsumeFinished();
}

// Entities. The graph runs against the play session's copy of the level (Editor swaps
// mEntities for the session), so whatever it moves snaps back when play stops, exactly
// as it does for the game module.

Entity* DX12SceneRenderer::NodeGraphUiHost::FindEntity(std::uint64_t entityId)
{
    if (entityId == 0 || mOwner.mEntities == nullptr)
        return nullptr;

    for (Entity& entity : *mOwner.mEntities)
    {
        if (entity.Id == entityId)
            return &entity;
    }

    return nullptr;
}

std::uint64_t DX12SceneRenderer::NodeGraphUiHost::FindEntityByName(const std::string& name)
{
    if (mOwner.mEntities == nullptr)
        return 0;

    for (const Entity& entity : *mOwner.mEntities)
    {
        if (entity.Name == name)
            return entity.Id;
    }

    return 0;
}

bool DX12SceneRenderer::NodeGraphUiHost::GetEntityName(std::uint64_t entityId, std::string& name)
{
    const Entity* entity = FindEntity(entityId);
    if (entity == nullptr)
        return false;

    name = entity->Name;
    return true;
}

bool DX12SceneRenderer::NodeGraphUiHost::GetEntityTransform(std::uint64_t entityId, NodeGraphTransform& transform)
{
    const Entity* entity = FindEntity(entityId);
    if (entity == nullptr)
        return false;

    const TransformComponent& source = entity->Transform;
    const DirectX::XMFLOAT3* vectors[3] = { &source.Position, &source.Rotation, &source.Scale };
    double* targets[3] = { transform.Position, transform.Rotation, transform.Scale };
    for (int v = 0; v < 3; ++v)
    {
        targets[v][0] = vectors[v]->x;
        targets[v][1] = vectors[v]->y;
        targets[v][2] = vectors[v]->z;
    }

    return true;
}

bool DX12SceneRenderer::NodeGraphUiHost::SetEntityTransform(std::uint64_t entityId, const NodeGraphTransform& transform)
{
    Entity* entity = FindEntity(entityId);
    if (entity == nullptr)
        return false;

    auto toFloat3 = [](const double* v) {
        return DirectX::XMFLOAT3(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
    };
    entity->Transform.Position = toFloat3(transform.Position);
    entity->Transform.Rotation = toFloat3(transform.Rotation);
    entity->Transform.Scale = toFloat3(transform.Scale);
    return true;
}

namespace
{
    // One row per name in the catalogue's entity property list (NodeGraphCatalog.cpp).
    // Each resolves to a float or bool field of a component the entity may not have.
    struct EntityPropertyBinding
    {
        const char* Name;
        float* (*Float)(Entity&);
        bool* (*Flag)(Entity&);
    };

    const EntityPropertyBinding kEntityProperties[] = {
        { "Light Intensity", [](Entity& e) { return e.PointLight ? &e.PointLight->IntensityLumens : nullptr; }, nullptr },
        { "Light Radius", [](Entity& e) { return e.PointLight ? &e.PointLight->Radius : nullptr; }, nullptr },
        { "Light Color R", [](Entity& e) { return e.PointLight ? &e.PointLight->ColorR : nullptr; }, nullptr },
        { "Light Color G", [](Entity& e) { return e.PointLight ? &e.PointLight->ColorG : nullptr; }, nullptr },
        { "Light Color B", [](Entity& e) { return e.PointLight ? &e.PointLight->ColorB : nullptr; }, nullptr },
        { "Light Temperature", [](Entity& e) { return e.PointLight ? &e.PointLight->TemperatureKelvin : nullptr; }, nullptr },
        { "Light Uses Temperature", nullptr, [](Entity& e) { return e.PointLight ? &e.PointLight->UseTemperature : nullptr; } },
        { "Light Casts Shadows", nullptr, [](Entity& e) { return e.PointLight ? &e.PointLight->CastShadows : nullptr; } },
        { "Particles Enabled", nullptr, [](Entity& e) { return e.ParticleSystem ? &e.ParticleSystem->Enabled : nullptr; } },
        { "Particle Spawn Rate", [](Entity& e) { return e.ParticleSystem ? &e.ParticleSystem->SpawnRate : nullptr; }, nullptr },
        { "Rain Enabled", nullptr, [](Entity& e) { return e.Rain ? &e.Rain->Enabled : nullptr; } },
        { "Rain Intensity", [](Entity& e) { return e.Rain ? &e.Rain->Intensity : nullptr; }, nullptr },
    };

    const EntityPropertyBinding* FindEntityProperty(const std::string& name)
    {
        for (const EntityPropertyBinding& binding : kEntityProperties)
        {
            if (name == binding.Name)
                return &binding;
        }
        return nullptr;
    }
}

bool DX12SceneRenderer::NodeGraphUiHost::GetEntityProperty(
    std::uint64_t entityId, const std::string& property, double& value)
{
    Entity* entity = FindEntity(entityId);
    const EntityPropertyBinding* binding = FindEntityProperty(property);
    if (entity == nullptr || binding == nullptr)
        return false;

    if (binding->Float != nullptr)
    {
        if (const float* field = binding->Float(*entity))
        {
            value = *field;
            return true;
        }
    }
    else if (const bool* flag = binding->Flag(*entity))
    {
        value = *flag ? 1.0 : 0.0;
        return true;
    }

    return false;
}

bool DX12SceneRenderer::NodeGraphUiHost::SetEntityProperty(
    std::uint64_t entityId, const std::string& property, double value)
{
    Entity* entity = FindEntity(entityId);
    const EntityPropertyBinding* binding = FindEntityProperty(property);
    if (entity == nullptr || binding == nullptr)
        return false;

    if (binding->Float != nullptr)
    {
        if (float* field = binding->Float(*entity))
        {
            *field = static_cast<float>(value);
            return true;
        }
    }
    else if (bool* flag = binding->Flag(*entity))
    {
        *flag = value != 0.0;
        return true;
    }

    return false;
}

bool DX12SceneRenderer::NodeGraphUiHost::SetEntityAudioPlaying(std::uint64_t entityId, bool playing)
{
    Entity* entity = FindEntity(entityId);
    if (entity == nullptr || !entity->AudioEmitter || mOwner.mAudioManager == nullptr)
        return false;

    // The emitter is registered by the per-frame audio update (DX12RendererAPI.cpp), which
    // also keeps it positioned on the entity. Until that has run once there is no handle.
    // RuntimeAutoPlayStarted is deliberately left alone: that update stops an emitter whose
    // flag is set while AutoPlay is off, which would cut a graph-started sound instantly.
    const int handle = entity->AudioEmitter->RuntimeEmitterHandle;
    if (handle < 0)
        return false;

    const AudioManager::EmitterHandle emitter{ handle };
    return playing ? mOwner.mAudioManager->PlayEmitter(emitter) : mOwner.mAudioManager->StopEmitter(emitter);
}

NodeGraphCameraPose DX12SceneRenderer::NodeGraphUiHost::GetCamera()
{
    const DirectX::XMFLOAT3 position = mOwner.mCamera.GetPosition();
    const DirectX::XMFLOAT3 rotation = mOwner.mCamera.GetRotation();

    NodeGraphCameraPose pose;
    pose.Position[0] = position.x;
    pose.Position[1] = position.y;
    pose.Position[2] = position.z;
    pose.Pitch = rotation.x;
    pose.Yaw = rotation.y;
    return pose;
}

void DX12SceneRenderer::NodeGraphUiHost::SetCamera(const NodeGraphCameraPose& pose)
{
    mOwner.mCamera.SetPosition(
        static_cast<float>(pose.Position[0]), static_cast<float>(pose.Position[1]), static_cast<float>(pose.Position[2]));
    mOwner.mCamera.SetRotation(static_cast<float>(pose.Pitch), static_cast<float>(pose.Yaw));
}

void DX12SceneRenderer::NodeGraphUiHost::PlayOneShot(const std::string& eventName)
{
    if (mOwner.mAudioManager != nullptr && !eventName.empty())
        mOwner.mAudioManager->PlayOneShotByName(eventName);
}

bool DX12SceneRenderer::NodeGraphUiHost::PlayMusic(const std::string& eventName)
{
    return mOwner.mAudioManager != nullptr && !eventName.empty() && mOwner.mAudioManager->PlayMusicByName(eventName);
}

void DX12SceneRenderer::NodeGraphUiHost::StopMusic()
{
    if (mOwner.mAudioManager != nullptr)
        mOwner.mAudioManager->StopMusic();
}

bool DX12SceneRenderer::NodeGraphUiHost::IsMusicPlaying()
{
    return mOwner.mAudioManager != nullptr && mOwner.mAudioManager->IsMusicPlaying();
}

bool DX12SceneRenderer::NodeGraphUiHost::IsKeyDown(int virtualKey)
{
    return QtUi::GameWindowHasFocus() && (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool DX12SceneRenderer::NodeGraphUiHost::PollUiClick(std::string& elementId)
{
    return mUiRenderer != nullptr && mUiRenderer->PollGraphClick(elementId);
}

void DX12SceneRenderer::NodeGraphUiHost::ToggleFullscreen()
{
    QtUi::ToggleGameFullscreen();
}

bool DX12SceneRenderer::NodeGraphUiHost::IsStandalone()
{
    return QtUi::IsStandaloneGame();
}
