// SubsurfaceScattering.cpp
// See SubsurfaceScattering.h for the frame flow.
//
// Root signature (shared by the compute passes and the composite):
//   0  CBV   b0  SubsurfaceConstants
//   1  table t0  pass input      (diffuse, X-blurred scratch, or ray-traced result)
//   2  table t1  diffuse target  (alpha = subsurface mask)
//   3  table t2  G-Buffer normal + depth + packed W
//   4  table t3  G-Buffer albedo (ray traced only)
//   5  table t4  scene TLAS      (ray traced only)
//   6  table u0  scratch target
//   7  table t5  RTGI vertex pool         (ray traced only)
//   8  table t6  RTGI index pool          (ray traced only)
//   9  table t7  RTGI instance info       (ray traced only)
//  10  table t8  sun shadow map           (ray traced only)
//  11  table t9  point shadow map array    (ray traced only)
//  static s0     shadow comparison sampler

#include "pch.h"
#include "SubsurfaceScattering.h"
#include "DX12ShaderCompiler.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

namespace
{
    constexpr DXGI_FORMAT kLightingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr UINT kGroupSize = 8;

    const wchar_t* kShaderScreenSpace = L"Data/Shaders/SubsurfaceScattering.hlsl";
    const wchar_t* kShaderRayTraced   = L"Data/Shaders/SubsurfaceScattering_RT.hlsl";

    D3D12_RESOURCE_DESC MakeTexture2DDesc(UINT width, UINT height)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kLightingFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        return desc;
    }

    int KernelSampleCount(int quality)
    {
        switch (std::clamp(quality, 0, 2))
        {
        case 0:  return 11;
        case 2:  return 25;
        default: return 17;
        }
    }
}

// ---------------------------------------------------------------------------
// Kernel (port of SeparableSSS::gaussian / profile / calculateKernel)
// ---------------------------------------------------------------------------
namespace
{
    XMFLOAT3 SssGaussian(float variance, float r, const XMFLOAT3& falloff)
    {
        // Falloff stretches each channel's profile: larger spreads it wider.
        const float f[3] = { falloff.x, falloff.y, falloff.z };
        float g[3];
        for (int i = 0; i < 3; ++i)
        {
            const float rr = r / (0.001f + f[i]);
            g[i] = std::exp(-(rr * rr) / (2.0f * variance)) / (2.0f * 3.14f * variance);
        }
        return { g[0], g[1], g[2] };
    }

    XMFLOAT3 SssProfile(float r, const XMFLOAT3& falloff)
    {
        // The red channel of d'Eon's skin profile for all three channels, scaled by falloff.
        // The 0.233 * gaussian(0.0064) term is left out: the SDK treats it as directly
        // bounced light, which the strength parameter accounts for.
        const float weights[] = { 0.100f, 0.118f, 0.113f, 0.358f, 0.078f };
        const float variances[] = { 0.0484f, 0.187f, 0.567f, 1.99f, 7.41f };
        XMFLOAT3 sum{ 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < 5; ++i)
        {
            const XMFLOAT3 g = SssGaussian(variances[i], r, falloff);
            sum.x += weights[i] * g.x;
            sum.y += weights[i] * g.y;
            sum.z += weights[i] * g.z;
        }
        return sum;
    }
}

void BuildSeparableSssKernel(
    const XMFLOAT3& strength,
    const XMFLOAT3& falloff,
    int sampleCount,
    XMFLOAT4* outKernel)
{
    const int n = std::clamp(sampleCount, 3, SubsurfaceGpuConstants::kMaxKernelSamples);
    const float range = n > 20 ? 3.0f : 2.0f;
    const float exponent = 2.0f;

    std::vector<XMFLOAT4> kernel(static_cast<size_t>(n));

    // Offsets, denser toward the centre.
    const float step = 2.0f * range / static_cast<float>(n - 1);
    for (int i = 0; i < n; ++i)
    {
        const float o = -range + static_cast<float>(i) * step;
        const float sign = o < 0.0f ? -1.0f : 1.0f;
        kernel[i].w = range * sign * std::abs(std::pow(o, exponent)) / std::pow(range, exponent);
    }

    // Weights: the profile times the width each tap stands for.
    for (int i = 0; i < n; ++i)
    {
        const float w0 = i > 0 ? std::abs(kernel[i].w - kernel[i - 1].w) : 0.0f;
        const float w1 = i < n - 1 ? std::abs(kernel[i].w - kernel[i + 1].w) : 0.0f;
        const float area = (w0 + w1) / 2.0f;
        const XMFLOAT3 t = SssProfile(kernel[i].w, falloff);
        kernel[i].x = area * t.x;
        kernel[i].y = area * t.y;
        kernel[i].z = area * t.z;
    }

    // Centre tap first.
    const XMFLOAT4 centre = kernel[n / 2];
    for (int i = n / 2; i > 0; --i)
        kernel[i] = kernel[i - 1];
    kernel[0] = centre;

    // Normalise.
    XMFLOAT3 sum{ 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < n; ++i)
    {
        sum.x += kernel[i].x;
        sum.y += kernel[i].y;
        sum.z += kernel[i].z;
    }
    for (int i = 0; i < n; ++i)
    {
        kernel[i].x /= (std::max)(sum.x, 1e-12f);
        kernel[i].y /= (std::max)(sum.y, 1e-12f);
        kernel[i].z /= (std::max)(sum.z, 1e-12f);
    }

    // Blend toward identity by strength: centre = lerp(1, k0, s), others = k * s.
    kernel[0].x = (1.0f - strength.x) + strength.x * kernel[0].x;
    kernel[0].y = (1.0f - strength.y) + strength.y * kernel[0].y;
    kernel[0].z = (1.0f - strength.z) + strength.z * kernel[0].z;
    for (int i = 1; i < n; ++i)
    {
        kernel[i].x *= strength.x;
        kernel[i].y *= strength.y;
        kernel[i].z *= strength.z;
    }

    std::copy(kernel.begin(), kernel.end(), outKernel);
}

// ---------------------------------------------------------------------------
// Profile registry
// ---------------------------------------------------------------------------
namespace
{
    struct ProfileSlot
    {
        SubsurfaceProfileDesc Desc{};
        uint64_t LastUsedFrame = 0;
        bool InUse = false;
    };

    ProfileSlot gProfileSlots[SubsurfaceProfiles::kMaxProfiles];
    uint64_t gProfileFrame = 1;
    bool gProfileActiveThisFrame = false;
    bool gProfileOverflowLogged = false;

    bool SameDesc(const SubsurfaceProfileDesc& a, const SubsurfaceProfileDesc& b)
    {
        return a.Color.x == b.Color.x && a.Color.y == b.Color.y && a.Color.z == b.Color.z
            && a.Falloff.x == b.Falloff.x && a.Falloff.y == b.Falloff.y && a.Falloff.z == b.Falloff.z
            && a.RadiusMeters == b.RadiusMeters && a.Translucency == b.Translucency;
    }
}

namespace SubsurfaceProfiles
{
    void BeginFrame()
    {
        ++gProfileFrame;
        gProfileActiveThisFrame = false;
    }

    int Acquire(const SubsurfaceProfileDesc& desc)
    {
        // An identical profile already has a slot: share it.
        for (int slot = 1; slot < kMaxProfiles; ++slot)
        {
            ProfileSlot& entry = gProfileSlots[slot];
            if (entry.InUse && SameDesc(entry.Desc, desc))
            {
                entry.LastUsedFrame = gProfileFrame;
                gProfileActiveThisFrame = true;
                return slot;
            }
        }

        // Otherwise take a free slot, or the least recently used one not needed this
        // frame. Taking it over is safe: each frame uploads its own copy of the table.
        int best = 0;
        uint64_t bestFrame = gProfileFrame;
        for (int slot = 1; slot < kMaxProfiles; ++slot)
        {
            const ProfileSlot& entry = gProfileSlots[slot];
            if (!entry.InUse)
            {
                best = slot;
                break;
            }
            if (entry.LastUsedFrame < bestFrame)
            {
                best = slot;
                bestFrame = entry.LastUsedFrame;
            }
        }

        if (best == 0)
        {
            if (!gProfileOverflowLogged)
            {
                PTERO_LOG_WARNING("Renderer", "Subsurface scattering: more than 15 distinct subsurface profiles "
                                  "on screen; the rest render without scattering.");
                gProfileOverflowLogged = true;
            }
            return 0;
        }

        gProfileSlots[best].Desc = desc;
        gProfileSlots[best].InUse = true;
        gProfileSlots[best].LastUsedFrame = gProfileFrame;
        gProfileActiveThisFrame = true;
        return best;
    }

    bool AnyActiveThisFrame()
    {
        return gProfileActiveThisFrame;
    }
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
bool SubsurfaceScatteringRenderer::Initialize(UINT width, UINT height, DXGI_FORMAT sceneColorFormat)
{
    if (width == 0 || height == 0)
        return false;

    mLastError.clear();
    mSceneColorFormat = sceneColorFormat;

    try
    {
        ID3D12Device* device = DX12Context_GetDevice();
        if (!device)
        {
            mLastError = "SubsurfaceScattering: no D3D12 device.";
            mInitFailed = true;
            return false;
        }

        if (!mRootSignature && !CreateRootSignature())
        {
            mInitFailed = true;
            return false;
        }
        if (!mPsoBlurX && !CreatePipelines())
        {
            mInitFailed = true;
            return false;
        }

        if (!mConstantBuffer)
        {
            mConstantStride = (sizeof(SubsurfaceGpuConstants) + 255ull) & ~255ull;
            const CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
            const CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(mConstantStride * kFramesInFlight);
            DX12_THROW_IF_FAILED(device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mConstantBuffer)));
            mConstantBuffer->SetName(L"SSS_Constants");
            void* mapped = nullptr;
            DX12_THROW_IF_FAILED(mConstantBuffer->Map(0, nullptr, &mapped));
            mMappedConstants = static_cast<std::uint8_t*>(mapped);
        }

        if (!mRtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.NumDescriptors = 1;
            DX12_THROW_IF_FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));
            mDiffuseRtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
        }

        if (!mDescriptorsAllocated)
        {
            if (!DX12Context_AllocateSrvDescriptor(&mDiffuseSrvCpu, &mDiffuseSrvGpu)
                || !DX12Context_AllocateSrvDescriptor(&mScratchSrvCpu, &mScratchSrvGpu)
                || !DX12Context_AllocateSrvDescriptor(&mScratchUavCpu, &mScratchUavGpu))
            {
                mLastError = "SubsurfaceScattering: ran out of SRV descriptor heap slots.";
                mInitFailed = true;
                return false;
            }
            mDescriptorsAllocated = true;
        }

        mWidth = width;
        mHeight = height;
        if (!CreateSizeDependentResources())
        {
            mInitFailed = true;
            return false;
        }

        PTERO_LOG_INFO("Renderer", "Subsurface scattering initialised at %ux%u (ray-traced path %s).",
                       mWidth, mHeight, mPsoRayTraced ? "available" : "unavailable");
        mIsInitialized = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("SubsurfaceScattering::Initialize: ") + ex.what();
        mInitFailed = true;
        mIsInitialized = false;
        return false;
    }
}

bool SubsurfaceScatteringRenderer::EnsureSize(UINT width, UINT height)
{
    if (!mIsInitialized)
        return false;
    if (width == mWidth && height == mHeight)
        return true;
    if (width == 0 || height == 0)
        return false;

    try
    {
        RetireSizeDependentResources();
        mWidth = width;
        mHeight = height;
        if (!CreateSizeDependentResources())
        {
            mIsInitialized = false;
            mInitFailed = true;
            return false;
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        mLastError = std::string("SubsurfaceScattering::EnsureSize: ") + ex.what();
        mIsInitialized = false;
        mInitFailed = true;
        return false;
    }
}

void SubsurfaceScatteringRenderer::Shutdown()
{
    if (mConstantBuffer && mMappedConstants)
        mConstantBuffer->Unmap(0, nullptr);
    mMappedConstants = nullptr;
    mConstantBuffer.Reset();
    mDiffuseTarget.Reset();
    mScratchTarget.Reset();
    mRtvHeap.Reset();
    mRootSignature.Reset();
    mPsoBlurX.Reset();
    mPsoRayTraced.Reset();
    for (auto& pso : mPsoCompositeBlurY) pso.Reset();
    for (auto& pso : mPsoCompositeRayTraced) pso.Reset();
    mRetiredResources.clear();
    mIsInitialized = false;
    // The shared-heap descriptors are never freed; keep them for a later re-init.
}

bool SubsurfaceScatteringRenderer::CreateRootSignature()
{
    ID3D12Device* device = DX12Context_GetDevice();

    // Parameters 1-5: t0-t4, 6: u0, 7-11: t5-t9.
    D3D12_DESCRIPTOR_RANGE ranges[11]{};
    for (UINT i = 0; i < 5; ++i)
        ranges[i] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, 0 };
    ranges[5] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    for (UINT i = 0; i < 5; ++i)
        ranges[6 + i] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5 + i, 0, 0 };

    D3D12_ROOT_PARAMETER params[12]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 11; ++i)
    {
        params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable = { 1, &ranges[i] };
        params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // Same comparison sampler as the deferred lighting pass's gShadowSampler.
    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MaxAnisotropy    = 1;
    shadowSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister   = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = static_cast<UINT>(std::size(params));
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &shadowSampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        mLastError = "SubsurfaceScattering: D3D12SerializeRootSignature failed.";
        if (errors)
            mLastError += std::string(" ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&mRootSignature))))
    {
        mLastError = "SubsurfaceScattering: CreateRootSignature failed.";
        return false;
    }
    mRootSignature->SetName(L"SSS_RootSignature");
    return true;
}

bool SubsurfaceScatteringRenderer::CreatePipelines()
{
    ID3D12Device* device = DX12Context_GetDevice();

    auto compile = [this](const wchar_t* file, const wchar_t* entry, const wchar_t* target,
                          ShaderStage stage, DX12Shader& shader) -> bool
    {
        ShaderCompileRequest request{};
        request.FilePath = file;
        request.EntryPoint = entry;
        request.TargetProfile = target;
        request.Stage = stage;
        if (!shader.Compile(request))
        {
            mLastError = "SubsurfaceScattering: shader compile failed: "
                + std::string(shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "?");
            return false;
        }
        return true;
    };

    // Screen-space horizontal blur.
    {
        DX12Shader shader;
        if (!compile(kShaderScreenSpace, L"CSBlurX", L"cs_6_0", ShaderStage::Compute, shader))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = mRootSignature.Get();
        desc.CS = shader.GetBytecode();
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&mPsoBlurX))))
        {
            mLastError = "SubsurfaceScattering: blur pipeline creation failed.";
            return false;
        }
    }

    // Composites: vertical blur + composite, and the ray-traced composite, each with
    // additive blending (normal) and without (debug views replace the scene).
    {
        DX12Shader vertexShader, blurYShader, rayTracedShader;
        if (!compile(kShaderScreenSpace, L"VSMain", L"vs_6_0", ShaderStage::Vertex, vertexShader)
            || !compile(kShaderScreenSpace, L"PSBlurYComposite", L"ps_6_0", ShaderStage::Pixel, blurYShader)
            || !compile(kShaderScreenSpace, L"PSRayTracedComposite", L"ps_6_0", ShaderStage::Pixel, rayTracedShader))
        {
            return false;
        }

        for (int variant = 0; variant < 2; ++variant)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = mRootSignature.Get();
            desc.VS = vertexShader.GetBytecode();
            desc.SampleMask = UINT_MAX;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = mSceneColorFormat;
            desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = TRUE;
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.StencilEnable = FALSE;
            D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
            blend.BlendEnable = FALSE;
            blend.LogicOpEnable = FALSE;
            blend.SrcBlend = D3D12_BLEND_ONE;
            blend.DestBlend = D3D12_BLEND_ZERO;
            blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            // Alpha is left alone either way; later passes treat scene alpha as coverage.
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED
                                        | D3D12_COLOR_WRITE_ENABLE_GREEN
                                        | D3D12_COLOR_WRITE_ENABLE_BLUE;
            if (variant == 0)
            {
                blend.BlendEnable = TRUE;
                blend.SrcBlend = D3D12_BLEND_ONE;
                blend.DestBlend = D3D12_BLEND_ONE;
                blend.BlendOp = D3D12_BLEND_OP_ADD;
                blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
                blend.DestBlendAlpha = D3D12_BLEND_ONE;
                blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }

            desc.PS = blurYShader.GetBytecode();
            if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&mPsoCompositeBlurY[variant]))))
            {
                mLastError = "SubsurfaceScattering: composite pipeline creation failed.";
                return false;
            }
            desc.PS = rayTracedShader.GetBytecode();
            if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&mPsoCompositeRayTraced[variant]))))
            {
                mLastError = "SubsurfaceScattering: ray-traced composite pipeline creation failed.";
                return false;
            }
        }
    }

    // Ray-traced scatter. Optional: without DXR 1.1 the screen-space path still works.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        const bool inlineRayTracing =
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))
            && options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;

        DX12Shader shader;
        const std::string screenSpaceError = mLastError;
        if (inlineRayTracing && compile(kShaderRayTraced, L"main", L"cs_6_5", ShaderStage::Compute, shader))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = mRootSignature.Get();
            desc.CS = shader.GetBytecode();
            if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&mPsoRayTraced))))
            {
                mPsoRayTraced.Reset();
                PTERO_LOG_WARNING("Renderer", "Subsurface scattering: ray-traced pipeline creation failed; "
                                  "ray-traced mode falls back to screen space.");
            }
        }
        else if (!inlineRayTracing)
        {
            PTERO_LOG_WARNING("Renderer", "Subsurface scattering: no DXR 1.1 support; ray-traced mode falls back to screen space.");
        }
        else if (!mLastError.empty())
        {
            PTERO_LOG_WARNING("Renderer", "%s", mLastError.c_str());
        }
        mLastError = screenSpaceError;
    }

    return true;
}

bool SubsurfaceScatteringRenderer::CreateSizeDependentResources()
{
    ID3D12Device* device = DX12Context_GetDevice();
    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    // Diffuse target: written by the lighting resolve, read by the passes here.
    {
        D3D12_RESOURCE_DESC desc = MakeTexture2DDesc(mWidth, mHeight);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = kLightingFormat;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&mDiffuseTarget)));
        mDiffuseTarget->SetName(L"SSS_Diffuse");
        mDiffuseState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }

    // Scratch: horizontal blur output, or the ray-traced result.
    {
        D3D12_RESOURCE_DESC desc = MakeTexture2DDesc(mWidth, mHeight);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        DX12_THROW_IF_FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mScratchTarget)));
        mScratchTarget->SetName(L"SSS_Scratch");
        mScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = kLightingFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(mDiffuseTarget.Get(), &rtvDesc, mDiffuseRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = kLightingFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mDiffuseTarget.Get(), &srvDesc, mDiffuseSrvCpu);
    device->CreateShaderResourceView(mScratchTarget.Get(), &srvDesc, mScratchSrvCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = kLightingFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(mScratchTarget.Get(), nullptr, &uavDesc, mScratchUavCpu);
    return true;
}

void SubsurfaceScatteringRenderer::RetireSizeDependentResources()
{
    // Command lists from the frames still in flight name these; keep them alive until
    // those frames are done rather than releasing them where they are replaced.
    if (mDiffuseTarget)
        mRetiredResources.push_back({ mDiffuseTarget, kFramesInFlight + 1 });
    if (mScratchTarget)
        mRetiredResources.push_back({ mScratchTarget, kFramesInFlight + 1 });
    mDiffuseTarget.Reset();
    mScratchTarget.Reset();
}

void SubsurfaceScatteringRenderer::TickRetiredResources()
{
    for (RetiredResource& retired : mRetiredResources)
    {
        if (retired.FramesRemaining > 0)
            --retired.FramesRemaining;
    }
    mRetiredResources.erase(
        std::remove_if(mRetiredResources.begin(), mRetiredResources.end(),
                       [](const RetiredResource& retired) { return retired.FramesRemaining == 0; }),
        mRetiredResources.end());
}

D3D12_GPU_VIRTUAL_ADDRESS SubsurfaceScatteringRenderer::PrepareFrame(const FrameInputs& inputs)
{
    mCurrentConstants = 0;
    if (!mIsInitialized || !mMappedConstants || inputs.Settings == nullptr)
        return 0;

    TickRetiredResources();

    const SubsurfaceSettings& settings = *inputs.Settings;
    mRayTracedThisFrame = settings.Mode == 1 && inputs.RayTracingAvailable && mPsoRayTraced != nullptr;
    mDebugView = std::clamp(settings.DebugView, 0, 2);

    mFrameSlot = (mFrameSlot + 1) % kFramesInFlight;
    ++mFrameIndex;

    // Built on the stack and copied in one go: the upload heap is write-combined, and
    // field-by-field writes (or any read back) there are slow.
    SubsurfaceGpuConstants constants{};

    const int kernelSamples = KernelSampleCount(settings.Quality);
    for (int slot = 1; slot < SubsurfaceProfiles::kMaxProfiles; ++slot)
    {
        const ProfileSlot& entry = gProfileSlots[slot];
        if (!entry.InUse)
            continue;

        const SubsurfaceProfileDesc& desc = entry.Desc;
        const XMFLOAT3 strength{ std::clamp(desc.Color.x, 0.0f, 1.0f),
                                 std::clamp(desc.Color.y, 0.0f, 1.0f),
                                 std::clamp(desc.Color.z, 0.0f, 1.0f) };
        const XMFLOAT3 falloff{ (std::max)(desc.Falloff.x, 0.0f),
                                (std::max)(desc.Falloff.y, 0.0f),
                                (std::max)(desc.Falloff.z, 0.0f) };

        constants.Profiles[slot].ColorRadius = { strength.x, strength.y, strength.z,
                                                 (std::max)(desc.RadiusMeters, 1e-5f) };
        constants.Profiles[slot].FalloffTranslucency = { falloff.x, falloff.y, falloff.z,
                                                         std::clamp(desc.Translucency, 0.0f, 1.0f) };
        BuildSeparableSssKernel(strength, falloff, kernelSamples,
                                &constants.Kernel[slot * SubsurfaceGpuConstants::kMaxKernelSamples]);
    }

    const XMMATRIX viewProjection = XMLoadFloat4x4(&inputs.ViewProjection);
    XMStoreFloat4x4(&constants.ViewProj, XMMatrixTranspose(viewProjection));
    XMStoreFloat4x4(&constants.InvViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));

    constants.CameraPos = inputs.CameraPosition;
    constants.KernelSamples = static_cast<uint32_t>(kernelSamples);
    constants.RenderSize = { static_cast<float>(mWidth), static_cast<float>(mHeight) };
    constants.InvRenderSize = { 1.0f / static_cast<float>(mWidth), 1.0f / static_cast<float>(mHeight) };
    constants.ProjScaleX = inputs.Projection._11;
    constants.ProjScaleY = inputs.Projection._22;
    constants.DepthA = inputs.Projection._33;
    constants.DepthB = inputs.Projection._43;

    constants.Enabled = settings.Enabled ? 1u : 0u;
    constants.Mode = mRayTracedThisFrame ? 1u : 0u;
    constants.FollowSurface = settings.FollowSurface ? 1u : 0u;
    constants.Transmission = settings.Transmission ? 1u : 0u;
    constants.TransmissionIntensity = (std::max)(settings.TransmissionIntensity, 0.0f);
    constants.RtSamples = static_cast<uint32_t>(std::clamp(settings.RtSamples, 1, 64));
    constants.FrameIndex = mFrameIndex;
    constants.DebugView = mDebugView;

    constants.SunDirection = inputs.SunDirection;
    constants.SunColor = inputs.SunColor;
    constants.SkyAmbient = inputs.SkyAmbient;
    constants.NumLights = 0;
    if (inputs.Lights != nullptr)
    {
        constants.NumLights = std::clamp(inputs.NumLights, 0, SubsurfaceGpuConstants::kMaxLights);
        std::copy(inputs.Lights, inputs.Lights + constants.NumLights, constants.Lights);
    }

    if (inputs.Shadows != nullptr)
    {
        const DeferredLightingPass::ShadowSnapshot& shadows = *inputs.Shadows;
        constants.LightViewProj = shadows.LightViewProj;
        constants.ShadowMapSize = shadows.ShadowMapSize;
        constants.ShadowBias = shadows.ShadowBias;
        constants.PointShadowMapSize = shadows.PointShadowMapSize;
        constants.PointShadowBias = shadows.PointShadowBias;
        std::copy(std::begin(shadows.PointFaceViewProj), std::end(shadows.PointFaceViewProj),
                  std::begin(constants.PointFaceViewProj));
        constants.HasSunShadow = shadows.SunShadowSrv.ptr != 0 ? 1u : 0u;
        constants.HasPointShadows = (shadows.PointShadowSrv.ptr != 0 && shadows.PointShadowLightCount > 0) ? 1u : 0u;
    }

    std::uint8_t* destination = mMappedConstants + mFrameSlot * mConstantStride;
    std::memcpy(destination, &constants, sizeof(constants));

    mCurrentConstants = mConstantBuffer->GetGPUVirtualAddress() + mFrameSlot * mConstantStride;
    return mCurrentConstants;
}

void SubsurfaceScatteringRenderer::BeginLightingOutput(ID3D12GraphicsCommandList* commandList)
{
    if (!mIsInitialized || !mDiffuseTarget || mDiffuseState == D3D12_RESOURCE_STATE_RENDER_TARGET)
        return;
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        mDiffuseTarget.Get(), mDiffuseState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barrier);
    mDiffuseState = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

void SubsurfaceScatteringRenderer::Apply(
    ID3D12GraphicsCommandList*  commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferAlbedoSrv,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalSrv,
    const RayTracedSceneSrvs&   scene)
{
    const D3D12_GPU_DESCRIPTOR_HANDLE tlasSrv = scene.Tlas;
    if (!mIsInitialized || mCurrentConstants == 0 || !mDiffuseTarget || !mScratchTarget)
        return;

    const bool rayTraced = mRayTracedThisFrame && tlasSrv.ptr != 0
        && scene.Vertices.ptr != 0 && scene.Indices.ptr != 0 && scene.InstanceInfo.ptr != 0;

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    commandList->SetDescriptorHeaps(1, heaps);

    // Diffuse: render target -> readable everywhere.
    D3D12_RESOURCE_BARRIER barriers[2];
    UINT barrierCount = 0;
    if (mDiffuseState != D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE)
    {
        barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
            mDiffuseTarget.Get(), mDiffuseState, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        mDiffuseState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    }
    if (mScratchState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
            mScratchTarget.Get(), mScratchState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        mScratchState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (barrierCount > 0)
        commandList->ResourceBarrier(barrierCount, barriers);

    // ---- Scatter into the scratch target ----
    commandList->SetComputeRootSignature(mRootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, mCurrentConstants);
    commandList->SetComputeRootDescriptorTable(1, mDiffuseSrvGpu);
    commandList->SetComputeRootDescriptorTable(2, mDiffuseSrvGpu);
    commandList->SetComputeRootDescriptorTable(3, gbufferNormalSrv);
    commandList->SetComputeRootDescriptorTable(6, mScratchUavGpu);
    if (rayTraced)
    {
        commandList->SetComputeRootDescriptorTable(4, gbufferAlbedoSrv);
        commandList->SetComputeRootDescriptorTable(5, tlasSrv);
        commandList->SetComputeRootDescriptorTable(7, scene.Vertices);
        commandList->SetComputeRootDescriptorTable(8, scene.Indices);
        commandList->SetComputeRootDescriptorTable(9, scene.InstanceInfo);
        // Unbound tables are only ever sampled behind HasSunShadow / HasPointShadows, but
        // every table the shader declares must name a valid descriptor, so fall back to
        // the diffuse SRV (a 2D texture, never actually read) when a map is missing.
        commandList->SetComputeRootDescriptorTable(10, scene.SunShadow.ptr != 0 ? scene.SunShadow : mDiffuseSrvGpu);
        commandList->SetComputeRootDescriptorTable(11, scene.PointShadows.ptr != 0 ? scene.PointShadows : mDiffuseSrvGpu);
        commandList->SetPipelineState(mPsoRayTraced.Get());
    }
    else
    {
        commandList->SetPipelineState(mPsoBlurX.Get());
    }
    commandList->Dispatch((mWidth + kGroupSize - 1) / kGroupSize, (mHeight + kGroupSize - 1) / kGroupSize, 1);

    const D3D12_RESOURCE_BARRIER scratchToRead[] = {
        CD3DX12_RESOURCE_BARRIER::UAV(mScratchTarget.Get()),
        CD3DX12_RESOURCE_BARRIER::Transition(
            mScratchTarget.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE),
    };
    commandList->ResourceBarrier(2, scratchToRead);
    mScratchState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

    // ---- Composite into the scene colour ----
    const int blendVariant = mDebugView != 0 ? 1 : 0;
    commandList->SetGraphicsRootSignature(mRootSignature.Get());
    commandList->SetPipelineState(rayTraced ? mPsoCompositeRayTraced[blendVariant].Get()
                                            : mPsoCompositeBlurY[blendVariant].Get());
    commandList->SetGraphicsRootConstantBufferView(0, mCurrentConstants);
    commandList->SetGraphicsRootDescriptorTable(1, mScratchSrvGpu);
    commandList->SetGraphicsRootDescriptorTable(2, mDiffuseSrvGpu);
    commandList->SetGraphicsRootDescriptorTable(3, gbufferNormalSrv);

    commandList->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f };
    const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}
