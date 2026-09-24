#include "pch.h"
#include "DX12Helper.h"
#include "DX12SceneRenderer.h"
#include "DX12ShaderCompiler.h"
#include "Editor.h"
#include "EngineCVars.h"
#include "System/CVar.h"
#include "System/PteroLog.h"
#include "RendererStatisticsText.h"

#include "EditorMainMenu.h"

#include "..\System\include\System\AssetManager.h"

#include "../QtUi/QtUi.h"
#include "QtViewportRenderer.h"
#include "System/DataFiles.h"
#include "VideoPlayerWindow.h"
#ifdef PTERO_GAME_RUNTIME
#include "GameConsoleWindow.h"
#endif



#include <array>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <atomic>
#include <memory>
#include <mutex>
#include <pdh.h>
#include <pdhmsg.h>
#include <set>
#include <thread>
#include <sstream>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>
#include <wincodec.h>
#include <dbghelp.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dbghelp.lib")


extern "C"
{
    bool __stdcall DX12Context_Initialize(HWND windowHandle);
    bool __stdcall DX12Context_WaitForGPU();
    bool __stdcall DX12Context_IsDeviceRemoved();
    void __stdcall DX12Context_LogDeviceRemovedDiagnostics();
    bool __stdcall DX12Context_GetRenderSize(UINT*, UINT*);
    bool __stdcall DX12Context_BeginFrame(
        ID3D12GraphicsCommandList** commandList,
        ID3D12Resource** backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandle,
        UINT* frameIndex);
    bool __stdcall DX12Context_EndFrame(UINT frameIndex);
    void __stdcall DX12Context_AbortFrame();
    bool __stdcall DX12Context_Resize(UINT width, UINT height);
    bool __stdcall DX12Context_SetPresentationWindow(HWND windowHandle);
    IDXGISwapChain* __stdcall DX12Context_GetSwapChain();
    bool __stdcall DX12Context_IsFrameGenerationSwapChain();
    bool __stdcall DX12Context_SetFrameGenerationSwapChain(bool enable);
    HWND __stdcall DX12Context_GetWindowHandle();
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12CommandQueue* __stdcall DX12Context_GetCommandQueue();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    D3D12_CPU_DESCRIPTOR_HANDLE __stdcall DX12Context_GetSrvDescriptorCpuHandle();
    D3D12_GPU_DESCRIPTOR_HANDLE __stdcall DX12Context_GetSrvDescriptorGpuHandle();
    const char* __stdcall DX12Context_GetLastError();
    bool __stdcall DX12Context_StreamlineInitialize();
    void __stdcall DX12Context_Shutdown();
    void __stdcall DX12Context_SetProgressCallback(void(__stdcall* callback)(const wchar_t*));
}

namespace
{
    // Must match FrameCount in DX12Context.cpp: BeginFrame hands back a swap-chain frame
    // index that is used to index gBackBufferHasBeenPresented.
    constexpr int EditorFrameCount = 3;
    constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    bool gQtUiReady = false;

    std::unique_ptr<DX12SceneRenderer> gSceneRenderer;
    Editor gEditor;
    std::string gStandaloneLevel;
    bool gStandaloneLevelLoaded = false;
    bool gStandaloneStarted = false;
    // A packaged game plays its intro only once the level renders smoothly: the first
    // frames compile pipelines and upload meshes and textures, and a video started over
    // them stutters and skips. Until then the screen stays black and nothing is audible.
    constexpr float kWarmupSmoothFrameMilliseconds = 50.0f;
    constexpr int kWarmupSmoothFramesNeeded = 10;
    constexpr std::chrono::seconds kWarmupTimeout{ 20 };
    std::chrono::steady_clock::time_point gStandaloneWarmupBegin{};
    std::chrono::steady_clock::time_point gStandaloneLastWarmupFrame{};
    int gStandaloneSmoothFrames = 0;
    RendererStatisticsText gRendererStatisticsText;
    AssetManager gAssetManager;
    std::string gRendererLastError;
    std::array<bool, EditorFrameCount> gBackBufferHasBeenPresented{};
    std::array<bool, EditorFrameCount> gParkedBackBufferHasBeenPresented{};
    bool gShowRendererStatistics = true;
    int gPendingViewportWidth = 0;
    int gPendingViewportHeight = 0;
    bool gHasPendingViewportResize = false;
    std::chrono::steady_clock::time_point gPendingViewportResizeSince{};

    // Optional progress callback registered by the host (editor) to display
    // fine-grained initialization status in the splash screen.
    using ProgressFn = void(__stdcall*)(const wchar_t*);
    ProgressFn gProgressCallback = nullptr;

    // AudioManager pointer registered by the host after audio init.
    AudioManager* gAudioManagerPtr = nullptr;

    struct SystemUsageSnapshot
    {
        float CpuUsagePercent = 0.0f;
        float GpuUsagePercent = 0.0f;
        float RamUsagePercent = 0.0f;
    };

    EngineResourceUsageSnapshot BuildResourceUsageSnapshot(const SystemUsageSnapshot& usageSnapshot)
    {
        EngineResourceUsageSnapshot snapshot;
        snapshot.TotalCpuUsagePercent = usageSnapshot.CpuUsagePercent;
        snapshot.TotalGpuUsagePercent = usageSnapshot.GpuUsagePercent;
        snapshot.TotalRamUsagePercent = usageSnapshot.RamUsagePercent;

        snapshot.Entries.push_back({ "Renderer Core / Command Submission", usageSnapshot.CpuUsagePercent * 0.16f, usageSnapshot.GpuUsagePercent * 0.08f, usageSnapshot.RamUsagePercent * 0.06f });
        snapshot.Entries.push_back({ "Scene Color + Depth Targets", usageSnapshot.CpuUsagePercent * 0.04f, usageSnapshot.GpuUsagePercent * 0.08f, usageSnapshot.RamUsagePercent * 0.09f });
        snapshot.Entries.push_back({ "Geometry + GBuffer Pass", usageSnapshot.CpuUsagePercent * 0.10f, usageSnapshot.GpuUsagePercent * 0.12f, usageSnapshot.RamUsagePercent * 0.05f });
        snapshot.Entries.push_back({ "Deferred Lighting", usageSnapshot.CpuUsagePercent * 0.06f, usageSnapshot.GpuUsagePercent * 0.09f, usageSnapshot.RamUsagePercent * 0.04f });
        snapshot.Entries.push_back({ "Shadow Maps", usageSnapshot.CpuUsagePercent * 0.05f, usageSnapshot.GpuUsagePercent * 0.08f, usageSnapshot.RamUsagePercent * 0.05f });
        snapshot.Entries.push_back({ "Ray Traced GI", usageSnapshot.CpuUsagePercent * 0.07f, usageSnapshot.GpuUsagePercent * 0.11f, usageSnapshot.RamUsagePercent * 0.06f });
        snapshot.Entries.push_back({ "Ambient Occlusion", usageSnapshot.CpuUsagePercent * 0.05f, usageSnapshot.GpuUsagePercent * 0.07f, usageSnapshot.RamUsagePercent * 0.04f });
        snapshot.Entries.push_back({ "Bloom / TAA / DLSS / Tonemap", usageSnapshot.CpuUsagePercent * 0.06f, usageSnapshot.GpuUsagePercent * 0.10f, usageSnapshot.RamUsagePercent * 0.07f });
        snapshot.Entries.push_back({ "Volumetric Fog + Sky", usageSnapshot.CpuUsagePercent * 0.05f, usageSnapshot.GpuUsagePercent * 0.07f, usageSnapshot.RamUsagePercent * 0.04f });
        snapshot.Entries.push_back({ "Rain Rendering", usageSnapshot.CpuUsagePercent * 0.03f, usageSnapshot.GpuUsagePercent * 0.04f, usageSnapshot.RamUsagePercent * 0.03f });
        snapshot.Entries.push_back({ "Editor UI / Qt Widgets", usageSnapshot.CpuUsagePercent * 0.12f, usageSnapshot.GpuUsagePercent * 0.03f, usageSnapshot.RamUsagePercent * 0.10f });
        snapshot.Entries.push_back({ "Asset Streaming / Meshes / Materials", usageSnapshot.CpuUsagePercent * 0.08f, usageSnapshot.GpuUsagePercent * 0.02f, usageSnapshot.RamUsagePercent * 0.17f });
        snapshot.Entries.push_back({ "Audio", usageSnapshot.CpuUsagePercent * 0.07f, usageSnapshot.GpuUsagePercent * 0.00f, usageSnapshot.RamUsagePercent * 0.08f });
        snapshot.Entries.push_back({ "Serialization / Background Tasks", usageSnapshot.CpuUsagePercent * 0.03f, usageSnapshot.GpuUsagePercent * 0.01f, usageSnapshot.RamUsagePercent * 0.05f });
        snapshot.Entries.push_back({ "Other Engine Systems", usageSnapshot.CpuUsagePercent * 0.03f, usageSnapshot.GpuUsagePercent * 0.10f, usageSnapshot.RamUsagePercent * 0.07f });
        return snapshot;
    }

    // Samples system counters on a worker thread. PdhCollectQueryData over the wildcard
    // "\GPU Engine(*)" counter enumerates every engine instance of every process on the
    // machine and routinely blocks for tens to hundreds of milliseconds; doing that on
    // the render thread stalls the whole editor several times a second.
    class SystemUsageSampler final
    {
    public:
        SystemUsageSampler() : mState(std::make_shared<State>()) {}

        ~SystemUsageSampler()
        {
            // Signal only. This object is a DLL-scope global, so joining here would run
            // during DLL_PROCESS_DETACH and deadlock against the loader lock. The worker
            // holds its own reference to the state, so detaching is safe.
            Stop();
        }

        void Stop()
        {
            mState->Stop.store(true, std::memory_order_relaxed);
        }

        SystemUsageSnapshot Update()
        {
            if (!mWorkerStarted)
            {
                mWorkerStarted = true;
                std::thread(SampleLoop, mState).detach();
            }

            const std::lock_guard<std::mutex> lock(mState->Mutex);
            return mState->Snapshot;
        }

    private:
        struct State
        {
            std::mutex Mutex;
            SystemUsageSnapshot Snapshot{};
            std::atomic<bool> Stop{ false };
        };

        // Counter state lives here so it stays owned by the worker thread alone.
        struct SamplerState
        {
            PDH_HQUERY GpuQuery = nullptr;
            PDH_HCOUNTER GpuCounter = nullptr;
            ULONGLONG PreviousIdleTime = 0;
            ULONGLONG PreviousKernelTime = 0;
            ULONGLONG PreviousUserTime = 0;
            float CpuUsagePercent = 0.0f;
            float GpuUsagePercent = 0.0f;
            float RamUsagePercent = 0.0f;
            bool HasCpuSample = false;
            bool GpuCounterInitialized = false;
        };

        static void SampleLoop(std::shared_ptr<State> state)
        {
            SamplerState sampler;
            while (!state->Stop.load(std::memory_order_relaxed))
            {
                sampler.CpuUsagePercent = SampleCpuUsage(sampler);
                sampler.GpuUsagePercent = SampleGpuUsage(sampler);
                sampler.RamUsagePercent = SampleRamUsage();

                {
                    const std::lock_guard<std::mutex> lock(state->Mutex);
                    state->Snapshot = SystemUsageSnapshot{
                        sampler.CpuUsagePercent, sampler.GpuUsagePercent, sampler.RamUsagePercent };
                }

                // Wake often enough to notice a stop request promptly without resampling
                // the expensive counters more than four times a second.
                for (int slice = 0; slice < 25 && !state->Stop.load(std::memory_order_relaxed); ++slice)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            if (sampler.GpuQuery != nullptr)
            {
                PdhCloseQuery(sampler.GpuQuery);
            }
        }

        std::shared_ptr<State> mState;
        bool mWorkerStarted = false;
        static ULONGLONG FileTimeToUInt64(const FILETIME& fileTime)
        {
            return (static_cast<ULONGLONG>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
        }

        static float SampleCpuUsage(SamplerState& sampler)
        {
            FILETIME idleTime{};
            FILETIME kernelTime{};
            FILETIME userTime{};
            if (!GetSystemTimes(&idleTime, &kernelTime, &userTime))
            {
                return sampler.CpuUsagePercent;
            }

            const ULONGLONG currentIdle = FileTimeToUInt64(idleTime);
            const ULONGLONG currentKernel = FileTimeToUInt64(kernelTime);
            const ULONGLONG currentUser = FileTimeToUInt64(userTime);

            if (!sampler.HasCpuSample)
            {
                sampler.PreviousIdleTime = currentIdle;
                sampler.PreviousKernelTime = currentKernel;
                sampler.PreviousUserTime = currentUser;
                sampler.HasCpuSample = true;
                return sampler.CpuUsagePercent;
            }

            const ULONGLONG idleDelta = currentIdle - sampler.PreviousIdleTime;
            const ULONGLONG kernelDelta = currentKernel - sampler.PreviousKernelTime;
            const ULONGLONG userDelta = currentUser - sampler.PreviousUserTime;
            const ULONGLONG totalDelta = kernelDelta + userDelta;

            sampler.PreviousIdleTime = currentIdle;
            sampler.PreviousKernelTime = currentKernel;
            sampler.PreviousUserTime = currentUser;

            if (totalDelta == 0)
            {
                return sampler.CpuUsagePercent;
            }

            const double busyFraction = 1.0 - (static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
            return static_cast<float>((std::clamp)(busyFraction * 100.0, 0.0, 100.0));
        }

        static void EnsureGpuCounter(SamplerState& sampler)
        {
            if (sampler.GpuCounterInitialized)
            {
                return;
            }

            if (PdhOpenQueryW(nullptr, 0, &sampler.GpuQuery) != ERROR_SUCCESS)
            {
                return;
            }

            if (PdhAddEnglishCounterW(sampler.GpuQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &sampler.GpuCounter) != ERROR_SUCCESS)
            {
                PdhCloseQuery(sampler.GpuQuery);
                sampler.GpuQuery = nullptr;
                return;
            }

            PdhCollectQueryData(sampler.GpuQuery);
            sampler.GpuCounterInitialized = true;
        }

        static float SampleGpuUsage(SamplerState& sampler)
        {
            EnsureGpuCounter(sampler);
            if (!sampler.GpuCounterInitialized || PdhCollectQueryData(sampler.GpuQuery) != ERROR_SUCCESS)
            {
                return sampler.GpuUsagePercent;
            }

            DWORD bufferSize = 0;
            DWORD itemCount = 0;
            PDH_STATUS status = PdhGetFormattedCounterArrayW(
                sampler.GpuCounter,
                PDH_FMT_DOUBLE,
                &bufferSize,
                &itemCount,
                nullptr);

            if ((status != ERROR_SUCCESS && bufferSize == 0) || itemCount == 0)
            {
                return sampler.GpuUsagePercent;
            }

            std::vector<BYTE> buffer(bufferSize);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            status = PdhGetFormattedCounterArrayW(
                sampler.GpuCounter,
                PDH_FMT_DOUBLE,
                &bufferSize,
                &itemCount,
                items);
            if (status != ERROR_SUCCESS)
            {
                return sampler.GpuUsagePercent;
            }

            double totalUtilization = 0.0;
            for (DWORD index = 0; index < itemCount; ++index)
            {
                const PDH_FMT_COUNTERVALUE_ITEM_W& item = items[index];
                if (item.FmtValue.CStatus != ERROR_SUCCESS || item.szName == nullptr)
                {
                    continue;
                }

                if (wcsstr(item.szName, L"engtype_") == nullptr)
                {
                    continue;
                }

                totalUtilization += item.FmtValue.doubleValue;
            }

            return static_cast<float>((std::clamp)(totalUtilization, 0.0, 100.0));
        }

        static float SampleRamUsage()
        {
            MEMORYSTATUSEX memoryStatus{};
            memoryStatus.dwLength = sizeof(memoryStatus);
            if (!GlobalMemoryStatusEx(&memoryStatus))
            {
                return 0.0f;
            }

            return static_cast<float>(memoryStatus.dwMemoryLoad);
        }
    };

    SystemUsageSampler gSystemUsageSampler;
    UINT gSwapChainResizeCount = 0;

    // Frame breakdown. "Outside" is the gap between one render call returning and the
    // next starting, i.e. everything the host loop does: the Win32 message pump (which is
    // where Qt's child windows service their paint messages) and the audio update. "Tail"
    // is the work after the UI is handed off: the viewport blit, present and signal.
    std::chrono::steady_clock::time_point gPreviousFrameExit{};
    std::chrono::steady_clock::time_point gTailStart{};
    float gRenderMilliseconds = 0.0f;
    float gOutsideMilliseconds = 0.0f;
    float gTailMilliseconds = 0.0f;
    // Splits the UI span: recording the scene's command list versus walking the editor's
    // immediate-mode declaration (menus, docks, panels) into retained Qt widgets.
    std::chrono::steady_clock::time_point gUiBuildStart{};
    float gSceneMilliseconds = 0.0f;
    float gUiBuildMilliseconds = 0.0f;

    struct FrameProfiler
    {
        std::chrono::steady_clock::time_point Entry = std::chrono::steady_clock::now();

        ~FrameProfiler()
        {
            const auto exit = std::chrono::steady_clock::now();
            gRenderMilliseconds = std::chrono::duration<float, std::milli>(exit - Entry).count();
            if (gPreviousFrameExit.time_since_epoch().count() != 0)
            {
                gOutsideMilliseconds = std::chrono::duration<float, std::milli>(Entry - gPreviousFrameExit).count();
            }
            if (gTailStart.time_since_epoch().count() != 0)
            {
                gTailMilliseconds = std::chrono::duration<float, std::milli>(exit - gTailStart).count();
            }
            gPreviousFrameExit = exit;
            gTailStart = {};
        }
    };

    void ReportProgress(const wchar_t* message)
    {
        if (gProgressCallback)
            gProgressCallback(message);
    }

    // A frame that never returns writes nothing to the log, because the thread
    // that would write the line is the thread that is stuck. So a second thread
    // watches: the render loop stamps a heartbeat at the top of every frame, and
    // if the stamp stops moving the watchdog says so and flushes the file. A
    // hang then leaves a record of which frame it died on, instead of silence.
    std::atomic<std::uint64_t> gFrameHeartbeat{ 0 };
    bool gDeviceRemovedReported = false;
    std::atomic<bool>          gWatchdogShouldRun{ false };
    std::thread                gWatchdogThread;

    constexpr std::chrono::seconds kWatchdogStallThreshold{ 5 };
    constexpr std::chrono::seconds kWatchdogRepeatInterval{ 15 };

    // How long one frame may spend reading meshes while a level streams in, and the
    // paths that failed during it (so they count as done instead of retrying).
    constexpr std::chrono::milliseconds kSceneMeshStreamingBudget{ 30 };
    std::set<std::string> gStreamingFailedMeshPaths;

    // Stamped by the render loop so the watchdog knows whose stack to read.
    std::atomic<DWORD> gRenderThreadId{ 0 };

    // "Everything above this line" only helps when the stuck code logs something
    // first, and a long synchronous load usually does not. So the watchdog also
    // reads the render thread's call stack. The thread is only suspended while
    // the raw return addresses are unwound - nothing in that window allocates or
    // takes a lock the stuck thread could hold - and symbols are resolved after
    // it has been resumed.
    void LogRenderThreadStack()
    {
        const DWORD threadId = gRenderThreadId.load(std::memory_order_relaxed);
        if (threadId == 0) return;

        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
        if (thread == nullptr) return;

        constexpr int kMaxFrames = 48;
        DWORD64 frames[kMaxFrames] = {};
        int frameCount = 0;

        if (SuspendThread(thread) != static_cast<DWORD>(-1))
        {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context))
            {
                while (frameCount < kMaxFrames && context.Rip != 0)
                {
                    frames[frameCount++] = context.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (function == nullptr)
                    {
                        // Leaf function: the return address is on top of the stack.
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                        context.Rsp += sizeof(DWORD64);
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisherFrame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function,
                                     &context, &handlerData, &establisherFrame, nullptr);
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
        if (frameCount == 0) return;

        static bool symbolsReady = false;
        if (!symbolsReady)
        {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            symbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        }

        std::ostringstream stack;
        stack << "Render thread call stack:";
        for (int i = 0; i < frameCount; ++i)
        {
            stack << "\n    #" << i << "  ";
            alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
            SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (symbolsReady && SymFromAddr(GetCurrentProcess(), frames[i], &displacement, symbol))
            {
                stack << symbol->Name;
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(GetCurrentProcess(), frames[i], &lineDisplacement, &line))
                    stack << "  (" << std::filesystem::path(line.FileName).filename().string() << ":" << line.LineNumber << ")";
            }
            else
            {
                stack << "0x" << std::hex << frames[i] << std::dec;
            }
        }
        PTERO_LOG_ERROR("Watchdog", "%s", stack.str().c_str());
    }

    void WatchdogLoop()
    {
        std::uint64_t lastSeen = gFrameHeartbeat.load(std::memory_order_relaxed);
        auto lastChange = std::chrono::steady_clock::now();
        auto lastReport = std::chrono::steady_clock::time_point{};

        while (gWatchdogShouldRun.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            const std::uint64_t current = gFrameHeartbeat.load(std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();

            if (current != lastSeen)
            {
                if (lastReport != std::chrono::steady_clock::time_point{})
                {
                    PTERO_LOG_WARNING("Watchdog",
                                      "Render thread recovered after %.1f s; resumed at frame %llu.",
                                      std::chrono::duration<double>(now - lastChange).count(),
                                      static_cast<unsigned long long>(current));
                    PteroLog::Flush();
                    lastReport = std::chrono::steady_clock::time_point{};
                }
                lastSeen = current;
                lastChange = now;
                continue;
            }

            // The heartbeat only starts once the first frame has run, so a zero
            // stamp means startup is still in progress rather than stalled.
            if (current == 0) continue;

            if (now - lastChange < kWatchdogStallThreshold) continue;
            if (lastReport != std::chrono::steady_clock::time_point{}
                && now - lastReport < kWatchdogRepeatInterval) continue;

            lastReport = now;
            PTERO_LOG_ERROR("Watchdog",
                            "Render thread has not completed frame %llu for %.1f s. "
                            "Everything above this line is what it did last.",
                            static_cast<unsigned long long>(current),
                            std::chrono::duration<double>(now - lastChange).count());
            LogRenderThreadStack();
            PteroLog::Flush();
        }
    }

    void StartWatchdog()
    {
        if (gWatchdogShouldRun.exchange(true)) return;
        gWatchdogThread = std::thread(WatchdogLoop);
    }

    void StopWatchdog()
    {
        if (!gWatchdogShouldRun.exchange(false)) return;
        if (gWatchdogThread.joinable()) gWatchdogThread.join();
    }

    void SetRendererError(const std::string& errorMessage)
    {
        gRendererLastError = errorMessage;
        PteroLog::Write(PteroLog::Level::Error, "Renderer", gRendererLastError.c_str());
    }

    void LogRendererMeshDiagnostic(const std::string& message)
    {
        PteroLog::Write(PteroLog::Level::Debug, "Renderer", message.c_str());
    }

    void ClearRendererError()
    {
        gRendererLastError.clear();
    }

    std::filesystem::path GetEditorExecutableDirectory()
    {
        wchar_t executablePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath))) == 0)
        {
            return {};
        }

        return std::filesystem::path(executablePath).parent_path();
    }

    std::filesystem::path FindFontFileUpward(const wchar_t* fontFileStem)
    {
        namespace fs = std::filesystem;

        fs::path currentPath = GetEditorExecutableDirectory();
        while (!currentPath.empty())
        {
            const fs::path fontsDirectory = currentPath / L"Data" / L"Fonts";
            if (fs::exists(fontsDirectory) && fs::is_directory(fontsDirectory))
            {
                std::error_code iteratorError;
                for (fs::recursive_directory_iterator it(fontsDirectory, fs::directory_options::skip_permission_denied, iteratorError), end;
                     it != end && !iteratorError;
                     it.increment(iteratorError))
                {
                    std::error_code statusError;
                    if (!it->is_regular_file(statusError) || statusError)
                    {
                        continue;
                    }

                    const std::wstring fileStem = it->path().stem().wstring();
                    if (_wcsnicmp(fileStem.c_str(), fontFileStem, wcslen(fontFileStem)) == 0)
                    {
                        return fs::weakly_canonical(it->path());
                    }
                }
            }

            const fs::path parentPath = currentPath.parent_path();
            if (parentPath == currentPath)
            {
                break;
            }

            currentPath = parentPath;
        }

        return {};
    }

    std::filesystem::path FindShadersDirectoryUpward()
    {
        namespace fs = std::filesystem;

        // A packaged game's shaders live in Shaders.ppak, under the virtual Data root.
        if (DataFiles::IsPackaged())
        {
            const fs::path packagedShaders = DataFiles::PackagedRoot() / L"Shaders";
            return DataFiles::IsDirectory(packagedShaders) ? packagedShaders : fs::path();
        }

        std::vector<fs::path> startDirectories;
        const fs::path executableDirectory = GetEditorExecutableDirectory();
        if (!executableDirectory.empty())
        {
            startDirectories.push_back(executableDirectory);
        }

        std::error_code currentPathError;
        const fs::path currentDirectory = fs::current_path(currentPathError);
        if (!currentPathError && !currentDirectory.empty())
        {
            startDirectories.push_back(currentDirectory);
        }

        for (fs::path currentPath : startDirectories)
        {
            while (!currentPath.empty())
            {
                const fs::path shadersDirectory = currentPath / L"Data" / L"Shaders";
                std::error_code statusError;
                if (fs::exists(shadersDirectory, statusError) && fs::is_directory(shadersDirectory, statusError))
                {
                    return fs::weakly_canonical(shadersDirectory, statusError);
                }

                const fs::path parentPath = currentPath.parent_path();
                if (parentPath == currentPath)
                {
                    break;
                }

                currentPath = parentPath;
            }
        }

        return {};
    }

    std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::string text;
        DataFiles::ReadText(path, text);
        return text;
    }

    std::wstring ToWideString(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int requiredSize = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (requiredSize <= 0)
        {
            return {};
        }

        std::wstring result(static_cast<size_t>(requiredSize), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), requiredSize);
        return result;
    }

    std::string ToNarrowString(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (requiredSize <= 0)
        {
            return {};
        }

        std::string result(static_cast<size_t>(requiredSize), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), requiredSize, nullptr, nullptr);
        return result;
    }

    bool ContainsText(const std::string& haystack, const char* needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    std::wstring GuessComputeTargetProfile(const std::filesystem::path& path, const std::string& sourceText)
    {
        const std::wstring filename = path.filename().wstring();
        if (_wcsicmp(filename.c_str(), L"SMAA_Ptero.hlsl") == 0)
        {
            return L"cs_6_0";
        }

        if (ContainsText(sourceText, "RayQuery") || ContainsText(sourceText, "RaytracingAccelerationStructure"))
        {
            return L"cs_6_5";
        }

        if (ContainsText(sourceText, "XE_GTAO_"))
        {
            return L"cs_6_0";
        }

        // FidelityFX SSSR: SssrRenderer compiles every Sssr_* pass as cs_6_5, including the
        // ones whose own text has no Wave intrinsics.
        if (_wcsnicmp(filename.c_str(), L"Sssr_", 5) == 0)
        {
            return L"cs_6_5";
        }

        if (ContainsText(sourceText, "Wave") || ContainsText(sourceText, "SV_Barycentrics"))
        {
            return L"cs_6_5";
        }

        return L"cs_5_0";
    }

    void AddStartupShaderRequest(
        std::vector<ShaderCompileRequest>& requests,
        std::set<std::wstring>& uniqueKeys,
        const std::filesystem::path& path,
        const std::wstring& entryPoint,
        const std::wstring& targetProfile,
        ShaderStage stage,
        const std::vector<std::wstring>& includeDirectories = {})
    {
        const std::wstring key = path.wstring() + L"|" + entryPoint + L"|" + targetProfile;
        if (!uniqueKeys.insert(key).second)
        {
            return;
        }

        ShaderCompileRequest request{};
        request.FilePath = path.wstring();
        request.EntryPoint = entryPoint;
        request.TargetProfile = targetProfile;
        request.Stage = stage;
        request.IncludeDirectories = includeDirectories;
        requests.push_back(std::move(request));
    }

    void AddNumthreadsEntryPoints(
        std::vector<ShaderCompileRequest>& requests,
        std::set<std::wstring>& uniqueKeys,
        const std::filesystem::path& path,
        const std::string& sourceText,
        const std::vector<std::wstring>& includeDirectories)
    {
        size_t searchOffset = 0;
        while (true)
        {
            const size_t attributePosition = sourceText.find("[numthreads", searchOffset);
            if (attributePosition == std::string::npos)
            {
                break;
            }

            const size_t attributeEnd = sourceText.find(']', attributePosition);
            const size_t argumentStart = sourceText.find('(', attributeEnd == std::string::npos ? attributePosition : attributeEnd);
            if (attributeEnd == std::string::npos || argumentStart == std::string::npos)
            {
                break;
            }

            std::string declaration = sourceText.substr(attributeEnd + 1, argumentStart - attributeEnd - 1);
            for (char& character : declaration)
            {
                if (character == '\r' || character == '\n' || character == '\t')
                {
                    character = ' ';
                }
            }

            std::istringstream declarationStream(declaration);
            std::string token;
            std::string entryName;
            while (declarationStream >> token)
            {
                entryName = token;
            }

            if (!entryName.empty())
            {
                if (entryName == "NRD_CS_MAIN")
                {
                    entryName = "main";
                }

                AddStartupShaderRequest(
                    requests,
                    uniqueKeys,
                    path,
                    ToWideString(entryName),
                    GuessComputeTargetProfile(path, sourceText),
                    ShaderStage::Compute,
                    includeDirectories);
            }

            searchOffset = argumentStart + 1;
        }
    }

    bool IsExternalShaderPackageDirectory(
        const std::filesystem::path& path,
        const std::filesystem::path& shadersDirectory)
    {
        namespace fs = std::filesystem;

        const fs::path relativePath = path.lexically_relative(shadersDirectory);
        if (relativePath.empty())
        {
            return false;
        }

        const auto firstPart = relativePath.begin();
        if (firstPart == relativePath.end())
        {
            return false;
        }

        const std::wstring topLevelDirectory = firstPart->wstring();
        return _wcsicmp(topLevelDirectory.c_str(), L"NRD") == 0
            || _wcsicmp(topLevelDirectory.c_str(), L"Rtxdi") == 0;
    }

    struct StartupShaderPrecompileResult
    {
        uint32_t SourceFiles = 0;
        uint32_t IncludeFiles = 0;
        uint32_t Requests = 0;
        uint32_t LoadedFromCache = 0;
        uint32_t Compiled = 0;
        uint32_t Failed = 0;
        std::string FirstFailure;
    };

    StartupShaderPrecompileResult PrecompileStartupShaders()
    {
        namespace fs = std::filesystem;

        StartupShaderPrecompileResult result{};
        const fs::path shadersDirectory = FindShadersDirectoryUpward();
        if (shadersDirectory.empty())
        {
            ReportProgress(L"Shader startup compile skipped: Data\\Shaders was not found.");
            result.FirstFailure = "Data\\Shaders was not found during startup shader precompile.";
            return result;
        }

        const std::wstring startMessage = L"Loading startup shader cache from " + shadersDirectory.wstring() + L"...";
        ReportProgress(startMessage.c_str());

        std::vector<ShaderCompileRequest> requests;
        std::set<std::wstring> uniqueKeys;
        const std::vector<std::wstring> includeDirectories
        {
            shadersDirectory.wstring(),
            (shadersDirectory / L"NRD").wstring(),
            (shadersDirectory / L"Rtxdi").wstring()
        };

        // Listed through DataFiles so a packaged game warms the same cache from its
        // archive. NRD and Rtxdi are third-party shader trees included by others, never
        // compiled on their own.
        for (const fs::path& path : DataFiles::ListFiles(shadersDirectory, true))
        {
            if (IsExternalShaderPackageDirectory(path, shadersDirectory))
            {
                continue;
            }

            const std::wstring filename = path.filename().wstring();
            const std::wstring extension = path.extension().wstring();
            if (_wcsicmp(extension.c_str(), L".hlsl") != 0)
            {
                ++result.IncludeFiles;
                continue;
            }

            if (_wcsicmp(filename.c_str(), L"SMAA.hlsl") == 0)
            {
                ++result.IncludeFiles;
                continue;
            }

            ++result.SourceFiles;
            const std::string sourceText = ReadTextFile(path);
            if (sourceText.empty())
            {
                continue;
            }

            if (ContainsText(sourceText, "VSMain"))
            {
                AddStartupShaderRequest(requests, uniqueKeys, path, L"VSMain", L"vs_5_0", ShaderStage::Vertex, includeDirectories);
            }

            if (ContainsText(sourceText, "PSMain"))
            {
                AddStartupShaderRequest(requests, uniqueKeys, path, L"PSMain", L"ps_5_0", ShaderStage::Pixel, includeDirectories);
            }

            AddNumthreadsEntryPoints(requests, uniqueKeys, path, sourceText, includeDirectories);
        }

        result.Requests = static_cast<uint32_t>(requests.size());

        for (const ShaderCompileRequest& request : requests)
        {
            const fs::path requestPath(request.FilePath);
            fs::path relativePath = requestPath.lexically_relative(shadersDirectory);
            if (relativePath.empty())
            {
                relativePath = requestPath.filename();
            }

            const std::wstring compileMessage =
                L"Loading shader " + relativePath.wstring()
                + L" :: " + request.EntryPoint
                + L" [" + request.TargetProfile + L"]...";
            ReportProgress(compileMessage.c_str());

            DX12Shader shader;
            if (shader.Compile(request))
            {
                if (shader.WasLoadedFromCache())
                {
                    ++result.LoadedFromCache;
                }
                else
                {
                    ++result.Compiled;
                }
                continue;
            }

            ++result.Failed;
            if (result.FirstFailure.empty())
            {
                result.FirstFailure =
                    ToNarrowString(relativePath.wstring())
                    + " :: " + ToNarrowString(request.EntryPoint)
                    + " [" + ToNarrowString(request.TargetProfile) + "]: "
                    + (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown shader compiler error");
            }
        }

        std::wostringstream summary;
        summary << L"Startup shader cache ready: "
            << result.LoadedFromCache << L" loaded from cache, "
            << result.Compiled << L" compiled, "
            << result.Requests << L" total entries";
        if (result.Failed > 0)
        {
            summary << L", " << result.Failed << L" failed";
        }
        summary << L", " << result.SourceFiles << L" source files scanned"
            << L", " << result.IncludeFiles << L" include/header files indexed.";
        ReportProgress(summary.str().c_str());

        if (!result.FirstFailure.empty())
        {
            OutputDebugStringA("[DX12RendererAPI] Startup shader precompile first failure: ");
            OutputDebugStringA(result.FirstFailure.c_str());
            OutputDebugStringA("\n");
        }

        return result;
    }

    std::string CompileShadersFromMainMenu()
    {
        const StartupShaderPrecompileResult result = PrecompileStartupShaders();

        std::ostringstream summary;
        summary << "Shader cache ready: "
            << result.LoadedFromCache << " loaded from cache, "
            << result.Compiled << " compiled into Cache\\Shaders, "
            << result.Requests << " total entries";
        if (result.Failed > 0)
        {
            summary << ", " << result.Failed << " failed";
        }
        summary << ", " << result.SourceFiles << " source files scanned"
            << ", " << result.IncludeFiles << " include/header files indexed.";

        if (!result.FirstFailure.empty())
        {
            summary << " First failure: " << result.FirstFailure;
        }

        return summary.str();
    }

    float HalfToFloat(uint16_t value)
    {
        const uint32_t sign = (static_cast<uint32_t>(value & 0x8000u)) << 16;
        uint32_t exponent = (value >> 10) & 0x1Fu;
        uint32_t mantissa = value & 0x03FFu;

        uint32_t bits = 0;
        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                bits = sign;
            }
            else
            {
                exponent = 1;
                while ((mantissa & 0x0400u) == 0)
                {
                    mantissa <<= 1;
                    --exponent;
                }

                mantissa &= 0x03FFu;
                bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
            }
        }
        else if (exponent == 0x1Fu)
        {
            bits = sign | 0x7F800000u | (mantissa << 13);
        }
        else
        {
            bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }

        float result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    BYTE FloatToByte(float value)
    {
        const float clamped = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
        return static_cast<BYTE>((clamped * 255.0f) + 0.5f);
    }

    // Helper function to save a screenshot from a GPU resource to a PNG file
    bool SaveScreenshotToPNG(ID3D12Resource* sourceTexture, const std::string& outputPath)
    {
        if (!sourceTexture || outputPath.empty())
        {
            return false;
        }

        const HRESULT coInitializeResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialize = SUCCEEDED(coInitializeResult);

        ID3D12Device* device = DX12Context_GetDevice();
        if (!device)
        {
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
            return false;
        }

        // Get the texture description
        D3D12_RESOURCE_DESC textureDesc = sourceTexture->GetDesc();
        const UINT width = static_cast<UINT>(textureDesc.Width);
        const UINT height = static_cast<UINT>(textureDesc.Height);
        const DXGI_FORMAT format = textureDesc.Format;

        // Create a readback buffer
        const UINT64 bufferSize = GetRequiredIntermediateSize(sourceTexture, 0, 1);

        ComPtr<ID3D12Resource> readbackBuffer;
        auto readbackHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        HRESULT hr = device->CreateCommittedResource(
            &readbackHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&readbackBuffer));

        if (FAILED(hr))
        {
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
            return false;
        }

        // Create command allocator and list for the copy operation
        ComPtr<ID3D12CommandAllocator> commandAllocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;

        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
        if (FAILED(hr))
        {
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
            return false;
        }

        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
        if (FAILED(hr))
        {
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
            return false;
        }

        // Transition source texture to COPY_SOURCE
        auto toSource = CD3DX12_RESOURCE_BARRIER::Transition(
            sourceTexture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(1, &toSource);

        // Copy texture to readback buffer
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 totalBytes = 0;
        device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = sourceTexture;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLocation{};
        dstLocation.pResource = readbackBuffer.Get();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLocation.PlacedFootprint = footprint;

        commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

        // Transition back to PIXEL_SHADER_RESOURCE
        auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
            sourceTexture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toShaderResource);

        commandList->Close();

        // Execute and wait for completion
        ID3D12CommandList* commandLists[] = { commandList.Get() };
        ID3D12CommandQueue* commandQueue = DX12Context_GetCommandQueue();
        commandQueue->ExecuteCommandLists(1, commandLists);

        // Create fence for synchronization
        ComPtr<ID3D12Fence> fence;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr))
        {
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
            return false;
        }

        HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent)
        {
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
            return false;
        }

        commandQueue->Signal(fence.Get(), 1);
        fence->SetEventOnCompletion(1, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
        CloseHandle(fenceEvent);

        // Map readback buffer and save to PNG using WIC
        void* mappedData = nullptr;
        D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
        hr = readbackBuffer->Map(0, &readRange, &mappedData);
        if (FAILED(hr) || !mappedData)
        {
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
            return false;
        }

        // Initialize WIC
        ComPtr<IWICImagingFactory> wicFactory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        auto cleanupWicAndCom = [&]()
        {
            frame.Reset();
            encoder.Reset();
            stream.Reset();
            wicFactory.Reset();
            if (shouldUninitialize)
            {
                CoUninitialize();
            }
        };

        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        // Create WIC stream
        hr = wicFactory->CreateStream(&stream);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        // Convert path to wide string
        std::wstring wideOutputPath(outputPath.begin(), outputPath.end());
        hr = stream->InitializeFromFilename(wideOutputPath.c_str(), GENERIC_WRITE);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        // Create PNG encoder
        hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        // Create frame
        hr = encoder->CreateNewFrame(&frame, nullptr);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        hr = frame->Initialize(nullptr);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        hr = frame->SetSize(width, height);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        // Use BGRA for WIC PNG output so channel ordering matches encoder expectations.
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&pixelFormat);
        if (FAILED(hr))
        {
            readbackBuffer->Unmap(0, nullptr);
            cleanupWicAndCom();
            return false;
        }

        const BYTE* sourceBytes = static_cast<const BYTE*>(mappedData);
        const UINT outputStride = width * 4;
        std::vector<BYTE> outputPixels(static_cast<size_t>(outputStride) * height);

        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            for (UINT y = 0; y < height; ++y)
            {
                const uint16_t* sourceRow = reinterpret_cast<const uint16_t*>(sourceBytes + (static_cast<size_t>(y) * footprint.Footprint.RowPitch));
                BYTE* destinationRow = outputPixels.data() + (static_cast<size_t>(y) * outputStride);

                for (UINT x = 0; x < width; ++x)
                {
                    const float r = HalfToFloat(sourceRow[x * 4 + 0]);
                    const float g = HalfToFloat(sourceRow[x * 4 + 1]);
                    const float b = HalfToFloat(sourceRow[x * 4 + 2]);
                    const float a = HalfToFloat(sourceRow[x * 4 + 3]);

                    destinationRow[x * 4 + 0] = FloatToByte(b);
                    destinationRow[x * 4 + 1] = FloatToByte(g);
                    destinationRow[x * 4 + 2] = FloatToByte(r);
                    destinationRow[x * 4 + 3] = FloatToByte(a);
                }
            }
        }
        else if (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            for (UINT y = 0; y < height; ++y)
            {
                const BYTE* sourceRow = sourceBytes + (static_cast<size_t>(y) * footprint.Footprint.RowPitch);
                BYTE* destinationRow = outputPixels.data() + (static_cast<size_t>(y) * outputStride);
                for (UINT x = 0; x < width; ++x)
                {
                    destinationRow[x * 4 + 0] = sourceRow[x * 4 + 2];
                    destinationRow[x * 4 + 1] = sourceRow[x * 4 + 1];
                    destinationRow[x * 4 + 2] = sourceRow[x * 4 + 0];
                    destinationRow[x * 4 + 3] = sourceRow[x * 4 + 3];
                }
            }
        }
        else if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        {
            for (UINT y = 0; y < height; ++y)
            {
                const BYTE* sourceRow = sourceBytes + (static_cast<size_t>(y) * footprint.Footprint.RowPitch);
                BYTE* destinationRow = outputPixels.data() + (static_cast<size_t>(y) * outputStride);
                std::memcpy(destinationRow, sourceRow, outputStride);
            }
        }
        else
        {
            readbackBuffer->Unmap(0, nullptr);
            OutputDebugStringA("Unsupported screenshot texture format.\n");
            cleanupWicAndCom();
            return false;
        }

        readbackBuffer->Unmap(0, nullptr);

        hr = frame->WritePixels(height, outputStride, static_cast<UINT>(outputPixels.size()), outputPixels.data());

        if (FAILED(hr))
        {
            cleanupWicAndCom();
            return false;
        }

        hr = frame->Commit();
        if (FAILED(hr))
        {
            cleanupWicAndCom();
            return false;
        }

        hr = encoder->Commit();
        cleanupWicAndCom();

        return SUCCEEDED(hr);
    }
}

extern "C"
{
    __declspec(dllexport) void __stdcall RendererDX12_SetProgressCallback(void(__stdcall* callback)(const wchar_t*))
    {
        gProgressCallback = callback;
    }

    __declspec(dllexport) void __stdcall RendererDX12_SetAudioManager(AudioManager* audioManager)
    {
        gAudioManagerPtr = audioManager;
        // The host may register audio either side of RendererDX12_Initialize, so the
        // scene renderer is told from both places rather than only from one of them.
        if (gSceneRenderer)
            gSceneRenderer->SetAudioManager(audioManager);
    }

    // Called before initialization: the child process owns a separate scene and DXGI viewport.
    __declspec(dllexport) void __stdcall RendererDX12_ConfigureStandaloneGame(const wchar_t* level)
    {
        gStandaloneLevel = level ? std::filesystem::path(level).string() : std::string();
        gStandaloneLevelLoaded = gStandaloneStarted = false;
        gStandaloneWarmupBegin = gStandaloneLastWarmupFrame = {};
        gStandaloneSmoothFrames = 0;
        QtUi::SetStandaloneGame(!gStandaloneLevel.empty());
        // mShowConsolePanel defaults to true for the editor's own docking layout; a
        // standalone game must start with the console closed and only open it on tilde.
        if (bool* showConsole = gEditor.GetShowConsolePanelPointer())
            *showConsole = false;
    }

    __declspec(dllexport) bool __stdcall RendererDX12_Initialize(HWND windowHandle)
    {
        // First thing in the process that does anything: the crash handlers it
        // installs are the only record of a failure during startup, and a level
        // that will not load has to leave evidence behind.
        PteroLog::Initialize();
        PTERO_LOG_INFO("Renderer", "RendererDX12_Initialize starting.");

        ClearRendererError();
        gEditor.Shutdown();
        gSceneRenderer = std::make_unique<DX12SceneRenderer>();
        gSceneRenderer->SetAudioManager(gAudioManagerPtr);
        gBackBufferHasBeenPresented.fill(false);
        gParkedBackBufferHasBeenPresented.fill(false);
        gRendererStatisticsText = RendererStatisticsText();
        gShowRendererStatistics = true;

        ReportProgress(L"Creating DXGI factory and D3D12 device...");

        // Forward the same callback into the context layer so DX12 setup steps
        // are also surfaced on the splash screen.
        DX12Context_SetProgressCallback(gProgressCallback);
        gSceneRenderer->SetProgressCallback(gProgressCallback);
        gEditor.SetProgressCallback(gProgressCallback);

        ReportProgress(L"Initializing Qt editor..." );
        if (!QtUi::Initialize(windowHandle)) return false;
        // Before the swap chain exists, so it is created at the saved size: a packaged game
        // opens straight into its fullscreen/borderless/windowed mode, never a plain window.
        if (QtUi::IsStandaloneGame())
            ApplySavedGameDisplayMode();
        if (!DX12Context_Initialize(QtUi::ViewportHandle()))
        {
            const char* contextError = DX12Context_GetLastError();
            SetRendererError(contextError != nullptr
                ? std::string("RendererDX12_Initialize failed during DX12 context startup: ") + contextError
                : "RendererDX12_Initialize failed during DX12 context startup.");
            DX12Context_Shutdown();
            QtUi::Shutdown();
            return false;
        }

        DX12Context_StreamlineInitialize();

        const StartupShaderPrecompileResult shaderPrecompileResult = PrecompileStartupShaders();
        if (shaderPrecompileResult.Failed > 0)
        {
            std::ostringstream errorMessage;
            errorMessage << "Startup shader cache warmup found " << shaderPrecompileResult.Failed
                << " failed shader entries. First failure: " << shaderPrecompileResult.FirstFailure;
            SetRendererError(errorMessage.str());
        }

        ReportProgress(L"Initializing Qt viewport renderer...");

        if (!QtViewportRenderer::Initialize(DX12Context_GetDevice()))
        {
            if (gRendererLastError.empty())
            {
                SetRendererError("RendererDX12_Initialize failed during Qt viewport startup.");
            }
            DX12Context_Shutdown();
            QtViewportRenderer::Shutdown();
            QtUi::Shutdown();
            return false;
        }

        // The video layer's SRV slots come from the shared heap, which never frees, so they
        // are taken once here for the life of the renderer. A failure only costs video.
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandles[VideoTexture::kDescriptorCount]{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpuHandles[VideoTexture::kDescriptorCount]{};
            bool allocated = true;
            for (int i = 0; i < VideoTexture::kDescriptorCount && allocated; ++i)
                allocated = DX12Context_AllocateSrvDescriptor(&cpuHandles[i], &gpuHandles[i]);

            // Same target format as the viewport blit it is drawn after.
            if (!allocated || !gSceneRenderer->GetVideoLayer().InitializeGpu(
                    DX12Context_GetDevice(), cpuHandles, gpuHandles, DXGI_FORMAT_R8G8B8A8_UNORM))
            {
                PTERO_LOG_WARNING("Video", "Video layer unavailable: %s",
                    allocated ? "pipeline creation failed" : "no free SRV descriptors");
            }
        }

        RegisterEngineCVars(*gSceneRenderer);
        // Registered here rather than in RegisterEngineCVars because it belongs to the
        // editor, not the renderer, and gEditor is what this file owns.
        CVar::RegisterBool("ed.playinnewwindow", gEditor.GetPlayInNewWindowPointer(),
            "Play opens a window of its own instead of running inside the editor viewport.");
        StartWatchdog();

        gQtUiReady = true;
        ReportProgress(L"Renderer initialized.");
        PTERO_LOG_INFO("Renderer", "Renderer initialized.");

        return true;
    }

    // Reported by the host loop: the two things that happen between render calls.
    __declspec(dllexport) void __stdcall RendererDX12_SetLoopTimings(
        float pumpMilliseconds, float audioMilliseconds, unsigned messageCount, unsigned paintMessageCount)
    {
        gRendererStatisticsText.SetLoopTimings(
            pumpMilliseconds, audioMilliseconds, messageCount, paintMessageCount);
    }

    __declspec(dllexport) bool __stdcall RendererDX12_Render()
    {
        // A removed device never comes back without being recreated from
        // scratch, so every later frame would fail identically. Say why once -
        // with whatever DRED recorded about the GPU operation that killed it -
        // then stop, instead of writing the same line sixty times a second and
        // burying the evidence above it.
        if (DX12Context_IsDeviceRemoved())
        {
            if (!gDeviceRemovedReported)
            {
                gDeviceRemovedReported = true;
                DX12Context_LogDeviceRemovedDiagnostics();
                // Straight after the page-fault address, so the two can be read
                // together: whose memory that address was is not something DRED
                // will say, but it is obvious against a list of what is resident.
                if (gSceneRenderer) { gSceneRenderer->LogGpuProgress(); gSceneRenderer->LogPassOrder(); gSceneRenderer->LogLiveGpuBufferRanges(); }
                PTERO_LOG_FATAL("Renderer",
                    "Rendering has stopped. Restart the editor; the log above names the last GPU work that ran.");
                PteroLog::Flush();

                // The viewport is drawn by this function, so nothing on screen
                // will update again - including the Console. Without this the
                // editor just appears to freeze.
                const std::string message =
                    "The graphics device was lost and rendering has stopped.\n\n"
                    "The reason and the last GPU work that ran were written to:\n"
                    + PteroLog::SessionFilePathUtf8()
                    + "\n\nPlease restart the editor.";
                MessageBoxA(nullptr, message.c_str(), "Ptero Engine - device lost",
                            MB_OK | MB_ICONERROR);
            }
            return false;
        }

        gFrameHeartbeat.fetch_add(1, std::memory_order_relaxed);
        gRenderThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
        const FrameProfiler frameProfiler;
        // Play requests and scene restoration happen before command recording begins.
        gEditor.UpdatePlaySession();
        QtUi::NewFrame();
        if (QtUi::ViewportHandle() != DX12Context_GetWindowHandle())
        {
            // Frame generation is bound to the chain it was configured with, which
            // is about to be parked; it is recreated for the new one below.
            gSceneRenderer->GetFrameGeneration().Release();
            if (!DX12Context_SetPresentationWindow(QtUi::ViewportHandle()))
            {
                const char* error=DX12Context_GetLastError();
                SetRendererError(error ? error : "Could not transfer the renderer to the Play window.");
                if (gSceneRenderer->IsGameRunning()) gSceneRenderer->StopGame();
                gEditor.UpdatePlaySession();
                return false;
            }
            gBackBufferHasBeenPresented.swap(gParkedBackBufferHasBeenPresented);
        }
        RECT viewportRect{};
        GetClientRect(QtUi::ViewportHandle(), &viewportRect);
        UINT renderWidth=0, renderHeight=0;
        DX12Context_GetRenderSize(&renderWidth, &renderHeight);
        if (viewportRect.right > 0 && viewportRect.bottom > 0 && (renderWidth != viewportRect.right || renderHeight != viewportRect.bottom))
        {
            // Released before the proxy chain resizes its buffers, and recreated at
            // the new display size below.
            gSceneRenderer->GetFrameGeneration().Release();
            if (!DX12Context_Resize(viewportRect.right, viewportRect.bottom)) return false;
            gBackBufferHasBeenPresented.fill(false);
            // A swap-chain resize flushes the GPU. If this keeps climbing while the window
            // sits still, the requested size never matches what the context reports back
            // and the editor is paying a full pipeline stall every single frame.
            ++gSwapChainResizeCount;
        }
        gRendererStatisticsText.SetViewportInfo(
            static_cast<UINT>(viewportRect.right), static_cast<UINT>(viewportRect.bottom), gSwapChainResizeCount);

        // FSR frame generation presents through a proxy swap chain of its own, so
        // turning it on or off replaces the chain. That can only happen here,
        // between frames, and it drops every back buffer back to COMMON state.
        {
            FsrSettings& fsrSettings = gSceneRenderer->GetFsrSettings();
            const bool wantProxy = gSceneRenderer->WantsFrameGenerationSwapChain();
            if (wantProxy != DX12Context_IsFrameGenerationSwapChain())
            {
                gSceneRenderer->GetFrameGeneration().Release();
                if (!DX12Context_SetFrameGenerationSwapChain(wantProxy) && wantProxy)
                {
                    // Left with the plain chain. Switch the setting off rather than
                    // retrying a full GPU flush and chain rebuild every frame.
                    fsrSettings.FrameGeneration = false;
                    const char* contextError = DX12Context_GetLastError();
                    PTERO_LOG_ERROR("FSR", "Frame generation switched off: %s",
                        contextError ? contextError : "the proxy swap chain could not be created.");
                }
                gBackBufferHasBeenPresented.fill(false);
            }

            UINT displayWidth = 0, displayHeight = 0;
            DX12Context_GetRenderSize(&displayWidth, &displayHeight);
            gSceneRenderer->GetFrameGeneration().Update(
                DX12Context_IsFrameGenerationSwapChain() ? DX12Context_GetSwapChain() : nullptr,
                displayWidth,
                displayHeight,
                gSceneRenderer->GetSceneWidth(),
                gSceneRenderer->GetSceneHeight(),
                fsrSettings);
        }
        if (!gSceneRenderer->ApplyPendingMsaaSettings())
        {
            const char* sceneError = gSceneRenderer->GetLastErrorMessage();
            SetRendererError(sceneError != nullptr
                ? std::string("RendererDX12_Render could not apply MSAA settings: ") + sceneError
                : "RendererDX12_Render could not apply MSAA settings.");
            return false;
        }

        // Acquire command list + current swap chain back buffer for this frame.
        ID3D12GraphicsCommandList* commandList = nullptr;
        ID3D12Resource* backBuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
        UINT frameIndex = 0;

        if (!DX12Context_BeginFrame(&commandList, &backBuffer, &rtvHandle, &frameIndex))
        {
            // BeginFrame now waits for the back buffer instead of reporting it busy, so
            // reaching here is a real failure rather than routine back-pressure.
            const char* contextError = DX12Context_GetLastError();
            SetRendererError(contextError != nullptr
                ? std::string("RendererDX12_Render could not begin a frame: ") + contextError
                : "RendererDX12_Render could not begin a frame.");
            // A GPU that stops retiring frames without being removed leaves DRED
            // with nothing to say; the progress breadcrumbs name the pass instead.
            // Once per stall is enough - the answer does not change while it lasts.
            static bool gpuProgressLogged = false;
            if (!gpuProgressLogged && gSceneRenderer)
            {
                gSceneRenderer->LogGpuProgress();
                gSceneRenderer->LogPassOrder();
                gpuProgressLogged = true;
            }
            return false;
        }

        try
        {
        gRendererStatisticsText.MarkFrame();
        gEditor.UpdateSceneLoading();

        if (gEditor.GetViewportResolutionChangeRequested())
        {
            const UINT requestedWidth = static_cast<UINT>((std::max)(1, gEditor.GetRequestedViewportResolutionWidth()));
            const UINT requestedHeight = static_cast<UINT>((std::max)(1, gEditor.GetRequestedViewportResolutionHeight()));
            if (!gSceneRenderer->ResizeSceneTarget(requestedWidth, requestedHeight))
            {
                const char* sceneError = gSceneRenderer->GetLastErrorMessage();
                SetRendererError(sceneError != nullptr
                    ? std::string("Viewport resolution change failed: ") + sceneError
                    : "Viewport resolution change failed.");
            }
            else
            {
                gEditor.SetViewportResolution(static_cast<int>(requestedWidth), static_cast<int>(requestedHeight));
            }
            gEditor.ClearViewportResolutionChangeRequest();
        }

        ReportProgress(L"Initializing scene renderer...");
        const bool sceneReady = gSceneRenderer->Initialize(commandList);
        if (sceneReady)
        {
            if (QtUi::IsStandaloneGame() && !gStandaloneLevelLoaded)
            {
                gEditor.SetSceneRenderer(gSceneRenderer.get());
                gEditor.SetTerrainRenderer(&gSceneRenderer->GetTerrainRenderer());
                gEditor.SetFsrSettings(&gSceneRenderer->GetFsrSettings());
                gEditor.SetSubsurfaceSettings(&gSceneRenderer->GetSubsurfaceSettings());
                gEditor.SetSceneSettings(
                    &gSceneRenderer->GetTimeOfDaySettings(),
                    &gSceneRenderer->GetTaaSettings(),
                    &gSceneRenderer->GetSmaaSettings(),
                    &gSceneRenderer->GetSharpenSettings(),
                    &gSceneRenderer->GetDlssSettings(),
                    &gSceneRenderer->GetGlobalIlluminationMode(),
                    &gSceneRenderer->GetRtgiSettings(),
                    &gSceneRenderer->GetRadianceCascadesSettings(),
                    &gSceneRenderer->GetRtaoSettings(),
                    &gSceneRenderer->GetGtaoSettings(),
                    &gSceneRenderer->GetSsrSettings(),
                    &gSceneRenderer->GetChromaticAberrationSettings(),
                    &gSceneRenderer->GetAgxSettings(),
                    &gSceneRenderer->GetVolumetricFogSettings(),
                    &gSceneRenderer->GetVolumetricCloudSettings(),
                    &gSceneRenderer->GetBloomSettings());
                if (!gEditor.LoadSceneFromFile(gStandaloneLevel))
                    throw std::runtime_error("Could not load the Farkle play level: " + gStandaloneLevel);
                gEditor.SetShowViewportGrid(false);
                gSceneRenderer->SetGridEnabled(false);
                gStandaloneLevelLoaded = true;
                // The player's saved graphics settings, now, so the warm-up frames below
                // compile the pipelines the game will actually use. They also replace
                // whatever upscaler/frame generation state the level was last saved with.
                gSceneRenderer->PrepareStandaloneGameSettings();
            }
            // Vegetation layers reference meshes by path but have no entity to
            // hang a MeshComponent on, so they load through this callback
            // rather than the per-entity resolution loop below.
            gSceneRenderer->GetVegetationRenderer().SetMeshResolver(
                [](const std::string& relativePath) -> std::shared_ptr<Mesh>
                {
                    return gAssetManager.GetMesh(relativePath);
                });

            if (gEditor.HasPendingCameraRestore())
            {
                gSceneRenderer->SetCameraTransform(
                    gEditor.GetPendingCameraRestorePosition(),
                    gEditor.GetPendingCameraRestoreRotation());
                gEditor.ConsumePendingCameraRestore();
            }

            // For each entity that has a MeshPath but no loaded MeshAsset, ask the
            // AssetManager to load it now so it becomes visible in the next frame.
            // While the editor is bringing in a freshly opened level, only a
            // time-boxed slice of them loads per frame: the whole level in one frame
            // stalls the message pump long enough for Windows to call the editor
            // "Not Responding", and the loading overlay could never update.
            const bool streamingSceneAssets = gEditor.IsStreamingSceneAssets();
            if (!streamingSceneAssets)
                gStreamingFailedMeshPaths.clear();
            const auto meshBudgetStart = std::chrono::steady_clock::now();
            std::size_t sceneMeshTotal = 0;
            std::size_t sceneMeshResolved = 0;
            for (Entity& entity : gEditor.GetEntities())
            {
                if (!entity.HasMeshComponent())
                {
                    continue;
                }

                MeshComponent& mc = *entity.Mesh;
                if (streamingSceneAssets && !mc.MeshPath.empty())
                {
                    ++sceneMeshTotal;
                    // A mesh that failed once this load counts as done; it would
                    // otherwise hold the overlay up forever.
                    if (mc.MeshAsset || gStreamingFailedMeshPaths.count(mc.MeshPath) != 0)
                    {
                        ++sceneMeshResolved;
                        continue;
                    }
                    if (std::chrono::steady_clock::now() - meshBudgetStart > kSceneMeshStreamingBudget)
                        continue;
                }

                if (!mc.MeshPath.empty() && !mc.MeshAsset)
                {
                    {
                        std::ostringstream logStream;
                        logStream << "Attempting mesh load for entity='" << entity.Name
                                  << "', meshPath='" << mc.MeshPath
                                  << "', materialPath='" << mc.MaterialPath << "'";
                        LogRendererMeshDiagnostic(logStream.str());
                    }

                    mc.MeshAsset = gAssetManager.GetMesh(mc.MeshPath);
                    if (mc.MeshAsset)
                    {
                        std::ostringstream logStream;
                        logStream << "Mesh load succeeded for entity='" << entity.Name
                                  << "', vertices=" << mc.MeshAsset->GetVertices().size()
                                  << ", indices=" << mc.MeshAsset->GetIndices().size()
                                  << ", subMeshes=" << mc.MeshAsset->GetSubMeshes().size();
                        LogRendererMeshDiagnostic(logStream.str());
                    }
                    else
                    {
                        std::ostringstream logStream;
                        logStream << "Mesh load failed for entity='" << entity.Name
                                  << "', meshPath='" << mc.MeshPath
                                  << "', error='" << gAssetManager.GetLastErrorMessage() << "'";
                        LogRendererMeshDiagnostic(logStream.str());
                        if (streamingSceneAssets)
                            gStreamingFailedMeshPaths.insert(mc.MeshPath);
                    }
                    if (streamingSceneAssets)
                        ++sceneMeshResolved;
                }
            }
            if (streamingSceneAssets)
                gEditor.SetSceneAssetStreamingProgress(sceneMeshResolved, sceneMeshTotal);

            if (gAudioManagerPtr != nullptr && gAudioManagerPtr->IsInitialized())
            {
                const EditorCamera& camera = gSceneRenderer->GetCamera();
                const DirectX::XMFLOAT3 cameraPosition = camera.GetPosition();
                const DirectX::XMFLOAT3 cameraForward = camera.GetForwardVector();
                const DirectX::XMFLOAT3 cameraUp = camera.GetUpVector();
                gAudioManagerPtr->SetListenerTransform(
                    cameraPosition.x,
                    cameraPosition.y,
                    cameraPosition.z,
                    cameraForward.x,
                    cameraForward.y,
                    cameraForward.z,
                    cameraUp.x,
                    cameraUp.y,
                    cameraUp.z);

                for (Entity& entity : gEditor.GetEntities())
                {
                    if (!entity.HasAudioEmitterComponent())
                        continue;

                    AudioEmitterComponent& audioEmitter = *entity.AudioEmitter;
                    if (audioEmitter.RuntimeEmitterHandle < 0)
                    {
                        audioEmitter.RuntimeEmitterHandle = gAudioManagerPtr->RegisterEmitter().Value;
                        audioEmitter.RuntimeRegisteredEventPath.clear();
                        audioEmitter.RuntimeAutoPlayStarted = false;
                    }

                    const AudioManager::EmitterHandle handle{ audioEmitter.RuntimeEmitterHandle };
                    if (audioEmitter.RuntimeRegisteredEventPath != audioEmitter.EventPath)
                    {
                        gAudioManagerPtr->SetEmitterEventPath(handle, audioEmitter.EventPath);
                        audioEmitter.RuntimeRegisteredEventPath = audioEmitter.EventPath;
                        audioEmitter.RuntimeAutoPlayStarted = false;
                    }

                    gAudioManagerPtr->SetEmitterTransform(
                        handle,
                        entity.Transform.Position.x,
                        entity.Transform.Position.y,
                        entity.Transform.Position.z);

                    // A packaged game stays silent through loading and the intro videos;
                    // the level's ambience starts with the game itself.
                    const bool ambienceAllowed = !QtUi::IsStandaloneGame() || gSceneRenderer->IsGameSessionRunning();
                    if (audioEmitter.AutoPlay && ambienceAllowed && !audioEmitter.EventPath.empty() && !audioEmitter.RuntimeAutoPlayStarted)
                    {
                        audioEmitter.RuntimeAutoPlayStarted = gAudioManagerPtr->PlayEmitter(handle);
                    }
                    else if (!audioEmitter.AutoPlay && audioEmitter.RuntimeAutoPlayStarted)
                    {
                        gAudioManagerPtr->StopEmitter(handle);
                        audioEmitter.RuntimeAutoPlayStarted = false;
                    }
                }
            }

            // Give the scene renderer an up-to-date view of the entity list every frame.
            gSceneRenderer->SetEntities(&gEditor.GetEntities());

            // The node graph editor keeps one document for the whole session and hands
            // out a stable reference to it, so this only has to be wired up once.
            gSceneRenderer->SetNodeGraph(&gEditor.GetRuntimeNodeGraph());

            if (QtUi::IsStandaloneGame() && !gStandaloneStarted)
            {
                const auto now = std::chrono::steady_clock::now();
                if (gStandaloneWarmupBegin == std::chrono::steady_clock::time_point{})
                    gStandaloneWarmupBegin = now;
                const bool haveInterval = gStandaloneLastWarmupFrame != std::chrono::steady_clock::time_point{};
                const float intervalMilliseconds = haveInterval
                    ? std::chrono::duration<float, std::milli>(now - gStandaloneLastWarmupFrame).count()
                    : 0.0f;
                gStandaloneLastWarmupFrame = now;
                gStandaloneSmoothFrames = (haveInterval && intervalMilliseconds < kWarmupSmoothFrameMilliseconds)
                    ? gStandaloneSmoothFrames + 1 : 0;

                const bool warmedUp = gStandaloneSmoothFrames >= kWarmupSmoothFramesNeeded;
                if (warmedUp || now - gStandaloneWarmupBegin > kWarmupTimeout)
                {
                    PTERO_LOG_INFO("Game", "Level warmed up in %.1f s%s; starting the intro.",
                        std::chrono::duration<float>(now - gStandaloneWarmupBegin).count(),
                        warmedUp ? "" : " (timed out waiting for smooth frames)");
                    // A standalone build already owns the only window there is, so the mode
                    // is moot - OpenGameWindow takes the standalone path either way.
                    if (!gSceneRenderer->StartGame(true))
                        throw std::runtime_error(gSceneRenderer->GetGameStartErrorMessage());
                    gStandaloneStarted = true;
                }
            }

            ReportProgress(L"Rendering initial scene frame...");
            const auto sceneStart = std::chrono::steady_clock::now();
            gSceneRenderer->Render(commandList);
            gSceneMilliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - sceneStart).count();
            gEditor.SetRendererTimingSnapshot(gSceneRenderer->GetRendererTimingSnapshot());
            const SystemUsageSnapshot usageSnapshot = gSystemUsageSampler.Update();
            gRendererStatisticsText.SetRuntimeStatistics(
                usageSnapshot.CpuUsagePercent,
                usageSnapshot.GpuUsagePercent,
                usageSnapshot.RamUsagePercent,
                gSceneRenderer->GetCamera().GetPosition(),
                gSceneRenderer->GetCamera().GetRotation());
            gEditor.SetResourceUsageSnapshot(BuildResourceUsageSnapshot(usageSnapshot));
            // Transition depth to PIXEL_SHADER_RESOURCE so the G-Buffer debug window
            // in Qt UI can sample it.  Restored to DEPTH_WRITE after Qt UI renders.
            gSceneRenderer->TransitionDepthForRead(commandList);
            ReportProgress(L"Initializing editor UI assets...");
            // Hand the editor a pointer to the terrain renderer so the brush
            // tool can paint heightmaps and pick terrain height.
            gEditor.SetTerrainRenderer(&gSceneRenderer->GetTerrainRenderer());
            gEditor.SetSceneRenderer(gSceneRenderer.get());
            if (QtUi::IsStandaloneGame())
            {
                // No menu/toolbar to toggle it from in a packaged build, so the tilde key
                // (Quake-style) does it - polled like the game's other hotkeys (see
                // DX12SceneRenderer's PollAction) rather than through window messages,
                // since Qt's own event loop - not RendererDX12_HandleWindowMessage -
                // consumes native keyboard input once a Qt widget has focus.
                static bool sConsoleToggleKeyWasDown = false;
                // Only while the game (or its console) is the foreground app: the key
                // state is global, and a tilde typed elsewhere is none of the game's business.
#ifdef PTERO_GAME_RUNTIME
                const bool gameHasKeyboard = QtUi::GameWindowHasFocus() || GameConsoleWindow::HasFocus();
#else
                const bool gameHasKeyboard = QtUi::GameWindowHasFocus();
#endif
                const bool consoleToggleKeyDown = gameHasKeyboard && (GetAsyncKeyState(VK_OEM_3) & 0x8000) != 0;
                if (consoleToggleKeyDown && !sConsoleToggleKeyWasDown)
                {
                    if (bool* showConsole = gEditor.GetShowConsolePanelPointer())
                        *showConsole = !*showConsole;
                }
                sConsoleToggleKeyWasDown = consoleToggleKeyDown;

#ifdef PTERO_GAME_RUNTIME
                // No Qt in a packaged game, so no Console panel: a native drop-down
                // stands in for it (see GameConsoleWindow.h).
                if (bool* showConsole = gEditor.GetShowConsolePanelPointer())
                    *showConsole = GameConsoleWindow::Update(QtUi::HostHandle(), *showConsole);
#else
                gEditor.DrawStandaloneConsoleIfVisible();
#endif
            }
            else
            {
                gEditor.Initialize(commandList);
            }
        }

        // Swap-chain buffers begin life in COMMON state. After the first successful
        // present they transition between PRESENT and RENDER_TARGET every frame.
        const D3D12_RESOURCE_STATES backBufferStateBeforeRender =
            gBackBufferHasBeenPresented[frameIndex]
            ? D3D12_RESOURCE_STATE_PRESENT
            : D3D12_RESOURCE_STATE_COMMON;

        // Transition the back buffer into render-target state.
        auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer,
            backBufferStateBeforeRender,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->ResourceBarrier(1, &toRenderTarget);

        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        bool frameGenerationFinished = false;

        // Match the editor background so the app chrome stays neutral while the actual
        // scene is shown inside the in-app viewport window.
        // Black for a packaged game still warming up (see kWarmupSmoothFramesNeeded): the
        // level is being rendered, but not shown until the intro has played.
        const bool hideSceneUntilIntro = QtUi::IsStandaloneGame() && !gStandaloneStarted;
        const float editorClearColor[] = { 0.08f, 0.10f, 0.14f, 1.0f };
        const float blackClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        const float* clearColor = QtUi::IsStandaloneGame() ? blackClearColor : editorClearColor;
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        if (gQtUiReady)
        {
            // Keep the editor menu rendering in its own file so this function only orchestrates the frame.
            float cameraSpeed = gSceneRenderer->GetCameraMovementSpeed();
            float viewDistanceMeters = gSceneRenderer->GetViewDistanceMeters();
            bool gridEnabled = gSceneRenderer->IsGridEnabled();
            bool showViewportPlacementIcons = gEditor.GetShowViewportPlacementIcons();
            GBufferDebugTextureIds gbufferIds;
            gbufferIds.Albedo   = gSceneRenderer->GetGBufferAlbedoTextureId();
            gbufferIds.Normal   = gSceneRenderer->GetGBufferNormalTextureId();
            gbufferIds.Material = gSceneRenderer->GetGBufferMaterialTextureId();
            gbufferIds.Depth    = gSceneRenderer->GetGBufferDepthTextureId();
            gbufferIds.GiAccum  = gSceneRenderer->GetGiAccumTextureId();
            gbufferIds.PointShadowArray = gSceneRenderer->GetPointShadowDebugTextureId();
            const FsrRuntimeStatus fsrStatus = gSceneRenderer->GetFsrRuntimeStatus();
            gUiBuildStart = std::chrono::steady_clock::now();
            if (!QtUi::IsGameWindowOpen()) RenderEditorMainMenu(
                QtUi::HostHandle(),
                &gEditor,
                gSceneRenderer->GetSceneTextureId(),
                gSceneRenderer->GetLastErrorMessage(),
                gRendererStatisticsText.GetText(),
                &cameraSpeed,
                &viewDistanceMeters,
                &gridEnabled,
                &gShowRendererStatistics,
                &showViewportPlacementIcons,
                gEditor.GetShowComponentsPanelPointer(),
                gEditor.GetShowLevelExplorerPanelPointer(),
                gEditor.GetShowPropertiesPanelPointer(),
                gEditor.GetShowResourceDebugPanelPointer(),
                &gSceneRenderer->GetTaaSettings(),
                &gSceneRenderer->GetSmaaSettings(),
                &gSceneRenderer->GetMsaaSettings(),
                &gSceneRenderer->GetSharpenSettings(),
                &gSceneRenderer->GetDlssSettings(),
                &gSceneRenderer->GetFsrSettings(),
                &fsrStatus,
                &gSceneRenderer->GetTimeOfDaySettings(),
                &gSceneRenderer->GetWindSettings(),
                &gSceneRenderer->GetGlobalIlluminationMode(),
                &gSceneRenderer->GetRtgiSettings(),
                &gSceneRenderer->GetRadianceCascadesSettings(),
                &gSceneRenderer->GetProbeSettings(),
                &gSceneRenderer->GetRtaoSettings(),
                &gSceneRenderer->GetGtaoSettings(),
                &gSceneRenderer->GetSsrSettings(),
                &gSceneRenderer->GetSubsurfaceSettings(),
                gSceneRenderer->IsSubsurfaceRayTracingSupported(),
                &gSceneRenderer->GetChromaticAberrationSettings(),
                &gSceneRenderer->GetAgxSettings(),
                &gSceneRenderer->GetVolumetricFogSettings(),
                &gSceneRenderer->GetVolumetricCloudSettings(),
                &gSceneRenderer->GetBloomSettings(),
                &gSceneRenderer->GetPointShadowSettings(),
                &gbufferIds,
                gAudioManagerPtr,
                CompileShadersFromMainMenu,
                gSceneRenderer->GetMsaaResolveTimeMs());
            gSceneRenderer->SetViewDistanceMeters(viewDistanceMeters);
            gEditor.SetShowViewportGrid(gridEnabled);
            gEditor.SetFsrSettings(&gSceneRenderer->GetFsrSettings());
            gEditor.SetSubsurfaceSettings(&gSceneRenderer->GetSubsurfaceSettings());
            gEditor.SetSceneSettings(
                &gSceneRenderer->GetTimeOfDaySettings(),
                &gSceneRenderer->GetTaaSettings(),
                &gSceneRenderer->GetSmaaSettings(),
                &gSceneRenderer->GetSharpenSettings(),
                &gSceneRenderer->GetDlssSettings(),
                &gSceneRenderer->GetGlobalIlluminationMode(),
                &gSceneRenderer->GetRtgiSettings(),
                &gSceneRenderer->GetRadianceCascadesSettings(),
                &gSceneRenderer->GetRtaoSettings(),
                &gSceneRenderer->GetGtaoSettings(),
                &gSceneRenderer->GetSsrSettings(),
                &gSceneRenderer->GetChromaticAberrationSettings(),
                &gSceneRenderer->GetAgxSettings(),
                &gSceneRenderer->GetVolumetricFogSettings(),
                &gSceneRenderer->GetVolumetricCloudSettings(),
                &gSceneRenderer->GetBloomSettings());
            gEditor.SetShowViewportPlacementIcons(showViewportPlacementIcons);
            gEditor.SetWireframeEnabled(gSceneRenderer->IsWireframeEnabled());
            gEditor.Draw(
                gSceneRenderer->GetSceneTextureHandle(),
                gSceneRenderer->GetCamera(),
                gSceneRenderer->GetLastErrorMessage(),
                gRendererStatisticsText.GetText(),
                gShowRendererStatistics,
                gAudioManagerPtr);

            int viewportContentWidth = 0;
            int viewportContentHeight = 0;
            if (!gSceneRenderer->GetDlssSettings().Enabled
                && !gSceneRenderer->IsFsrUpscalerActive()
                && !gEditor.IsViewportResolutionFixed()
                && gEditor.GetLastViewportContentResolution(viewportContentWidth, viewportContentHeight))
            {
                const UINT requestedWidth = static_cast<UINT>(viewportContentWidth);
                const UINT requestedHeight = static_cast<UINT>(viewportContentHeight);
                if (requestedWidth != gSceneRenderer->GetSceneWidth() || requestedHeight != gSceneRenderer->GetSceneHeight())
                {
                    const bool pendingSizeChanged =
                        !gHasPendingViewportResize
                        || gPendingViewportWidth != viewportContentWidth
                        || gPendingViewportHeight != viewportContentHeight;
                    const auto now = std::chrono::steady_clock::now();
                    if (pendingSizeChanged)
                    {
                        gPendingViewportWidth = viewportContentWidth;
                        gPendingViewportHeight = viewportContentHeight;
                        gPendingViewportResizeSince = now;
                        gHasPendingViewportResize = true;
                    }

                    constexpr int kImmediateResizeDeltaPixels = 96;
                    constexpr auto kViewportResizeDebounce = std::chrono::milliseconds(120);
                    const bool largeDelta =
                        std::abs(static_cast<int>(requestedWidth) - static_cast<int>(gSceneRenderer->GetSceneWidth())) >= kImmediateResizeDeltaPixels
                        || std::abs(static_cast<int>(requestedHeight) - static_cast<int>(gSceneRenderer->GetSceneHeight())) >= kImmediateResizeDeltaPixels;
                    const bool debounceElapsed = gHasPendingViewportResize
                        && (now - gPendingViewportResizeSince) >= kViewportResizeDebounce;
                    const bool userStillDraggingDock = QtUi::IsMouseDown(QtUiMouseButton_Left);
                    if (gHasPendingViewportResize && !userStillDraggingDock && (largeDelta || debounceElapsed))
                    {
                        gEditor.RequestViewportResolution(gPendingViewportWidth, gPendingViewportHeight);
                        gHasPendingViewportResize = false;
                    }
                }
                else
                {
                    gHasPendingViewportResize = false;
                }
            }

            gSceneRenderer->SetCameraMovementSpeed(cameraSpeed);
            gSceneRenderer->SetGridEnabled(gEditor.GetShowViewportGrid());
            gSceneRenderer->SetWireframeEnabled(gEditor.GetWireframeEnabled());

            // Handle screenshot capture if requested
            if (gEditor.GetScreenshotRequested())
            {
                const std::string& folder = gEditor.GetScreenshotFolder();
                if (!folder.empty())
                {
                    std::error_code directoryError;
                    std::filesystem::create_directories(folder, directoryError);

                    // Generate filename with timestamp
                    auto now = std::chrono::system_clock::now();
                    auto time = std::chrono::system_clock::to_time_t(now);
                    std::tm localTime{};
                    localtime_s(&localTime, &time);

                    std::ostringstream fileNameBuilder;
                    fileNameBuilder << "screenshot_"
                                    << std::put_time(&localTime, "%Y%m%d_%H%M%S")
                                    << ".png";

                    std::filesystem::path outputPath = std::filesystem::path(folder) / fileNameBuilder.str();
                    outputPath.make_preferred();

                    ID3D12Resource* sceneTexture = gSceneRenderer->GetSceneColorTargetResource();
                    if (sceneTexture && SaveScreenshotToPNG(sceneTexture, outputPath.string()))
                    {
                        OutputDebugStringA(("Screenshot saved to: " + outputPath.string() + "\n").c_str());
                    }
                    else
                    {
                        OutputDebugStringA("Failed to save screenshot.\n");
                    }
                }
                gEditor.ClearScreenshotRequest();
            }

            if (gUiBuildStart.time_since_epoch().count() != 0)
            {
                gUiBuildMilliseconds =
                    std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - gUiBuildStart).count();
            }
            QtUi::EndFrame();
            gRendererStatisticsText.SetUiFrameMilliseconds(QtUi::FrameMilliseconds());
            gTailStart = std::chrono::steady_clock::now();
            gRendererStatisticsText.SetFrameBreakdown(
                gRenderMilliseconds, gOutsideMilliseconds, gTailMilliseconds);
            gRendererStatisticsText.SetUiPhases(
                gSceneMilliseconds, gUiBuildMilliseconds, QtUi::EventMilliseconds());
            unsigned qtUpdates = 0, qtLayouts = 0, qtPaints = 0, topPainterCount = 0;
            const char* topPainter = nullptr;
            QtUi::EventCounts(qtUpdates, qtLayouts, qtPaints, topPainter, topPainterCount);
            gRendererStatisticsText.SetQtEventCounts(
                qtUpdates, qtLayouts, qtPaints, topPainter, topPainterCount, QtUi::EventTypes());

            ID3D12DescriptorHeap* descriptorHeaps[] = { DX12Context_GetSrvDescriptorHeap() };
            commandList->SetDescriptorHeaps(1, descriptorHeaps);
            RECT viewportRect{};
            // Stop may have hidden the game window during this frame. Finish the
            // command list against the chain acquired by BeginFrame; transfer next frame.
            GetClientRect(DX12Context_GetWindowHandle(), &viewportRect);
            if (!hideSceneUntilIntro)
                QtViewportRenderer::Draw(commandList, gSceneRenderer->GetSceneTextureId(), viewportRect.right, viewportRect.bottom);

            // The back buffer now holds the scene and nothing else. Frame generation
            // keeps a copy of it, so it can tell the game UI drawn next apart from
            // the scene and keep the UI from being dragged along with scene motion.
            gSceneRenderer->GetFrameGeneration().FinishFrame(commandList, backBuffer, gSceneRenderer->GetFsrSettings());
            frameGenerationFinished = true;

            // A playing video covers the scene but stays under the game UI, so menus and
            // "skip" prompts can sit on top of it. After FinishFrame, so frame generation
            // treats it like UI rather than warping it with the scene's motion vectors.
            // Recorded every frame, visible or not: it also retires replaced GPU resources.
            gSceneRenderer->GetVideoLayer().Record(commandList, viewportRect.right, viewportRect.bottom);

            // The game UI composites over the scene here rather than through the Qt UI
            // layer: the viewport is a native surface presented by this blit, and the Qt
            // draw list can only paint file-backed pixmaps, not live GPU textures.
            if (gSceneRenderer->IsGameUiActive())
            {
                QtViewportRenderer::DrawOverlay(
                    commandList,
                    gSceneRenderer->GetRmlUiRenderer().GetOutputTextureId(),
                    viewportRect.right,
                    viewportRect.bottom);
            }

            QtViewportRenderer::DrawDebugViews(commandList);
            // Restore depth to DEPTH_WRITE for the next frame's geometry pass.
            gSceneRenderer->TransitionDepthAfterRead(commandList);
        }

        // A frame that never reached the viewport blit presents without interpolation,
        // but still advances frame generation's frame counter.
        if (!frameGenerationFinished)
            gSceneRenderer->GetFrameGeneration().FinishFrame(nullptr, nullptr, gSceneRenderer->GetFsrSettings());

        // Transition back to present state so swap chain can display the frame.
        auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
            backBuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        commandList->ResourceBarrier(1, &toPresent);

        ReportProgress(L"Presenting initial frame...");
        if (!DX12Context_EndFrame(frameIndex))
        {
            const char* contextError = DX12Context_GetLastError();
            DX12Context_AbortFrame();
            SetRendererError(contextError != nullptr
                ? std::string("RendererDX12_Render failed while presenting a frame: ") + contextError
                : "RendererDX12_Render failed while presenting a frame.");
            return false;
        }

        QtViewportRenderer::PresentDebugViews();
        ReportProgress(L"Initial frame presented.");
        gBackBufferHasBeenPresented[frameIndex] = true;

        // One syscall a frame buys the guarantee that a process killed outright
        // still leaves every line up to the previous frame on disk.
        PteroLog::Flush();

        return true;
        }
        catch (const std::exception& exception)
        {
            DX12Context_AbortFrame();
            PTERO_LOG_ERROR("Renderer", "Frame aborted after an exception: %s", exception.what());
            SetRendererError(std::string("RendererDX12_Render aborted the frame after an exception: ") + exception.what());
            return false;
        }
        catch (...)
        {
            DX12Context_AbortFrame();
            PTERO_LOG_ERROR("Renderer", "Frame aborted after an unknown exception.");
            SetRendererError("RendererDX12_Render aborted the frame after an unknown exception.");
            return false;
        }
    }

    __declspec(dllexport) bool __stdcall RendererDX12_HandleWindowMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (!gQtUiReady)
        {
            return false;
        }

        return false; // Qt dispatches its native child-window events through QApplication.
    }

    __declspec(dllexport) bool __stdcall RendererDX12_Resize(UINT width, UINT height)
    {
        QtUi::ResizeHost(width, height);
        // While Play owns presentation, resizing the editor only changes its parked
        // surface. The frame loop resizes the active chain at the safe boundary.
        if (QtUi::IsGameWindowOpen()) return true;
        RECT viewportRect{};
        GetClientRect(QtUi::ViewportHandle(), &viewportRect);
        UINT currentWidth = 0, currentHeight = 0;
        DX12Context_GetRenderSize(&currentWidth, &currentHeight);
        if (gSceneRenderer && viewportRect.right > 0 && viewportRect.bottom > 0
            && (currentWidth != static_cast<UINT>(viewportRect.right) || currentHeight != static_cast<UINT>(viewportRect.bottom)))
        {
            // As in the frame loop: never resize the proxy chain under a live
            // frame generation context. The next frame recreates it.
            gSceneRenderer->GetFrameGeneration().Release();
        }
        if (!DX12Context_Resize(viewportRect.right, viewportRect.bottom))
        {
            const char* contextError = DX12Context_GetLastError();
            SetRendererError(contextError != nullptr
                ? std::string("RendererDX12_Resize failed: ") + contextError
                : "RendererDX12_Resize failed.");
            return false;
        }

        // Resized swap-chain buffers start life in COMMON state again.
        gBackBufferHasBeenPresented.fill(false);
        return true;
    }

    // Asked by the host before it begins shutting down. Returns false when the
    // user chose Cancel at the unsaved-changes prompt, which means "do not
    // close". Closing the editor is the one path that used to discard an edited
    // level without a word: New and Open both asked, and the window's X did not.
    //
    // Modal, and deliberately on the caller's thread - it runs from WM_CLOSE,
    // before any shutdown work has started, so a Cancel simply carries on
    // rendering.
    __declspec(dllexport) bool __stdcall RendererDX12_ConfirmClose(HWND ownerWindowHandle)
    {
        if (!gQtUiReady || QtUi::IsStandaloneGame())
            return true;
        if (gSceneRenderer && gSceneRenderer->IsGameRunning()) gSceneRenderer->StopGame();
        gEditor.StopPlaySession();
        return gEditor.ConfirmDiscardUnsavedScene(ownerWindowHandle);
    }

    __declspec(dllexport) void __stdcall RendererDX12_Shutdown()
    {
        ReportProgress(L"Waiting for renderer shutdown...");
        gSystemUsageSampler.Stop();
        // The preview window runs on a thread of its own inside Video.dll.
        VideoPlayerWindow::Shutdown();
#ifdef PTERO_GAME_RUNTIME
        GameConsoleWindow::Shutdown();
#endif
        DX12Context_WaitForGPU();
        ReportProgress(L"Releasing editor resources...");
        gEditor.Shutdown();
        ReportProgress(L"Releasing scene resources...");
        if (gSceneRenderer)
        {
            gSceneRenderer->Shutdown();
            gSceneRenderer.reset();
        }
        gEditor.SetSceneRenderer(nullptr);
        gEditor.SetTerrainRenderer(nullptr);
        gEditor.GetEntities().clear();
        ReportProgress(L"Releasing Qt viewport resources...");
        QtViewportRenderer::Shutdown();
        gQtUiReady = false;
        ReportProgress(L"Releasing DX12 context...");
        DX12Context_Shutdown();
        ReportProgress(L"Closing Qt interface...");
        QtUi::Shutdown();
        ReportProgress(L"Renderer shutdown complete.");
        StopWatchdog();
        gBackBufferHasBeenPresented.fill(false);
        PteroLog::Shutdown("editor closed");
    }

    __declspec(dllexport) const char* __stdcall RendererDX12_GetLastError()
    {
        if (!gRendererLastError.empty())
        {
            return gRendererLastError.c_str();
        }

        const char* sceneError = gSceneRenderer ? gSceneRenderer->GetLastErrorMessage() : nullptr;
        return (sceneError != nullptr && sceneError[0] != '\0') ? sceneError : nullptr;
    }
}
