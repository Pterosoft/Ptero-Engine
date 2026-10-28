#include "pch.h"
#include "XessLoader.h"

#include "System/PteroLog.h"

#include <filesystem>
#include <string>
#include <windows.h>

namespace
{
    std::string gLastError;

    std::filesystem::path ExecutableDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path))) == 0)
            return {};
        return std::filesystem::path(path).parent_path();
    }

    // Binaries first, so a shipped build uses the DLLs next to it; otherwise the
    // SDK checkout, found by walking up the way the FidelityFX loader does.
    std::filesystem::path FindDll(const wchar_t* fileName)
    {
        const std::filesystem::path exeDir = ExecutableDirectory();
        if (exeDir.empty())
            return {};

        std::error_code error;
        if (std::filesystem::exists(exeDir / fileName, error))
            return exeDir / fileName;

        for (std::filesystem::path dir = exeDir; !dir.empty(); dir = dir.parent_path())
        {
            const std::filesystem::path candidate = dir / "Source" / "SDKs" / "xess-3.0.2" / "bin" / fileName;
            if (std::filesystem::exists(candidate, error))
                return candidate;
            if (dir.parent_path() == dir)
                break;
        }
        return {};
    }

    HMODULE LoadXessModule(const wchar_t* fileName)
    {
        const std::filesystem::path path = FindDll(fileName);
        if (path.empty())
        {
            gLastError = std::filesystem::path(fileName).string()
                + " was not found next to the editor or in Source\\SDKs\\xess-3.0.2\\bin.";
            PteroLog::Write(PteroLog::Level::Warning, "XeSS", gLastError.c_str());
            return nullptr;
        }

        HMODULE module = LoadLibraryW(path.c_str());
        if (module == nullptr)
        {
            gLastError = "Could not load " + path.string() + " (error " + std::to_string(::GetLastError()) + ").";
            PteroLog::Write(PteroLog::Level::Warning, "XeSS", gLastError.c_str());
            return nullptr;
        }

        PteroLog::Writef(PteroLog::Level::Info, "XeSS", "Loaded %s.", path.string().c_str());
        return module;
    }

    // Resolves one export into a typed pointer; false when it is missing.
    template <typename T>
    bool Resolve(HMODULE module, const char* name, T& out)
    {
        out = reinterpret_cast<T>(GetProcAddress(module, name));
        if (out != nullptr)
            return true;
        gLastError = std::string("XeSS: export ") + name + " is missing.";
        PteroLog::Write(PteroLog::Level::Warning, "XeSS", gLastError.c_str());
        return false;
    }

    // One library's load state. Loaded once; a failure is remembered so a
    // missing DLL is not searched for again every frame.
    template <typename Functions>
    struct Library
    {
        bool      Attempted = false;
        bool      Available = false;
        HMODULE   Module = nullptr;
        Functions Table{};
    };

    Library<XessLoader::SuperResolutionFunctions> gSr;
    Library<XessLoader::FrameGenerationFunctions> gFg;
    Library<XessLoader::LowLatencyFunctions>      gLl;

    template <typename Functions, typename Binder>
    const Functions* LoadOnce(Library<Functions>& library, const wchar_t* fileName, Binder bind)
    {
        if (library.Attempted)
            return library.Available ? &library.Table : nullptr;
        library.Attempted = true;

        library.Module = LoadXessModule(fileName);
        if (library.Module == nullptr)
            return nullptr;

        if (!bind(library.Module, library.Table))
        {
            FreeLibrary(library.Module);
            library.Module = nullptr;
            library.Table = {};
            return nullptr;
        }

        library.Available = true;
        return &library.Table;
    }
}

namespace XessLoader
{
    const SuperResolutionFunctions* SuperResolution()
    {
        return LoadOnce(gSr, L"libxess.dll", [](HMODULE m, SuperResolutionFunctions& f)
        {
            return Resolve(m, "xessGetVersion", f.GetVersion)
                && Resolve(m, "xessD3D12CreateContext", f.D3D12CreateContext)
                && Resolve(m, "xessD3D12BuildPipelines", f.D3D12BuildPipelines)
                && Resolve(m, "xessD3D12Init", f.D3D12Init)
                && Resolve(m, "xessD3D12Execute", f.D3D12Execute)
                && Resolve(m, "xessDestroyContext", f.DestroyContext)
                && Resolve(m, "xessGetOptimalInputResolution", f.GetOptimalInputResolution)
                && Resolve(m, "xessSetVelocityScale", f.SetVelocityScale)
                && Resolve(m, "xessSetJitterScale", f.SetJitterScale)
                && Resolve(m, "xessSetLoggingCallback", f.SetLoggingCallback)
                && Resolve(m, "xessIsOptimalDriver", f.IsOptimalDriver);
        });
    }

    const FrameGenerationFunctions* FrameGeneration()
    {
        return LoadOnce(gFg, L"libxess_fg.dll", [](HMODULE m, FrameGenerationFunctions& f)
        {
            return Resolve(m, "xefgSwapChainGetVersion", f.GetVersion)
                && Resolve(m, "xefgSwapChainD3D12CreateContext", f.D3D12CreateContext)
                && Resolve(m, "xefgSwapChainD3D12InitFromSwapChainDesc", f.D3D12InitFromSwapChainDesc)
                && Resolve(m, "xefgSwapChainD3D12GetSwapChainPtr", f.D3D12GetSwapChainPtr)
                && Resolve(m, "xefgSwapChainD3D12TagFrameResource", f.D3D12TagFrameResource)
                && Resolve(m, "xefgSwapChainD3D12GetInitializationParameters", f.D3D12GetInitializationParameters)
                && Resolve(m, "xefgSwapChainTagFrameConstants", f.TagFrameConstants)
                && Resolve(m, "xefgSwapChainSetEnabled", f.SetEnabled)
                && Resolve(m, "xefgSwapChainSetPresentId", f.SetPresentId)
                && Resolve(m, "xefgSwapChainGetLastPresentStatus", f.GetLastPresentStatus)
                && Resolve(m, "xefgSwapChainSetLoggingCallback", f.SetLoggingCallback)
                && Resolve(m, "xefgSwapChainDestroy", f.Destroy)
                && Resolve(m, "xefgSwapChainSetLatencyReduction", f.SetLatencyReduction)
                && Resolve(m, "xefgSwapChainSetNumInterpolatedFrames", f.SetNumInterpolatedFrames)
                && Resolve(m, "xefgSwapChainSetUiCompositionState", f.SetUiCompositionState);
        });
    }

    const LowLatencyFunctions* LowLatency()
    {
        return LoadOnce(gLl, L"libxell.dll", [](HMODULE m, LowLatencyFunctions& f)
        {
            return Resolve(m, "xellD3D12CreateContext", f.D3D12CreateContext)
                && Resolve(m, "xellDestroyContext", f.DestroyContext)
                && Resolve(m, "xellSetSleepMode", f.SetSleepMode)
                && Resolve(m, "xellSleep", f.Sleep)
                && Resolve(m, "xellAddMarkerData", f.AddMarkerData)
                && Resolve(m, "xellGetVersion", f.GetVersion)
                && Resolve(m, "xellSetLoggingCallback", f.SetLoggingCallback);
        });
    }

    const char* GetLastError()
    {
        return gLastError.empty() ? nullptr : gLastError.c_str();
    }

    void SuperResolutionLog(const char* message, xess_logging_level_t level)
    {
        if (message == nullptr)
            return;
        PteroLog::Write(level >= XESS_LOGGING_LEVEL_ERROR ? PteroLog::Level::Error : PteroLog::Level::Warning,
                        "XeSS", message);
    }

    void FrameGenerationLog(const char* message, xefg_swapchain_logging_level_t level, void* /*userData*/)
    {
        if (message == nullptr)
            return;
        PteroLog::Write(level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? PteroLog::Level::Error : PteroLog::Level::Warning,
                        "XeSS-FG", message);
    }

    void LowLatencyLog(const char* message, xell_logging_level_t level)
    {
        if (message == nullptr)
            return;
        PteroLog::Write(level >= XELL_LOGGING_LEVEL_ERROR ? PteroLog::Level::Error : PteroLog::Level::Warning,
                        "XeLL", message);
    }
}
