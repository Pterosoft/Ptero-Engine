#include "pch.h"
#include "ReleaseGame.h"

#include "System/PackageFormat.h"
#include "System/PackagingKeyObfuscation.h"
#include "System/ProjectKey.h"
#include "System/AssetFilter.h"

#include "..\SDKs\DirectXTex\DirectXTex\DirectXTex.h"

#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ReleaseGame
{
    namespace
    {
        // Must match GameLauncher's main.cpp exactly - these are the resource IDs a
        // patched copy of GameLauncherTemplate.exe is expected to carry.
        constexpr int kGameIconResourceId = 100;
        constexpr int kGameIconImageResourceId = 9000;
        constexpr int kPackagingKeyResourceId = 200;

#pragma pack(push, 2)
        struct IconDirEntry
        {
            std::uint8_t Width;
            std::uint8_t Height;
            std::uint8_t ColorCount;
            std::uint8_t Reserved;
            std::uint16_t Planes;
            std::uint16_t BitCount;
            std::uint32_t BytesInResource;
            std::uint32_t ImageOffset;
        };

        struct GroupIconDir
        {
            std::uint16_t Reserved;
            std::uint16_t Type;
            std::uint16_t Count;
        };

        struct GroupIconDirEntry
        {
            std::uint8_t Width;
            std::uint8_t Height;
            std::uint8_t ColorCount;
            std::uint8_t Reserved;
            std::uint16_t Planes;
            std::uint16_t BitCount;
            std::uint32_t BytesInResource;
            std::uint16_t ResourceId;
        };
#pragma pack(pop)

        std::wstring SanitizeFileName(const std::wstring& name)
        {
            std::wstring result = name.empty() ? L"Game" : name;
            for (wchar_t& c : result)
            {
                if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
                    c == L'\\' || c == L'|' || c == L'?' || c == L'*' || c < 32)
                {
                    c = L'_';
                }
            }
            while (!result.empty() && (result.back() == L'.' || result.back() == L' '))
                result.pop_back();
            return result.empty() ? L"Game" : result;
        }

        std::string ToUtf8(const std::wstring& text)
        {
            if (text.empty())
                return {};
            const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<size_t>((std::max)(required, 0)), '\0');
            if (required > 0)
                WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
            return result;
        }

        // Loads any WIC-supported image (PNG/JPG/BMP/...), converts to RGBA8 and resizes to
        // a single 256x256 icon image saved as PNG bytes - the modern (Vista+) icon format
        // Windows accepts directly inside an ICONDIRENTRY/RT_ICON, no BMP masks required.
        // A single size is a deliberate simplification: Windows still scales it acceptably
        // for the taskbar/title-bar/Explorer icon, just not pixel-perfect at 16x16.
        bool BuildIconImagePng(const std::wstring& sourceImagePath, std::vector<unsigned char>& outPngBytes, std::string& outError)
        {
            DirectX::TexMetadata metadata{};
            DirectX::ScratchImage loadedImage;
            HRESULT hr = DirectX::LoadFromWICFile(sourceImagePath.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, loadedImage);
            if (FAILED(hr))
            {
                outError = "Could not load the icon image (unsupported format?).";
                return false;
            }

            DirectX::ScratchImage convertedImage;
            const DirectX::Image* sourceForResize = loadedImage.GetImage(0, 0, 0);
            if (metadata.format != DXGI_FORMAT_R8G8B8A8_UNORM)
            {
                hr = DirectX::Convert(*sourceForResize, DXGI_FORMAT_R8G8B8A8_UNORM,
                    DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, convertedImage);
                if (FAILED(hr))
                {
                    outError = "Could not convert the icon image to RGBA.";
                    return false;
                }
                sourceForResize = convertedImage.GetImage(0, 0, 0);
            }

            DirectX::ScratchImage resizedImage;
            hr = DirectX::Resize(*sourceForResize, 256, 256, DirectX::TEX_FILTER_DEFAULT, resizedImage);
            if (FAILED(hr))
            {
                outError = "Could not resize the icon image to 256x256.";
                return false;
            }

            DirectX::Blob pngBlob;
            hr = DirectX::SaveToWICMemory(*resizedImage.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
                GUID_ContainerFormatPng, pngBlob);
            if (FAILED(hr))
            {
                outError = "Could not encode the icon image as PNG.";
                return false;
            }

            outPngBytes.assign(static_cast<unsigned char*>(pngBlob.GetBufferPointer()),
                static_cast<unsigned char*>(pngBlob.GetBufferPointer()) + pngBlob.GetBufferSize());
            return true;
        }

        // Injects (or replaces) the RT_GROUP_ICON/RT_ICON pair and the RT_RCDATA packaging
        // key into a standalone copy of GameLauncherTemplate.exe. `iconPngBytes` may be
        // null to leave whatever icon the exe already has untouched.
        bool PatchExeResources(const std::wstring& exePath, const std::vector<unsigned char>* iconPngBytes,
                               const Packaging::Key& key, std::string& outError)
        {
            HANDLE updateHandle = BeginUpdateResourceW(exePath.c_str(), FALSE);
            if (updateHandle == nullptr)
            {
                outError = "BeginUpdateResourceW failed (is the exe in use, or read-only?).";
                return false;
            }

            bool ok = true;

            if (iconPngBytes != nullptr && !iconPngBytes->empty())
            {
                IconDirEntry iconEntry{};
                iconEntry.Width = 0;   // 0 means 256 in the icon directory format
                iconEntry.Height = 0;
                iconEntry.ColorCount = 0;
                iconEntry.Planes = 1;
                iconEntry.BitCount = 32;
                iconEntry.BytesInResource = static_cast<std::uint32_t>(iconPngBytes->size());

                ok = ok && UpdateResourceW(updateHandle, RT_ICON, MAKEINTRESOURCE(kGameIconImageResourceId),
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                    const_cast<unsigned char*>(iconPngBytes->data()), static_cast<DWORD>(iconPngBytes->size()));

                GroupIconDir groupHeader{};
                groupHeader.Reserved = 0;
                groupHeader.Type = 1;
                groupHeader.Count = 1;
                GroupIconDirEntry groupEntry{};
                groupEntry.Width = iconEntry.Width;
                groupEntry.Height = iconEntry.Height;
                groupEntry.ColorCount = iconEntry.ColorCount;
                groupEntry.Planes = iconEntry.Planes;
                groupEntry.BitCount = iconEntry.BitCount;
                groupEntry.BytesInResource = iconEntry.BytesInResource;
                groupEntry.ResourceId = static_cast<std::uint16_t>(kGameIconImageResourceId);

                std::vector<unsigned char> groupBytes(sizeof(groupHeader) + sizeof(groupEntry));
                std::memcpy(groupBytes.data(), &groupHeader, sizeof(groupHeader));
                std::memcpy(groupBytes.data() + sizeof(groupHeader), &groupEntry, sizeof(groupEntry));

                ok = ok && UpdateResourceW(updateHandle, RT_GROUP_ICON, MAKEINTRESOURCE(kGameIconResourceId),
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), groupBytes.data(), static_cast<DWORD>(groupBytes.size()));
            }

            Packaging::Key obfuscatedKey = Packaging::ObfuscateKey(key);
            ok = ok && UpdateResourceW(updateHandle, RT_RCDATA, MAKEINTRESOURCE(kPackagingKeyResourceId),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), obfuscatedKey.data(), static_cast<DWORD>(obfuscatedKey.size()));

            if (!ok)
            {
                outError = "UpdateResourceW failed while patching the icon/key into the exe.";
                EndUpdateResourceW(updateHandle, TRUE); // discard
                return false;
            }

            if (!EndUpdateResourceW(updateHandle, FALSE))
            {
                outError = "EndUpdateResourceW failed while saving the patched exe.";
                return false;
            }
            return true;
        }

        bool CopyFileLogged(const fs::path& from, const fs::path& to, const LogCallback& onLog, bool required, std::string& outError)
        {
            std::error_code error;
            fs::create_directories(to.parent_path(), error);
            if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, error))
            {
                const std::string message = "Could not copy " + from.string() + " -> " + to.string() +
                    (error ? (" (" + error.message() + ")") : std::string());
                if (required)
                {
                    outError = message;
                    return false;
                }
                if (onLog) onLog("Warning: " + message);
                return true;
            }
            if (onLog) onLog("Copied " + from.filename().string());
            return true;
        }

        // Renderer_DX12 statically compiles System's sources in (see Renderer_DX12.vcxproj),
        // so System.dll is not one of the modules the shipped game actually loads - these
        // four are. Built in dependency order: Renderer_DX12 links Video.lib and Audio.lib.
        //
        // The renderer ships as its game-runtime flavour (PteroGameRuntime=true): the same
        // DLL minus Qt, built into x64\GameRuntime so it never replaces the editor's own.
        struct EngineModule
        {
            const wchar_t* Project;
            const wchar_t* Dll;
            const wchar_t* OutputFolder;    // under Source/<Project>/x64/
            const wchar_t* ExtraProperties; // appended to the MSBuild command line
        };
        const EngineModule kEngineModules[] = {
            { L"Video", L"Video.dll", L"Release", L"" },
            { L"Audio", L"Audio.dll", L"Release", L"" },
            { L"Game", L"Game.dll", L"Release", L"" },
            { L"Renderer_DX12", L"Renderer_DX12.dll", L"GameRuntime", L" /p:PteroGameRuntime=true" },
        };

        fs::path ModuleOutputPath(const fs::path& projectRoot, const EngineModule& module_)
        {
            return projectRoot / L"Source" / module_.Project / L"x64" / module_.OutputFolder / module_.Dll;
        }

        // Editor-only DLLs that happen to sit in Binaries/: the packaged renderer has no Qt,
        // so none of these may ship (a stale copy would only mislead).
        bool IsEditorOnlyRuntimeDll(const std::wstring& fileName)
        {
            return _wcsnicmp(fileName.c_str(), L"Qt6", 3) == 0;
        }

        // Runs `commandLine` (a single, already-quoted Win32 command line, argv[0] included)
        // with its stdout+stderr redirected straight to `logFilePath`, and its working
        // directory set to `workingDirectory`. Bypasses cmd.exe entirely - CreateProcessW's
        // argument parsing is the well-defined Win32 one, not cmd.exe's separate and much
        // trickier quote-stripping rules, which a first real run of this code (2026-09-23)
        // mangled badly enough that the child process's own output never reached the log
        // file at all: BeginBuildGame showed "MSBuild failed for Video.vcxproj" with zero
        // lines of actual MSBuild output above it, because the redirection cmd.exe was
        // supposed to set up never took effect as written (`_wsystem` wraps its argument in
        // its own outer quotes, and cmd.exe's quote-stripping heuristic only cleanly handles
        // "exactly two quote characters", not this command's many quoted segments).
        // Returns the child's exit code, or -1 if it could not even be started.
        int RunProcessCapturingOutput(std::wstring commandLine, const fs::path& workingDirectory,
                                      const fs::path& logFilePath, std::string& outStartError)
        {
            SECURITY_ATTRIBUTES inheritableHandle{};
            inheritableHandle.nLength = sizeof(inheritableHandle);
            inheritableHandle.bInheritHandle = TRUE;

            HANDLE logHandle = CreateFileW(logFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritableHandle,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (logHandle == INVALID_HANDLE_VALUE)
            {
                outStartError = "Could not create log file: " + logFilePath.string();
                return -1;
            }
            HANDLE nullInputHandle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &inheritableHandle, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            startupInfo.dwFlags = STARTF_USESTDHANDLES;
            startupInfo.hStdOutput = logHandle;
            startupInfo.hStdError = logHandle;
            startupInfo.hStdInput = nullInputHandle;

            // CreateProcessW may write into this buffer (splitting argv), so it cannot point
            // at a string literal or a std::wstring's own (possibly shared/COW) storage.
            std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
            mutableCommandLine.push_back(L'\0');

            PROCESS_INFORMATION processInfo{};
            const BOOL started = CreateProcessW(
                nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startupInfo, &processInfo);

            CloseHandle(logHandle);
            if (nullInputHandle != INVALID_HANDLE_VALUE) CloseHandle(nullInputHandle);

            if (!started)
            {
                outStartError = "CreateProcessW failed (error " + std::to_string(GetLastError()) + ").";
                return -1;
            }

            WaitForSingleObject(processInfo.hProcess, INFINITE);
            DWORD exitCode = 1;
            GetExitCodeProcess(processInfo.hProcess, &exitCode);
            CloseHandle(processInfo.hProcess);
            CloseHandle(processInfo.hThread);
            return static_cast<int>(exitCode);
        }

        // Finds MSBuild.exe: an explicit override, then vswhere (the portable way - it asks
        // whichever Visual Studio is installed, wherever that is), then the path recorded
        // for this machine's setup as a last resort.
        std::wstring FindMSBuildExecutable()
        {
            wchar_t override_[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"PTERO_MSBUILD_PATH", override_, static_cast<DWORD>(std::size(override_))) > 0
                && fs::exists(override_))
            {
                return override_;
            }

            const wchar_t* vswhereCandidates[] = {
                L"%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe",
                L"%ProgramFiles%\\Microsoft Visual Studio\\Installer\\vswhere.exe",
            };
            for (const wchar_t* candidate : vswhereCandidates)
            {
                wchar_t expanded[MAX_PATH] = {};
                ExpandEnvironmentStringsW(candidate, expanded, static_cast<DWORD>(std::size(expanded)));
                if (!fs::exists(expanded))
                    continue;

                std::error_code tempError;
                const fs::path outputFile = fs::temp_directory_path(tempError) / L"ptero_vswhere_msbuild.txt";
                const std::wstring commandLine = L"\"" + std::wstring(expanded) +
                    L"\" -latest -prerelease -requires Microsoft.Component.MSBuild -find MSBuild\\**\\Bin\\MSBuild.exe";
                std::string startError;
                RunProcessCapturingOutput(commandLine, fs::path(expanded).parent_path(), outputFile, startError);

                std::wifstream resultFile(outputFile);
                std::wstring firstLine;
                std::getline(resultFile, firstLine);
                while (!firstLine.empty() && (firstLine.back() == L'\r' || firstLine.back() == L'\n'))
                    firstLine.pop_back();
                if (!firstLine.empty() && fs::exists(firstLine))
                    return firstLine;
            }

            // Recorded working path for this machine (see memory: build-config-debug-x64-only).
            const wchar_t* knownPath = L"K:\\Program Files\\Microsoft Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe";
            if (fs::exists(knownPath))
                return knownPath;

            return {};
        }

        // Runs one project's Release|x64 build via MSBuild and streams its output through
        // onLog. Serial by design (no /m): building the whole solution in parallel is known
        // to race on link order (see memory build-config-debug-x64-only), and each of these
        // is invoked as its own single-project build anyway, not the .slnx.
        bool RunMSBuildProject(const std::wstring& msbuildPath, const fs::path& vcxprojPath, const std::wstring& extraProperties,
                               const LogCallback& onLog, std::string& outError)
        {
            std::error_code tempError;
            const fs::path logFile = fs::temp_directory_path(tempError) /
                (vcxprojPath.stem().wstring() + L"_release_build.log");

            const std::wstring commandLine = L"\"" + msbuildPath + L"\" \"" + vcxprojPath.wstring() +
                L"\" /p:Configuration=Release /p:Platform=x64 /nologo /verbosity:minimal" + extraProperties;

            std::string startError;
            const int exitCode = RunProcessCapturingOutput(commandLine, vcxprojPath.parent_path(), logFile, startError);

            std::ifstream logStream(logFile);
            std::string line;
            bool sawUserMappedSection = false;
            bool sawAnyOutput = false;
            while (std::getline(logStream, line))
            {
                sawAnyOutput = true;
                if (onLog) onLog("    " + line);
                if (line.find("user-mapped section") != std::string::npos)
                    sawUserMappedSection = true;
            }

            if (exitCode == -1)
            {
                outError = "Could not start MSBuild: " + startError;
                return false;
            }
            if (exitCode != 0)
            {
                outError = "MSBuild failed for " + ToUtf8(vcxprojPath.filename().wstring()) +
                    (sawUserMappedSection ? " (known intermittent MSBuild failure - retrying)" : "") +
                    (!sawAnyOutput ? " (no output captured - is the exe/vcxproj path valid?)" : "");
                return false;
            }
            return true;
        }

        // Builds Release|x64 of Video/Audio/Game/Renderer_DX12 (game runtime)/GameLauncher.
        // Every project is always handed to MSBuild, whose own incremental check is the
        // only reliable answer to "is this up to date" - comparing against the Debug DLL's
        // timestamp missed source edits made after the last Debug build, and shipped a
        // stale renderer. An up-to-date project costs a few seconds. Retries a project once on failure,
        // since this toolchain has a known intermittent "user-mapped section" MSBuild
        // failure that is unrelated to the code (see memory
        // msbuild-user-mapped-section-flakiness) - not something worth surfacing as a real
        // error on the first occurrence.
        bool EnsureReleaseBinariesBuilt(const fs::path& projectRoot, const LogCallback& onLog, std::string& outError)
        {
            const std::wstring msbuildPath = FindMSBuildExecutable();
            if (msbuildPath.empty())
            {
                outError = "Could not find MSBuild.exe. Set the PTERO_MSBUILD_PATH environment variable, "
                    "or build Release|x64 yourself in Visual Studio and turn off automatic building.";
                return false;
            }

            const auto buildOneProject = [&](const fs::path& vcxprojPath, const std::wstring& extraProperties) -> bool
            {
                if (!fs::exists(vcxprojPath))
                {
                    outError = "Missing project file: " + vcxprojPath.string();
                    return false;
                }
                if (onLog) onLog("Building Release|x64: " + vcxprojPath.filename().string() + ToUtf8(extraProperties) + "...");

                std::string buildError;
                if (RunMSBuildProject(msbuildPath, vcxprojPath, extraProperties, onLog, buildError))
                    return true;

                if (onLog) onLog(buildError);
                if (!RunMSBuildProject(msbuildPath, vcxprojPath, extraProperties, onLog, buildError))
                {
                    outError = buildError;
                    return false;
                }
                return true;
            };

            for (const EngineModule& module_ : kEngineModules)
            {
                const fs::path vcxprojPath = projectRoot / L"Source" / module_.Project / (std::wstring(module_.Project) + L".vcxproj");
                if (!buildOneProject(vcxprojPath, module_.ExtraProperties))
                    return false;
            }

            const fs::path vcxprojPath = projectRoot / L"Source" / L"Ptero-Engine" / L"GameLauncher" / L"GameLauncher.vcxproj";
            return buildOneProject(vcxprojPath, L"");
        }

        // Every module the shipped game actually loads at runtime, plus whatever
        // third-party runtime DLLs (Qt, dxcompiler/dxil, NRD, FidelityFX, FMOD) already live
        // in Binaries/ from the engine's own setup, since none of those have a
        // per-configuration build step.
        bool GatherRuntimeBinaries(const fs::path& projectRoot, const fs::path& outputDirectory,
                                   const LogCallback& onLog, std::string& outError)
        {
            const fs::path binariesDirectory = projectRoot / L"Binaries";
            for (const auto& module_ : kEngineModules)
            {
                const fs::path releasePath = ModuleOutputPath(projectRoot, module_);
                if (!fs::exists(releasePath))
                {
                    outError = std::string("Release build of ") + ToUtf8(module_.Dll) + " not found at " +
                        releasePath.string() + ". Turn on automatic building, or build Release|x64 in Visual Studio" +
                        (_wcsicmp(module_.OutputFolder, L"GameRuntime") == 0 ? " with the MSBuild property PteroGameRuntime=true." : ".");
                    return false;
                }

                std::error_code timeError;
                const fs::path debugPath = binariesDirectory / module_.Dll;
                if (fs::exists(debugPath, timeError))
                {
                    const auto releaseTime = fs::last_write_time(releasePath, timeError);
                    const auto debugTime = fs::last_write_time(debugPath, timeError);
                    if (!timeError && releaseTime < debugTime && onLog)
                    {
                        onLog("Warning: Release build of " + ToUtf8(module_.Dll) +
                            " looks older than the Debug one - rebuild Release before shipping.");
                    }
                }

                if (!CopyFileLogged(releasePath, outputDirectory / module_.Dll, onLog, true, outError))
                    return false;
            }

            // dxc.exe: the shader compiler now runs every shader through it (see
            // DX12ShaderCompiler.cpp), including SM5 profiles that used to fall back to the
            // legacy D3DCompiler. It lives only under Source/SDKs/dxc, never copied to
            // Binaries/, so it needs its own explicit copy - ResolveBundledDxcExecutablePath
            // knows to look for it flat next to the exe in a packaged build.
            if (!CopyFileLogged(projectRoot / L"Source" / L"SDKs" / L"dxc" / L"bin" / L"x64" / L"dxc.exe",
                                 outputDirectory / L"dxc.exe", onLog, true, outError))
                return false;

            // Everything else already sitting in Binaries/ (dxcompiler/dxil, NRD,
            // FidelityFX, FMOD, ...) that isn't one of the engine modules above or an
            // editor-only artifact such as Qt.
            static const std::vector<std::wstring> skipNames = {
                L"Editor.exe", L"Renderer_DX12.dll", L"Renderer_DX12.pdb", L"Renderer_DX12.lib", L"Renderer_DX12.exp",
                L"Game.dll", L"Game.pdb", L"Game.lib", L"Game.exp",
                L"Audio.dll", L"Audio.pdb", L"Audio.lib", L"Audio.exp",
                L"Video.dll", L"Video.pdb", L"Video.lib", L"Video.exp",
                L"System.dll", L"System.pdb", L"System.lib", L"System.exp",
                L"Editor.pdb", L"GameLauncherTemplateDebug.exe", L"GameLauncherTemplateDebug.pdb", L"GameLauncherTemplateDebug.lib",
            };

            std::error_code iterateError;
            for (const auto& entry : fs::directory_iterator(binariesDirectory, iterateError))
            {
                if (!entry.is_regular_file())
                    continue;
                const std::wstring fileName = entry.path().filename().wstring();
                const std::wstring extension = entry.path().extension().wstring();
                if (_wcsicmp(extension.c_str(), L".dll") != 0)
                    continue;
                if (std::find_if(skipNames.begin(), skipNames.end(), [&](const std::wstring& s) {
                        return _wcsicmp(s.c_str(), fileName.c_str()) == 0; }) != skipNames.end())
                    continue;
                if (IsEditorOnlyRuntimeDll(fileName))
                    continue;

                std::string copyError;
                CopyFileLogged(entry.path(), outputDirectory / fileName, onLog, false, copyError);
            }
            return true;
        }

        bool WriteGameConfig(const fs::path& outputDirectory, const std::wstring& gameName, std::string& outError)
        {
            std::ofstream configFile(outputDirectory / L"game.cfg", std::ios::binary | std::ios::trunc);
            if (!configFile)
            {
                outError = "Could not write game.cfg.";
                return false;
            }
            configFile << ToUtf8(gameName) << "\n";
            return true;
        }

        // Collects every file under Data/<folderName>, filtered per Packaging::ShouldIncludeAsset.
        std::vector<Packaging::BuildEntry> CollectFolderEntries(const fs::path& dataDirectory, const std::wstring& folderName)
        {
            std::vector<Packaging::BuildEntry> entries;
            const fs::path folderPath = dataDirectory / folderName;
            std::error_code error;
            if (!fs::is_directory(folderPath, error))
                return entries;

            for (fs::recursive_directory_iterator it(folderPath, fs::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error))
            {
                if (!it->is_regular_file())
                    continue;
                const fs::path& path = it->path();
                if (!Packaging::ShouldIncludeAsset(folderName, path.filename().wstring()))
                    continue;

                Packaging::BuildEntry entry;
                entry.AbsoluteSourcePath = path.wstring();
                fs::path relative = fs::relative(path, folderPath, error);
                std::wstring relativeWide = relative.generic_wstring();
                entry.RelativePackagePath = ToUtf8(relativeWide);
                entries.push_back(std::move(entry));
            }
            return entries;
        }
    }

    const std::vector<std::wstring>& DefaultIncludedFolders()
    {
        static const std::vector<std::wstring> folders = {
            L"Textures", L"Geometry", L"Audio", L"UI", L"Levels", L"Fonts",
            L"Materials", L"MultiMaterials", L"Shaders", L"Icons", L"Media", L"Videos",
        };
        return folders;
    }

    bool ExtractOnePackage(const std::wstring& projectRoot, const std::wstring& pakPath,
                           const std::wstring& destinationDirectory, std::string& outError)
    {
        Packaging::Key key{};
        if (!Packaging::LoadOrCreateProjectKey(projectRoot, key, outError))
            return false;
        return Packaging::ExtractPackage(pakPath, destinationDirectory, key, outError);
    }

    namespace
    {
        // Rebuilding into a folder an older build went to can leave files this build no
        // longer uses: Qt DLLs from before the renderer went Qt-free, and the Data folder
        // the old launcher unpacked the archives into (recognisable by its marker file -
        // anything else named Data is left alone). The game reads Content/*.ppak directly now.
        void RemoveObsoleteOutput(const fs::path& outputDirectory, const LogCallback& onLog)
        {
            std::error_code error;
            for (const auto& entry : fs::directory_iterator(outputDirectory, error))
            {
                const std::wstring fileName = entry.path().filename().wstring();
                if (entry.is_regular_file() && IsEditorOnlyRuntimeDll(fileName))
                {
                    std::error_code removeError;
                    if (fs::remove(entry.path(), removeError) && onLog)
                        onLog("Removed obsolete " + ToUtf8(fileName));
                }
            }

            const fs::path unpackedData = outputDirectory / L"Data";
            if (fs::exists(unpackedData / L".ppak_version", error))
            {
                std::error_code removeError;
                fs::remove_all(unpackedData, removeError);
                if (onLog)
                    onLog(removeError ? "Warning: could not remove the old unpacked Data folder: " + removeError.message()
                                      : std::string("Removed the old unpacked Data folder (assets are read from Content/*.ppak)."));
            }
        }
    }

    bool Build(const BuildOptions& options, const LogCallback& onLog)
    {
        const auto log = [&onLog](const std::string& line) { if (onLog) onLog(line); };

        const fs::path projectRoot(options.ProjectRoot);
        const fs::path dataDirectory = projectRoot / L"Data";
        const fs::path outputDirectory(options.OutputDirectory);

        std::error_code dirError;
        fs::create_directories(outputDirectory, dirError);

        Packaging::Key projectKey{};
        std::string error;
        if (!Packaging::LoadOrCreateProjectKey(options.ProjectRoot, projectKey, error))
        {
            log("Error: " + error);
            return false;
        }

        if (options.BuildPackages)
        {
            const fs::path contentDirectory = outputDirectory / L"Content";
            fs::create_directories(contentDirectory, dirError);

            for (const std::wstring& folder : options.IncludedFolders)
            {
                std::vector<Packaging::BuildEntry> entries = CollectFolderEntries(dataDirectory, folder);
                if (entries.empty())
                {
                    log("Skipping " + ToUtf8(folder) + " (nothing to package).");
                    continue;
                }

                const fs::path pakPath = contentDirectory / (folder + L".ppak");
                log("Packaging " + ToUtf8(folder) + " (" + std::to_string(entries.size()) + " files)...");
                if (!Packaging::BuildPackage(pakPath.wstring(), entries, projectKey, error))
                {
                    log("Error: " + error);
                    return false;
                }
            }
            log("Packaging complete.");
        }

        if (options.BuildExe)
        {
            if (options.AutoBuildRelease)
            {
                log("Checking Release|x64 build...");
                if (!EnsureReleaseBinariesBuilt(projectRoot, log, error))
                {
                    log("Error: " + error);
                    return false;
                }
            }

            const fs::path templateExe = projectRoot / L"Source" / L"Ptero-Engine" / L"GameLauncher" / L"x64" / L"Release" / L"GameLauncherTemplate.exe";
            if (!fs::exists(templateExe))
            {
                log("Error: GameLauncherTemplate.exe not found. Build the GameLauncher project's "
                    "Release|x64 configuration in Visual Studio first, or turn on automatic building.");
                return false;
            }

            const std::wstring exeFileName = SanitizeFileName(options.GameName) + L".exe";
            const fs::path outputExe = outputDirectory / exeFileName;

            if (!CopyFileLogged(templateExe, outputExe, log, true, error))
            {
                log("Error: " + error);
                return false;
            }

            std::vector<unsigned char> iconPngBytes;
            std::vector<unsigned char>* iconPngBytesPtr = nullptr;
            if (!options.IconSourcePath.empty())
            {
                if (!BuildIconImagePng(options.IconSourcePath, iconPngBytes, error))
                {
                    log("Error: " + error);
                    return false;
                }
                iconPngBytesPtr = &iconPngBytes;
            }

            if (!PatchExeResources(outputExe.wstring(), iconPngBytesPtr, projectKey, error))
            {
                log("Error: " + error);
                return false;
            }
            log("Icon and packaging key embedded.");

            if (!WriteGameConfig(outputDirectory, options.GameName, error))
            {
                log("Error: " + error);
                return false;
            }

            if (!GatherRuntimeBinaries(projectRoot, outputDirectory, log, error))
            {
                log("Error: " + error);
                return false;
            }

            RemoveObsoleteOutput(outputDirectory, log);
            log("Build finished: " + outputExe.string());
        }

        return true;
    }
}
