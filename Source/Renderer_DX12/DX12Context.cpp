#include "pch.h"
#include "DX12Helper.h"
#include "System/PteroLog.h"
#include "FfxLoader.h"
#include "FidelityFX-SDK-2.3.0/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

#include "..\SDKs\Streamline\include\sl.h"

#include <d3d12sdklayers.h>

#include <algorithm>
#include <array>
#include <vector>
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
        // One device and renderer, two retained presentation surfaces. Parking the
        // editor chain preserves its last frame while Play owns the renderer.
        ComPtr<IDXGISwapChain3> ParkedSwapChain;
        HWND ParkedWindowHandle = nullptr;
        // Set when the chain beside it is AMD FSR's frame generation proxy rather
        // than a plain DXGI chain. The proxy is destroyed through this context.
        ffxContext SwapChainContext = nullptr;
        ffxContext ParkedSwapChainContext = nullptr;

        ComPtr<ID3D12DescriptorHeap> RtvHeap;
        ComPtr<ID3D12DescriptorHeap> SrvHeap;
        UINT RtvDescriptorSize = 0;
        UINT SrvDescriptorSize = 0;
        // Raised from 512 when FidelityFX SSSR arrived: it alone holds 47 slots (a depth pyramid
        // of per-level UAVs plus ping-ponged denoiser history). Slots are never freed, so the
        // margin also has to absorb every pass that is created lazily.
        UINT SrvDescriptorCapacity = 1024;
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

        // Render latency: from a frame starting (the renderer samples input right after
        // BeginFrame) to the GPU finishing it. The GPU writes a timestamp at the end of each
        // frame; it is read back once that frame's fence has passed and mapped onto the CPU
        // clock with the queue's clock calibration.
        ComPtr<ID3D12QueryHeap> TimestampHeap;
        ComPtr<ID3D12Resource> TimestampReadback;
        const UINT64* MappedTimestamps = nullptr;
        UINT64 GpuTimestampFrequency = 0;
        std::array<LONGLONG, FrameCount> FrameStartQpc{};
        std::array<bool, FrameCount> TimestampPending{};
        double RenderLatencyMilliseconds = 0.0;
    };

    DX12ContextState g_Context;

    // The shared heap is a bump allocator that never frees. Running out used to be
    // silent, and a pass that then kept its stale descriptors hung the GPU.
    void LogSrvHeapExhausted()
    {
        static bool logged = false;
        if (logged)
            return;
        logged = true;
        PteroLog::Writef(PteroLog::Level::Error, "Device",
            "The shared shader-visible descriptor heap is full (%u slots). A pass that "
            "re-allocates descriptors on every resize is leaking them.",
            g_Context.SrvDescriptorCapacity);
    }

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

    DXGI_SWAP_CHAIN_DESC1 MakeSwapChainDesc(UINT width, UINT height)
    {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.BufferCount = FrameCount;
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.SampleDesc.Count = 1;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        return desc;
    }

    // The proxy holds a reference to the chain it wraps, so the context goes first
    // and the application's reference is the last one released.
    void DestroySwapChain(ComPtr<IDXGISwapChain3>& swapChain, ffxContext& swapChainContext)
    {
        if (swapChainContext != nullptr)
        {
            if (const ffxFunctions* ffx = FfxLoader::Get())
                ffx->DestroyContext(&swapChainContext, nullptr);
            swapChainContext = nullptr;
        }
        swapChain.Reset();
    }

    // Replaces the active chain for the same window with a plain DXGI chain or with
    // FSR's frame generation proxy. A window can have only one flip-model chain, so
    // the old one is fully released before the new one is created.
    void ReplaceActiveSwapChain(bool frameGenerationProxy)
    {
        auto& ctx = g_Context;
        DXGI_SWAP_CHAIN_DESC1 current{};
        ThrowIfFailedWithContext(ctx.SwapChain->GetDesc1(&current), "IDXGISwapChain3::GetDesc1");
        // Only the size is carried over. The proxy reports its own flags, and the
        // chain that replaces it must be exactly what the engine would have made.
        DXGI_SWAP_CHAIN_DESC1 desc = MakeSwapChainDesc(current.Width, current.Height);

        for (auto& target : ctx.RenderTargets)
            target.Reset();
        DestroySwapChain(ctx.SwapChain, ctx.SwapChainContext);

        if (frameGenerationProxy)
        {
            const ffxFunctions* ffx = FfxLoader::Get();
            IDXGISwapChain4* proxy = nullptr;
            ffxReturnCode_t result = FFX_API_RETURN_ERROR;
            if (ffx != nullptr)
            {
                ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version{};
                version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
                version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

                ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
                create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
                create.header.pNext = &version.header;
                create.swapchain = &proxy;
                create.hwnd = ctx.WindowHandle;
                create.desc = &desc;
                create.fullscreenDesc = nullptr;
                create.dxgiFactory = ctx.Factory.Get();
                create.gameQueue = ctx.CommandQueue.Get();
                result = ffx->CreateContext(&ctx.SwapChainContext, &create.header, nullptr);
            }

            if (result == FFX_API_RETURN_OK && proxy != nullptr)
            {
                // The context hands back a reference the caller owns.
                ctx.SwapChain.Attach(static_cast<IDXGISwapChain3*>(proxy));
                PteroLog::Write(PteroLog::Level::Info, "FSR", "Frame generation swap chain installed.");
            }
            else
            {
                if (proxy != nullptr)
                    proxy->Release();
                if (ctx.SwapChainContext != nullptr && ffx != nullptr)
                    ffx->DestroyContext(&ctx.SwapChainContext, nullptr);
                ctx.SwapChainContext = nullptr;
                SetContextError("Could not create the FSR frame generation swap chain (ffxCreateContext returned "
                    + std::to_string(result) + "). "
                    + (FfxLoader::GetLastError() ? FfxLoader::GetLastError() : ""));
                PteroLog::Write(PteroLog::Level::Error, "FSR", gLastContextError.c_str());
            }
        }

        if (!ctx.SwapChain)
        {
            ComPtr<IDXGISwapChain1> created;
            ThrowIfFailedWithContext(ctx.Factory->CreateSwapChainForHwnd(
                ctx.CommandQueue.Get(), ctx.WindowHandle, &desc, nullptr, nullptr, &created),
                "CreateSwapChainForHwnd(replace)");
            ThrowIfFailedWithContext(created.As(&ctx.SwapChain), "QueryInterface(replacement swap chain)");
        }

        ThrowIfFailedWithContext(ctx.Factory->MakeWindowAssociation(ctx.WindowHandle, DXGI_MWA_NO_ALT_ENTER),
            "MakeWindowAssociation(replace)");
        ctx.FrameIndex = ctx.SwapChain->GetCurrentBackBufferIndex();
        ctx.FenceValues.fill(0);
        ThrowIfFailedWithContext(RecreateSwapChainRenderTargets() ? S_OK : E_FAIL, "RecreateSwapChainRenderTargets");
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

            // GPU-based validation, opt in with -gpuvalidation on the command line.
            //
            // DRED is a post-mortem: it names the operation that was running and the
            // address that faulted, but not who owned that address or which draw
            // read it. GBV rewrites every shader to bounds-check its accesses, so an
            // index buffer read past its end, or a descriptor pointing at a released
            // resource, is reported at the draw that did it, by name, while the
            // device is still alive. It costs far too much to leave on - hence the
            // switch - but it is the tool for a page fault whose address is in freed
            // memory and whose owner nothing has named.
            {
                const wchar_t* commandLine = GetCommandLineW();
                const bool wantGpuValidation =
                    commandLine != nullptr && wcsstr(commandLine, L"-gpuvalidation") != nullptr;
                ComPtr<ID3D12Debug1> debugController;
                if (wantGpuValidation && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
                {
                    debugController->EnableDebugLayer();
                    debugController->SetEnableGPUBasedValidation(TRUE);
                    PteroLog::Writef(PteroLog::Level::Warning, "Device",
                        "GPU-based validation is ON (-gpuvalidation). Expect a large slowdown; "
                        "validation failures are reported to the debug output.");
                }
            }

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

            // Validation messages go to the debugger's output window, which is no use
            // to anyone running the editor normally and reporting what they saw. Route
            // them into the session log instead, so a validation failure arrives with
            // the rest of the evidence. Only worth doing when validation is on: with
            // the debug layer off this queue stays empty.
            {
                ComPtr<ID3D12InfoQueue1> infoQueue;
                if (SUCCEEDED(ctx.Device.As(&infoQueue)))
                {
                    DWORD callbackCookie = 0;
                    infoQueue->RegisterMessageCallback(
                        [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                           D3D12_MESSAGE_ID id, LPCSTR description, void*)
                        {
                            const PteroLog::Level level =
                                (severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                                 severity == D3D12_MESSAGE_SEVERITY_ERROR)
                                    ? PteroLog::Level::Error
                                    : PteroLog::Level::Warning;
                            PteroLog::Writef(level, "D3D12", "[%d] %s",
                                             static_cast<int>(id),
                                             description ? description : "(no description)");
                        },
                        D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &callbackCookie);
                }
            }

            ReportContextProgress(L"Creating D3D12 command queue...");
            D3D12_COMMAND_QUEUE_DESC queueDesc{};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            ThrowIfFailedWithContext(ctx.Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ctx.CommandQueue)), "ID3D12Device::CreateCommandQueue");

            // Non-fatal: without timestamps the render latency simply reads 0.
            {
                D3D12_QUERY_HEAP_DESC queryHeapDesc{};
                queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                queryHeapDesc.Count = FrameCount;
                const D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC readbackDesc{};
                readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                readbackDesc.Width = sizeof(UINT64) * FrameCount;
                readbackDesc.Height = 1;
                readbackDesc.DepthOrArraySize = 1;
                readbackDesc.MipLevels = 1;
                readbackDesc.SampleDesc.Count = 1;
                readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                void* mapped = nullptr;
                if (SUCCEEDED(ctx.CommandQueue->GetTimestampFrequency(&ctx.GpuTimestampFrequency))
                    && SUCCEEDED(ctx.Device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&ctx.TimestampHeap)))
                    && SUCCEEDED(ctx.Device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ctx.TimestampReadback)))
                    && SUCCEEDED(ctx.TimestampReadback->Map(0, nullptr, &mapped)))
                {
                    ctx.MappedTimestamps = static_cast<const UINT64*>(mapped);
                }
                else
                {
                    ctx.TimestampHeap.Reset();
                    ctx.TimestampReadback.Reset();
                }
            }

            ReportContextProgress(L"Creating swap chain...");
            DXGI_SWAP_CHAIN_DESC1 swapChainDesc = MakeSwapChainDesc(ctx.Width, ctx.Height);

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

            // This slot's previous frame has retired, so its end timestamp is readable.
            if (ctx.TimestampPending[ctx.FrameIndex] && ctx.MappedTimestamps && ctx.GpuTimestampFrequency)
            {
                UINT64 gpuNow = 0, cpuNow = 0;
                LARGE_INTEGER qpcFrequency{};
                if (SUCCEEDED(ctx.CommandQueue->GetClockCalibration(&gpuNow, &cpuNow)) && QueryPerformanceFrequency(&qpcFrequency))
                {
                    const UINT64 gpuEnd = ctx.MappedTimestamps[ctx.FrameIndex];
                    const double gpuAgoSeconds = gpuNow >= gpuEnd
                        ? static_cast<double>(gpuNow - gpuEnd) / static_cast<double>(ctx.GpuTimestampFrequency)
                        : 0.0;
                    const double endQpc = static_cast<double>(cpuNow) - gpuAgoSeconds * static_cast<double>(qpcFrequency.QuadPart);
                    const double latency = (endQpc - static_cast<double>(ctx.FrameStartQpc[ctx.FrameIndex]))
                        * 1000.0 / static_cast<double>(qpcFrequency.QuadPart);
                    if (latency > 0.0 && latency < 2000.0)
                        ctx.RenderLatencyMilliseconds = latency;
                }
                ctx.TimestampPending[ctx.FrameIndex] = false;
            }
            LARGE_INTEGER frameStart{};
            QueryPerformanceCounter(&frameStart);
            ctx.FrameStartQpc[ctx.FrameIndex] = frameStart.QuadPart;

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

            const bool timestamped = ctx.TimestampHeap && frameIndex < FrameCount;
            if (timestamped)
            {
                ctx.CommandList->EndQuery(ctx.TimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex);
                ctx.CommandList->ResolveQueryData(ctx.TimestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex, 1,
                    ctx.TimestampReadback.Get(), sizeof(UINT64) * frameIndex);
            }

            ThrowIfFailedWithContext(ctx.CommandList->Close(), "ID3D12GraphicsCommandList::Close(frame)");
            ctx.CommandListOpen = false;

            ID3D12CommandList* commandLists[] = { ctx.CommandList.Get() };
            ctx.CommandQueue->ExecuteCommandLists(1, commandLists);
            if (timestamped)
                ctx.TimestampPending[frameIndex] = true;

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

    // Most recent render latency (frame start to GPU completion), in milliseconds.
    __declspec(dllexport) double __stdcall DX12Context_GetRenderLatencyMilliseconds()
    {
        return g_Context.RenderLatencyMilliseconds;
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

    __declspec(dllexport) bool __stdcall DX12Context_SetPresentationWindow(HWND windowHandle)
    {
        auto& ctx = g_Context;
        if (windowHandle == ctx.WindowHandle) return true;
        try
        {
            if (!IsWindow(windowHandle) || !ctx.SwapChain || ctx.CommandListOpen)
                throw std::runtime_error("Presentation can only switch between frames to a valid window.");
            if (!FlushGPU(2000))
                throw std::runtime_error("GPU wait timed out before switching presentation windows.");

            ComPtr<IDXGISwapChain3> next;
            ffxContext nextContext = nullptr;
            if (ctx.ParkedWindowHandle == windowHandle)
            {
                next = ctx.ParkedSwapChain;
                nextContext = ctx.ParkedSwapChainContext;
            }
            else
            {
                RECT rect{};
                GetClientRect(windowHandle, &rect);
                const DXGI_SWAP_CHAIN_DESC1 desc = MakeSwapChainDesc(
                    static_cast<UINT>((std::max)(1L, rect.right)), static_cast<UINT>((std::max)(1L, rect.bottom)));
                ComPtr<IDXGISwapChain1> created;
                ThrowIfFailedWithContext(ctx.Factory->CreateSwapChainForHwnd(
                    ctx.CommandQueue.Get(), windowHandle, &desc, nullptr, nullptr, &created),
                    "CreateSwapChainForHwnd(Play)");
                ThrowIfFailedWithContext(created.As(&next), "QueryInterface(Play swap chain)");
                ThrowIfFailedWithContext(ctx.Factory->MakeWindowAssociation(windowHandle, DXGI_MWA_NO_ALT_ENTER),
                    "MakeWindowAssociation(Play)");
            }
            DXGI_SWAP_CHAIN_DESC1 desc{};
            ThrowIfFailedWithContext(next->GetDesc1(&desc), "GetDesc1(Play swap chain)");
            for (auto& target : ctx.RenderTargets) target.Reset();
            // The parked slot holds one chain. Whatever it held before, other than
            // the chain now becoming active, belongs to a window that is no longer
            // presented to and is destroyed rather than overwritten, so its proxy
            // context is released with it.
            if (ctx.ParkedSwapChain && ctx.ParkedSwapChain != next)
                DestroySwapChain(ctx.ParkedSwapChain, ctx.ParkedSwapChainContext);
            ctx.ParkedSwapChain = ctx.SwapChain;
            ctx.ParkedSwapChainContext = ctx.SwapChainContext;
            ctx.ParkedWindowHandle = ctx.WindowHandle;
            ctx.SwapChain = next;
            ctx.SwapChainContext = nextContext;
            ctx.WindowHandle = windowHandle;
            ctx.Width = desc.Width;
            ctx.Height = desc.Height;
            ctx.FrameIndex = ctx.SwapChain->GetCurrentBackBufferIndex();
            ctx.FenceValues.fill(0);
            return RecreateSwapChainRenderTargets();
        }
        catch (const std::exception& exception)
        {
            SetContextError(std::string("Could not switch presentation window: ") + exception.what());
            return false;
        }
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

    __declspec(dllexport) IDXGISwapChain* __stdcall DX12Context_GetSwapChain()
    {
        return g_Context.SwapChain.Get();
    }

    __declspec(dllexport) bool __stdcall DX12Context_IsFrameGenerationSwapChain()
    {
        return g_Context.SwapChainContext != nullptr;
    }

    // Installs or removes FSR's frame generation proxy for the active window. Only
    // between frames: the GPU is flushed and every back buffer reference dropped.
    // Returns false when the proxy was asked for but could not be created; the
    // window is then left with a working plain chain.
    __declspec(dllexport) bool __stdcall DX12Context_SetFrameGenerationSwapChain(bool enable)
    {
        auto& ctx = g_Context;
        if (enable == (ctx.SwapChainContext != nullptr))
            return true;

        try
        {
            if (!(ctx.Device && ctx.SwapChain && ctx.Factory && ctx.CommandQueue) || ctx.CommandListOpen)
                throw std::runtime_error("The swap chain can only be replaced between frames.");
            if (!FlushGPU(2000))
                throw std::runtime_error("GPU wait timed out before replacing the swap chain.");

            ReplaceActiveSwapChain(enable);
            return enable == (ctx.SwapChainContext != nullptr);
        }
        catch (const std::exception& exception)
        {
            SetContextError(std::string("DX12Context_SetFrameGenerationSwapChain failed: ") + exception.what());
            return false;
        }
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
            LogSrvHeapExhausted();
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
        {
            LogSrvHeapExhausted();
            return false;
        }

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

                // The renderer wraps each pass in a GPU event. Replaying the Begin/End
                // pairs up to the stopping point gives the events it was inside, by
                // ordinal; the scene renderer logs which pass each ordinal is, since
                // DRED keeps the event strings only on some drivers.
                {
                    const auto contextAt = [node](UINT op) -> const wchar_t*
                    {
                        for (UINT c = 0; c < node->BreadcrumbContextsCount && node->pBreadcrumbContexts; ++c)
                        {
                            if (node->pBreadcrumbContexts[c].BreadcrumbIndex == op)
                                return node->pBreadcrumbContexts[c].pContextString;
                        }
                        return nullptr;
                    };

                    struct OpenEvent { UINT Ordinal; const wchar_t* Name; };
                    std::vector<OpenEvent> openEvents;
                    UINT beginCount = 0;
                    const UINT replayEnd = (std::min)(completed + 1, node->BreadcrumbCount);
                    for (UINT op = 0; op < replayEnd; ++op)
                    {
                        const D3D12_AUTO_BREADCRUMB_OP kind = node->pCommandHistory[op];
                        if (kind == D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT)
                            openEvents.push_back({ ++beginCount, contextAt(op) });
                        else if (kind == D3D12_AUTO_BREADCRUMB_OP_ENDEVENT && !openEvents.empty())
                            openEvents.pop_back();
                    }

                    if (openEvents.empty())
                    {
                        PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                                         "    Inside no GPU event (%u events had been opened and closed before it).", beginCount);
                    }
                    for (const OpenEvent& open : openEvents)
                    {
                        char narrow[128] = {};
                        if (open.Name != nullptr)
                            WideCharToMultiByte(CP_UTF8, 0, open.Name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
                        PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                                         "    Inside GPU event BeginEvent #%u%s%s", open.Ordinal,
                                         narrow[0] ? " = " : " (see the pass list below)", narrow);
                    }
                }

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
            // A VA of zero means the device died without faulting - a hang, not a
            // bad address - and the two lists below will be empty. Saying so keeps
            // a timeout from being read as a use-after-free.
            PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                             "Page fault at GPU virtual address 0x%llx.%s",
                             static_cast<unsigned long long>(pageFault.PageFaultVA),
                             pageFault.PageFaultVA == 0
                                 ? " No faulting address: the GPU hung rather than reading bad memory."
                                 : "");

            // Both lists matter and they answer different questions. A name in the
            // freed list means something was released while the GPU still needed
            // it; a name in the existing list means the address is live and the
            // access ran off the end of it instead. Reporting only the freed list,
            // as this used to, makes every hang look like a lifetime bug.
            const auto logAllocations =
                [](const char* label, const D3D12_DRED_ALLOCATION_NODE* head)
            {
                int count = 0;
                for (const D3D12_DRED_ALLOCATION_NODE* node = head;
                     node != nullptr; node = node->pNext, ++count)
                {
                    // ID3D12Object::SetName stores the WIDE name, so ObjectNameA is
                    // null for everything this engine names and reading only it
                    // reported "(unnamed)" for resources that were named all along.
                    // Prefer the wide name and fall back to the narrow one.
                    char name[256] = {};
                    if (node->ObjectNameW != nullptr && node->ObjectNameW[0] != L'\0')
                    {
                        WideCharToMultiByte(CP_UTF8, 0, node->ObjectNameW, -1,
                                            name, sizeof(name) - 1, nullptr, nullptr);
                    }
                    else if (node->ObjectNameA != nullptr && node->ObjectNameA[0] != '\0')
                    {
                        std::snprintf(name, sizeof(name), "%s", node->ObjectNameA);
                    }

                    PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                                     "    %s: %s (type %d)",
                                     label,
                                     name[0] ? name : "(unnamed)",
                                     static_cast<int>(node->AllocationType));
                }
                if (count == 0)
                {
                    PteroLog::Writef(PteroLog::Level::Fatal, "Device",
                                     "    %s: none reported.", label);
                }
            };

            logAllocations("recently freed", pageFault.pHeadRecentFreedAllocationNode);
            logAllocations("still allocated", pageFault.pHeadExistingAllocationNode);
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
        const bool flushed = FlushGPU(2000);

        // Almost every caller frees textures immediately afterwards and ignores this
        // return value, so a flush that gives up two seconds in releases memory the
        // GPU is still reading - a page fault a few frames later, in a pass that has
        // nothing to do with whoever resized. It should never time out; say so loudly
        // when it does rather than leaving the next failure unexplained.
        if (!flushed)
        {
            auto& ctx = g_Context;
            PteroLog::Writef(PteroLog::Level::Error, "Device",
                "DX12Context_WaitForGPU timed out after 2000 ms. targetFence=%llu completedFence=%llu. "
                "Anything released by the caller from here is being freed while the GPU may still be using it.",
                static_cast<unsigned long long>(ctx.NextFenceValue - 1),
                static_cast<unsigned long long>(ctx.Fence ? ctx.Fence->GetCompletedValue() : 0));
        }

        return flushed;
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
            DestroySwapChain(ctx.SwapChain, ctx.SwapChainContext);
            DestroySwapChain(ctx.ParkedSwapChain, ctx.ParkedSwapChainContext);
            ctx.ParkedWindowHandle = nullptr;
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
