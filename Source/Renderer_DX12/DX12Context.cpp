#include "pch.h"
#include "DX12Helper.h"

#include <array>
#include <exception>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT FrameCount = 2;

    std::string gLastContextError;
    using ContextProgressFn = void(__stdcall*)(const wchar_t*);
    ContextProgressFn gContextProgressCallback = nullptr;

    void ReportContextProgress(const wchar_t* message)
    {
        if (gContextProgressCallback)
            gContextProgressCallback(message);
    }

    void SetContextError(const std::string& errorMessage)
    {
        gLastContextError = errorMessage;
        OutputDebugStringA(gLastContextError.c_str());
        OutputDebugStringA("\n");
    }

    void ThrowIfFailedWithContext(HRESULT hr, const char* operation)
    {
        if (FAILED(hr))
        {
            throw std::runtime_error(std::string(operation) + " failed. HRESULT: " + std::to_string(hr));
        }
    }

    struct DX12ContextState
    {
        HWND WindowHandle = nullptr;
        UINT Width = 1280;
        UINT Height = 720;

        ComPtr<IDXGIFactory4> Factory;
        ComPtr<ID3D12Device> Device;
        ComPtr<ID3D12CommandQueue> CommandQueue;
        ComPtr<IDXGISwapChain3> SwapChain;

        ComPtr<ID3D12DescriptorHeap> RtvHeap;
        ComPtr<ID3D12DescriptorHeap> SrvHeap;
        UINT RtvDescriptorSize = 0;
        UINT SrvDescriptorSize = 0;
        UINT SrvDescriptorCapacity = 256; // increased to accommodate TAA, GI, and other pass descriptors
        UINT NextAvailableSrvDescriptor = 1;
        std::array<ComPtr<ID3D12Resource>, FrameCount> RenderTargets;

        std::array<ComPtr<ID3D12CommandAllocator>, FrameCount> CommandAllocators;
        ComPtr<ID3D12GraphicsCommandList> CommandList;

        ComPtr<ID3D12Fence> Fence;
        std::array<UINT64, FrameCount> FenceValues{};
        HANDLE FenceEvent = nullptr;

        UINT FrameIndex = 0;
    };

    DX12ContextState g_Context;

    bool WaitForGPU(DWORD timeoutMs)
    {
        auto& ctx = g_Context;

        if (!ctx.CommandQueue || !ctx.Fence || !ctx.FenceEvent)
        {
            return false;
        }

        const UINT64 fenceToWaitFor = ++ctx.FenceValues[ctx.FrameIndex];
        if (FAILED(ctx.CommandQueue->Signal(ctx.Fence.Get(), fenceToWaitFor)))
        {
            return false;
        }

        if (ctx.Fence->GetCompletedValue() < fenceToWaitFor)
        {
            if (FAILED(ctx.Fence->SetEventOnCompletion(fenceToWaitFor, ctx.FenceEvent)))
            {
                return false;
            }

            return WaitForSingleObject(ctx.FenceEvent, timeoutMs) == WAIT_OBJECT_0;
        }

        return true;
    }

    bool RecreateSwapChainRenderTargets()
    {
        auto& ctx = g_Context;
        if (!(ctx.Device && ctx.SwapChain && ctx.RtvHeap))
        {
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = ctx.RtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < FrameCount; ++i)
        {
            ctx.RenderTargets[i].Reset();
        }

        for (UINT i = 0; i < FrameCount; ++i)
        {
            ThrowIfFailedWithContext(ctx.SwapChain->GetBuffer(i, IID_PPV_ARGS(&ctx.RenderTargets[i])), "IDXGISwapChain3::GetBuffer");
            ctx.Device->CreateRenderTargetView(ctx.RenderTargets[i].Get(), nullptr, rtvHandle);
            rtvHandle.ptr += ctx.RtvDescriptorSize;
        }

        return true;
    }
}

extern "C"
{
    __declspec(dllexport) void __stdcall DX12Context_SetProgressCallback(void(__stdcall* callback)(const wchar_t*))
    {
        gContextProgressCallback = callback;
    }

    __declspec(dllexport) bool __stdcall DX12Context_Initialize(HWND windowHandle)
    {
        if (windowHandle == nullptr)
        {
            SetContextError("DX12Context_Initialize received a null window handle.");
            return false;
        }

        try
        {
            auto& ctx = g_Context;
            gLastContextError.clear();
            ctx.WindowHandle = windowHandle;

            RECT rect{};
            GetClientRect(windowHandle, &rect);
            ctx.Width = static_cast<UINT>(rect.right - rect.left);
            ctx.Height = static_cast<UINT>(rect.bottom - rect.top);
            if (ctx.Width == 0) ctx.Width = 1280;
            if (ctx.Height == 0) ctx.Height = 720;

            ReportContextProgress(L"Creating DXGI factory...");
            ThrowIfFailedWithContext(CreateDXGIFactory2(0, IID_PPV_ARGS(&ctx.Factory)), "CreateDXGIFactory2");

            ReportContextProgress(L"Creating D3D12 device...");
            ThrowIfFailedWithContext(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&ctx.Device)), "D3D12CreateDevice");

            ReportContextProgress(L"Creating D3D12 command queue...");
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            ThrowIfFailedWithContext(ctx.Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ctx.CommandQueue)), "ID3D12Device::CreateCommandQueue");

            ReportContextProgress(L"Creating swap chain...");
            DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
            swapChainDesc.BufferCount = FrameCount;
            swapChainDesc.Width = ctx.Width;
            swapChainDesc.Height = ctx.Height;
            swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swapChainDesc.SampleDesc.Count = 1;
            swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

            ComPtr<IDXGISwapChain1> swapChain;
            ThrowIfFailedWithContext(ctx.Factory->CreateSwapChainForHwnd(
                ctx.CommandQueue.Get(),
                windowHandle,
                &swapChainDesc,
                nullptr,
                nullptr,
                &swapChain), "IDXGIFactory4::CreateSwapChainForHwnd");
            ThrowIfFailedWithContext(swapChain.As(&ctx.SwapChain), "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)");
            ThrowIfFailedWithContext(ctx.Factory->MakeWindowAssociation(windowHandle, DXGI_MWA_NO_ALT_ENTER), "IDXGIFactory4::MakeWindowAssociation");

            ctx.FrameIndex = ctx.SwapChain->GetCurrentBackBufferIndex();

            ReportContextProgress(L"Creating descriptor heaps...");
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
            rtvHeapDesc.NumDescriptors = FrameCount;
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            ThrowIfFailedWithContext(ctx.Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&ctx.RtvHeap)), "CreateDescriptorHeap(RTV)");

            // Reserve a shader-visible descriptor heap that can serve both Dear ImGui and editor-owned
            // render targets, such as the off-screen scene texture shown in the viewport panel.
            D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
            srvHeapDesc.NumDescriptors = ctx.SrvDescriptorCapacity;
            srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ThrowIfFailedWithContext(ctx.Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&ctx.SrvHeap)), "CreateDescriptorHeap(SRV)");

            ctx.RtvDescriptorSize = ctx.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            ctx.SrvDescriptorSize = ctx.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            ctx.NextAvailableSrvDescriptor = 1;

            ReportContextProgress(L"Creating swap chain render targets...");
            ThrowIfFailedWithContext(RecreateSwapChainRenderTargets() ? S_OK : E_FAIL, "RecreateSwapChainRenderTargets");

            ReportContextProgress(L"Creating command allocators and command list...");
            for (UINT i = 0; i < FrameCount; ++i)
            {
                ThrowIfFailedWithContext(ctx.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.CommandAllocators[i])), "CreateCommandAllocator");
            }

            ThrowIfFailedWithContext(ctx.Device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                ctx.CommandAllocators[ctx.FrameIndex].Get(),
                nullptr,
                IID_PPV_ARGS(&ctx.CommandList)), "CreateCommandList");
            ThrowIfFailedWithContext(ctx.CommandList->Close(), "ID3D12GraphicsCommandList::Close(initial)");

            ReportContextProgress(L"Creating GPU synchronization fence...");
            ThrowIfFailedWithContext(ctx.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx.Fence)), "CreateFence");
            ctx.FenceValues.fill(0);
            ctx.FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (ctx.FenceEvent == nullptr)
            {
                SetContextError("CreateEvent failed while creating the DX12 fence event.");
                return false;
            }

            return true;
        }
        catch (const std::exception& exception)
        {
            SetContextError(std::string("DX12Context_Initialize failed: ") + exception.what());
            return false;
        }
        catch (...)
        {
            SetContextError("DX12Context_Initialize failed with an unknown exception.");
            return false;
        }
    }

    __declspec(dllexport) bool __stdcall DX12Context_BeginFrame(
        ID3D12GraphicsCommandList** commandList,
        ID3D12Resource** backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandle,
        UINT* frameIndex)
    {
        if (commandList == nullptr || backBuffer == nullptr || rtvHandle == nullptr || frameIndex == nullptr)
        {
            SetContextError("DX12Context_BeginFrame received one or more null output pointers.");
            return false;
        }

        try
        {
            auto& ctx = g_Context;

            if (!(ctx.SwapChain && ctx.CommandList && ctx.CommandAllocators[0] && ctx.CommandAllocators[1]))
            {
                SetContextError("DX12Context_BeginFrame was called before the DX12 context finished initializing.");
                return false;
            }

            // Query and cache the initial back buffer index for per-frame resource lookup.
            ctx.FrameIndex = ctx.SwapChain->GetCurrentBackBufferIndex();

            ThrowIfFailedWithContext(ctx.CommandAllocators[ctx.FrameIndex]->Reset(), "ID3D12CommandAllocator::Reset");
            ThrowIfFailedWithContext(ctx.CommandList->Reset(ctx.CommandAllocators[ctx.FrameIndex].Get(), nullptr), "ID3D12GraphicsCommandList::Reset");

            *commandList = ctx.CommandList.Get();
            *backBuffer = ctx.RenderTargets[ctx.FrameIndex].Get();

            D3D12_CPU_DESCRIPTOR_HANDLE handle = ctx.RtvHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += ctx.FrameIndex * ctx.RtvDescriptorSize;
            *rtvHandle = handle;
            *frameIndex = ctx.FrameIndex;

            return true;
        }
        catch (const std::exception& exception)
        {
            SetContextError(std::string("DX12Context_BeginFrame failed: ") + exception.what());
            return false;
        }
        catch (...)
        {
            SetContextError("DX12Context_BeginFrame failed with an unknown exception.");
            return false;
        }
    }

    __declspec(dllexport) bool __stdcall DX12Context_EndFrame(UINT frameIndex)
    {
        UNREFERENCED_PARAMETER(frameIndex);

        try
        {
            auto& ctx = g_Context;

            if (!ctx.CommandList || !ctx.CommandQueue || !ctx.SwapChain)
            {
                SetContextError("DX12Context_EndFrame was called before the command list, command queue, or swap chain was ready.");
                return false;
            }

            ThrowIfFailedWithContext(ctx.CommandList->Close(), "ID3D12GraphicsCommandList::Close(frame)");

            ID3D12CommandList* commandLists[] = { ctx.CommandList.Get() };
            ctx.CommandQueue->ExecuteCommandLists(1, commandLists);

            ThrowIfFailedWithContext(ctx.SwapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING), "IDXGISwapChain3::Present");

            // Use a bounded GPU wait to avoid hanging editor shutdown.
            if (!WaitForGPU(2000))
            {
                SetContextError("WaitForGPU timed out or failed after presenting the frame.");
                return false;
            }

            return true;
        }
        catch (const std::exception& exception)
        {
            SetContextError(std::string("DX12Context_EndFrame failed: ") + exception.what());
            return false;
        }
        catch (...)
        {
            SetContextError("DX12Context_EndFrame failed with an unknown exception.");
            return false;
        }
    }

    __declspec(dllexport) const char* __stdcall DX12Context_GetLastError()
    {
        return gLastContextError.empty() ? nullptr : gLastContextError.c_str();
    }

    __declspec(dllexport) bool __stdcall DX12Context_Resize(UINT width, UINT height)
    {
        if (width == 0 || height == 0)
        {
            return true;
        }

        try
        {
            auto& ctx = g_Context;
            if (!(ctx.Device && ctx.SwapChain && ctx.CommandQueue && ctx.Fence))
            {
                SetContextError("DX12Context_Resize was called before the swap chain was initialized.");
                return false;
            }

            if (ctx.Width == width && ctx.Height == height)
            {
                return true;
            }

            if (!WaitForGPU(2000))
            {
                SetContextError("WaitForGPU timed out before resizing the swap chain.");
                return false;
            }

            for (auto& renderTarget : ctx.RenderTargets)
            {
                renderTarget.Reset();
            }

            ThrowIfFailedWithContext(
                ctx.SwapChain->ResizeBuffers(FrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING),
                "IDXGISwapChain3::ResizeBuffers");

            ctx.Width = width;
            ctx.Height = height;
            ctx.FrameIndex = ctx.SwapChain->GetCurrentBackBufferIndex();
            ThrowIfFailedWithContext(RecreateSwapChainRenderTargets() ? S_OK : E_FAIL, "RecreateSwapChainRenderTargets");
            return true;
        }
        catch (const std::exception& exception)
        {
            SetContextError(std::string("DX12Context_Resize failed: ") + exception.what());
            return false;
        }
        catch (...)
        {
            SetContextError("DX12Context_Resize failed with an unknown exception.");
            return false;
        }
    }

    __declspec(dllexport) bool __stdcall DX12Context_GetRenderSize(UINT* width, UINT* height)
    {
        if (width == nullptr || height == nullptr)
        {
            return false;
        }

        *width = g_Context.Width;
        *height = g_Context.Height;
        return true;
    }

    __declspec(dllexport) HWND __stdcall DX12Context_GetWindowHandle()
    {
        return g_Context.WindowHandle;
    }

    __declspec(dllexport) ID3D12Device* __stdcall DX12Context_GetDevice()
    {
        return g_Context.Device.Get();
    }

    __declspec(dllexport) ID3D12CommandQueue* __stdcall DX12Context_GetCommandQueue()
    {
        return g_Context.CommandQueue.Get();
    }

    __declspec(dllexport) ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap()
    {
        return g_Context.SrvHeap.Get();
    }

    __declspec(dllexport) D3D12_CPU_DESCRIPTOR_HANDLE __stdcall DX12Context_GetSrvDescriptorCpuHandle()
    {
        if (!g_Context.SrvHeap)
        {
            return D3D12_CPU_DESCRIPTOR_HANDLE{};
        }

        return g_Context.SrvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    __declspec(dllexport) D3D12_GPU_DESCRIPTOR_HANDLE __stdcall DX12Context_GetSrvDescriptorGpuHandle()
    {
        if (!g_Context.SrvHeap)
        {
            return D3D12_GPU_DESCRIPTOR_HANDLE{};
        }

        return g_Context.SrvHeap->GetGPUDescriptorHandleForHeapStart();
    }

    __declspec(dllexport) bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle)
    {
        if (cpuHandle == nullptr || gpuHandle == nullptr)
        {
            return false;
        }

        auto& ctx = g_Context;
        if (!ctx.SrvHeap || ctx.NextAvailableSrvDescriptor >= ctx.SrvDescriptorCapacity)
        {
            return false;
        }

        *cpuHandle = ctx.SrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle->ptr += static_cast<SIZE_T>(ctx.NextAvailableSrvDescriptor) * ctx.SrvDescriptorSize;

        *gpuHandle = ctx.SrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle->ptr += static_cast<UINT64>(ctx.NextAvailableSrvDescriptor) * ctx.SrvDescriptorSize;

        ++ctx.NextAvailableSrvDescriptor;
        return true;
    }

    // Same as DX12Context_AllocateSrvDescriptor but also returns the zero-based
    // descriptor heap index so shaders can use ResourceDescriptorHeap[index].
    __declspec(dllexport) bool __stdcall DX12Context_AllocateSrvDescriptorWithIndex(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle,
        UINT*                        outIndex)
    {
        auto& ctx = g_Context;
        if (!ctx.SrvHeap || ctx.NextAvailableSrvDescriptor >= ctx.SrvDescriptorCapacity)
            return false;

        UINT idx = ctx.NextAvailableSrvDescriptor;
        *cpuHandle = ctx.SrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle->ptr += static_cast<SIZE_T>(idx) * ctx.SrvDescriptorSize;
        *gpuHandle = ctx.SrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle->ptr += static_cast<UINT64>(idx) * ctx.SrvDescriptorSize;
        if (outIndex) *outIndex = idx;
        ++ctx.NextAvailableSrvDescriptor;
        return true;
    }

    __declspec(dllexport) UINT __stdcall DX12Context_GetSrvDescriptorSize()
    {
        return g_Context.SrvDescriptorSize;
    }

    __declspec(dllexport) bool __stdcall DX12Context_WaitForGPU()
    {
        // Flush the GPU command queue and wait for all in-flight work to complete.
        // Called before releasing resources that may still be referenced by the GPU.
        return WaitForGPU(2000);
    }

    __declspec(dllexport) void __stdcall DX12Context_Shutdown()
    {
        try
        {
            auto& ctx = g_Context;

            // Attempt a short flush; avoid blocking forever on application exit.
            if (ctx.CommandQueue && ctx.Fence)
            {
                WaitForGPU(250);
            }

            if (ctx.FenceEvent)
            {
                CloseHandle(ctx.FenceEvent);
                ctx.FenceEvent = nullptr;
            }

            ctx.CommandList.Reset();
            for (auto& allocator : ctx.CommandAllocators)
            {
                allocator.Reset();
            }

            for (auto& target : ctx.RenderTargets)
            {
                target.Reset();
            }

            ctx.RtvHeap.Reset();
            ctx.SrvHeap.Reset();
            ctx.SwapChain.Reset();
            ctx.Fence.Reset();
            ctx.CommandQueue.Reset();
            ctx.Device.Reset();
            ctx.Factory.Reset();
            ctx.WindowHandle = nullptr;
            ctx.FrameIndex = 0;
            ctx.NextAvailableSrvDescriptor = 1;
        }
        catch (...)
        {
            // Never allow shutdown exceptions to leak across DLL boundaries.
        }
    }
}
