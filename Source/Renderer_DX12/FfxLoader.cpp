#include "pch.h"
#include "FfxLoader.h"

#include "System/PteroLog.h"

#include <filesystem>
#include <iterator>
#include <string>
#include <windows.h>

namespace
{
    constexpr const wchar_t* kLoaderDll = L"amd_fidelityfx_loader_dx12.dll";

    // The loader finds its effect DLLs by module name. Loading them first, by full
    // path, means that lookup resolves to these copies whichever directory they
    // came from - including the SDK's signedbin folder, which is not on any search
    // path. Neither is required: a missing one only disables its effect.
    constexpr const wchar_t* kEffectDlls[] =
    {
        L"amd_fidelityfx_upscaler_dx12.dll",
        L"amd_fidelityfx_framegeneration_dx12.dll",
    };

    bool gAttempted = false;
    bool gAvailable = false;
    ffxFunctions gFunctions{};
    HMODULE gLoaderModule = nullptr;
    HMODULE gEffectModules[std::size(kEffectDlls)] = {};
    std::string gLastError;

    std::filesystem::path ExecutableDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path))) == 0)
            return {};
        return std::filesystem::path(path).parent_path();
    }

    // Binaries first, so a shipped build uses the DLLs next to it; otherwise the
    // SDK checkout, found the same way Streamline's plugin folder is.
    std::filesystem::path FindDllDirectory()
    {
        const std::filesystem::path exeDir = ExecutableDirectory();
        if (exeDir.empty())
            return {};

        std::error_code error;
        if (std::filesystem::exists(exeDir / kLoaderDll, error))
            return exeDir;

        for (std::filesystem::path dir = exeDir; !dir.empty(); dir = dir.parent_path())
        {
            const std::filesystem::path candidate =
                dir / "Source" / "SDKs" / "FidelityFX-SDK-2.3.0" / "Kits" / "FidelityFX" / "signedbin";
            if (std::filesystem::exists(candidate / kLoaderDll, error))
                return candidate;
            if (dir.parent_path() == dir)
                break;
        }
        return {};
    }
}

namespace FfxLoader
{
    const ffxFunctions* Get()
    {
        if (gAttempted)
            return gAvailable ? &gFunctions : nullptr;
        gAttempted = true;

        const std::filesystem::path directory = FindDllDirectory();
        if (directory.empty())
        {
            gLastError = "amd_fidelityfx_loader_dx12.dll was not found next to the editor or in "
                         "Source\\SDKs\\FidelityFX-SDK-2.3.0\\Kits\\FidelityFX\\signedbin.";
            PteroLog::Write(PteroLog::Level::Warning, "FSR", gLastError.c_str());
            return nullptr;
        }

        for (std::size_t i = 0; i < std::size(kEffectDlls); ++i)
        {
            const std::filesystem::path effectPath = directory / kEffectDlls[i];
            gEffectModules[i] = LoadLibraryW(effectPath.c_str());
            if (gEffectModules[i] == nullptr)
            {
                PteroLog::Writef(PteroLog::Level::Warning, "FSR", "Could not load %s (error %lu).",
                                 effectPath.string().c_str(), ::GetLastError());
            }
        }

        const std::filesystem::path loaderPath = directory / kLoaderDll;
        gLoaderModule = LoadLibraryW(loaderPath.c_str());
        if (gLoaderModule == nullptr)
        {
            gLastError = "Could not load " + loaderPath.string() + " (error " + std::to_string(::GetLastError()) + ").";
            PteroLog::Write(PteroLog::Level::Warning, "FSR", gLastError.c_str());
            Unload();
            gAttempted = true;
            return nullptr;
        }

        ffxLoadFunctions(&gFunctions, gLoaderModule);
        if (!(gFunctions.CreateContext && gFunctions.DestroyContext && gFunctions.Configure
              && gFunctions.Query && gFunctions.Dispatch))
        {
            gLastError = "amd_fidelityfx_loader_dx12.dll does not export the FFX API.";
            PteroLog::Write(PteroLog::Level::Warning, "FSR", gLastError.c_str());
            Unload();
            gAttempted = true;
            return nullptr;
        }

        gAvailable = true;
        gLastError.clear();
        PteroLog::Writef(PteroLog::Level::Info, "FSR", "FidelityFX API loaded from %s.",
                         directory.string().c_str());
        return &gFunctions;
    }

    const char* GetLastError()
    {
        return gLastError.empty() ? nullptr : gLastError.c_str();
    }

    void Unload()
    {
        if (gLoaderModule != nullptr)
            FreeLibrary(gLoaderModule);
        gLoaderModule = nullptr;
        for (HMODULE& module : gEffectModules)
        {
            if (module != nullptr)
                FreeLibrary(module);
            module = nullptr;
        }
        gFunctions = {};
        gAvailable = false;
        gAttempted = false;
    }

    void __cdecl LogMessage(uint32_t type, const wchar_t* message)
    {
        if (message == nullptr)
            return;

        char narrow[1024] = {};
        WideCharToMultiByte(CP_UTF8, 0, message, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        PteroLog::Write(type == FFX_API_MESSAGE_TYPE_ERROR ? PteroLog::Level::Error : PteroLog::Level::Warning,
                        "FSR", narrow);
    }
}
