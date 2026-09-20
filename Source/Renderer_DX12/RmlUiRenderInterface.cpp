#include "pch.h"

#include "RmlUiRenderInterface.h"

#include <RmlUi/Core/Types.h>

#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
}

namespace
{
    // Enough slots that the ring wraps far behind whatever the GPU is still reading, so
    // draws never need to wait on a fence just to publish their translation.
    constexpr UINT ConstantBufferSlotCount = 8192;
    constexpr UINT ConstantBufferSlotSize = 256;

    std::wstring GetShaderDirectory()
    {
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::wstring directory(modulePath);
        const auto slash = directory.find_last_of(L"\\/");
        directory = (slash != std::wstring::npos) ? directory.substr(0, slash + 1) : L"";

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

    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
            return {};

        const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        if (required <= 0)
            return {};

        std::wstring result(static_cast<size_t>(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), required);
        return result;
    }

    // out = a * b, both stored column-major and applied to column vectors, matching both
    // RmlUi's Matrix4f and HLSL's default constant-buffer packing.
    void MultiplyColumnMajor(const float* a, const float* b, float* out)
    {
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[column * 4 + k];

                out[column * 4 + row] = sum;
            }
        }
    }
}

RmlUiRenderInterface::~RmlUiRenderInterface()
{
    Shutdown();
}

bool RmlUiRenderInterface::Initialize(DXGI_FORMAT renderTargetFormat, ID3D12GraphicsCommandList* uploadCommandList)
{
    if (mIsInitialized)
        return true;

    if (DX12Context_GetDevice() == nullptr)
    {
        mLastError = "RmlUiRenderInterface: DX12 device unavailable.";
        return false;
    }

    if (!CreateRootSignature())
        return false;

    if (!CreatePipelineState(renderTargetFormat))
        return false;

    if (!CreateConstantBuffer())
        return false;

    // The fallback texture is the only upload this class records outside a normal frame,
    // so the initialization list stands in for the frame list just long enough to make it.
    mCommandList = uploadCommandList;
    const bool whiteTextureCreated = CreateWhiteTexture();
    mCommandList = nullptr;
    if (!whiteTextureCreated)
        return false;

    mIsInitialized = true;
    return true;
}

void RmlUiRenderInterface::Shutdown()
{
    mGeometry.clear();
    mTextures.clear();
    // Shutdown runs after the device has been drained, so anything still waiting out
    // its countdown can go now rather than leaking with the interface.
    mPendingGeometryReleases.clear();
    mPendingTextureReleases.clear();
    mFreeSrvDescriptors.clear();

    if (mConstantBuffer && mMappedConstants)
    {
        mConstantBuffer->Unmap(0, nullptr);
        mMappedConstants = nullptr;
    }

    mConstantBuffer.Reset();
    mWhiteTexture = Texture{};
    mPipelineState.Reset();
    mRootSignature.Reset();
    mCommandList = nullptr;
    mIsInitialized = false;
}

bool RmlUiRenderInterface::CreateRootSignature()
{
    ID3D12Device* device = DX12Context_GetDevice();

    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;
    textureRange.RegisterSpace = 0;
    textureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
    rootSignatureDesc.NumParameters = _countof(parameters);
    rootSignatureDesc.pParameters = parameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(hr))
    {
        mLastError = "RmlUiRenderInterface: failed to serialize root signature.";
        if (errors)
            mLastError += std::string(" ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    }

    hr = device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature));
    if (FAILED(hr))
    {
        mLastError = "RmlUiRenderInterface: failed to create root signature.";
        return false;
    }

    mRootSignature->SetName(L"RmlUiRootSignature");
    return true;
}

bool RmlUiRenderInterface::CreatePipelineState(DXGI_FORMAT renderTargetFormat)
{
    ID3D12Device* device = DX12Context_GetDevice();
    const std::wstring shaderPath = GetShaderDirectory() + L"RmlUi.hlsl";

    {
        ShaderCompileRequest request{};
        request.FilePath = shaderPath;
        request.EntryPoint = L"VSMain";
        request.TargetProfile = L"vs_6_5";
        request.Stage = ShaderStage::Vertex;
        if (!mVertexShader.Compile(request))
        {
            mLastError = std::string("RmlUiRenderInterface: vertex shader compile failed: ") +
                (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
            return false;
        }
    }

    {
        ShaderCompileRequest request{};
        request.FilePath = shaderPath;
        request.EntryPoint = L"PSMain";
        request.TargetProfile = L"ps_6_5";
        request.Stage = ShaderStage::Pixel;
        if (!mPixelShader.Compile(request))
        {
            mLastError = std::string("RmlUiRenderInterface: pixel shader compile failed: ") +
                (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
            return false;
        }
    }

    const D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = mVertexShader.GetBytecode();
    psoDesc.PS = mPixelShader.GetBytecode();

    // RmlUi emits premultiplied-alpha colours, so source colour is added as-is rather
    // than scaled by its own alpha a second time.
    D3D12_RENDER_TARGET_BLEND_DESC blendDesc{};
    blendDesc.BlendEnable = TRUE;
    blendDesc.SrcBlend = D3D12_BLEND_ONE;
    blendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = blendDesc;

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = FALSE;
    psoDesc.RasterizerState.MultisampleEnable = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = renderTargetFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleMask = UINT_MAX;

    if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState))))
    {
        mLastError = "RmlUiRenderInterface: failed to create pipeline state.";
        return false;
    }

    mPipelineState->SetName(L"RmlUiPipeline");
    return true;
}

bool RmlUiRenderInterface::CreateConstantBuffer()
{
    ID3D12Device* device = DX12Context_GetDevice();

    const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(
        static_cast<UINT64>(ConstantBufferSlotCount) * ConstantBufferSlotSize);

    HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mConstantBuffer));
    if (FAILED(hr))
    {
        mLastError = "RmlUiRenderInterface: failed to create constant buffer.";
        return false;
    }

    void* mapped = nullptr;
    if (FAILED(mConstantBuffer->Map(0, nullptr, &mapped)))
    {
        mLastError = "RmlUiRenderInterface: failed to map constant buffer.";
        return false;
    }

    mMappedConstants = static_cast<std::uint8_t*>(mapped);
    mConstantBuffer->SetName(L"RmlUiConstantBuffer");
    return true;
}

bool RmlUiRenderInterface::CreateWhiteTexture()
{
    // Untextured RmlUi geometry still samples a texture, so a 1x1 opaque white pixel is
    // bound in its place and the shader stays branch-free.
    const std::uint32_t whitePixel = 0xFFFFFFFFu;
    const Rml::TextureHandle handle = CreateTextureFromPixels(&whitePixel, 1, 1, 4);
    if (handle == 0)
    {
        mLastError = "RmlUiRenderInterface: failed to create the fallback white texture.";
        return false;
    }

    const auto it = mTextures.find(static_cast<std::uintptr_t>(handle));
    if (it == mTextures.end())
        return false;

    mWhiteTexture = it->second;
    mTextures.erase(it);
    return true;
}

bool RmlUiRenderInterface::AcquireSrvDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle)
{
    if (!mFreeSrvDescriptors.empty())
    {
        cpuHandle = mFreeSrvDescriptors.back().first;
        gpuHandle = mFreeSrvDescriptors.back().second;
        mFreeSrvDescriptors.pop_back();
        return true;
    }

    return DX12Context_AllocateSrvDescriptor(&cpuHandle, &gpuHandle);
}

void RmlUiRenderInterface::ReleaseSrvDescriptor(const Texture& texture)
{
    if (texture.CpuHandle.ptr != 0)
        mFreeSrvDescriptors.emplace_back(texture.CpuHandle, texture.GpuHandle);
}

void RmlUiRenderInterface::BeginFrame(
    ID3D12GraphicsCommandList* commandList,
    UINT viewportWidth,
    UINT viewportHeight)
{
    mCommandList = commandList;
    mViewportWidth = (std::max)(viewportWidth, 1u);
    mViewportHeight = (std::max)(viewportHeight, 1u);
    mScissorEnabled = false;
    mHasTransform = false;

    // One tick per frame: anything released long enough ago that no submitted frame
    // can still name it goes back to the driver here, and nowhere else.
    TickPendingReleases();

    const float width = static_cast<float>(mViewportWidth);
    const float height = static_cast<float>(mViewportHeight);

    // Orthographic projection mapping UI pixels (origin top-left) onto clip space.
    std::memset(mProjection, 0, sizeof(mProjection));
    mProjection[0] = 2.0f / width;   // column 0, row 0
    mProjection[5] = -2.0f / height; // column 1, row 1
    mProjection[10] = 1.0f;          // column 2, row 2
    mProjection[12] = -1.0f;         // column 3, row 0
    mProjection[13] = 1.0f;          // column 3, row 1
    mProjection[15] = 1.0f;          // column 3, row 3

    if (mCommandList == nullptr)
        return;

    D3D12_VIEWPORT viewport{};
    viewport.Width = width;
    viewport.Height = height;
    viewport.MaxDepth = 1.0f;
    mCommandList->RSSetViewports(1, &viewport);

    mCommandList->SetPipelineState(mPipelineState.Get());
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* heaps[] = { DX12Context_GetSrvDescriptorHeap() };
    mCommandList->SetDescriptorHeaps(1, heaps);

    ApplyScissor();
}

void RmlUiRenderInterface::EndFrame()
{
    mCommandList = nullptr;
}

void RmlUiRenderInterface::ApplyScissor()
{
    if (mCommandList == nullptr)
        return;

    D3D12_RECT rect = mScissorEnabled
        ? mScissorRect
        : D3D12_RECT{ 0, 0, static_cast<LONG>(mViewportWidth), static_cast<LONG>(mViewportHeight) };

    // Clamp so a document that scrolls content off-screen cannot hand D3D an out-of-range
    // or inverted rectangle.
    rect.left = (std::clamp)(rect.left, 0L, static_cast<LONG>(mViewportWidth));
    rect.top = (std::clamp)(rect.top, 0L, static_cast<LONG>(mViewportHeight));
    rect.right = (std::clamp)(rect.right, rect.left, static_cast<LONG>(mViewportWidth));
    rect.bottom = (std::clamp)(rect.bottom, rect.top, static_cast<LONG>(mViewportHeight));

    mCommandList->RSSetScissorRects(1, &rect);
}

void RmlUiRenderInterface::UploadConstants(const Rml::Vector2f& translation)
{
    RmlUiConstants constants{};
    if (mHasTransform)
        MultiplyColumnMajor(mProjection, mTransform, constants.Transform);
    else
        std::memcpy(constants.Transform, mProjection, sizeof(mProjection));

    constants.TranslationX = translation.x;
    constants.TranslationY = translation.y;

    const UINT slot = mConstantSlotIndex;
    mConstantSlotIndex = (mConstantSlotIndex + 1) % ConstantBufferSlotCount;

    std::memcpy(mMappedConstants + static_cast<size_t>(slot) * ConstantBufferSlotSize, &constants, sizeof(constants));

    mCommandList->SetGraphicsRootConstantBufferView(
        0, mConstantBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * ConstantBufferSlotSize);
}

Rml::CompiledGeometryHandle RmlUiRenderInterface::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices,
    Rml::Span<const int> indices)
{
    if (!mIsInitialized || vertices.empty() || indices.empty())
        return 0;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return 0;

    const UINT64 vertexBytes = static_cast<UINT64>(vertices.size()) * sizeof(Rml::Vertex);
    const UINT64 indexBytes = static_cast<UINT64>(indices.size()) * sizeof(int);

    CompiledGeometry geometry{};
    const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    const auto vertexDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBytes);
    if (FAILED(device->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &vertexDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&geometry.VertexBuffer))))
    {
        return 0;
    }

    const auto indexDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBytes);
    if (FAILED(device->CreateCommittedResource(
            &heapProperties, D3D12_HEAP_FLAG_NONE, &indexDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&geometry.IndexBuffer))))
    {
        return 0;
    }

    void* mapped = nullptr;
    if (FAILED(geometry.VertexBuffer->Map(0, nullptr, &mapped)))
        return 0;
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(vertexBytes));
    geometry.VertexBuffer->Unmap(0, nullptr);

    if (FAILED(geometry.IndexBuffer->Map(0, nullptr, &mapped)))
        return 0;
    std::memcpy(mapped, indices.data(), static_cast<size_t>(indexBytes));
    geometry.IndexBuffer->Unmap(0, nullptr);

    geometry.VertexBufferView.BufferLocation = geometry.VertexBuffer->GetGPUVirtualAddress();
    geometry.VertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
    geometry.VertexBufferView.StrideInBytes = sizeof(Rml::Vertex);

    geometry.IndexBufferView.BufferLocation = geometry.IndexBuffer->GetGPUVirtualAddress();
    geometry.IndexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);
    geometry.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;

    geometry.IndexCount = static_cast<UINT>(indices.size());

    const std::uintptr_t handle = mNextGeometryHandle++;
    mGeometry.emplace(handle, std::move(geometry));
    return static_cast<Rml::CompiledGeometryHandle>(handle);
}

void RmlUiRenderInterface::RenderGeometry(
    Rml::CompiledGeometryHandle geometry,
    Rml::Vector2f translation,
    Rml::TextureHandle texture)
{
    if (mCommandList == nullptr)
        return;

    const auto it = mGeometry.find(static_cast<std::uintptr_t>(geometry));
    if (it == mGeometry.end())
        return;

    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = mWhiteTexture.GpuHandle;
    if (texture != 0)
    {
        const auto textureIt = mTextures.find(static_cast<std::uintptr_t>(texture));
        if (textureIt != mTextures.end())
            textureHandle = textureIt->second.GpuHandle;
    }

    UploadConstants(translation);
    mCommandList->SetGraphicsRootDescriptorTable(1, textureHandle);
    mCommandList->IASetVertexBuffers(0, 1, &it->second.VertexBufferView);
    mCommandList->IASetIndexBuffer(&it->second.IndexBufferView);
    mCommandList->DrawIndexedInstanced(it->second.IndexCount, 1, 0, 0, 0);
}

void RmlUiRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    const auto it = mGeometry.find(static_cast<std::uintptr_t>(geometry));
    if (it == mGeometry.end())
        return;

    // Not freed here: frames already submitted may still be drawing this geometry.
    // See mPendingGeometryReleases.
    mPendingGeometryReleases.push_back({ std::move(it->second), kFramesInFlight + 1 });
    mGeometry.erase(it);
}

void RmlUiRenderInterface::TickPendingReleases()
{
    for (auto it = mPendingGeometryReleases.begin(); it != mPendingGeometryReleases.end(); )
    {
        if (--it->FramesRemaining <= 0)
            it = mPendingGeometryReleases.erase(it);
        else
            ++it;
    }

    for (auto it = mPendingTextureReleases.begin(); it != mPendingTextureReleases.end(); )
    {
        if (--it->FramesRemaining <= 0)
        {
            // The descriptor slot goes back only now, with the resource it names.
            ReleaseSrvDescriptor(it->TextureData);
            it = mPendingTextureReleases.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

Rml::TextureHandle RmlUiRenderInterface::CreateTextureFromPixels(
    const void* pixels,
    UINT width,
    UINT height,
    UINT rowPitch)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr || pixels == nullptr || width == 0 || height == 0)
        return 0;

    // Textures are uploaded on the frame's command list, which only exists between
    // BeginFrame and EndFrame. RmlUi generates font atlases from inside Context::Render,
    // so in practice this is always satisfied; bail out rather than record into nothing.
    if (mCommandList == nullptr)
    {
        mLastError = "RmlUiRenderInterface: texture requested outside of a frame.";
        return 0;
    }

    Texture texture{};

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture.Resource))))
    {
        return 0;
    }

    const UINT64 uploadSize = GetRequiredIntermediateSize(texture.Resource.Get(), 0, 1);
    const auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture.Upload))))
    {
        return 0;
    }

    D3D12_SUBRESOURCE_DATA subresource{};
    subresource.pData = pixels;
    subresource.RowPitch = rowPitch;
    subresource.SlicePitch = static_cast<LONG_PTR>(rowPitch) * height;

    UpdateSubresources(mCommandList, texture.Resource.Get(), texture.Upload.Get(), 0, 0, 1, &subresource);

    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texture.Resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &barrier);

    if (!AcquireSrvDescriptor(texture.CpuHandle, texture.GpuHandle))
    {
        mLastError = "RmlUiRenderInterface: ran out of SRV descriptors.";
        return 0;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture.Resource.Get(), &srvDesc, texture.CpuHandle);

    const std::uintptr_t handle = mNextTextureHandle++;
    mTextures.emplace(handle, std::move(texture));
    return static_cast<Rml::TextureHandle>(handle);
}

Rml::TextureHandle RmlUiRenderInterface::LoadTexture(
    Rml::Vector2i& textureDimensions,
    const Rml::String& source)
{
    std::filesystem::path path = Utf8ToWide(source);
    if (path.is_relative() && !mDocumentDirectory.empty())
    {
        std::filesystem::path candidate = std::filesystem::path(mDocumentDirectory) / path;
        if (std::filesystem::exists(candidate))
            path = std::move(candidate);
    }

    if (!std::filesystem::exists(path))
    {
        mLastError = "RmlUiRenderInterface: texture not found: " + source;
        return 0;
    }

    ComPtr<IWICImagingFactory> imagingFactory;
    if (FAILED(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&imagingFactory))))
    {
        mLastError = "RmlUiRenderInterface: failed to create WIC factory.";
        return 0;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(imagingFactory->CreateDecoderFromFilename(
            path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
    {
        mLastError = "RmlUiRenderInterface: failed to decode texture: " + source;
        return 0;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return 0;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(imagingFactory->CreateFormatConverter(&converter)))
        return 0;

    // RmlUi blends premultiplied colours, so convert straight to a premultiplied format
    // rather than premultiplying the pixels by hand afterwards.
    if (FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone,
            nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
    {
        return 0;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return 0;

    const UINT rowPitch = width * 4;
    std::vector<std::uint8_t> pixels(static_cast<size_t>(rowPitch) * height);
    if (FAILED(converter->CopyPixels(nullptr, rowPitch, static_cast<UINT>(pixels.size()), pixels.data())))
        return 0;

    const Rml::TextureHandle handle = CreateTextureFromPixels(pixels.data(), width, height, rowPitch);
    if (handle != 0)
        textureDimensions = Rml::Vector2i(static_cast<int>(width), static_cast<int>(height));

    return handle;
}

Rml::TextureHandle RmlUiRenderInterface::GenerateTexture(
    Rml::Span<const Rml::byte> source,
    Rml::Vector2i sourceDimensions)
{
    if (sourceDimensions.x <= 0 || sourceDimensions.y <= 0)
        return 0;

    const UINT width = static_cast<UINT>(sourceDimensions.x);
    const UINT height = static_cast<UINT>(sourceDimensions.y);
    const UINT rowPitch = width * 4;
    if (source.size() < static_cast<size_t>(rowPitch) * height)
        return 0;

    return CreateTextureFromPixels(source.data(), width, height, rowPitch);
}

void RmlUiRenderInterface::ReleaseTexture(Rml::TextureHandle texture)
{
    const auto it = mTextures.find(static_cast<std::uintptr_t>(texture));
    if (it == mTextures.end())
        return;

    // Neither the resource nor its descriptor slot is released here - in-flight
    // frames may still sample through both. See mPendingTextureReleases.
    mPendingTextureReleases.push_back({ std::move(it->second), kFramesInFlight + 1 });
    mTextures.erase(it);
}

void RmlUiRenderInterface::EnableScissorRegion(bool enable)
{
    mScissorEnabled = enable;
    ApplyScissor();
}

void RmlUiRenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
    mScissorRect.left = static_cast<LONG>(region.Left());
    mScissorRect.top = static_cast<LONG>(region.Top());
    mScissorRect.right = static_cast<LONG>(region.Right());
    mScissorRect.bottom = static_cast<LONG>(region.Bottom());
    ApplyScissor();
}

void RmlUiRenderInterface::SetTransform(const Rml::Matrix4f* transform)
{
    mHasTransform = (transform != nullptr);
    if (mHasTransform)
        std::memcpy(mTransform, transform->data(), sizeof(mTransform));
}
