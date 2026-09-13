#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "Components.h"

#include "../QtUi/UiTypes.h"

#include <DirectXMath.h>
#include <string>
#include <map>
#include <unordered_map>
#include <vector>

class MotionVectorRenderer
{
public:
    bool Initialize(UINT width, UINT height);
    void Shutdown();
    bool EnsureSize(UINT width, UINT height);

    void SetEntities(std::vector<Entity>* entities)
    {
        mEntities = entities;
    }

    void Render(
        ID3D12GraphicsCommandList* commandList,
        const std::unordered_map<std::size_t, DirectX::XMFLOAT4X4>& previousTransforms,
        const DirectX::XMFLOAT4X4& currentViewProjection,
        const DirectX::XMFLOAT4X4& previousViewProjection,
        bool resetHistory);

    void ResetHistory();

    // Re-bind the motion vector target so another renderer can contribute to
    // it -- vegetation needs this because its motion vectors depend on the wind
    // bend and so cannot be produced by this class's per-entity pipeline.
    // Clears the target if Render() did not already do so this frame.
    // Returns false when the target is not ready.
    bool BeginExternalPass(ID3D12GraphicsCommandList* commandList);
    void EndExternalPass(ID3D12GraphicsCommandList* commandList);

    // Format of the motion vector render target, needed by external passes to
    // build a matching pipeline state.
    static constexpr DXGI_FORMAT OutputFormat = DXGI_FORMAT_R16G16_FLOAT;

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv() const { return mOutputSrvGpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputCpuSrv() const { return mOutputSrvCpu; }
    ID3D12Resource* GetOutputResource() const { return mOutputTexture.Get(); }
    UiTextureID GetOutputTextureId() const { return mOutputTextureId; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    struct alignas(256) MotionVectorConstants
    {
        DirectX::XMFLOAT4X4 CurrentModelViewProjection{};
        DirectX::XMFLOAT4X4 PreviousModelViewProjection{};
    };

    struct GpuMesh
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> VertexUpload;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
        Microsoft::WRL::ComPtr<ID3D12Resource> IndexUpload;
        D3D12_VERTEX_BUFFER_VIEW VertexView{};
        D3D12_INDEX_BUFFER_VIEW IndexView{};
        UINT IndexCount = 0;
        const Mesh* SourceMesh = nullptr;
    };

    bool CreatePipeline();
    bool CreateOutput(UINT width, UINT height);
    bool EnsureConstantBuffer(std::size_t requiredCount);
    bool EnsureMesh(ID3D12GraphicsCommandList* commandList, const Mesh* mesh);
    static bool CreateCommittedBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState, Microsoft::WRL::ComPtr<ID3D12Resource>& outResource);

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE mRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpu{};
    UiTextureID mOutputTextureId = UiTextureID_Invalid;
    bool mOutputSrvAllocated = false;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    MotionVectorConstants* mMappedConstants = nullptr;
    std::size_t mConstantBufferCapacity = 0;

// Keyed by the mesh asset, not the entity index. Keying by index meant every
    // entity uploaded a private copy of geometry it shared with others, and that
    // deleting one entity shifted every later index so this cache erased live
    // buffers mid-frame - a released resource under a running command list,
    // which the driver reports as a device hang. See EntityMeshRenderer, which
    // carried the same bug.
    std::map<const Mesh*, GpuMesh> mGpuMeshes;
    std::vector<Entity>* mEntities = nullptr;

    UINT mWidth = 0;
    UINT mHeight = 0;
    bool mInitialized = false;
    // Whether Render() already cleared the target this frame, so an external
    // pass knows whether it has to.
    bool mClearedThisFrame = false;
    std::string mLastError;
};