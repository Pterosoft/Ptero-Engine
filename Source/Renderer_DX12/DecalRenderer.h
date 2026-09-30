#pragma once

// DecalRenderer
// Draws an oriented wireframe box gizmo and a facing-direction arrow for every
// entity that has a DecalComponent, so artists can see each decal's position,
// extents, and projection direction in the editor viewport.

#include "Components.h"
#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
}

class DecalRenderer
{
public:
    bool Initialize(
        ID3D12GraphicsCommandList* commandList,
        DXGI_FORMAT                colorFormat,
        DXGI_FORMAT                depthFormat);

    void Render(
        ID3D12GraphicsCommandList*  commandList,
        const std::vector<Entity>&  entities,
        const DirectX::XMMATRIX&   viewProjection);

    void Shutdown();

    bool IsInitialized() const { return mPipelineReady; }

    const char* GetLastError() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // Per-draw constant buffer layout – must match DecalGizmoConstants in Decal.hlsl.
    struct alignas(256) GizmoConstants
    {
        DirectX::XMFLOAT4X4 MVP;
        DirectX::XMFLOAT4   Color;
        std::byte            Padding[128]{};
    };
    static_assert(sizeof(GizmoConstants) == 256);

    bool CreateBoxMesh(ID3D12GraphicsCommandList* commandList);
    bool CreateArrowMesh(ID3D12GraphicsCommandList* commandList);
    bool CreatePipeline(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
    bool EnsureConstantBuffer(std::size_t requiredCount);

    // Box wire geometry
    Microsoft::WRL::ComPtr<ID3D12Resource> mBoxVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBoxVertexUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBoxIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mBoxIndexUpload;
    D3D12_VERTEX_BUFFER_VIEW               mBoxVBView{};
    D3D12_INDEX_BUFFER_VIEW                mBoxIBView{};
    UINT                                   mBoxIndexCount = 0;

    // Arrow geometry
    Microsoft::WRL::ComPtr<ID3D12Resource> mArrowVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mArrowVertexUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mArrowIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mArrowIndexUpload;
    D3D12_VERTEX_BUFFER_VIEW               mArrowVBView{};
    D3D12_INDEX_BUFFER_VIEW                mArrowIBView{};
    UINT                                   mArrowIndexCount = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    GizmoConstants*                        mMappedCB   = nullptr;
    std::size_t                            mCBCapacity = 0;

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;

    bool        mPipelineReady = false;
    std::string mLastError;
};
