#include "pch.h"
#include "QtViewportRenderer.h"
#include "../QtUi/QtUi.h"
#include "DX12Helper.h"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <map>
#include <array>
#pragma comment(lib, "d3dcompiler.lib")
using Microsoft::WRL::ComPtr;
extern "C" ID3D12CommandQueue *__stdcall DX12Context_GetCommandQueue();
extern "C" bool __stdcall DX12Context_WaitForGPU();
namespace
{
ComPtr<ID3D12Device> device;
ComPtr<ID3D12RootSignature> root;
ComPtr<ID3D12PipelineState> overlayPipeline;
ComPtr<ID3D12PipelineState> pipeline;
struct DebugView
{
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtv;
    std::array<ComPtr<ID3D12Resource>, 2> buffers;
    unsigned width = 0, height = 0;
    bool used = false;
};
std::map<HWND, DebugView> views;
void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("Qt DX12 viewport operation failed (HRESULT " + std::to_string(hr) + ").");
}
} // namespace
namespace QtViewportRenderer
{
bool Initialize(ID3D12Device *d)
{
    device = d;
    const char *shader = R"(
Texture2D scene : register(t0); SamplerState sceneSampler : register(s0);
struct V { float4 position:SV_Position; float2 uv:TEXCOORD0; };
V VS(uint id:SV_VertexID) { V v; v.uv=float2((id<<1)&2,id&2); v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v; }
float4 PS(V v):SV_Target { return float4(scene.SampleLevel(sceneSampler,v.uv,0).rgb,1); }
float4 PSOverlay(V v):SV_Target { return scene.SampleLevel(sceneSampler,v.uv,0); }
)";
    ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(
            D3DCompile(shader, strlen(shader), "QtViewport", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &errors)) ||
        FAILED(D3DCompile(shader, strlen(shader), "QtViewport", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &errors)))
        return false;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{1, &parameter, 1, &sampler,
                                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> signature;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)))
        return false;
    if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                           IID_PPV_ARGS(&root))))
        return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root.Get();
    state.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    state.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto &blend = state.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    state.DepthStencilState.DepthEnable = FALSE;
    state.DepthStencilState.StencilEnable = FALSE;
    state.SampleMask = UINT_MAX;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = 1;
    state.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    state.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&pipeline))))
        return false;

    // Overlay variant: same full-screen blit, but composited over what is already in the
    // viewport instead of replacing it. The source carries premultiplied alpha, so the
    // colour is added as-is and the destination is attenuated by coverage.
    ComPtr<ID3DBlob> overlayPs;
    if (FAILED(D3DCompile(shader, strlen(shader), "QtViewport", nullptr, nullptr, "PSOverlay", "ps_5_0", 0, 0,
                          &overlayPs, &errors)))
        return false;
    state.PS = {overlayPs->GetBufferPointer(), overlayPs->GetBufferSize()};
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    if (FAILED(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&overlayPipeline))))
        return false;

    return true;
}
void Shutdown()
{
    views.clear();
    overlayPipeline.Reset();
    pipeline.Reset();
    root.Reset();
    device.Reset();
}
void Draw(ID3D12GraphicsCommandList *cmd, UiTextureID texture, unsigned width, unsigned height)
{
    if (!texture || !pipeline)
        return;
    D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->SetPipelineState(pipeline.Get());
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetGraphicsRootDescriptorTable(0, {texture});
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
void DrawOverlay(ID3D12GraphicsCommandList *cmd, UiTextureID texture, unsigned width, unsigned height)
{
    if (!texture || !overlayPipeline)
        return;
    D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->SetPipelineState(overlayPipeline.Get());
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetGraphicsRootDescriptorTable(0, {texture});
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
void DrawDebugViews(ID3D12GraphicsCommandList *cmd)
{
    for (auto &[hwnd, v] : views)
        v.used = false;
    std::size_t count = 0;
    const auto *targets = QtUi::TextureViews(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &target = targets[i];
        if (!target.Texture || !target.Width || !target.Height)
            continue;
        auto &v = views[target.Window];
        if (!v.swapchain)
        {
            ComPtr<IDXGIFactory4> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = target.Width;
            desc.Height = target.Height;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> chain;
            check(factory->CreateSwapChainForHwnd(DX12Context_GetCommandQueue(), target.Window, &desc, nullptr, nullptr,
                                                  &chain));
            check(chain.As(&v.swapchain));
            factory->MakeWindowAssociation(target.Window, DXGI_MWA_NO_ALT_ENTER);
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.NumDescriptors = 2;
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&v.rtv)));
        }
        if (v.width != target.Width || v.height != target.Height)
        {
            if (!DX12Context_WaitForGPU())
                throw std::runtime_error("Timed out resizing Qt debug viewport.");
            for (auto &b : v.buffers)
                b.Reset();
            check(v.swapchain->ResizeBuffers(2, target.Width, target.Height, DXGI_FORMAT_R8G8B8A8_UNORM, 0));
            auto handle = v.rtv->GetCPUDescriptorHandleForHeapStart();
            for (int b = 0; b < 2; ++b)
            {
                check(v.swapchain->GetBuffer(b, IID_PPV_ARGS(&v.buffers[b])));
                device->CreateRenderTargetView(v.buffers[b].Get(), nullptr, handle);
                handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            }
            v.width = target.Width;
            v.height = target.Height;
        }
        const UINT index = v.swapchain->GetCurrentBackBufferIndex();
        auto *buffer = v.buffers[index].Get();
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(buffer, D3D12_RESOURCE_STATE_PRESENT,
                                                            D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &barrier);
        auto handle = v.rtv->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += index * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        cmd->OMSetRenderTargets(1, &handle, FALSE, nullptr);
        Draw(cmd, target.Texture, target.Width, target.Height);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        cmd->ResourceBarrier(1, &barrier);
        v.used = true;
    }
}
void PresentDebugViews()
{
    for (auto &[hwnd, v] : views)
        if (v.used)
            check(v.swapchain->Present(0, 0));
}
} // namespace QtViewportRenderer
