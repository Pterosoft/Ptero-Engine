#include "UnrealOodle.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace Ptero::Unreal::Oodle
{
    namespace
    {
        // OodleLZ_Decompress from the Oodle 2.9 SDK (stable C ABI since 2.6).
        using DecompressFn = std::intptr_t(__stdcall*)(
            const void* compressed, std::intptr_t compressedSize,
            void* raw, std::intptr_t rawSize,
            int fuzzSafe, int checkCrc, int verbosity,
            void* decodeBufferBase, std::intptr_t decodeBufferSize,
            void* callback, void* callbackUserData,
            void* decoderMemory, std::intptr_t decoderMemorySize,
            int threadPhase);

        std::once_flag gLoadOnce;
        HMODULE gModule = nullptr;
        DecompressFn gDecompress = nullptr;
        std::string gLoadedFrom;
        std::string gReason;

        std::wstring ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName)
        {
            wchar_t buffer[1024] = {};
            DWORD size = sizeof(buffer);
            if (RegGetValueW(root, subKey, valueName, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS)
            {
                return buffer;
            }
            return {};
        }

        std::vector<std::filesystem::path> RegisteredEngineRoots()
        {
            std::vector<std::filesystem::path> roots;

            // Launcher installs: HKLM\SOFTWARE\EpicGames\Unreal Engine\<version>\InstalledDirectory.
            HKEY enginesKey = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EpicGames\\Unreal Engine", 0, KEY_READ | KEY_WOW64_64KEY, &enginesKey) == ERROR_SUCCESS)
            {
                wchar_t name[256];
                for (DWORD index = 0;; ++index)
                {
                    DWORD nameLength = static_cast<DWORD>(std::size(name));
                    if (RegEnumKeyExW(enginesKey, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                    {
                        break;
                    }
                    const std::wstring subKey = std::wstring(L"SOFTWARE\\EpicGames\\Unreal Engine\\") + name;
                    const std::wstring directory = ReadRegistryString(HKEY_LOCAL_MACHINE, subKey.c_str(), L"InstalledDirectory");
                    if (!directory.empty())
                    {
                        roots.emplace_back(directory);
                    }
                }
                RegCloseKey(enginesKey);
            }

            // Source builds register themselves under HKCU\...\Builds as {guid} = path.
            HKEY buildsKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Epic Games\\Unreal Engine\\Builds", 0, KEY_READ, &buildsKey) == ERROR_SUCCESS)
            {
                wchar_t valueName[256];
                wchar_t data[1024];
                for (DWORD index = 0;; ++index)
                {
                    DWORD nameLength = static_cast<DWORD>(std::size(valueName));
                    DWORD dataSize = sizeof(data);
                    DWORD type = 0;
                    if (RegEnumValueW(buildsKey, index, valueName, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataSize) != ERROR_SUCCESS)
                    {
                        break;
                    }
                    if (type == REG_SZ)
                    {
                        roots.emplace_back(std::wstring(data));
                    }
                }
                RegCloseKey(buildsKey);
            }

            // The launcher's manifest also lists installs that never wrote the registry key.
            wchar_t programData[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH) > 0)
            {
                std::ifstream manifest(std::filesystem::path(programData) / L"Epic" / L"UnrealEngineLauncher" / L"LauncherInstalled.dat");
                std::string line;
                while (std::getline(manifest, line))
                {
                    const std::size_t key = line.find("\"InstallLocation\"");
                    if (key == std::string::npos)
                    {
                        continue;
                    }
                    const std::size_t open = line.find('"', line.find(':', key) + 1);
                    const std::size_t close = open == std::string::npos ? std::string::npos : line.find('"', open + 1);
                    if (close == std::string::npos)
                    {
                        continue;
                    }
                    std::string location = line.substr(open + 1, close - open - 1);
                    std::string unescaped;
                    for (std::size_t index = 0; index < location.size(); ++index)
                    {
                        if (location[index] == '\\' && index + 1 < location.size())
                        {
                            ++index;
                        }
                        unescaped.push_back(location[index]);
                    }
                    if (std::filesystem::exists(std::filesystem::path(unescaped) / "Engine"))
                    {
                        roots.emplace_back(unescaped);
                    }
                }
            }

            return roots;
        }

        std::vector<std::filesystem::path> CandidateDlls()
        {
            std::vector<std::filesystem::path> candidates;

            wchar_t overridePath[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"PTERO_OODLE_DLL", overridePath, MAX_PATH) > 0)
            {
                candidates.emplace_back(overridePath);
            }

            // A copy placed next to the editor by the user.
            wchar_t modulePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            const std::filesystem::path exeDirectory = std::filesystem::path(modulePath).parent_path();
            candidates.push_back(exeDirectory / L"oo2core_9_win64.dll");
            candidates.push_back(exeDirectory / L"oo2core.dll");

            // Newest engine first: later Oodle versions still decode older streams.
            std::vector<std::filesystem::path> roots = RegisteredEngineRoots();
            std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) { return a.wstring() > b.wstring(); });

            static const wchar_t* kRelativeLocations[] = {
                L"Engine\\Binaries\\DotNET\\UnrealBuildTool\\runtimes\\win-x64\\native\\oo2core.dll",
                L"Engine\\Binaries\\DotNET\\AutomationTool\\runtimes\\win-x64\\native\\oo2core.dll",
                L"Engine\\Binaries\\DotNET\\CsvTools\\runtimes\\win-x64\\native\\oo2core.dll",
                L"Engine\\Binaries\\Win64\\oo2core_9_win64.dll",
            };
            for (const std::filesystem::path& root : roots)
            {
                for (const wchar_t* relative : kRelativeLocations)
                {
                    candidates.push_back(root / relative);
                }

                // EpicGames.Oodle keeps one folder per SDK version.
                std::error_code ec;
                const std::filesystem::path shared = root / L"Engine\\Source\\Programs\\Shared\\EpicGames.Oodle";
                for (const auto& entry : std::filesystem::directory_iterator(shared, ec))
                {
                    candidates.push_back(entry.path() / L"runtimes\\win-x64\\native\\oo2core.dll");
                }
            }

            return candidates;
        }

        void LoadOnce()
        {
            for (const std::filesystem::path& candidate : CandidateDlls())
            {
                std::error_code ec;
                if (!std::filesystem::is_regular_file(candidate, ec))
                {
                    continue;
                }

                HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (module == nullptr)
                {
                    continue;
                }

                auto decompress = reinterpret_cast<DecompressFn>(GetProcAddress(module, "OodleLZ_Decompress"));
                if (decompress == nullptr)
                {
                    FreeLibrary(module);
                    continue;
                }

                gModule = module;
                gDecompress = decompress;
                gLoadedFrom = candidate.string();
                return;
            }

            gReason =
                "Unreal assets are Oodle-compressed and no Oodle decompressor was found. "
                "Install Unreal Engine 5 through the Epic Games Launcher (Ptero uses the oo2core.dll it ships), "
                "or set PTERO_OODLE_DLL to the path of an oo2core DLL.";
        }
    }

    bool IsAvailable()
    {
        std::call_once(gLoadOnce, LoadOnce);
        return gDecompress != nullptr;
    }

    std::string UnavailableReason()
    {
        std::call_once(gLoadOnce, LoadOnce);
        return gReason;
    }

    std::string LoadedFrom()
    {
        std::call_once(gLoadOnce, LoadOnce);
        return gLoadedFrom;
    }

    bool Decompress(const std::uint8_t* source, const std::size_t sourceSize, std::uint8_t* destination, const std::size_t rawSize)
    {
        if (!IsAvailable())
        {
            return false;
        }

        const std::intptr_t decoded = gDecompress(
            source, static_cast<std::intptr_t>(sourceSize),
            destination, static_cast<std::intptr_t>(rawSize),
            1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
        return decoded == static_cast<std::intptr_t>(rawSize);
    }
}
