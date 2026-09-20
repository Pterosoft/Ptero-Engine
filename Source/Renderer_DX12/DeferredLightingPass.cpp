#include "pch.h"
#include "DeferredLightingPass.h"

#include "VolumetricFogSettings.h"

#include "d3dx12.h"

#include <cmath>
#include <cstring>

// Extern C helpers shared across the renderer.
extern "C"
{
    ID3D12Device*         __stdcall DX12Context_GetDevice();
    bool                  __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// ---------------------------------------------------------------------------
// G-Buffer format table
// ---------------------------------------------------------------------------
// RT0 Albedo   – RGBA8 UNORM  (base colour + padding alpha)
// RT1 Normal   – RGBA32 FLOAT (encoded world-space normal + device depth)
// RT2 Material – RGBA8 UNORM  (roughness / metallic / AO)
//
// RT1 is 32-bit per channel because of what shares it. The oct-encoded normal
// in .xy would be perfectly happy at 16-bit, but GBuffer.hlsl also writes the
// post-projection device depth into .z, and fp16 cannot carry that.
//
// A perspective depth buffer concentrates almost all of its range just below
// 1.0 - with this engine's near and far planes an entire interior lands between
// roughly 0.98 and 1.0 - while fp16's spacing near 1.0 is about 0.000488. That
// left only a few dozen representable depths for a whole room. Every pass that
// reconstructs world position from this target (RTGI ray generation, the NRD
// prepare pass that derives viewZ and motion vectors, spatial reuse, specular)
// therefore snapped its positions to a handful of depth planes, and surfaces
// that rounded to exactly 1.0 were misread as sky. Under camera motion those
// values dithered between buckets and the reconstructed positions jumped,
// which is what produced flickering black and over-bright patches in the GI.
//
// The deferred lighting resolve was immune because it samples the real D32
// depth buffer instead, which is why the fault looked like an RTGI-only bug.
static constexpr DXGI_FORMAT kGBufferFormats[3] =
{
    DXGI_FORMAT_R8G8B8A8_UNORM,     // albedo
    DXGI_FORMAT_R32G32B32A32_FLOAT, // normal + depth
    DXGI_FORMAT_R8G8B8A8_UNORM,     // material
};
static constexpr UINT kGBufferCount = 3;

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool DeferredLightingPass::Initialize(
    UINT        width,
    UINT        height,
    DXGI_FORMAT depthFormat,
    const MsaaSettings& msaaSettings)
{
    mLastError.clear();

    // Validate and store MSAA settings
    mMsaaSettings = msaaSettings;
    mMsaaSettings.Validate();

    if (!CreateGBufferResources(width, height))
        return false;

    if (!CreateConstantBuffers())
        return false;

    // Pipeline creation is deferred until ResolveLight() is first called so we
    // know the scene colour format.  Mark as initialized even before that.
    mIsInitialized = true;
    return true;
}

bool DeferredLightingPass::EnsureSize(UINT width, UINT height, const MsaaSettings& msaaSettings)
{
    MsaaSettings requestedSettings = msaaSettings;
    requestedSettings.Validate();

    // Check if MSAA settings changed
    const bool msaaChanged =
        requestedSettings.Enabled != mMsaaSettings.Enabled ||
        requestedSettings.GetEffectiveSampleCount() != mMsaaSettings.GetEffectiveSampleCount() ||
        requestedSettings.GetEffectiveQuality() != mMsaaSettings.GetEffectiveQuality();

    if (width == mWidth && height == mHeight && !msaaChanged)
        return true;

    // Update MSAA settings
    mMsaaSettings = requestedSettings;

    return CreateGBufferResources(width, height);
}

void DeferredLightingPass::BeginGeometryPass(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDsvHandle,
    UINT                        width,
    UINT                        height) const
{
    // Choose MSAA or single-sample resources based on settings
    const auto* resources = mMsaaSettings.Enabled ? mMsaaGBufferResources : mGBufferResources;
    const auto* rtvHandles = mMsaaSettings.Enabled ? mMsaaRtvHandles : mRtvHandles;

    // Transition all three G-Buffer RTs from SRV/RESOLVE_SOURCE -> RTV.
    D3D12_RESOURCE_BARRIER barriers[kGBufferCount];
    for (UINT i = 0; i < kGBufferCount; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            resources[i].Get(),
            mMsaaSettings.Enabled ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                  : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    commandList->ResourceBarrier(kGBufferCount, barriers);

    // Clear all three G-Buffer RTs to zero/identity.
    const float clearBlack[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // The normal RT stores oct-encoded normal XY plus copied scene depth in Z.
    // Clear depth to 1.0 for untouched sky pixels so RTGI can reject them safely.
    const float clearNormal[] = { 0.5f, 0.5f, 1.0f, 0.0f };
    commandList->ClearRenderTargetView(rtvHandles[0], clearBlack,  0, nullptr);
    commandList->ClearRenderTargetView(rtvHandles[1], clearNormal, 0, nullptr);
    commandList->ClearRenderTargetView(rtvHandles[2], clearBlack,  0, nullptr);

    // Bind all three MRT outputs together with the shared depth buffer.
    commandList->OMSetRenderTargets(kGBufferCount, rtvHandles, FALSE, &sceneDsvHandle);

    // Set viewport / scissor to the current scene dimensions.
    const D3D12_VIEWPORT vp = { 0,0, static_cast<float>(width), static_cast<float>(height), 0,1 };
    const D3D12_RECT     sr = { 0,0, static_cast<LONG>(width),  static_cast<LONG>(height) };
    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sr);
}

void DeferredLightingPass::EndGeometryPass(
    ID3D12GraphicsCommandList* commandList) const
{
    // If MSAA is enabled, transition MSAA buffers to RESOLVE_SOURCE.
    // Otherwise, transition single-sample buffers to PSR for lighting.
    if (mMsaaSettings.Enabled)
    {
        D3D12_RESOURCE_BARRIER barriers[kGBufferCount];
        for (UINT i = 0; i < kGBufferCount; ++i)
        {
            barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mMsaaGBufferResources[i].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        }
        commandList->ResourceBarrier(kGBufferCount, barriers);
    }
    else
    {
        D3D12_RESOURCE_BARRIER barriers[kGBufferCount];
        for (UINT i = 0; i < kGBufferCount; ++i)
        {
            barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                mGBufferResources[i].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        commandList->ResourceBarrier(kGBufferCount, barriers);
    }
}

void DeferredLightingPass::ResolveGBuffer(ID3D12GraphicsCommandList* commandList)
{
    if (!mMsaaSettings.Enabled)
        return; // No resolve needed for single-sample rendering

    // Transition single-sample buffers to RESOLVE_DEST
    D3D12_RESOURCE_BARRIER preResolveBarriers[kGBufferCount];
    for (UINT i = 0; i < kGBufferCount; ++i)
    {
        preResolveBarriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mGBufferResources[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RESOLVE_DEST);
    }
    commandList->ResourceBarrier(kGBufferCount, preResolveBarriers);

    // Resolve each MSAA G-Buffer RT to its single-sample counterpart
    for (UINT i = 0; i < kGBufferCount; ++i)
    {
        commandList->ResolveSubresource(
            mGBufferResources[i].Get(),      // Dest (single-sample)
            0,                                // DestSubresource
            mMsaaGBufferResources[i].Get(),  // Source (MSAA)
            0,                                // SourceSubresource
            kGBufferFormats[i]);             // Format
    }

    // Transition single-sample buffers to PSR for lighting pass
    D3D12_RESOURCE_BARRIER postResolveBarriers[kGBufferCount];
    for (UINT i = 0; i < kGBufferCount; ++i)
    {
        postResolveBarriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mGBufferResources[i].Get(),
            D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    commandList->ResourceBarrier(kGBufferCount, postResolveBarriers);
}

void DeferredLightingPass::SetSceneLighting(
    const XMFLOAT3& sunDirection,
    const XMFLOAT3& sunColor,
    const XMFLOAT3& skyAmbient)
{
    if (!mMappedLightingCB) return;

    LightingConstants lc{};
    std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
    lc.SunDirection = sunDirection;
    lc.SunColor     = sunColor;
    lc.SkyAmbient   = skyAmbient;
    std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
}

void DeferredLightingPass::SetPointLights(const PointLightGpu* lights, int count)
{
    if (!mMappedLightingCB || !lights || count < 0) return;

    const int n = count < kMaxPointLights ? count : kMaxPointLights;

    LightingConstants lc{};
    std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
    lc.NumPointLights = n;
    for (int i = 0; i < n; ++i)
        lc.PointLights[i] = lights[i];
    std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
}

void DeferredLightingPass::SetCameraData(
    const XMFLOAT4X4& invViewProj,
    const XMFLOAT3&   cameraPosition)
{
    if (!mMappedCameraCB) return;

    CameraConstants cc{};
    cc.InvViewProj = invViewProj;
    cc.CameraPos   = cameraPosition;
    std::memcpy(mMappedCameraCB, &cc, sizeof(cc));
}

void DeferredLightingPass::SetProbeSrv(
    D3D12_GPU_DESCRIPTOR_HANDLE   probeSrvHandle,
    const RadianceProbeSettings*  settings,
    const XMFLOAT3&               cameraPosition)
{
    mProbeSrvHandle = probeSrvHandle;

    if (!mMappedProbeCB)
        return;

    ProbeConstants pc{};
    if (probeSrvHandle.ptr != 0 && settings != nullptr)
    {
        pc.ProbeGridX = static_cast<uint32_t>((std::max)(settings->GridX, 0));
        pc.ProbeGridY = static_cast<uint32_t>((std::max)(settings->GridY, 0));
        pc.ProbeGridZ = static_cast<uint32_t>((std::max)(settings->GridZ, 0));
        pc.ProbeSpacing = settings->Spacing;

        float ox = 0.0f;
        float oy = 0.0f;
        float oz = 0.0f;
        ResolveProbeGridOrigin(*settings, cameraPosition.x, cameraPosition.y, cameraPosition.z, ox, oy, oz);

        pc.ProbeOriginX = ox;
        pc.ProbeOriginY = oy;
        pc.ProbeOriginZ = oz;
    }

    std::memcpy(mMappedProbeCB, &pc, sizeof(pc));
}

void DeferredLightingPass::SetGiSrv(
    D3D12_GPU_DESCRIPTOR_HANDLE giSrvHandle,
    float                       intensity)
{
    mGiSrvHandle  = giSrvHandle;
    mGiIntensity  = intensity;

    // Write intensity into the lighting constant buffer so the shader can read it.
    if (mMappedLightingCB)
    {
        LightingConstants lc{};
        std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
        lc.GiIntensity = intensity;
        std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
    }
}

void DeferredLightingPass::SetPointShadowSrv(
    D3D12_GPU_DESCRIPTOR_HANDLE pointShadowSrvHandle,
    int                         activeShadowLightCount,
    float                       shadowMapSize,
    float                       shadowBias)
{
    mPointShadowSrvHandle = pointShadowSrvHandle;
    mPointShadowLightCount = activeShadowLightCount;
    mPointShadowMapSize = shadowMapSize;
    mPointShadowBias = shadowBias;

    if (mMappedShadowCB)
    {
        ShadowConstants sc{};
        std::memcpy(&sc, mMappedShadowCB, sizeof(sc));
        sc.PointShadowMapSize = shadowMapSize;
        sc.PointShadowBias = shadowBias;
        std::memcpy(mMappedShadowCB, &sc, sizeof(sc));
    }
}

void DeferredLightingPass::SetPointShadowMatrices(
    const XMFLOAT4X4* faceViewProjections,
    int               activeShadowLightCount)
{
    if (!mMappedShadowCB)
        return;

    ShadowConstants sc{};
    std::memcpy(&sc, mMappedShadowCB, sizeof(sc));

    const int clampedLightCount = (std::max)(0, (std::min)(activeShadowLightCount, kMaxShadowCastingPointLights));
    const int matrixCount = clampedLightCount * kPointShadowFacesPerLight;
    for (int i = 0; i < matrixCount; ++i)
        sc.PointShadowFaceViewProj[i] = faceViewProjections[i];

    std::memcpy(mMappedShadowCB, &sc, sizeof(sc));
}

void DeferredLightingPass::SetPointShadowDebug(int debugView, int filterRadius, float seamBlendDistance, float normalOffset)
{
    if (!mMappedLightingCB)
        return;

    LightingConstants lc{};
    std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
    lc.PointShadowDebugView = debugView;
    lc.PointShadowFilterRadius = (std::max)(0, filterRadius);
    lc.PointShadowSeamBlendDistance = seamBlendDistance;
    lc.PointShadowNormalOffset = (std::max)(0.0f, normalOffset);
    std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
}

void DeferredLightingPass::SetRtgiDebugView(int debugView)
{
    if (mMappedLightingCB)
    {
        LightingConstants lc{};
        std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
        lc.RtgiDebugView = debugView;
        std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
    }
}

void DeferredLightingPass::SetSpecularSrv(
    D3D12_GPU_DESCRIPTOR_HANDLE specularSrvHandle,
    float                       intensity)
{
    mSpecularSrvHandle  = specularSrvHandle;
    mSpecularIntensity  = intensity;

    if (mMappedLightingCB)
    {
        LightingConstants lc{};
        std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
        lc.SpecularIntensity = intensity;
        std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
    }
}

void DeferredLightingPass::SetVolumetricFogSrv(
    D3D12_GPU_DESCRIPTOR_HANDLE fogSrvHandle,
    const VolumetricFogSettings* settings)
{
    mFogSrvHandle = fogSrvHandle;

    if (mMappedLightingCB)
    {
        LightingConstants lc{};
        std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
        lc.VolumetricFogEnabled = (settings != nullptr && fogSrvHandle.ptr != 0) ? 1 : 0;
        lc.FogStartDistance = settings != nullptr ? settings->StartDistance : 0.1f;
        lc.FogMaxDistance = settings != nullptr ? settings->MaxDistance : 100.0f;
        lc.FogDebugView = settings != nullptr ? settings->DebugView : 0;
        std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
    }
}

void DeferredLightingPass::SetAoSrv(
    D3D12_GPU_DESCRIPTOR_HANDLE aoSrvHandle,
    float                       intensity,
    int                         debugView)
{
    mAoSrvHandle = aoSrvHandle;
    mAoIntensity = intensity;

    if (mMappedLightingCB)
    {
        LightingConstants lc{};
        std::memcpy(&lc, mMappedLightingCB, sizeof(lc));
        lc.AoIntensity = intensity;
        lc.AoDebugView = debugView;
        std::memcpy(mMappedLightingCB, &lc, sizeof(lc));
    }
}

void DeferredLightingPass::SetShadowData(
    const XMFLOAT4X4&           lightViewProjection,
    float                       shadowMapSize,
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvHandle)
{
    mShadowSrvHandle = shadowSrvHandle;

    if (!mMappedShadowCB) return;

    ShadowConstants sc{};
    sc.LightViewProj = lightViewProjection;
    sc.ShadowMapSize = shadowMapSize;
    // Use a larger bias to avoid shadow acne on large-scale geometry.
    sc.ShadowBias    = 0.003f;
    sc.PointShadowMapSize = mPointShadowMapSize;
    sc.PointShadowBias = mPointShadowBias;
    std::memcpy(mMappedShadowCB, &sc, sizeof(sc));
}

void DeferredLightingPass::ResolveLight(
    ID3D12GraphicsCommandList*  commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtvHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvHandle,
    DXGI_FORMAT                 sceneColorFormat,
    UINT                        width,
    UINT                        height) const
{
    // Lazily compile and create the lighting pipeline on first call.
    if (!mPipelineReady)
    {
        // Const cast to allow lazy init from a const Render call; safe because
        // state only changes once (pipeline creation).
        const_cast<DeferredLightingPass*>(this)->CreateLightingPipeline(sceneColorFormat);
    }
    if (!mPipelineReady) return;

    // Bind the scene colour RT.
    commandList->OMSetRenderTargets(1, &sceneRtvHandle, FALSE, nullptr);

    const D3D12_VIEWPORT vp = { 0,0, static_cast<float>(width), static_cast<float>(height), 0,1 };
    const D3D12_RECT     sr = { 0,0, static_cast<LONG>(width),  static_cast<LONG>(height) };
    commandList->RSSetViewports(1, &vp);
    commandList->RSSetScissorRects(1, &sr);

    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(mPipelineState.Get());

    // Root slots (see CreateLightingPipeline):
    //   0 – CBV camera constants                (b0)
    //   1 – CBV lighting constants              (b1)
    //   2 – CBV shadow constants                (b2)
    //   3 – CBV probe constants                 (b3)
    //   4 – SRV table: albedo(t0), normal(t1), material(t2)
    //   5 – SRV table: depth(t3)
    //   6 – SRV table: shadow map(t4)
    //   7 – SRV table: GI accumulation (t5)
    //   8 – SRV table: RTAO (t6)
    //   9 – SRV table: volumetric fog (t7)
    //  10 – SRV table: specular reflections (t8)
    //  11 – SRV table: point shadow map array (t9)
    //  12 – SRV table: radiance probes (t10)
    if (mCameraCB)
        commandList->SetGraphicsRootConstantBufferView(0, mCameraCB->GetGPUVirtualAddress());
    if (mLightingCB)
        commandList->SetGraphicsRootConstantBufferView(1, mLightingCB->GetGPUVirtualAddress());
    if (mShadowCB)
        commandList->SetGraphicsRootConstantBufferView(2, mShadowCB->GetGPUVirtualAddress());
    if (mProbeCB)
        commandList->SetGraphicsRootConstantBufferView(3, mProbeCB->GetGPUVirtualAddress());

    // G-buffer SRVs (t0-t2): albedo, normal, material – contiguous since allocated in a loop.
    if (mSrvs.Albedo.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(4, mSrvs.Albedo);

    // Scene depth SRV (t3) – passed in by DX12SceneRenderer from its depth SRV allocation.
    if (sceneDepthSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(5, sceneDepthSrvHandle);

    // Shadow map SRV (t4) – comes from ShadowMapRenderer.
    if (mShadowSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(6, mShadowSrvHandle);

    // GI accumulation SRV (t5) – only bound when GI is active this frame.
    if (mGiSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(7, mGiSrvHandle);

    // RTAO SRV (t6) – only bound when RTAO is active this frame.
    if (mAoSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(8, mAoSrvHandle);

    if (mFogSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(9, mFogSrvHandle);

    // Specular reflections SRV (t8) – only bound when specular is active.
    if (mSpecularSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(10, mSpecularSrvHandle);

    if (mPointShadowSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(11, mPointShadowSrvHandle);

    if (mProbeSrvHandle.ptr != 0)
        commandList->SetGraphicsRootDescriptorTable(12, mProbeSrvHandle);

    // Draw a fullscreen triangle (3 vertices, no vertex buffer needed).
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);
    commandList->DrawInstanced(3, 1, 0, 0);
}

void DeferredLightingPass::Shutdown()
{
    if (mCameraCB && mMappedCameraCB)
    {
        mCameraCB->Unmap(0, nullptr);
        mMappedCameraCB = nullptr;
    }
    if (mLightingCB && mMappedLightingCB)
    {
        mLightingCB->Unmap(0, nullptr);
        mMappedLightingCB = nullptr;
    }
    if (mShadowCB && mMappedShadowCB)
    {
        mShadowCB->Unmap(0, nullptr);
        mMappedShadowCB = nullptr;
    }
    if (mProbeCB && mMappedProbeCB)
    {
        mProbeCB->Unmap(0, nullptr);
        mMappedProbeCB = nullptr;
    }
    mCameraCB.Reset();
    mLightingCB.Reset();
    mShadowCB.Reset();
    mProbeCB.Reset();

    // Release MSAA G-Buffer resources
    for (UINT i = 0; i < kGBufferCount; ++i)
        mMsaaGBufferResources[i].Reset();
    mMsaaRtvHeap.Reset();

    // Release single-sample G-Buffer resources
    for (UINT i = 0; i < kGBufferCount; ++i)
        mGBufferResources[i].Reset();
    mRtvHeap.Reset();
    mRetiredGBufferResources.clear();
    mRetiredGBufferDescriptorHeaps.clear();

    mRootSignature.Reset();
    mPipelineState.Reset();
    mVertexShader = DX12Shader{};
    mPixelShader  = DX12Shader{};
    mIsInitialized = false;
    mPipelineReady = false;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool DeferredLightingPass::CreateGBufferResources(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) { mLastError = "DeferredLightingPass: device is null."; return false; }

    // Keep old resources alive while the GPU may still reference the previous
    // frame, especially when toggling MSAA without a full GPU idle wait.
    for (UINT i = 0; i < kGBufferCount; ++i)
    {
        if (mMsaaGBufferResources[i]) mRetiredGBufferResources.push_back(mMsaaGBufferResources[i]);
        if (mGBufferResources[i]) mRetiredGBufferResources.push_back(mGBufferResources[i]);
        mMsaaGBufferResources[i].Reset();
        mGBufferResources[i].Reset();
    }
    if (mMsaaRtvHeap) mRetiredGBufferDescriptorHeaps.push_back(mMsaaRtvHeap);
    if (mRtvHeap) mRetiredGBufferDescriptorHeaps.push_back(mRtvHeap);
    mMsaaRtvHeap.Reset();
    mRtvHeap.Reset();

    // ========================================================================
    // Create MSAA G-Buffer resources if MSAA is enabled
    // ========================================================================
    if (mMsaaSettings.Enabled)
    {
        // Create MSAA RTV heap
        D3D12_DESCRIPTOR_HEAP_DESC msaaRtvHeapDesc{};
        msaaRtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        msaaRtvHeapDesc.NumDescriptors = kGBufferCount;
        msaaRtvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&msaaRtvHeapDesc, IID_PPV_ARGS(&mMsaaRtvHeap))))
        {
            mLastError = "DeferredLightingPass: failed to create MSAA RTV heap.";
            return false;
        }

        const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE msaaRtvBase = mMsaaRtvHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_HEAP_PROPERTIES defaultHeap{};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Create MSAA render targets
        for (UINT i = 0; i < kGBufferCount; ++i)
        {
            D3D12_RESOURCE_DESC texDesc{};
            texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width              = width;
            texDesc.Height             = height;
            texDesc.DepthOrArraySize   = 1;
            texDesc.MipLevels          = 1;
            texDesc.Format             = kGBufferFormats[i];
            texDesc.SampleDesc.Count   = mMsaaSettings.GetEffectiveSampleCount();
            texDesc.SampleDesc.Quality = mMsaaSettings.GetEffectiveQuality();
            texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            float clearColor[4] = {};
            if (i == 1) { clearColor[0] = clearColor[1] = clearColor[2] = 0.5f; }
            D3D12_CLEAR_VALUE clearVal{ kGBufferFormats[i], {} };
            std::memcpy(clearVal.Color, clearColor, sizeof(clearColor));

            if (FAILED(device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE,
                &texDesc, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                &clearVal, IID_PPV_ARGS(&mMsaaGBufferResources[i]))))
            {
                mLastError = "DeferredLightingPass: failed to create MSAA G-Buffer texture.";
                return false;
            }

            const wchar_t* names[] = { L"GBuffer_MSAA_Albedo", L"GBuffer_MSAA_Normal", L"GBuffer_MSAA_Material" };
            mMsaaGBufferResources[i]->SetName(names[i]);

            // Create MSAA RTV
            mMsaaRtvHandles[i].ptr = msaaRtvBase.ptr + static_cast<SIZE_T>(i) * rtvSize;
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format        = kGBufferFormats[i];
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;  // MSAA view
            device->CreateRenderTargetView(mMsaaGBufferResources[i].Get(), &rtvDesc, mMsaaRtvHandles[i]);
        }
    }

    // ========================================================================
    // Create single-sample G-Buffer resources (always created)
    // These are resolve targets if MSAA enabled, or direct render targets otherwise
    // ========================================================================

    // Create a private RTV heap for the three G-Buffer targets.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kGBufferCount;
    rtvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap))))
    {
        mLastError = "DeferredLightingPass: failed to create RTV heap.";
        return false;
    }

    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = mRtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (UINT i = 0; i < kGBufferCount; ++i)
    {
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width              = width;
        texDesc.Height             = height;
        texDesc.DepthOrArraySize   = 1;
        texDesc.MipLevels          = 1;
        texDesc.Format             = kGBufferFormats[i];
        texDesc.SampleDesc.Count   = 1;
        texDesc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        // Clear value must match the clear colour in BeginGeometryPass.
        float clearColor[4] = {};
        if (i == 1) { clearColor[0] = clearColor[1] = clearColor[2] = 0.5f; } // normal
        D3D12_CLEAR_VALUE clearVal{ kGBufferFormats[i], {} };
        std::memcpy(clearVal.Color, clearColor, sizeof(clearColor));

        if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE,
            &texDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearVal, IID_PPV_ARGS(&mGBufferResources[i]))))
        {
            mLastError = "DeferredLightingPass: failed to create G-Buffer texture.";
            return false;
        }

        const wchar_t* names[] = { L"GBuffer_Albedo", L"GBuffer_Normal", L"GBuffer_Material" };
        mGBufferResources[i]->SetName(names[i]);

        // RTV.
        mRtvHandles[i].ptr = rtvBase.ptr + static_cast<SIZE_T>(i) * rtvSize;
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format        = kGBufferFormats[i];
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(mGBufferResources[i].Get(), &rtvDesc, mRtvHandles[i]);

        // SRV – allocate the descriptor slot once, then re-write it on resize.
        if (mSrvCpuHandles[i].ptr == 0)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv{};
            if (!DX12Context_AllocateSrvDescriptor(&cpuSrv, &gpuSrv))
            {
                mLastError = "DeferredLightingPass: SRV heap full.";
                return false;
            }
            mSrvCpuHandles[i] = cpuSrv;
            // Store GPU handles once – they don't change after allocation.
            if (i == 0) mSrvs.Albedo   = gpuSrv;
            if (i == 1) mSrvs.Normal   = gpuSrv;
            if (i == 2) mSrvs.Material = gpuSrv;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        // Force alpha to 1 for debug display so float targets written with A=0
        // do not appear black/transparent in the Ui texture viewer.
        srvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
            D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
            D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
        srvDesc.Format                  = kGBufferFormats[i];
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels     = 1;
        // Re-write the SRV to point at the (possibly new) resource.
        device->CreateShaderResourceView(mGBufferResources[i].Get(), &srvDesc, mSrvCpuHandles[i]);
    }

    mWidth  = width;
    mHeight = height;
    return true;
}

bool DeferredLightingPass::CreateConstantBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type             = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask  = 1;

    auto makeCB = [&](UINT64 size,
                      ComPtr<ID3D12Resource>& outCB,
                      void** outMapped,
                      const wchar_t* name) -> bool
    {
     const UINT64 alignedSize = (size + 255ull) & ~255ull;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
       desc.Width            = alignedSize;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outCB))))
            return false;
        outCB->SetName(name);
        return SUCCEEDED(outCB->Map(0, nullptr, outMapped));
    };

    if (!makeCB(sizeof(CameraConstants),  mCameraCB,  reinterpret_cast<void**>(&mMappedCameraCB),  L"DeferredLighting_CameraCB"))  return false;
    if (!makeCB(sizeof(LightingConstants), mLightingCB, reinterpret_cast<void**>(&mMappedLightingCB), L"DeferredLighting_LightingCB")) return false;
    if (!makeCB(sizeof(ShadowConstants),  mShadowCB,  reinterpret_cast<void**>(&mMappedShadowCB),  L"DeferredLighting_ShadowCB"))  return false;
    if (!makeCB(sizeof(ProbeConstants),   mProbeCB,   reinterpret_cast<void**>(&mMappedProbeCB),   L"DeferredLighting_ProbeCB"))   return false;
    return true;
}

bool DeferredLightingPass::CreateLightingPipeline(DXGI_FORMAT sceneColorFormat)
{
    mPipelineReady = false;

    ID3D12Device* device = DX12Context_GetDevice();
    if (!device) return false;

    const ShaderCompileRequest vsReq{ L"Shaders\\DeferredLighting.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
    const ShaderCompileRequest psReq{ L"Shaders\\DeferredLighting.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel  };

    if (!mVertexShader.Compile(vsReq))
    {
        mLastError = std::string("DeferredLighting VS: ")
            + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
        return false;
    }
    if (!mPixelShader.Compile(psReq))
    {
        mLastError = std::string("DeferredLighting PS: ")
            + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
        return false;
    }

    // Root signature:
    //   slot 0 – CBV (b0) camera constants       – pixel/vertex
    //   slot 1 – CBV (b1) scene lighting          – pixel
    //   slot 2 – CBV (b2) shadow constants        – pixel
    //   slot 3 – CBV (b3) probe constants         – pixel
    //   slot 4 – Descriptor table: G-Buffer SRVs  t0,t1,t2  (albedo,normal,material)
    //   slot 5 – Descriptor table: depth SRV      t3
    //   slot 6 – Descriptor table: shadow map SRV t4
    //   slot 7 – Descriptor table: GI SRV         t5  (null-safe: only bound when GI is active)
    //   slot 8 – Descriptor table: RTAO SRV       t6  (null-safe: only bound when RTAO is active)
    //   slot 9 – Descriptor table: volumetric fog t7
    //  slot 10 – Descriptor table: specular SRV   t8  (null-safe: only bound when specular is active)
    //  slot 11 – Descriptor table: point shadow map array t9
    //  slot 12 – Descriptor table: radiance probes t10
    D3D12_ROOT_PARAMETER rootParams[13]{};

    rootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0; // b0
    rootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1; // b1
    rootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 2; // b2
    rootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[3].Descriptor.ShaderRegister = 3; // b3
    rootParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // G-Buffer SRVs: t0..t2 (albedo, normal, material) – contiguous since allocated in a loop.
    D3D12_DESCRIPTOR_RANGE gbufRange{};
    gbufRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gbufRange.NumDescriptors                    = 3; // albedo(t0), normal(t1), material(t2)
    gbufRange.BaseShaderRegister                = 0; // t0
    gbufRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges   = &gbufRange;
    rootParams[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Depth SRV: t3 – separate table so it can come from a different heap slot.
    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors                    = 1;
    depthRange.BaseShaderRegister                = 3; // t3
    depthRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[5].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[5].DescriptorTable.pDescriptorRanges   = &depthRange;
    rootParams[5].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Shadow map SRV: t4 – separate table.
    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors                    = 1;
    shadowRange.BaseShaderRegister                = 4; // t4
    shadowRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[6].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[6].DescriptorTable.pDescriptorRanges   = &shadowRange;
    rootParams[6].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // GI accumulation texture SRV: t5 – separate table; only bound when GI is active.
    D3D12_DESCRIPTOR_RANGE giRange{};
    giRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    giRange.NumDescriptors                    = 1;
    giRange.BaseShaderRegister                = 5; // t5
    giRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[7].DescriptorTable.pDescriptorRanges   = &giRange;
    rootParams[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // RTAO texture SRV: t6 – separate table; only bound when RTAO is active.
    D3D12_DESCRIPTOR_RANGE aoRange{};
    aoRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    aoRange.NumDescriptors                    = 1;
    aoRange.BaseShaderRegister                = 6; // t6
    aoRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[8].DescriptorTable.pDescriptorRanges   = &aoRange;
    rootParams[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE fogRange{};
    fogRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    fogRange.NumDescriptors                    = 1;
    fogRange.BaseShaderRegister                = 7; // t7
    fogRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[9].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[9].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[9].DescriptorTable.pDescriptorRanges   = &fogRange;
    rootParams[9].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Specular reflection SRV: t8 – separate table; only bound when specular is active.
    D3D12_DESCRIPTOR_RANGE specRange{};
    specRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    specRange.NumDescriptors                    = 1;
    specRange.BaseShaderRegister                = 8; // t8
    specRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[10].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[10].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[10].DescriptorTable.pDescriptorRanges   = &specRange;
    rootParams[10].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE pointShadowRange{};
    pointShadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    pointShadowRange.NumDescriptors                    = 1;
    pointShadowRange.BaseShaderRegister                = 9; // t9
    pointShadowRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[11].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[11].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[11].DescriptorTable.pDescriptorRanges   = &pointShadowRange;
    rootParams[11].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_DESCRIPTOR_RANGE probeRange{};
    probeRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    probeRange.NumDescriptors                    = 1;
    probeRange.BaseShaderRegister                = 10; // t10
    probeRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[12].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[12].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[12].DescriptorTable.pDescriptorRanges   = &probeRange;
    rootParams[12].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers.
    D3D12_STATIC_SAMPLER_DESC pointSampler{};
    pointSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
    pointSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    pointSampler.MaxAnisotropy    = 1;
    pointSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    pointSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    pointSampler.ShaderRegister   = 0; // s0
    pointSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MaxAnisotropy    = 1;
    shadowSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister   = 1; // s1
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC linearSampler{};
    linearSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearSampler.MaxAnisotropy    = 1;
    linearSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    linearSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    linearSampler.ShaderRegister   = 2; // s2
    linearSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    const D3D12_STATIC_SAMPLER_DESC samplers[] = { pointSampler, shadowSampler, linearSampler };

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters     = static_cast<UINT>(std::size(rootParams));
    rsDesc.pParameters       = rootParams;
    rsDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    rsDesc.pStaticSamplers   = samplers;
    rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE; // no IA input layout

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "DeferredLighting: D3D12SerializeRootSignature failed.";
        return false;
    }
    if (FAILED(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "DeferredLighting: CreateRootSignature failed.";
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = mRootSignature.Get();
    psoDesc.VS                    = mVertexShader.GetBytecode();
    psoDesc.PS                    = mPixelShader.GetBytecode();
    psoDesc.SampleMask            = UINT_MAX;
    psoDesc.NumRenderTargets      = 1;
    psoDesc.RTVFormats[0]         = sceneColorFormat;
    psoDesc.DSVFormat             = DXGI_FORMAT_UNKNOWN; // no depth writes in lighting pass
    psoDesc.SampleDesc.Count      = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // Alpha-blend: geometry pixels return alpha=1 (fully overwrite), sky pixels return
    // alpha=0 (preserve the sky colour already in the render target).
    D3D12_RENDER_TARGET_BLEND_DESC rtBlend{};
    rtBlend.BlendEnable           = TRUE;
    rtBlend.LogicOpEnable         = FALSE;
    rtBlend.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rtBlend.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rtBlend.BlendOp               = D3D12_BLEND_OP_ADD;
    rtBlend.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rtBlend.DestBlendAlpha        = D3D12_BLEND_ZERO;
    rtBlend.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = rtBlend;

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // fullscreen tri, no culling

    // Disable depth testing for the fullscreen lighting quad.
    psoDesc.DepthStencilState.DepthEnable    = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;

    // No input layout – vertex shader generates positions from SV_VertexID.
    psoDesc.InputLayout = { nullptr, 0 };

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "DeferredLighting: CreateGraphicsPipelineState failed.";
        return false;
    }

    mPipelineReady = true;
    return true;
}
