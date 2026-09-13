#include "pch.h"
#include "DX12Helper.h"
#include "DX12SceneRenderer.h"
#include "DX12ShaderCompiler.h"
#include "Editor.h"
#include "EngineCVars.h"
#include "System/PteroLog.h"
#include "RendererStatisticsText.h"

#include "EditorMainMenu.h"

#include "..\System\include\System\AssetManager.h"

#include "../QtUi/QtUi.h"
#include "QtViewportRenderer.h"



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
#include <utility>
#include <vector>
#include <wincodec.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "windowscodecs.lib")


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
    HWND __stdcall DX12Context_GetWindowHandle();
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12CommandQueue* __stdcall DX12Context_GetCommandQueue();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
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
    RendererStatisticsText gRendererStatisticsText;
    AssetManager gAssetManager;
    std::string gRendererLastError;
    std::array<bool, EditorFrameCount> gBackBufferHasBeenPresented{};
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
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return {};
        }

        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
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

        std::error_code relativeError;
        const fs::path relativePath = fs::relative(path, shadersDirectory, relativeError);
        if (relativeError || relativePath.empty())
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

        std::error_code iteratorError;
        for (fs::recursive_directory_iterator it(shadersDirectory, fs::directory_options::skip_permission_denied, iteratorError), end;
             it != end && !iteratorError;
             it.increment(iteratorError))
        {
            std::error_code statusError;
            if (it->is_directory(statusError) && !statusError)
            {
                if (IsExternalShaderPackageDirectory(it->path(), shadersDirectory))
                {
                    it.disable_recursion_pending();
                }

                continue;
            }

            statusError.clear();
            if (!it->is_regular_file(statusError) || statusError)
            {
                continue;
            }

            const fs::path path = it->path();
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
            std::error_code relativeError;
            fs::path relativePath = fs::relative(requestPath, shadersDirectory, relativeError);
            if (relativeError || relativePath.empty())
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
        gBackBufferHasBeenPresented.fill(false);
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

        RegisterEngineCVars(*gSceneRenderer);
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
        const FrameProfiler frameProfiler;
        QtUi::NewFrame();
        RECT viewportRect{};
        GetClientRect(QtUi::ViewportHandle(), &viewportRect);
        UINT renderWidth=0, renderHeight=0;
        DX12Context_GetRenderSize(&renderWidth, &renderHeight);
        if (viewportRect.right > 0 && viewportRect.bottom > 0 && (renderWidth != viewportRect.right || renderHeight != viewportRect.bottom))
        {
            if (!DX12Context_Resize(viewportRect.right, viewportRect.bottom)) return false;
            gBackBufferHasBeenPresented.fill(false);
            // A swap-chain resize flushes the GPU. If this keeps climbing while the window
            // sits still, the requested size never matches what the context reports back
            // and the editor is paying a full pipeline stall every single frame.
            ++gSwapChainResizeCount;
        }
        gRendererStatisticsText.SetViewportInfo(
            static_cast<UINT>(viewportRect.right), static_cast<UINT>(viewportRect.bottom), gSwapChainResizeCount);
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
            for (Entity& entity : gEditor.GetEntities())
            {
                if (!entity.HasMeshComponent())
                {
                    continue;
                }

                MeshComponent& mc = *entity.Mesh;
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
                    }
                }
            }

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

                    if (audioEmitter.AutoPlay && !audioEmitter.EventPath.empty() && !audioEmitter.RuntimeAutoPlayStarted)
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
            gSceneRenderer->SetNodeGraph(&NodeGraphEditor::Document());

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
            gEditor.Initialize(commandList);
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

        // Match the editor background so the app chrome stays neutral while the actual
        // scene is shown inside the in-app viewport window.
        const float clearColor[] = { 0.08f, 0.10f, 0.14f, 1.0f };
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
            gUiBuildStart = std::chrono::steady_clock::now();
            RenderEditorMainMenu(
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
                &gSceneRenderer->GetTimeOfDaySettings(),
                &gSceneRenderer->GetWindSettings(),
                &gSceneRenderer->GetGlobalIlluminationMode(),
                &gSceneRenderer->GetRtgiSettings(),
                &gSceneRenderer->GetRadianceCascadesSettings(),
                &gSceneRenderer->GetProbeSettings(),
                &gSceneRenderer->GetRtaoSettings(),
                &gSceneRenderer->GetGtaoSettings(),
                &gSceneRenderer->GetSsrSettings(),
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
            GetClientRect(QtUi::ViewportHandle(), &viewportRect);
            QtViewportRenderer::Draw(commandList, gSceneRenderer->GetSceneTextureId(), viewportRect.right, viewportRect.bottom);

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
        RECT viewportRect{};
        GetClientRect(QtUi::ViewportHandle(), &viewportRect);
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

    __declspec(dllexport) void __stdcall RendererDX12_Shutdown()
    {
        ReportProgress(L"Waiting for renderer shutdown...");
        gSystemUsageSampler.Stop();
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
