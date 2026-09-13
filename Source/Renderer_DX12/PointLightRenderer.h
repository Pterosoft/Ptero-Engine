#pragma once

// PointLightRenderer
// Manages GPU resources and draw calls for the point light editor gizmo.
// Each frame it draws a wireframe sphere at every entity that carries a
// PointLightComponent so the artist can see the light's position and radius
// in the viewport.
//
// Actual per-fragment point light illumination is evaluated inside
// MeshEntity.hlsl via the shared LightingConstants constant buffer that
// EntityMeshRenderer uploads each frame.

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

class PointLightRenderer
{
public:
    // Initialise GPU resources (sphere geometry, root signature, pipeline).
    // Must be called once after the D3D12 device is ready.
    bool Initialize(
        ID3D12GraphicsCommandList* commandList,
        DXGI_FORMAT                colorFormat,
        DXGI_FORMAT                depthFormat);

    // Draw a wireframe sphere gizmo for every entity that has a PointLightComponent.
    // Must be called while the scene render-target and depth target are bound.
    void Render(
        ID3D12GraphicsCommandList*    commandList,
        const std::vector<Entity>&    entities,
        const DirectX::XMMATRIX&     viewProjection);

    void Shutdown();

    bool IsInitialized() const { return mPipelineReady; }

    const char* GetLastError() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    // Per-gizmo constant buffer layout – must match PointLightGizmoConstants in PointLight.hlsl.
    struct alignas(256) GizmoConstants
    {
        DirectX::XMFLOAT4X4 MVP;
        DirectX::XMFLOAT4   Color;
        std::byte            Padding[128]{};
    };
    static_assert(sizeof(GizmoConstants) == 256);

    bool CreateSphereMesh(ID3D12GraphicsCommandList* commandList);
    bool CreatePipeline(DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat);
    bool EnsureConstantBuffer(std::size_t requiredCount);

    // One vertex/index buffer holds all three gizmo shapes back to back; a
    // light picks its shape by index range rather than by switching buffers.
    struct MeshRange
    {
        UINT IndexOffset = 0;
        UINT IndexCount = 0;
        INT  BaseVertex = 0;
    };
    MeshRange mSphereRange;
    MeshRange mConeRange;    // unit cone: apex at origin, opening along -Z
    MeshRange mRectRange;    // unit quad in XY plus a short -Z normal stalk

    static void AppendWireframeCone(
        int                              slices,
        std::vector<DirectX::XMFLOAT3>&  outVerts,
        std::vector<std::uint16_t>&      outIndices,
        MeshRange&                       outRange);

    static void AppendWireframeRect(
        std::vector<DirectX::XMFLOAT3>&  outVerts,
        std::vector<std::uint16_t>&      outIndices,
        MeshRange&                       outRange);

    Microsoft::WRL::ComPtr<ID3D12Resource> mVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mVertexUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndexUpload;
    D3D12_VERTEX_BUFFER_VIEW               mVBView{};
    D3D12_INDEX_BUFFER_VIEW                mIBView{};
    UINT                                   mIndexCount = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;

    Microsoft::WRL::ComPtr<ID3D12Resource> mConstantBuffer;
    GizmoConstants*                        mMappedCB    = nullptr;
    std::size_t                            mCBCapacity  = 0;

    DX12Shader mVertexShader;
    DX12Shader mPixelShader;

    bool        mPipelineReady = false;
    std::string mLastError;
};
