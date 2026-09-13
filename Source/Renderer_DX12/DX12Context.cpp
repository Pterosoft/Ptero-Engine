#include "pch.h"
#include "DX12Helper.h"
#include "System/PteroLog.h"

#include "..\SDKs\Streamline\include\sl.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>

#pragma comment(lib, "..\\SDKs\\Streamline\\lib\\x64\\sl.interposer.lib")
#pragma comment(lib, "delayimp.lib")
#pragma comment(linker, "/DELAYLOAD:sl.interposer.dll")

using Microsoft::WRL::ComPtr;

namespace
{
    // Triple buffered: with two buffers the CPU reaches BeginFrame and immediately waits
    // on the back buffer the GPU is still drawing, so the two never overlap for long.
    // Must stay in step with EditorFrameCount in DX12RendererAPI.cpp.
    constexpr UINT FrameCount = 3;

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

    std::string FormatDeviceStatus(HRESULT hr)
    {
        std::ostringstream stream;
        stream << "HRESULT=" << hr;
        if (hr == DXGI_ERROR_DEVICE_REMOVED)
            stream << " (DXGI_ERROR_DEVICE_REMOVED)";
        else if (hr == DXGI_ERROR_DEVICE_HUNG)
            stream << " (DXGI_ERROR_DEVICE_HUNG)";
        else if (hr == DXGI_ERROR_DEVICE_RESET)
            stream << " (DXGI_ERROR_DEVICE_RESET)";
        else if (hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
            stream << " (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
        else if (hr == DXGI_ERROR_INVALID_CALL)
            stream << " (DXGI_ERROR_INVALID_CALL)";
        return stream.str();
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
        // Device removal is terminal: the device never comes back without being
        // recreated from scratch. Latching it lets the caller report the reason
        // once and stop, instead of failing every frame forever.
        bool DeviceRemoved = false;
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
        UINT SrvDescriptorCapacity = 512; // increased to accommodate TAA, GI, volumetric cloud, and other pass descriptors
        UINT NextAvailableSrvDescriptor = 1;
        std::array<ComPtr<ID3D12Resource>, FrameCount> RenderTargets;

        std::array<ComPtr<ID3D12CommandAllocator>, FrameCount> CommandAllocators;
        ComPtr<ID3D12GraphicsCommandList> CommandList;

        ComPtr<ID3D12Fence> Fence;
        std::array<UINT64, FrameCount> FenceValues{};
        HANDLE FenceEvent = nullptr;
        UINT64 NextFenceValue = 1;

        UINT FrameIndex = 0;
        bool CommandListOpen = false;
        bool StreamlineCoreInitialized = false;
        bool StreamlineInitialized = false;
    };

    DX12ContextState g_Context;
    HMODULE gStreamlineModule = nullptr;
    bool gOwnsStreamlineModule = false;

    std::string DescribeDeviceState(const char* prefix)
    {
        auto& ctx = g_Context;
        if (!ctx.Device)
        {
            return std::string(prefix) + " No D3D12 device is available.";
        }

        const HRESULT removeReason = ctx.Device->GetDeviceRemovedReason();
        if (SUCCEEDED(removeReason))
        {
            return std::string(prefix) + " Device is still reported as operational.";
        }

        return std::string(prefix) + " Device status: " + FormatDeviceStatus(removeReason) + ".";
    }

    // D3D12 has no name-for-op helper, so the ones a scene renderer can
    // plausibly hang inside are spelled out and the rest fall through to the
    // raw value.
    const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
    {
        switch (op)
        {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:                return "SetMarker";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:               return "BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:                 return "EndEvent";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:            return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:     return "DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:          return "ExecuteIndirect";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:                 return "Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:         return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:        return "CopyTextureRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:             return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:       return "ResolveSubresource";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:    return "ClearRenderTargetView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
        case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:    return "ClearDepthStencilView";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:          return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                  return "Present";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:             return "DispatchRays";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
                                                                return "BuildRaytracingAccelerationStructure";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE:
                                                                return "CopyRaytracingAccelerationStructure";
        default:                                                return "other";
        }
    }

    bool WaitForFenceValue(UINT64 fenceValue, DWORD timeoutMs)
    {
        auto& ctx = g_Context;

        if (!ctx.Fence || !ctx.FenceEvent)
        {
            return false;
        }

        if (ctx.Fence->GetCompletedValue() >= fenceValue)
        {
            return true;
        }

        // One auto-reset event serves every wait in the process, and a
        // SetEventOnCompletion registration outlives a wait that gives up on it.
        // So a wait that times out leaves its value armed; when the GPU reaches
        // that older value later, the event is signalled with nobody waiting and
        // stays signalled. Without the reset below, the next wait - for a
        // different, higher value - would return WAIT_OBJECT_0 immediately and
        // report the GPU finished when it had not, and the caller would go on to
        // reset an allocator and overwrite constants the GPU was still reading.
        //
        // Reset before arming, never after: arming can signal immediately when
        // the fence crosses the value in between, and that signal must survive.
        ResetEvent(ctx.FenceEvent);

        if (FAILED(ctx.Fence->SetEventOnCompletion(fenceValue, ctx.FenceEvent)))
        {
            return false;
        }

        // A wake-up can still come from an older registration on the shared
        // event, so treat the event as a hint and the fence as the answer.
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;)
        {
            if (ctx.Fence->GetCompletedValue() >= fenceValue)
            {
                return true;
            }

            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
            {
                return false;
            }

            if (WaitForSingleObject(ctx.FenceEvent, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            {
                // Timed out or failed. The fence is the only thing worth
                // believing, and our registration stays armed for the next wait.
                return ctx.Fence->GetCompletedValue() >= fenceValue;
            }
        }
    }

    bool IsFenceValueCompleted(UINT64 fenceValue)
    {
        auto& ctx = g_Context;
        return ctx.Fence && ctx.Fence->GetCompletedValue() >= fenceValue;
    }

    bool FlushGPU(DWORD timeoutMs)
    {
        auto& ctx = g_Context;

        if (!ctx.CommandQueue || !ctx.Fence || !ctx.FenceEvent)
        {
            return false;
        }

        const UINT64 fenceValue = ctx.NextFenceValue++;
        if (FAILED(ctx.CommandQueue->Signal(ctx.Fence.Get(), fenceValue)))
        {
            return false;
        }

        return WaitForFenceValue(fenceValue, timeoutMs);
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

    std::filesystem::path FindStreamlinePluginDirectory()
    {
        wchar_t executablePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath))) == 0)
        {
            return {};
        }

        std::filesystem::path currentPath = std::filesystem::path(executablePath).parent_path();
        while (!currentPath.empty())
        {
            const std::filesystem::path candidate = currentPath / "Source" / "SDKs" / "Streamline" / "bin" / "x64";
            if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate))
            {
                return candidate;
            }

            const std::filesystem::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
            {
                break;
            }

            currentPath = parentPath;
        }

        return {};
    }

    bool EnsureStreamlineCoreInitialized()
    {
        auto& ctx = g_Context;
        if (ctx.StreamlineCoreInitialized)
        {
            return true;
        }

        const std::filesystem::path pluginDirectory = FindStreamlinePluginDirectory();
        if (pluginDirectory.empty())
        {
            SetContextError("DX12Context_StreamlineInitialize failed to locate Source\\SDKs\\Streamline\\bin\\x64.");
            return false;
        }

        if (gStreamlineModule == nullptr)
        {
            gStreamlineModule = GetModuleHandleW(L"sl.interposer.dll");
            gOwnsStreamlineModule = false;

            if (gStreamlineModule == nullptr)
            {
                const std::filesystem::path interposerPath = pluginDirectory / "sl.interposer.dll";
                gStreamlineModule = LoadLibraryW(interposerPath.c_str());
                if (gStreamlineModule == nullptr)
                {
                    SetContextError("DX12Context_StreamlineInitialize failed to load sl.interposer.dll from the Streamline SDK.");
                    return false;
                }

                gOwnsStreamlineModule = true;
            }
        }

        const sl::Feature featuresToLoad[] = { sl::kFeatureDLSS };
        const wchar_t* pluginPaths[] = { pluginDirectory.c_str() };

        sl::Preferences preferences{};
        preferences.showConsole = false;
        preferences.logLevel = sl::LogLevel::eOff;
        preferences.pathsToPlugins = pluginPaths;
        preferences.numPathsToPlugins = static_cast<uint32_t>(std::size(pluginPaths));
        preferences.logMessageCallback = nullptr;
        preferences.flags = sl::PreferenceFlags::eAllowOTA
            | sl::PreferenceFlags::eLoadDownloadedPlugins
            | sl::PreferenceFlags::eUseFrameBasedResourceTagging
            | sl::PreferenceFlags::eUseDXGIFactoryProxy;
        preferences.featuresToLoad = featuresToLoad;
        preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(featuresToLoad));
        preferences.engine = sl::EngineType::eCustom;
        preferences.engineVersion = "Ptero-Engine";
        preferences.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
        preferences.renderAPI = sl::RenderAPI::eD3D12;

        const sl::Result initResult = slInit(preferences);
        if (initResult != sl::Result::eOk)
        {
            SetContextError("DX12Context_StreamlineInitialize failed to initialize Streamline.");
            return false;
        }

        ctx.StreamlineCoreInitialized = true;
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

            EnsureStreamlineCoreInitialized();

            ReportContextProgress(L"Creating DXGI factory...");
            ThrowIfFailedWithContext(CreateDXGIFactory2(0, IID_PPV_ARGS(&ctx.Factory)), "CreateDXGIFactory2");

            // DRED costs a little per command list and is the only thing that
            // turns a device hang from "something timed out" into a named GPU
            // operation plus the address of the page fault. Enable it before the
            // device exists, which is the only point at which it can be.
            {
                ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
                {
                    dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    dredSettings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                }
            }

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

            // Reserve a shader-visible descriptor heap that can serve both Dear Ui and editor-owned
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
            ctx.NextFenceValue = 1;
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

            const bool allocatorsReady = std::all_of(
                ctx.CommandAllocators.begin(), ctx.CommandAllocators.end(),
                [](const ComPtr<ID3D12CommandAllocator>& allocator) { return allocator != nullptr; });
            if (!(ctx.SwapChain && ctx.CommandList && allocatorsReady))
            {
                SetContextError("DX12Context_BeginFrame was called before the DX12 context finished initializing.");
                return false;
            }

            ctx.FrameIndex = ctx.SwapChain->GetCurrentBackBufferIndex();

            // Block until this back buffer's last frame retires. Reporting failure instead
            // turned the caller into a busy-wait that threw the frame away and immediately
            // rebuilt it, so the editor burned a full CPU core re-running per-frame work it
            // then discarded. Only a wait that actually times out is an error.
            const UINT64 fenceValue = ctx.FenceValues[ctx.FrameIndex];
            if (fenceValue != 0 && !WaitForFenceValue(fenceValue, 5000))
            {
                std::ostringstream stream;
                stream << "Timed out waiting for the previous frame to retire. targetFence=" << fenceValue;
                if (ctx.Fence)
                {
                    stream << ", completedFence=" << ctx.Fence->GetCompletedValue();
                }
                stream << ".";
                SetContextError(stream.str());
                return false;
            }

            ThrowIfFailedWithContext(ctx.CommandAllocators[ctx.FrameIndex]->Reset(), "ID3D12CommandAllocator::Reset");
            ThrowIfFailedWithContext(ctx.CommandList->Reset(ctx.CommandAllocators[ctx.FrameIndex].Get(), nullptr), "ID3D12GraphicsCommandList::Reset");
            ctx.CommandListOpen = true;

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
        try
        {
            auto& ctx = g_Context;

            if (!ctx.CommandList || !ctx.CommandQueue || !ctx.SwapChain)
            {
                SetContextError("DX12Context_EndFrame was called before the command list, command queue, or swap chain was ready.");
                return false;
            }

            ThrowIfFailedWithContext(ctx.CommandList->Close(), "ID3D12GraphicsCommandList::Close(frame)");
            ctx.CommandListOpen = false;

            ID3D12CommandList* commandLists[] = { ctx.CommandList.Get() };
            ctx.CommandQueue->ExecuteCommandLists(1, commandLists);

            const HRESULT deviceStatusAfterExecute = ctx.Device ? ctx.Device->GetDeviceRemovedReason() : S_OK;
            if (FAILED(deviceStatusAfterExecute))
            {
                ctx.DeviceRemoved = true;
                SetContextError(std::string("DX12Context_EndFrame detected a device problem after command execution. ")
                    + FormatDeviceStatus(deviceStatusAfterExecute));
                return false;
            }

            ThrowIfFailedWithContext(ctx.SwapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING), "IDXGISwapChain3::Present");

            const UINT64 fenceValue = ctx.NextFenceValue++;
            ThrowIfFailedWithContext(ctx.CommandQueue->Signal(ctx.Fence.Get(), fenceValue), "ID3D12CommandQueue::Signal");
            if (frameIndex < FrameCount)
            {
                ctx.FenceValues[frameIndex] = fenceValue;
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

    __declspec(dllexport) void __stdcall DX12Context_AbortFrame()
    {
        auto& ctx = g_Context;
        if (ctx.CommandList && ctx.CommandListOpen)
        {
            const HRESULT closeHr = ctx.CommandList->Close();
            ctx.CommandListOpen = false;
            if (FAILED(closeHr))
            {
                SetContextError("DX12Context_AbortFrame failed to close the in-progress command list. HRESULT: " + std::to_string(closeHr));
                ctx.CommandList.Reset();
                if (ctx.Device && ctx.CommandAllocators[ctx.FrameIndex])
                {
                    const HRESULT createHr = ctx.Device->CreateCommandList(
                        0,
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        ctx.CommandAllocators[ctx.FrameIndex].Get(),
                        nullptr,
                        IID_PPV_ARGS(&ctx.CommandList));
                    if (SUCCEEDED(createHr) && ctx.CommandList)
                    {
                        ctx.CommandList->Close();
                    }
                    else
                    {
                        SetContextError("DX12Context_AbortFrame failed to recreate the command list. HRESULT: " + std::to_string(createHr));
                    }
                }
            }
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

            if (!FlushGPU(2000))
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
            ctx.FenceValues.fill(0);
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

    __declspec(dllexport) bool __stdcall DX12Context_StreamlineInitialize()
    {
        auto& ctx = g_Context;
        if (ctx.StreamlineInitialized)
        {
            return true;
        }

        if (!EnsureStreamlineCoreInitialized())
        {
            return false;
        }

        if (ctx.Device == nullptr)
        {
            SetContextError("DX12Context_StreamlineInitialize called before the D3D12 device was created.");
            return false;
        }

        const sl::Result deviceResult = slSetD3DDevice(ctx.Device.Get());
        if (deviceResult != sl::Result::eOk)
        {
            if (ctx.StreamlineCoreInitialized)
            {
                slShutdown();
                ctx.StreamlineCoreInitialized = false;
            }
            SetContextError("DX12Context_StreamlineInitialize failed to register the D3D12 device with Streamline.");
            return false;
        }

        ctx.StreamlineInitialized = true;
        return true;
    }

    __declspec(dllexport) void __stdcall DX12Context_StreamlineShutdown()
    {
        auto& ctx = g_Context;
        if (!ctx.StreamlineInitialized)
        {
            return;
        }

        slShutdown();
        ctx.StreamlineCoreInitialized = false;
        ctx.StreamlineInitialized = false;
        if (gStreamlineModule != nullptr && gOwnsStreamlineModule)
        {
            FreeLibrary(gStreamlineModule);
        }

        gStreamlineModule = nullptr;
        gOwnsStreamlineModule = false;
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

    // Everything DRED knows about why the device went away: the last GPU
    // operations each command list actually completed, and the page fault that
    // killed it if there was one. This is the difference between "the device
    // hung" and knowing which pass and which address did it.
    __declspec(dllexport) void __stdcall DX12Context_LogDeviceRemovedDiagnostics()
    {
        auto& ctx = g_Context;
        if (!ctx.Device) return;

        const HRESULT reason = ctx.Device->GetDeviceRemovedReason();
        PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                         "Device removed. %s", FormatDeviceStatus(reason).c_str());

        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        if (FAILED(ctx.Device->QueryInterface(IID_PPV_ARGS(&dred))))
        {
            PteroLog::Write(PteroLog::Level::Warning, "Device",
                            "DRED is not available on this device, so there are no breadcrumbs to show.");
            return;
        }

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            int listIndex = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
                 node != nullptr; node = node->pNext, ++listIndex)
            {
                const UINT completed = (node->pLastBreadcrumbValue != nullptr) ? *node->pLastBreadcrumbValue : 0;
                // A node that completed everything it was given did not hang;
                // the one that stopped part-way through is the culprit.
                if (completed == node->BreadcrumbCount) continue;

                char listName[128] = {};
                if (node->pCommandListDebugNameA != nullptr)
                    std::snprintf(listName, sizeof(listName), "%s", node->pCommandListDebugNameA);

                PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                                 "Command list %d ('%s') stopped after %u of %u operations.",
                                 listIndex, listName[0] ? listName : "unnamed",
                                 completed, node->BreadcrumbCount);

                // The few operations either side of where it stopped are what
                // identify the pass.
                const UINT first = (completed > 4) ? completed - 4 : 0;
                const UINT last = (std::min)(node->BreadcrumbCount, completed + 4);
                for (UINT op = first; op < last; ++op)
                {
                    PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                                     "    %s op %u: %s",
                                     op == completed ? "->" : "  ", op,
                                     BreadcrumbOpName(node->pCommandHistory[op]));
                }
            }
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
        {
            PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                             "Page fault at GPU virtual address 0x%llx.",
                             static_cast<unsigned long long>(pageFault.PageFaultVA));
            for (const D3D12_DRED_ALLOCATION_NODE* node = pageFault.pHeadRecentFreedAllocationNode;
                 node != nullptr; node = node->pNext)
            {
                PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                                 "    recently freed: %s",
                                 node->ObjectNameA ? node->ObjectNameA : "(unnamed)");
            }
        }
    }

    __declspec(dllexport) bool __stdcall DX12Context_IsDeviceRemoved()
    {
        return g_Context.DeviceRemoved;
    }

    __declspec(dllexport) bool __stdcall DX12Context_WaitForGPU()
    {
        // Flush the GPU command queue and wait for all in-flight work to complete.
        // Called before releasing resources that may still be referenced by the GPU.
        return FlushGPU(2000);
    }

    __declspec(dllexport) void __stdcall DX12Context_Shutdown()
    {
        try
        {
            auto& ctx = g_Context;

            if (ctx.StreamlineInitialized)
            {
                DX12Context_StreamlineShutdown();
            }

            // Attempt a short flush; avoid blocking forever on application exit.
            if (ctx.CommandQueue && ctx.Fence)
            {
                FlushGPU(250);
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
