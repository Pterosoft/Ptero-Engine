#include "pch.h"
#include "ParticleRenderer.h"

#include "LightStyles.h"

#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12CommandQueue* __stdcall DX12Context_GetCommandQueue();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    UINT __stdcall DX12Context_GetSrvDescriptorSize();
}

namespace
{
    // Scene colour and depth formats the particle PSOs must match.
    constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT kSceneDepthFormat = DXGI_FORMAT_D32_FLOAT;

    // Normalisation for the proxy light, matching the point-light convention in
    // DX12SceneRenderer: 800 lm (a 60 W-equivalent bulb) is brightness 1.
    constexpr float kReferenceLumens = 800.0f;

    std::wstring GetShaderDirectory()
    {
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::wstring executablePath(modulePath);
        const auto slash = executablePath.find_last_of(L"\\/");
        std::wstring directory = (slash != std::wstring::npos) ? executablePath.substr(0, slash + 1) : L"";

        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const std::wstring candidate = directory + L"Data\\Shaders\\";
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                return candidate;
            const auto up = directory.find_last_of(L"\\/", directory.size() - 2);
            if (up == std::wstring::npos)
                break;
            directory = directory.substr(0, up + 1);
        }

        return L"Data\\Shaders\\";
    }

    // Resolves a Data-relative asset path ("Materials/Fire.json") to an absolute
    // path by walking up from the executable looking for a Data directory.
    std::filesystem::path ResolveDataRelativePath(const std::string& relativePath)
    {
        if (relativePath.empty())
            return {};

        std::filesystem::path candidate(relativePath);
        if (candidate.is_absolute() && std::filesystem::exists(candidate))
            return candidate;

        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::filesystem::path directory = std::filesystem::path(modulePath).parent_path();

        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const std::filesystem::path dataCandidate = directory / L"Data" / relativePath;
            std::error_code error;
            if (std::filesystem::exists(dataCandidate, error))
                return dataCandidate;

            const std::filesystem::path parent = directory.parent_path();
            if (parent == directory)
                break;
            directory = parent;
        }

        return {};
    }

    ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, UINT64 size)
    {
        ComPtr<ID3D12Resource> resource;
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
        device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&resource));
        return resource;
    }

    uint32_t HashCombine(uint32_t seed, uint32_t value)
    {
        return seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2));
    }

    uint32_t HashFloat(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // Particles the buffer must hold for these settings: the spawn rate times
    // the longest a particle can live, so the ring never has to recycle a slot
    // whose occupant is still alive.
    uint32_t ComputeRequiredCapacity(const ParticleSystemComponent& settings)
    {
        const float maximumLifetime = settings.Lifetime * (1.0f + settings.LifetimeVariance);

        float required;
        if (settings.Burst)
        {
            // A burst emitter needs room for every burst that can be in the air
            // at once, plus one for the burst about to fire.
            const float burstsAlive = std::ceil(maximumLifetime / (std::max)(settings.BurstInterval, 0.01f)) + 1.0f;
            required = burstsAlive * static_cast<float>(settings.BurstCount);
        }
        else
        {
            required = settings.SpawnRate * maximumLifetime;
        }

        // One extra so the ring cursor never lands on the particle it is about
        // to overwrite in the same frame it was spawned.
        const long rounded = std::lround(std::ceil(required)) + 1;
        return static_cast<uint32_t>(std::clamp<long>(rounded, 1, settings.MaxParticles));
    }

    // Settings that invalidate live particles when they change. Positions,
    // velocities and lifetimes are baked in at spawn, so changing the shape or
    // the lifetime with a full buffer of particles in flight would otherwise
    // leave the old ones visibly inconsistent with the new ones for a second.
    uint64_t ComputeShapeSignature(const ParticleSystemComponent& settings)
    {
        uint32_t hash = 0x811c9dc5u;
        hash = HashCombine(hash, static_cast<uint32_t>(settings.Shape));
        hash = HashCombine(hash, HashFloat(settings.ShapeRadius));
        hash = HashCombine(hash, HashFloat(settings.ConeAngleDegrees));
        hash = HashCombine(hash, HashFloat(settings.ShapeExtents.x));
        hash = HashCombine(hash, HashFloat(settings.ShapeExtents.y));
        hash = HashCombine(hash, HashFloat(settings.ShapeExtents.z));
        hash = HashCombine(hash, HashFloat(settings.ShapeShellBias));
        hash = HashCombine(hash, HashFloat(settings.Lifetime));
        hash = HashCombine(hash, HashFloat(settings.LifetimeVariance));
        hash = HashCombine(hash, HashFloat(settings.InitialSpeed));
        hash = HashCombine(hash, HashFloat(settings.SpeedVariance));
        return hash;
    }

    // Average of the colour gradient weighted the way the eye sees the plume:
    // the bright early part of a flame dominates what it lights the room with,
    // so the start and middle keys count for more than the cold tail.
    XMFLOAT3 ComputeAverageParticleColor(const ParticleSystemComponent& settings)
    {
        const float weightStart = 0.45f;
        const float weightMid = 0.40f;
        const float weightEnd = 0.15f;

        // Weight each key by its own alpha too: a key that has faded out is not
        // contributing light, however saturated its colour still is.
        const float a0 = weightStart * (std::max)(settings.ColorStart.w, 0.0f);
        const float a1 = weightMid * (std::max)(settings.ColorMid.w, 0.0f);
        const float a2 = weightEnd * (std::max)(settings.ColorEnd.w, 0.0f);
        const float total = a0 + a1 + a2;

        if (total <= 1.0e-5f)
            return XMFLOAT3(1.0f, 1.0f, 1.0f);

        return XMFLOAT3(
            (settings.ColorStart.x * a0 + settings.ColorMid.x * a1 + settings.ColorEnd.x * a2) / total,
            (settings.ColorStart.y * a0 + settings.ColorMid.y * a1 + settings.ColorEnd.y * a2) / total,
            (settings.ColorStart.z * a0 + settings.ColorMid.z * a1 + settings.ColorEnd.z * a2) / total);
    }
}

// ---------------------------------------------------------------------------
// Initialize / Shutdown
// ---------------------------------------------------------------------------

bool ParticleRenderer::Initialize()
{
    if (mIsInitialized)
        return true;

    mLastError.clear();

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "ParticleRenderer: no DX12 device.";
        return false;
    }

    mDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kParticleMaxSystems;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mUavHeap))))
    {
        mLastError = "ParticleRenderer: failed to create the UAV descriptor heap.";
        return false;
    }
    mUavHeap->SetName(L"ParticleUavHeap");

    if (!CreateConstantBuffers())  return false;
    if (!CreateFallbackTexture())  return false;
    if (!CreateComputePipeline())  return false;
    if (!CreateDrawPipelines())    return false;

    mIsInitialized = true;
    return true;
}

void ParticleRenderer::Shutdown()
{
    if (mUpdateCbPtr) { mUpdateCb->Unmap(0, nullptr); mUpdateCbPtr = nullptr; }
    if (mDrawCbPtr)   { mDrawCb->Unmap(0, nullptr);   mDrawCbPtr = nullptr; }

    mUpdateCb.Reset();
    mDrawCb.Reset();

    mStates.clear();
    mSystems.clear();
    mProxyLights.clear();
    mDrawOrder.clear();
    mMaterialCache.clear();
    mSpriteCache.clear();
    mTextureManager.Shutdown();

    mFallbackTexture.Reset();
    mFallbackTextureSrv = {};

    mUavHeap.Reset();
    mUpdateRootSig.Reset();
    mUpdatePso.Reset();
    mDrawRootSig.Reset();
    mDrawPsoAdditive.Reset();
    mDrawPsoAlphaBlend.Reset();
    mDrawPsoPremultiplied.Reset();
    mDrawPsoAdditiveNoDepth.Reset();
    mDrawPsoAlphaBlendNoDepth.Reset();
    mDrawPsoPremultipliedNoDepth.Reset();

    mUavHeapSlotsUsed = 0;
    mFrameSlot = 0;
    mGlobalTime = 0.0f;
    mIsInitialized = false;
}

void ParticleRenderer::ResetSimulation()
{
    for (SystemState& state : mStates)
    {
        state.NeedsPrewarm = true;
        state.SpawnCursor = 0;
        state.SpawnAccumulator = 0.0f;
        state.BurstTimer = 0.0f;
        state.SimulationTime = 0.0f;
    }

    mGlobalTime = 0.0f;
}

uint32_t ParticleRenderer::GetActiveParticleCount() const
{
    uint32_t total = 0;
    for (const SystemState& state : mStates)
        total += state.ActiveCount;
    return total;
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

bool ParticleRenderer::CreateConstantBuffers()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    const UINT64 updateSize = static_cast<UINT64>(sizeof(UpdateConstants)) * kParticleMaxSystems * kFramesInFlight;
    const UINT64 drawSize = static_cast<UINT64>(sizeof(DrawConstants)) * kParticleMaxSystems * kFramesInFlight;

    mUpdateCb = CreateUploadBuffer(device, updateSize);
    mDrawCb = CreateUploadBuffer(device, drawSize);
    if (!mUpdateCb || !mDrawCb)
    {
        mLastError = "ParticleRenderer: failed to create constant buffers.";
        return false;
    }

    mUpdateCb->SetName(L"ParticleUpdateCB");
    mDrawCb->SetName(L"ParticleDrawCB");

    if (FAILED(mUpdateCb->Map(0, nullptr, reinterpret_cast<void**>(&mUpdateCbPtr))) ||
        FAILED(mDrawCb->Map(0, nullptr, reinterpret_cast<void**>(&mDrawCbPtr))))
    {
        mLastError = "ParticleRenderer: failed to map constant buffers.";
        return false;
    }

    return true;
}

bool ParticleRenderer::CreateFallbackTexture()
{
    ID3D12Device* device = DX12Context_GetDevice();
    ID3D12CommandQueue* queue = DX12Context_GetCommandQueue();
    if (device == nullptr || queue == nullptr)
    {
        mLastError = "ParticleRenderer: no device or command queue for the fallback texture.";
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = 1;
    textureDesc.Height = 1;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&mFallbackTexture))))
    {
        mLastError = "ParticleRenderer: failed to create the fallback texture.";
        return false;
    }
    mFallbackTexture->SetName(L"ParticleFallbackTexture");

    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

    ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(device, uploadSize);
    if (!uploadBuffer)
    {
        mLastError = "ParticleRenderer: failed to create the fallback texture upload buffer.";
        return false;
    }

    const uint32_t whitePixel = 0xFFFFFFFFu;
    D3D12_SUBRESOURCE_DATA subresource{};
    subresource.pData = &whitePixel;
    subresource.RowPitch = sizeof(whitePixel);
    subresource.SlicePitch = sizeof(whitePixel);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList))))
    {
        mLastError = "ParticleRenderer: failed to create an upload command list.";
        return false;
    }

    UpdateSubresources(commandList.Get(), mFallbackTexture.Get(), uploadBuffer.Get(), 0, 0, 1, &subresource);

    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        mFallbackTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);
    commandList->Close();

    ID3D12CommandList* lists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;

    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent != nullptr)
    {
        queue->Signal(fence.Get(), 1);
        fence->SetEventOnCompletion(1, fenceEvent);
        WaitForSingleObject(fenceEvent, 5000);
        CloseHandle(fenceEvent);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle{};
    if (!DX12Context_AllocateSrvDescriptor(&srvCpuHandle, &mFallbackTextureSrv))
    {
        mLastError = "ParticleRenderer: failed to allocate the fallback texture descriptor.";
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mFallbackTexture.Get(), &srvDesc, srvCpuHandle);

    return true;
}

bool ParticleRenderer::CreateComputePipeline()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Root signature: [0] CBV b0, [1] UAV table u0.
    {
        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &uavRange;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
        rootSignatureDesc.NumParameters = 2;
        rootSignatureDesc.pParameters = parameters;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> blob, errorBlob;
        if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob)))
        {
            mLastError = "ParticleRenderer: failed to serialize the compute root signature.";
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&mUpdateRootSig))))
        {
            mLastError = "ParticleRenderer: failed to create the compute root signature.";
            return false;
        }
        mUpdateRootSig->SetName(L"ParticleUpdateRootSig");
    }

    DX12Shader computeShader;
    ShaderCompileRequest request{};
    request.FilePath = GetShaderDirectory() + L"Particle_Update.hlsl";
    request.EntryPoint = L"CSMain";
    // cs_5_0 matches what the startup shader cache guesses for this file, so
    // the warmed entry is reused instead of compiled again here.
    request.TargetProfile = L"cs_5_0";
    request.Stage = ShaderStage::Compute;
    if (!computeShader.Compile(request))
    {
        mLastError = std::string("ParticleRenderer: Particle_Update.hlsl failed to compile: ") +
            (computeShader.GetLastErrorMessage() ? computeShader.GetLastErrorMessage() : "unknown error");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mUpdateRootSig.Get();
    psoDesc.CS = computeShader.GetBytecode();
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mUpdatePso))))
    {
        mLastError = "ParticleRenderer: failed to create the compute pipeline state.";
        return false;
    }
    mUpdatePso->SetName(L"ParticleUpdatePSO");

    return true;
}

bool ParticleRenderer::CreateDrawPipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Root signature: [0] CBV b0, [1] SRV t0 (particles), [2] SRV t1 (sprite),
    // [3] SRV t2 (scene depth), static sampler s0.
    {
        D3D12_DESCRIPTOR_RANGE ranges[3]{};
        for (UINT i = 0; i < 3; ++i)
        {
            ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors = 1;
            ranges[i].BaseShaderRegister = i;
            ranges[i].RegisterSpace = 0;
            ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        }

        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        for (UINT i = 0; i < 3; ++i)
        {
            parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i + 1].DescriptorTable.NumDescriptorRanges = 1;
            parameters[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
            parameters[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
        rootSignatureDesc.NumParameters = 4;
        rootSignatureDesc.pParameters = parameters;
        rootSignatureDesc.NumStaticSamplers = 1;
        rootSignatureDesc.pStaticSamplers = &sampler;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> blob, errorBlob;
        if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob)))
        {
            mLastError = "ParticleRenderer: failed to serialize the draw root signature.";
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&mDrawRootSig))))
        {
            mLastError = "ParticleRenderer: failed to create the draw root signature.";
            return false;
        }
        mDrawRootSig->SetName(L"ParticleDrawRootSig");
    }

    DX12Shader vertexShader;
    DX12Shader pixelShader;
    {
        ShaderCompileRequest request{};
        request.FilePath = GetShaderDirectory() + L"Particle_Draw.hlsl";
        request.EntryPoint = L"VSMain";
        request.TargetProfile = L"vs_5_0";
        request.Stage = ShaderStage::Vertex;
        if (!vertexShader.Compile(request))
        {
            mLastError = std::string("ParticleRenderer: Particle_Draw.hlsl VSMain failed to compile: ") +
                (vertexShader.GetLastErrorMessage() ? vertexShader.GetLastErrorMessage() : "unknown error");
            return false;
        }
    }
    {
        ShaderCompileRequest request{};
        request.FilePath = GetShaderDirectory() + L"Particle_Draw.hlsl";
        request.EntryPoint = L"PSMain";
        request.TargetProfile = L"ps_5_0";
        request.Stage = ShaderStage::Pixel;
        if (!pixelShader.Compile(request))
        {
            mLastError = std::string("ParticleRenderer: Particle_Draw.hlsl PSMain failed to compile: ") +
                (pixelShader.GetLastErrorMessage() ? pixelShader.GetLastErrorMessage() : "unknown error");
            return false;
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mDrawRootSig.Get();
    psoDesc.VS = vertexShader.GetBytecode();
    psoDesc.PS = pixelShader.GetBytecode();

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // Depth tested so particles are occluded by geometry, but never written:
    // sprites are transparent, and a depth write would make whichever one drew
    // first reject the ones behind it.
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = kSceneColorFormat;
    psoDesc.DSVFormat = kSceneDepthFormat;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;

    const auto createBlendPso = [&](
        D3D12_BLEND sourceBlend,
        D3D12_BLEND destinationBlend,
        bool noDepth,
        ComPtr<ID3D12PipelineState>& outPso,
        const wchar_t* debugName) -> bool
    {
        D3D12_RENDER_TARGET_BLEND_DESC blendDesc{};
        blendDesc.BlendEnable = TRUE;
        blendDesc.SrcBlend = sourceBlend;
        blendDesc.DestBlend = destinationBlend;
        blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.BlendState.RenderTarget[0] = blendDesc;

        // With no depth test there is no depth-stencil view either, so the
        // format has to be UNKNOWN or pipeline creation is rejected.
        psoDesc.DepthStencilState.DepthEnable = noDepth ? FALSE : TRUE;
        psoDesc.DSVFormat = noDepth ? DXGI_FORMAT_UNKNOWN : kSceneDepthFormat;

        if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&outPso))))
        {
            mLastError = "ParticleRenderer: failed to create a draw pipeline state.";
            return false;
        }
        outPso->SetName(debugName);
        return true;
    };

    if (!createBlendPso(D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_ONE, false, mDrawPsoAdditive, L"ParticleDrawPSO_Additive"))
        return false;
    if (!createBlendPso(D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, false, mDrawPsoAlphaBlend, L"ParticleDrawPSO_AlphaBlend"))
        return false;
    if (!createBlendPso(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, false, mDrawPsoPremultiplied, L"ParticleDrawPSO_Premultiplied"))
        return false;

    if (!createBlendPso(D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_ONE, true, mDrawPsoAdditiveNoDepth, L"ParticleDrawPSO_Additive_NoDepth"))
        return false;
    if (!createBlendPso(D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, true, mDrawPsoAlphaBlendNoDepth, L"ParticleDrawPSO_AlphaBlend_NoDepth"))
        return false;
    if (!createBlendPso(D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, true, mDrawPsoPremultipliedNoDepth, L"ParticleDrawPSO_Premultiplied_NoDepth"))
        return false;

    return true;
}

ID3D12PipelineState* ParticleRenderer::GetPipelineForBlend(ParticleBlendMode blend, bool noDepth) const
{
    switch (blend)
    {
    case ParticleBlendMode::AlphaBlend:
        return noDepth ? mDrawPsoAlphaBlendNoDepth.Get() : mDrawPsoAlphaBlend.Get();
    case ParticleBlendMode::Premultiplied:
        return noDepth ? mDrawPsoPremultipliedNoDepth.Get() : mDrawPsoPremultiplied.Get();
    case ParticleBlendMode::Additive:
    default:
        return noDepth ? mDrawPsoAdditiveNoDepth.Get() : mDrawPsoAdditive.Get();
    }
}

// ---------------------------------------------------------------------------
// Per-system buffers
// ---------------------------------------------------------------------------

bool ParticleRenderer::EnsureSystemBuffer(SystemState& state, uint32_t capacity, const std::string& debugName)
{
    if (state.ParticleBuffer && state.Capacity >= capacity)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Grow in powers of two so a slider being dragged across a range of spawn
    // rates does not reallocate on every frame of the drag.
    uint32_t newCapacity = (std::max)(capacity, 256u);
    newCapacity = static_cast<uint32_t>(std::pow(2.0, std::ceil(std::log2(static_cast<double>(newCapacity)))));
    newCapacity = (std::min)(newCapacity, static_cast<uint32_t>(kParticleMaxPerSystem));
    newCapacity = (std::max)(newCapacity, capacity);

    ComPtr<ID3D12Resource> buffer;
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(
        static_cast<UINT64>(newCapacity) * sizeof(GpuParticle),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    if (FAILED(device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&buffer))))
    {
        mLastError = "ParticleRenderer: failed to create a particle buffer.";
        return false;
    }

    const std::wstring resourceName = L"ParticleBuffer_" + std::wstring(debugName.begin(), debugName.end());
    buffer->SetName(resourceName.c_str());

    state.ParticleBuffer = buffer;
    state.Capacity = newCapacity;
    state.BufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // A fresh buffer holds garbage, so the first dispatch must clear it before
    // anything reads Life.
    state.NeedsClear = true;

    // Allocate the descriptor pair once per system slot and then reuse it: the
    // shared SRV heap is a bump allocator, so allocating per resize would leak
    // a slot on every change.
    if (!state.DescriptorsAllocated)
    {
        if (mUavHeapSlotsUsed >= kParticleMaxSystems)
        {
            mLastError = "ParticleRenderer: ran out of particle system descriptor slots.";
            return false;
        }
        state.UavHeapSlot = mUavHeapSlotsUsed++;

        if (!DX12Context_AllocateSrvDescriptor(&state.SrvCpuHandle, &state.SrvGpuHandle))
        {
            mLastError = "ParticleRenderer: failed to allocate a shared SRV descriptor.";
            return false;
        }
        state.DescriptorsAllocated = true;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = newCapacity;
    uavDesc.Buffer.StructureByteStride = sizeof(GpuParticle);
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE uavCpuHandle = mUavHeap->GetCPUDescriptorHandleForHeapStart();
    uavCpuHandle.ptr += static_cast<SIZE_T>(state.UavHeapSlot) * mDescriptorSize;
    device->CreateUnorderedAccessView(buffer.Get(), nullptr, &uavDesc, uavCpuHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = newCapacity;
    srvDesc.Buffer.StructureByteStride = sizeof(GpuParticle);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(buffer.Get(), &srvDesc, state.SrvCpuHandle);

    return true;
}

// ---------------------------------------------------------------------------
// Material and texture resolution
// ---------------------------------------------------------------------------

const ParticleRenderer::ResolvedMaterial& ParticleRenderer::ResolveMaterial(const std::string& dataRelativePath)
{
    static const ResolvedMaterial kDefaultMaterial{};

    if (dataRelativePath.empty())
        return kDefaultMaterial;

    const std::filesystem::path absolutePath = ResolveDataRelativePath(dataRelativePath);

    std::error_code error;
    const bool exists = !absolutePath.empty() && std::filesystem::exists(absolutePath, error);
    std::filesystem::file_time_type writeTime{};
    if (exists)
    {
        writeTime = std::filesystem::last_write_time(absolutePath, error);
        if (error)
            writeTime = {};
    }

    auto cached = mMaterialCache.find(dataRelativePath);
    if (cached != mMaterialCache.end() &&
        cached->second.FileExists == exists &&
        cached->second.LastWriteTime == writeTime)
    {
        return cached->second.Material;
    }

    CachedMaterial entry;
    entry.FileExists = exists;
    entry.LastWriteTime = writeTime;

    if (exists && mMaterialLoader.LoadMaterialFromFile(absolutePath))
    {
        const MaterialDefinition& definition = mMaterialLoader.GetCurrentMaterial();

        entry.Material.BaseColorTint = XMFLOAT4(
            definition.BaseColorTint[0],
            definition.BaseColorTint[1],
            definition.BaseColorTint[2],
            definition.BaseColorTint[3]);
        entry.Material.EmissiveColor = XMFLOAT3(
            definition.EmissiveColor[0],
            definition.EmissiveColor[1],
            definition.EmissiveColor[2]);
        entry.Material.EmissiveIntensity = definition.ParticleEmissiveIntensity;
        entry.Material.Blend = definition.ParticleBlend;
        entry.Material.LightingInfluence = definition.ParticleLightingInfluence;
        entry.Material.SphericalNormal = definition.ParticleSphericalNormal;
        entry.Material.CameraFadeDistance = definition.ParticleCameraFadeDistance;
        entry.Material.FlipbookColumns = (std::max)(definition.ParticleFlipbookColumns, 1);
        entry.Material.FlipbookRows = (std::max)(definition.ParticleFlipbookRows, 1);
        entry.Material.AlphaFromLuminance = definition.ParticleAlphaFromLuminance;
        entry.Material.Loaded = true;

        if (!definition.Textures.BaseColorTexturePath.empty())
        {
            const std::filesystem::path texturePath =
                ResolveDataRelativePath(definition.Textures.BaseColorTexturePath);
            if (!texturePath.empty())
                entry.Material.SpriteTexturePath = texturePath.string();
        }

        // A material that was authored for surfaces but assigned to an emitter
        // still has to render as something. Falling back to a white emissive
        // makes the mistake obvious in the viewport instead of producing an
        // invisible system the artist has to debug.
        if (!definition.IsParticleMaterial &&
            entry.Material.EmissiveColor.x <= 0.0f &&
            entry.Material.EmissiveColor.y <= 0.0f &&
            entry.Material.EmissiveColor.z <= 0.0f)
        {
            entry.Material.EmissiveColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
        }
    }

    auto inserted = mMaterialCache.insert_or_assign(dataRelativePath, std::move(entry));
    return inserted.first->second.Material;
}

bool ParticleRenderer::ResolveSpriteTexture(const std::string& absolutePath, D3D12_GPU_DESCRIPTOR_HANDLE& outHandle)
{
    outHandle = {};
    if (absolutePath.empty())
        return false;

    auto cached = mSpriteCache.find(absolutePath);
    if (cached != mSpriteCache.end())
    {
        outHandle = cached->second.GpuHandle;
        return cached->second.Valid;
    }

    SpriteTexture texture;

    const std::filesystem::path path(absolutePath);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (extension == ".dds")
    {
        // DDS goes through the shared TextureManager so a sprite sheet that is
        // also used as a surface texture is only uploaded once.
        if (std::shared_ptr<GpuTexture> loaded = mTextureManager.LoadDDS(absolutePath, TextureSemantic::Color))
        {
            texture.Resource = loaded->Resource;
            texture.GpuHandle = loaded->GpuHandle;
            texture.Valid = loaded->IsValid();
        }
    }
    else
    {
        // Anything else - PNG, TGA, JPG, BMP - goes through WIC, so an artist's
        // flipbook works as authored without a conversion step first.
        LoadTextureWithWic(absolutePath, texture);
    }

    auto inserted = mSpriteCache.insert_or_assign(absolutePath, std::move(texture));
    outHandle = inserted.first->second.GpuHandle;
    return inserted.first->second.Valid;
}

bool ParticleRenderer::LoadTextureWithWic(const std::string& absolutePath, SpriteTexture& outTexture)
{
    ID3D12Device* device = DX12Context_GetDevice();
    ID3D12CommandQueue* queue = DX12Context_GetCommandQueue();
    if (device == nullptr || queue == nullptr)
        return false;

    const std::filesystem::path path(absolutePath);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory)
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter)
        return false;

    if (FAILED(converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom)))
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return false;

    const UINT rowPitch = width * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(rowPitch) * height);
    if (FAILED(converter->CopyPixels(nullptr, rowPitch, static_cast<UINT>(pixels.size()), pixels.data())))
        return false;

    // Sprite sheets are authored in sRGB; sampling them as UNORM would make a
    // flame wash out as soon as the tonemapper saw it.
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&resource))))
    {
        return false;
    }

    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);

    ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(device, uploadSize);
    if (!uploadBuffer)
        return false;

    D3D12_SUBRESOURCE_DATA subresource{};
    subresource.pData = pixels.data();
    subresource.RowPitch = rowPitch;
    subresource.SlicePitch = static_cast<LONG_PTR>(rowPitch) * height;

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList))))
    {
        return false;
    }

    UpdateSubresources(commandList.Get(), resource.Get(), uploadBuffer.Get(), 0, 0, 1, &subresource);

    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &barrier);
    commandList->Close();

    ID3D12CommandList* lists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, lists);

    // The upload buffer has to outlive the copy, so block here. This runs once
    // per unique sprite on first use, not per frame.
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return false;

    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent == nullptr)
        return false;

    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fenceEvent);
    WaitForSingleObject(fenceEvent, 5000);
    CloseHandle(fenceEvent);

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle{};
    if (!DX12Context_AllocateSrvDescriptor(&srvCpuHandle, &srvGpuHandle))
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource.Get(), &srvDesc, srvCpuHandle);

    outTexture.Resource = resource;
    outTexture.GpuHandle = srvGpuHandle;
    outTexture.Valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame system sync
// ---------------------------------------------------------------------------

void ParticleRenderer::SetSystems(const std::vector<ParticleSystemInstance>& systems, float deltaSeconds)
{
    if (!mIsInitialized)
        return;

    mGlobalTime += (std::max)(deltaSeconds, 0.0f);

    mSystems = systems;
    if (mSystems.size() > static_cast<size_t>(kParticleMaxSystems))
        mSystems.resize(kParticleMaxSystems);

    // Grow but never shrink the state array: a system that is toggled off and
    // back on, or a level that momentarily reports fewer systems mid-edit,
    // keeps its buffers and its simulation rather than restarting.
    if (mStates.size() < mSystems.size())
        mStates.resize(mSystems.size());

    for (size_t i = 0; i < mSystems.size(); ++i)
    {
        const ParticleSystemComponent& settings = mSystems[i].Settings;
        SystemState& state = mStates[i];

        state.Material = ResolveMaterial(settings.MaterialPath);
        state.HasSprite = ResolveSpriteTexture(state.Material.SpriteTexturePath, state.SpriteSrv);

        const uint32_t required = ComputeRequiredCapacity(settings);
        if (!EnsureSystemBuffer(state, required, mSystems[i].DebugName))
        {
            state.ActiveCount = 0;
            continue;
        }
        state.ActiveCount = (std::min)(required, state.Capacity);

        // The ring shrinks when the spawn rate or lifetime is lowered. A cursor
        // left past the new end would make the shader's wrap arithmetic
        // underflow and open a spawn window over the whole buffer.
        if (state.SpawnCursor >= state.ActiveCount)
            state.SpawnCursor = 0;

        const uint64_t signature = ComputeShapeSignature(settings);
        if (signature != state.ShapeSignature)
        {
            state.ShapeSignature = signature;
            state.NeedsPrewarm = true;
        }

        if (state.RandomSeed == 0)
        {
            // Derived from the slot rather than from a clock, so two runs of the
            // same level produce the same fire.
            state.RandomSeed = static_cast<uint32_t>(i) * 2654435761u + 0x9e3779b9u;
        }
    }

    RecomputeProxyLights();

    // Draw order: furthest first, so alpha-blended systems composite correctly
    // against each other. Distance is measured at SetSystems time from the
    // emitter origin, which is stable enough - a system is small next to the
    // distance between two of them.
    mDrawOrder.clear();
    mDrawOrder.reserve(mSystems.size());
    for (int i = 0; i < static_cast<int>(mSystems.size()); ++i)
        mDrawOrder.push_back(i);
}

void ParticleRenderer::RecomputeProxyLights()
{
    mProxyLights.clear();
    mProxyLights.reserve(mSystems.size());

    for (size_t i = 0; i < mSystems.size(); ++i)
    {
        const ParticleSystemComponent& settings = mSystems[i].Settings;
        if (!settings.Enabled || !settings.EmitLight || settings.LightIntensityLumens <= 0.0f)
            continue;

        static const ResolvedMaterial kDefaultMaterial{};
        const ResolvedMaterial& material = (i < mStates.size()) ? mStates[i].Material : kDefaultMaterial;

        // The proxy light's colour is built from exactly what the sprites are
        // drawn with, so tinting the fire tints the light it casts without the
        // artist having to keep two colours in sync by hand.
        XMFLOAT3 color;
        if (settings.UseParticleColorForLight)
        {
            const XMFLOAT3 particleColor = ComputeAverageParticleColor(settings);
            color = XMFLOAT3(
                particleColor.x * material.EmissiveColor.x * material.BaseColorTint.x,
                particleColor.y * material.EmissiveColor.y * material.BaseColorTint.y,
                particleColor.z * material.EmissiveColor.z * material.BaseColorTint.z);

            // Renormalise to the brightest channel. Without this, multiplying
            // three tints together dims the light every time the artist adjusts
            // one of them, and the lumen value stops meaning anything.
            const float peak = (std::max)((std::max)(color.x, color.y), color.z);
            if (peak > 1.0e-4f)
            {
                color.x /= peak;
                color.y /= peak;
                color.z /= peak;
            }
            else
            {
                color = XMFLOAT3(1.0f, 1.0f, 1.0f);
            }
        }
        else
        {
            color = XMFLOAT3(settings.LightColorR, settings.LightColorG, settings.LightColorB);
        }

        const float styleMultiplier = LightStyles::Evaluate(
            settings.LightStyle,
            mGlobalTime,
            settings.LightStyleSpeed,
            settings.LightStyleAmplitude,
            settings.LightStylePhaseOffset + static_cast<float>(i) * 3.77f,
            settings.LightCustomStylePattern);

        const float brightness = (settings.LightIntensityLumens / kReferenceLumens) * styleMultiplier;

        ParticleProxyLight light;
        light.Position = XMFLOAT3(
            mSystems[i].EmitterPosition.x,
            mSystems[i].EmitterPosition.y,
            mSystems[i].EmitterPosition.z + settings.LightHeightOffset);
        light.Radius = (std::max)(settings.LightRadius, 0.001f);
        light.InvRadiusSq = 1.0f / (light.Radius * light.Radius);
        light.Color = XMFLOAT3(color.x * brightness, color.y * brightness, color.z * brightness);
        light.CastShadows = settings.LightCastShadows;
        light.AffectVolumetricFog = settings.LightAffectVolumetricFog;
        light.GiContribution = settings.GiContribution;

        mProxyLights.push_back(light);
    }
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void ParticleRenderer::Dispatch(
    ID3D12GraphicsCommandList* cmdList,
    const XMFLOAT3&            windVelocity,
    float                      deltaSeconds)
{
    if (!mIsInitialized || cmdList == nullptr || mSystems.empty())
        return;

    // Advance the constant-buffer ring once per frame, before any system writes
    // into it.
    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;

    // A long frame (a level load, a breakpoint) would otherwise teleport every
    // particle across the level in one step.
    const float clampedDelta = std::clamp(deltaSeconds, 0.0f, 0.1f);

    ID3D12DescriptorHeap* heaps[] = { mUavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(mUpdateRootSig.Get());
    cmdList->SetPipelineState(mUpdatePso.Get());

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(mSystems.size());

    for (size_t i = 0; i < mSystems.size(); ++i)
    {
        const ParticleSystemInstance& instance = mSystems[i];
        const ParticleSystemComponent& settings = instance.Settings;
        SystemState& state = mStates[i];

        if (!state.ParticleBuffer || state.ActiveCount == 0)
            continue;

        if (!settings.Enabled)
        {
            // A disabled system keeps its buffer but stops simulating, and comes
            // back prewarmed rather than building up from empty.
            state.NeedsPrewarm = settings.Prewarm;
            continue;
        }

        state.SimulationTime += clampedDelta;

        uint32_t resetMode = 0;   // RESET_NONE
        uint32_t spawnCount = 0;

        if (state.NeedsClear && !state.NeedsPrewarm)
        {
            resetMode = 1;        // RESET_CLEAR
        }
        else if (state.NeedsPrewarm)
        {
            resetMode = settings.Prewarm ? 2u : 1u;   // RESET_PREWARM or RESET_CLEAR
        }
        else if (settings.Burst)
        {
            state.BurstTimer += clampedDelta;
            if (state.BurstTimer >= settings.BurstInterval)
            {
                state.BurstTimer -= settings.BurstInterval;
                spawnCount = static_cast<uint32_t>((std::max)(settings.BurstCount, 0));
            }
        }
        else
        {
            // Carry the fractional remainder between frames so a rate that is
            // not a whole number of particles per frame still averages exactly
            // right instead of quantising down to zero at high frame rates.
            state.SpawnAccumulator += settings.SpawnRate * clampedDelta;
            const float whole = std::floor(state.SpawnAccumulator);
            spawnCount = static_cast<uint32_t>((std::max)(whole, 0.0f));
            state.SpawnAccumulator -= whole;
        }

        // Never open a spawn window larger than the ring: that would have two
        // threads writing the same slot in one dispatch.
        spawnCount = (std::min)(spawnCount, state.ActiveCount);

        UpdateConstants constants{};
        XMStoreFloat4x4(&constants.EmitterToWorld, XMMatrixTranspose(instance.EmitterToWorld));
        constants.EmitterPosition = instance.EmitterPosition;
        constants.DeltaTime = clampedDelta;
        constants.Acceleration = settings.Acceleration;
        constants.GlobalTime = state.SimulationTime;
        constants.WindVelocity = windVelocity;
        constants.WindInfluence = settings.WindInfluence;
        constants.ShapeExtents = settings.ShapeExtents;
        constants.ShapeRadius = settings.ShapeRadius;
        constants.ConeAngleRadians = XMConvertToRadians(settings.ConeAngleDegrees);
        constants.ShapeShellBias = settings.ShapeShellBias;
        constants.ShapeType = static_cast<uint32_t>(settings.Shape);
        constants.ParticleCount = state.ActiveCount;
        constants.Lifetime = settings.Lifetime;
        constants.LifetimeVariance = settings.LifetimeVariance;
        constants.InitialSpeed = settings.InitialSpeed;
        constants.SpeedVariance = settings.SpeedVariance;
        constants.Drag = settings.Drag;
        constants.TurbulenceStrength = settings.TurbulenceStrength;
        constants.TurbulenceFrequency = settings.TurbulenceFrequency;
        constants.TurbulenceSpeed = settings.TurbulenceSpeed;
        constants.VortexStrength = settings.VortexStrength;
        constants.RotationRateRadians = XMConvertToRadians(settings.RotationSpeedDegrees);
        constants.RotationRateVariance = settings.RotationSpeedVariance;
        constants.StartRotationRadians = XMConvertToRadians(settings.StartRotationDegrees);
        constants.RandomStartRotation = settings.RandomStartRotation;
        constants.SizeVariance = settings.SizeVariance;
        constants.SpawnCursor = state.SpawnCursor;
        constants.SpawnCount = spawnCount;
        constants.RandomSeed = state.RandomSeed;
        constants.ResetMode = resetMode;
        constants.FlipbookRandomStart = settings.FlipbookRandomStartFrame ? 1u : 0u;
        constants.PrewarmSpan = settings.Lifetime;

        const size_t constantOffset =
            (static_cast<size_t>(mFrameSlot) * kParticleMaxSystems + i) * sizeof(UpdateConstants);
        std::memcpy(mUpdateCbPtr + constantOffset, &constants, sizeof(UpdateConstants));

        D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = mUavHeap->GetGPUDescriptorHandleForHeapStart();
        uavHandle.ptr += static_cast<UINT64>(state.UavHeapSlot) * mDescriptorSize;

        // The previous frame left this buffer readable by the pixel shader.
        if (state.BufferState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(
                state.ParticleBuffer.Get(),
                state.BufferState,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmdList->ResourceBarrier(1, &toUav);
            state.BufferState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        cmdList->SetComputeRootConstantBufferView(0, mUpdateCb->GetGPUVirtualAddress() + constantOffset);
        cmdList->SetComputeRootDescriptorTable(1, uavHandle);

        constexpr uint32_t kThreadGroupSize = 256;
        const UINT groups = (state.ActiveCount + kThreadGroupSize - 1) / kThreadGroupSize;
        cmdList->Dispatch(groups, 1, 1);

        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
            state.ParticleBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        state.BufferState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (resetMode != 0)
        {
            // A reset consumed this dispatch; emission starts next frame from a
            // clean ring.
            state.NeedsPrewarm = false;
            state.NeedsClear = false;
            state.SpawnCursor = 0;
            state.SpawnAccumulator = 0.0f;
            state.BurstTimer = 0.0f;
        }
        else
        {
            state.SpawnCursor = (state.SpawnCursor + spawnCount) % state.ActiveCount;
        }
    }

    if (!barriers.empty())
    {
        cmdList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void ParticleRenderer::Draw(ID3D12GraphicsCommandList* cmdList, const ParticleDrawContext& context)
{
    if (!mIsInitialized || cmdList == nullptr || mSystems.empty())
        return;

    // Sort back to front. Done here rather than in SetSystems so the order
    // tracks the camera even on a frame where the entity list did not change.
    std::sort(mDrawOrder.begin(), mDrawOrder.end(), [&](int left, int right)
    {
        const auto distanceSquared = [&](int index)
        {
            const XMFLOAT3& p = mSystems[index].EmitterPosition;
            const float dx = p.x - context.CameraPosition.x;
            const float dy = p.y - context.CameraPosition.y;
            const float dz = p.z - context.CameraPosition.z;
            return dx * dx + dy * dy + dz * dz;
        };
        return distanceSquared(left) > distanceSquared(right);
    });

    cmdList->SetGraphicsRootSignature(mDrawRootSig.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12PipelineState* currentPipeline = nullptr;

    for (const int index : mDrawOrder)
    {
        const ParticleSystemInstance& instance = mSystems[index];
        const ParticleSystemComponent& settings = instance.Settings;
        const SystemState& state = mStates[index];

        if (!settings.Enabled || !state.ParticleBuffer || state.ActiveCount == 0)
            continue;

        // Whole-system distance cull. Individual particles fade out near the
        // same distance in the vertex shader, so the system is already
        // invisible by the time it is dropped here.
        const float dx = instance.EmitterPosition.x - context.CameraPosition.x;
        const float dy = instance.EmitterPosition.y - context.CameraPosition.y;
        const float dz = instance.EmitterPosition.z - context.CameraPosition.z;
        if (dx * dx + dy * dy + dz * dz > settings.CullDistance * settings.CullDistance)
            continue;

        const ResolvedMaterial& material = state.Material;

        // The emitter's flipbook layout wins when it has been set; otherwise the
        // material's own layout applies, so a sprite sheet carries its frame
        // count with it and every emitter that uses it inherits the right one.
        int flipbookColumns = settings.FlipbookColumns;
        int flipbookRows = settings.FlipbookRows;
        if (flipbookColumns <= 1 && flipbookRows <= 1)
        {
            flipbookColumns = material.FlipbookColumns;
            flipbookRows = material.FlipbookRows;
        }

        // The same style curve that drives the proxy light can drive the
        // sprites, so the flame visibly brightens and dims with the light it is
        // casting rather than the two running independently.
        float emissiveStyle = 1.0f;
        if (settings.StyleDrivesParticleEmissive)
        {
            emissiveStyle = LightStyles::Evaluate(
                settings.LightStyle,
                mGlobalTime,
                settings.LightStyleSpeed,
                settings.LightStyleAmplitude,
                settings.LightStylePhaseOffset + static_cast<float>(index) * 3.77f,
                settings.LightCustomStylePattern);
        }

        DrawConstants constants{};
        constants.ViewProj = context.ViewProjection;
        constants.CameraPosition = context.CameraPosition;
        constants.ParticleCount = static_cast<float>(state.ActiveCount);
        constants.CameraRight = context.CameraRight;
        constants.StartSize = settings.StartSize;
        constants.CameraUp = context.CameraUp;
        constants.EndSize = settings.EndSize;
        constants.CameraForward = context.CameraForward;
        constants.StretchFactor = settings.StretchFactor;
        constants.ColorStart = settings.ColorStart;
        constants.ColorMid = settings.ColorMid;
        constants.ColorEnd = settings.ColorEnd;
        constants.EmissiveColor = material.EmissiveColor;
        constants.ColorMidPoint = settings.ColorMidPoint;
        constants.BaseColorTint = material.BaseColorTint;
        constants.EmissiveIntensity =
            settings.EmissiveIntensity * material.EmissiveIntensity * emissiveStyle;
        constants.FlipbookColumns = static_cast<float>((std::max)(flipbookColumns, 1));
        constants.FlipbookRows = static_cast<float>((std::max)(flipbookRows, 1));
        constants.FlipbookFps = settings.FlipbookFps;
        constants.FlipbookBlendFrames = settings.FlipbookBlendFrames ? 1.0f : 0.0f;
        constants.SoftFadeDistance = settings.SoftFadeDistance;
        constants.CameraFadeDistance = material.CameraFadeDistance;
        constants.UseSoftParticles =
            (settings.SoftParticles && context.SceneDepthSrv.ptr != 0) ? 1.0f : 0.0f;
        constants.NearPlane = context.NearPlane;
        constants.FarPlane = context.FarPlane;
        constants.ScreenWidth = context.ScreenWidth;
        constants.ScreenHeight = context.ScreenHeight;
        constants.LightingInfluence = material.LightingInfluence;
        constants.SphericalNormal = material.SphericalNormal;
        constants.CullDistance = settings.CullDistance;
        constants.FacingMode = static_cast<uint32_t>(settings.Facing);
        constants.SunDirection = context.SunDirection;
        constants.HasSpriteTexture = state.HasSprite ? 1.0f : 0.0f;
        constants.SunColor = context.SunColor;
        constants.NumLights = static_cast<float>(context.NumSceneLights);
        constants.SkyColor = context.SkyColor;
        constants.BlendModeIsPremultiplied =
            (material.Blend == ParticleBlendMode::Premultiplied) ? 1.0f : 0.0f;
        constants.AlphaFromLuminance = material.AlphaFromLuminance ? 1.0f : 0.0f;
        constants.ManualDepthTest =
            (context.ManualDepthTest && context.SceneDepthSrv.ptr != 0) ? 1.0f : 0.0f;

        for (int lightIndex = 0; lightIndex < context.NumSceneLights && lightIndex < ParticleDrawContext::kMaxSceneLights; ++lightIndex)
        {
            constants.Lights[lightIndex].Position = context.SceneLights[lightIndex].Position;
            constants.Lights[lightIndex].Radius = context.SceneLights[lightIndex].Radius;
            constants.Lights[lightIndex].Color = context.SceneLights[lightIndex].Color;
            constants.Lights[lightIndex].InvRadiusSq = context.SceneLights[lightIndex].InvRadiusSq;
        }

        const size_t constantOffset =
            (static_cast<size_t>(mFrameSlot) * kParticleMaxSystems + static_cast<size_t>(index)) * sizeof(DrawConstants);
        std::memcpy(mDrawCbPtr + constantOffset, &constants, sizeof(DrawConstants));

        ID3D12PipelineState* pipeline = GetPipelineForBlend(material.Blend, context.ManualDepthTest);
        if (pipeline != currentPipeline)
        {
            cmdList->SetPipelineState(pipeline);
            currentPipeline = pipeline;
        }

        cmdList->SetGraphicsRootConstantBufferView(0, mDrawCb->GetGPUVirtualAddress() + constantOffset);
        cmdList->SetGraphicsRootDescriptorTable(1, state.SrvGpuHandle);

        // Both texture slots must point at a texture of the declared type even
        // when the shader is going to gate them off, so an unused slot gets the
        // 1x1 fallback rather than a mismatched descriptor.
        cmdList->SetGraphicsRootDescriptorTable(
            2, state.HasSprite ? state.SpriteSrv : mFallbackTextureSrv);
        cmdList->SetGraphicsRootDescriptorTable(
            3, context.SceneDepthSrv.ptr != 0 ? context.SceneDepthSrv : mFallbackTextureSrv);

        cmdList->DrawInstanced(6, state.ActiveCount, 0, 0);
    }
}
