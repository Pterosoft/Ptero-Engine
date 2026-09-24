#include "pch.h"
#include "DX12ShaderCompiler.h"

#include "System/DataFiles.h"

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    constexpr const char* ShaderCacheVersion = "PteroShaderCacheV1";
    std::mutex gShaderCacheMutex;

    std::filesystem::path GetCurrentModuleDirectory()
    {
        wchar_t moduleFilePath[MAX_PATH]{};
        HMODULE currentModule = nullptr;
        if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetCurrentModuleDirectory),
            &currentModule)
            && GetModuleFileNameW(currentModule, moduleFilePath, static_cast<DWORD>(std::size(moduleFilePath))) > 0)
        {
            return std::filesystem::path(moduleFilePath).parent_path();
        }

        return {};
    }

    // True for a path inside a packaged game's archives rather than on disk.
    bool IsPackagedPath(const std::filesystem::path& path)
    {
        return DataFiles::IsPackaged() && !DataFiles::PackagedRelativePath(path).empty();
    }

    // weakly_canonical for files on disk; an archive path has nothing on disk to resolve.
    std::filesystem::path CanonicalOrNormal(const std::filesystem::path& path)
    {
        if (IsPackagedPath(path))
        {
            return path.lexically_normal();
        }

        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
        return error ? path.lexically_normal() : canonical;
    }

    std::filesystem::path FindAssetFromDirectory(
        const std::filesystem::path& startDirectory,
        const std::vector<std::filesystem::path>& relativeCandidates)
    {
        namespace fs = std::filesystem;

        fs::path currentDirectory = startDirectory;
        while (!currentDirectory.empty())
        {
            for (const fs::path& relativeCandidate : relativeCandidates)
            {
                const fs::path candidatePath = currentDirectory / relativeCandidate;
                if (DataFiles::Exists(candidatePath))
                {
                    return CanonicalOrNormal(candidatePath);
                }
            }

            const fs::path parentDirectory = currentDirectory.parent_path();
            if (parentDirectory == currentDirectory)
            {
                break;
            }

            currentDirectory = parentDirectory;
        }

        return {};
    }

    std::wstring QuoteCommandLineArgument(const std::wstring& argument)
    {
        if (argument.find_first_of(L" \t\"") == std::wstring::npos)
        {
            return argument;
        }

        std::wstring quoted = L"\"";
        for (const wchar_t character : argument)
        {
            if (character == L'\"')
            {
                quoted += L'\\';
            }

            quoted += character;
        }

        quoted += L'\"';
        return quoted;
    }

    std::filesystem::path ResolveBundledDxcExecutablePath()
    {
        namespace fs = std::filesystem;

        const fs::path moduleDirectory = GetCurrentModuleDirectory();
        if (!moduleDirectory.empty())
        {
            const fs::path discoveredDxcPath = FindAssetFromDirectory(
                moduleDirectory,
                {
#if defined(_WIN64)
                    fs::path(L"Source") / L"SDKs" / L"dxc" / L"bin" / L"x64" / L"dxc.exe",
                    fs::path(L"SDKs") / L"dxc" / L"bin" / L"x64" / L"dxc.exe",
#else
                    fs::path(L"Source") / L"SDKs" / L"dxc" / L"bin" / L"x86" / L"dxc.exe",
                    fs::path(L"SDKs") / L"dxc" / L"bin" / L"x86" / L"dxc.exe",
#endif
                    // A packaged build ships a flat folder (see ReleaseGame.cpp) with no
                    // Source/SDKs structure at all - dxc.exe sits right next to the exe.
                    fs::path(L"dxc.exe"),
                });
            if (!discoveredDxcPath.empty())
            {
                return discoveredDxcPath;
            }
        }

        throw std::runtime_error("Failed to locate the bundled DXC executable under Source/SDKs/dxc.");
    }

    std::string ExecuteProcessAndCaptureOutput(
        const std::filesystem::path& executablePath,
        const std::vector<std::wstring>& arguments,
        const std::filesystem::path& workingDirectory,
        DWORD& outExitCode)
    {
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &securityAttributes, 0))
        {
            throw std::runtime_error("Failed to create the output pipe for dxc.exe.");
        }

        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        std::wstring commandLine = QuoteCommandLineArgument(executablePath.wstring());
        for (const std::wstring& argument : arguments)
        {
            commandLine += L' ';
            commandLine += QuoteCommandLineArgument(argument);
        }

        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startupInfo.hStdOutput = writePipe;
        startupInfo.hStdError = writePipe;

        PROCESS_INFORMATION processInfo{};
        const BOOL created = CreateProcessW(
            executablePath.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startupInfo,
            &processInfo);

        CloseHandle(writePipe);
        writePipe = nullptr;

        if (!created)
        {
            CloseHandle(readPipe);
            throw std::runtime_error("Failed to launch the bundled dxc.exe compiler.");
        }

        std::string processOutput;
        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(readPipe, buffer, static_cast<DWORD>(std::size(buffer)), &bytesRead, nullptr) && bytesRead > 0)
        {
            processOutput.append(buffer, buffer + bytesRead);
        }

        CloseHandle(readPipe);
        readPipe = nullptr;

        WaitForSingleObject(processInfo.hProcess, INFINITE);
        GetExitCodeProcess(processInfo.hProcess, &outExitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);

        return processOutput;
    }

    std::wstring MakeTemporaryFilePath(const wchar_t* extension)
    {
        namespace fs = std::filesystem;

        const fs::path tempDirectory = fs::temp_directory_path();
        const std::wstring fileName =
            std::wstring(L"PteroDxc_")
            + std::to_wstring(GetCurrentProcessId())
            + L"_"
            + std::to_wstring(GetTickCount64())
            + extension;
        return (tempDirectory / fileName).wstring();
    }

    std::vector<std::uint8_t> ReadBinaryFile(const std::wstring& filePath)
    {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open the DXC output file.");
        }

        const std::streamsize fileSize = file.tellg();
        if (fileSize <= 0)
        {
            return {};
        }

        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(bytes.data()), fileSize);
        return bytes;
    }

    void WriteBinaryFile(const std::filesystem::path& filePath, const std::vector<std::uint8_t>& bytes)
    {
        std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open the shader cache output file.");
        }

        if (!bytes.empty())
        {
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
    }

    std::string ReadTextFile(const std::filesystem::path& filePath)
    {
        std::string text;
        DataFiles::ReadText(filePath, text);
        return text;
    }

    std::wstring ResolveShaderPath(const std::wstring& shaderPath)
    {
        namespace fs = std::filesystem;

        const fs::path requestedPath(shaderPath);
        const fs::path requestedFileName = requestedPath.filename();
        if (requestedPath.is_absolute() && DataFiles::Exists(requestedPath))
        {
            return requestedPath.wstring();
        }

        const fs::path moduleDirectory = GetCurrentModuleDirectory();
        if (!moduleDirectory.empty())
        {
            const fs::path candidateInOutput = moduleDirectory / requestedPath;
            if (DataFiles::Exists(candidateInOutput))
            {
                return candidateInOutput.lexically_normal().wstring();
            }

            const fs::path discoveredShaderPath = FindAssetFromDirectory(
                moduleDirectory,
                {
                    fs::path(L"Data") / L"Shaders" / requestedFileName,
                    fs::path(L"Renderer_DX12") / requestedPath,
                    fs::path(L"Renderer_DX12") / L"Shaders" / requestedFileName,
                    requestedPath,
                });
            if (!discoveredShaderPath.empty())
            {
                return discoveredShaderPath.wstring();
            }
        }

        const fs::path candidateFromWorkingDirectory = fs::current_path() / L"Data" / L"Shaders" / requestedFileName;
        if (DataFiles::Exists(candidateFromWorkingDirectory))
        {
            return CanonicalOrNormal(candidateFromWorkingDirectory).wstring();
        }

        if (DataFiles::Exists(requestedPath))
        {
            return CanonicalOrNormal(requestedPath).wstring();
        }

        throw std::runtime_error("Failed to locate the requested HLSL shader source file.");
    }

    std::vector<std::uint8_t> CopyBlobBytes(const void* blobData, SIZE_T blobSize)
    {
        const auto* begin = static_cast<const std::uint8_t*>(blobData);
        return std::vector<std::uint8_t>(begin, begin + blobSize);
    }

    std::string NarrowString(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }

        const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (requiredSize <= 1)
        {
            return {};
        }

        // Reserve space for the terminating null that WideCharToMultiByte writes,
        // then trim it back off before returning the std::string. The previous
        // code sized the string to requiredSize - 1 but still asked the Win32
        // API to write requiredSize bytes, which overflowed the heap by one byte.
        std::string result(static_cast<size_t>(requiredSize), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), requiredSize, nullptr, nullptr);
        result.resize(static_cast<size_t>(requiredSize - 1));
        return result;
    }

    uint64_t Fnv1aAppend(uint64_t hash, const char* text, const size_t length)
    {
        constexpr uint64_t prime = 1099511628211ull;
        for (size_t i = 0; i < length; ++i)
        {
            hash ^= static_cast<unsigned char>(text[i]);
            hash *= prime;
        }

        return hash;
    }

    void AppendHashString(uint64_t& hash, const std::string& value)
    {
        hash = Fnv1aAppend(hash, value.data(), value.size());
        hash = Fnv1aAppend(hash, "\n", 1);
    }

    std::string ToHex(const uint64_t value)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result(16, '0');
        for (size_t i = 0; i < result.size(); ++i)
        {
            const size_t shift = (result.size() - 1 - i) * 4;
            result[i] = digits[(value >> shift) & 0x0f];
        }

        return result;
    }

    std::string HexEncode(const std::string& value)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(value.size() * 2);
        for (const unsigned char character : value)
        {
            result.push_back(digits[character >> 4]);
            result.push_back(digits[character & 0x0f]);
        }

        return result;
    }

    std::string BuildShaderCacheKey(const ShaderCompileRequest& request, const std::wstring& resolvedPath)
    {
        constexpr uint64_t offsetBasis = 14695981039346656037ull;
        uint64_t hash = offsetBasis;

        AppendHashString(hash, ShaderCacheVersion);
#if defined(PTERO_DEBUG)
        AppendHashString(hash, "Debug");
#else
        AppendHashString(hash, "Release");
#endif
        AppendHashString(hash, NarrowString(resolvedPath));
        AppendHashString(hash, NarrowString(request.EntryPoint));
        AppendHashString(hash, NarrowString(request.TargetProfile));
        AppendHashString(hash, std::to_string(static_cast<int>(request.Stage)));

        for (const std::wstring& define : request.Defines)
        {
            AppendHashString(hash, "D:" + NarrowString(define));
        }

        for (const std::wstring& includeDirectory : request.IncludeDirectories)
        {
            AppendHashString(hash, "I:" + NarrowString(includeDirectory));
        }

        return ToHex(hash);
    }

    std::filesystem::path FindProjectRootFromShaderPath(const std::filesystem::path& resolvedShaderPath)
    {
        namespace fs = std::filesystem;

        fs::path currentDirectory = resolvedShaderPath.parent_path();
        while (!currentDirectory.empty())
        {
            if (_wcsicmp(currentDirectory.filename().wstring().c_str(), L"Shaders") == 0)
            {
                const fs::path dataDirectory = currentDirectory.parent_path();
                if (_wcsicmp(dataDirectory.filename().wstring().c_str(), L"Data") == 0)
                {
                    return dataDirectory.parent_path();
                }
            }

            const fs::path candidateDataShaders = currentDirectory / L"Data" / L"Shaders";
            if (DataFiles::IsDirectory(candidateDataShaders))
            {
                return currentDirectory;
            }

            const fs::path parentDirectory = currentDirectory.parent_path();
            if (parentDirectory == currentDirectory)
            {
                break;
            }

            currentDirectory = parentDirectory;
        }

        const fs::path moduleDirectory = GetCurrentModuleDirectory();
        if (!moduleDirectory.empty())
        {
            const fs::path dataShaders = FindAssetFromDirectory(moduleDirectory, { fs::path(L"Data") / L"Shaders" });
            if (!dataShaders.empty())
            {
                return dataShaders.parent_path().parent_path();
            }
        }

        return fs::current_path();
    }

    std::filesystem::path GetShaderCacheDirectory(const std::wstring& resolvedPath)
    {
        namespace fs = std::filesystem;

        const fs::path projectRoot = FindProjectRootFromShaderPath(fs::path(resolvedPath));
        fs::path cacheDirectory = projectRoot / L"Cache" / L"Shaders";
        std::error_code createError;
        fs::create_directories(cacheDirectory, createError);
        if (createError)
        {
            OutputDebugStringA("Failed to create shader cache directory: ");
            OutputDebugStringA(createError.message().c_str());
            OutputDebugStringA("\n");
        }

        return cacheDirectory;
    }

    struct ShaderDependencyStamp
    {
        std::filesystem::path Path;
        uintmax_t Size = 0;
        long long WriteTime = 0;
    };

    ShaderDependencyStamp MakeDependencyStamp(const std::filesystem::path& path, const std::string& sourceText)
    {
        ShaderDependencyStamp stamp{};
        stamp.Path = CanonicalOrNormal(path);

        // An archived source has no size or timestamp on disk. Its content hash stands in
        // for the timestamp, so a rebuilt game with changed shaders still misses the cache.
        if (IsPackagedPath(stamp.Path))
        {
            stamp.Size = sourceText.size();
            stamp.WriteTime = static_cast<long long>(Fnv1aAppend(14695981039346656037ull, sourceText.data(), sourceText.size()));
            return stamp;
        }

        std::error_code error;
        stamp.Size = std::filesystem::file_size(stamp.Path, error);
        if (error)
        {
            stamp.Size = 0;
        }

        error.clear();
        const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(stamp.Path, error);
        stamp.WriteTime = error ? 0 : writeTime.time_since_epoch().count();
        return stamp;
    }

    std::optional<std::filesystem::path> ResolveIncludePath(
        const std::string& includePath,
        const std::filesystem::path& sourceDirectory,
        const std::vector<std::filesystem::path>& includeDirectories)
    {
        namespace fs = std::filesystem;

        const fs::path requestedPath = fs::path(includePath);
        std::vector<fs::path> candidates;
        if (requestedPath.is_absolute())
        {
            candidates.push_back(requestedPath);
        }
        else
        {
            candidates.push_back(sourceDirectory / requestedPath);
            for (const fs::path& includeDirectory : includeDirectories)
            {
                candidates.push_back(includeDirectory / requestedPath);
            }
        }

        for (const fs::path& candidate : candidates)
        {
            if (DataFiles::IsFile(candidate))
            {
                return CanonicalOrNormal(candidate);
            }
        }

        return std::nullopt;
    }

    std::vector<std::string> ExtractIncludePaths(const std::string& sourceText)
    {
        std::vector<std::string> includes;
        size_t searchOffset = 0;
        while (true)
        {
            const size_t includePosition = sourceText.find("#include", searchOffset);
            if (includePosition == std::string::npos)
            {
                break;
            }

            const size_t delimiterPosition = sourceText.find_first_of("\"<", includePosition + 8);
            if (delimiterPosition == std::string::npos)
            {
                break;
            }

            const char closingDelimiter = sourceText[delimiterPosition] == '"' ? '"' : '>';
            const size_t endPosition = sourceText.find(closingDelimiter, delimiterPosition + 1);
            if (endPosition == std::string::npos)
            {
                break;
            }

            includes.push_back(sourceText.substr(delimiterPosition + 1, endPosition - delimiterPosition - 1));
            searchOffset = endPosition + 1;
        }

        return includes;
    }

    void CollectShaderDependencies(
        const std::filesystem::path& sourcePath,
        const std::vector<std::filesystem::path>& includeDirectories,
        std::unordered_set<std::wstring>& visitedPaths,
        std::vector<ShaderDependencyStamp>& dependencies)
    {
        namespace fs = std::filesystem;

        const fs::path canonicalPath = CanonicalOrNormal(sourcePath);

        const std::wstring canonicalKey = canonicalPath.wstring();
        if (!visitedPaths.insert(canonicalKey).second)
        {
            return;
        }

        const std::string sourceText = ReadTextFile(canonicalPath);
        dependencies.push_back(MakeDependencyStamp(canonicalPath, sourceText));
        if (sourceText.empty())
        {
            return;
        }

        const fs::path sourceDirectory = canonicalPath.parent_path();
        for (const std::string& includePath : ExtractIncludePaths(sourceText))
        {
            const std::optional<fs::path> resolvedIncludePath = ResolveIncludePath(includePath, sourceDirectory, includeDirectories);
            if (resolvedIncludePath.has_value())
            {
                CollectShaderDependencies(*resolvedIncludePath, includeDirectories, visitedPaths, dependencies);
            }
        }
    }

    std::vector<ShaderDependencyStamp> BuildShaderDependencySnapshot(
        const ShaderCompileRequest& request,
        const std::wstring& resolvedPath)
    {
        namespace fs = std::filesystem;

        const fs::path resolvedShaderPath(resolvedPath);
        std::vector<fs::path> includeDirectories;
        includeDirectories.push_back(resolvedShaderPath.parent_path());

        for (const std::wstring& includeDirectory : request.IncludeDirectories)
        {
            if (!includeDirectory.empty())
            {
                includeDirectories.emplace_back(includeDirectory);
            }
        }

        const fs::path projectRoot = FindProjectRootFromShaderPath(resolvedShaderPath);
        includeDirectories.push_back(projectRoot / L"Data" / L"Shaders");

        std::unordered_set<std::wstring> visitedPaths;
        std::vector<ShaderDependencyStamp> dependencies;
        CollectShaderDependencies(resolvedShaderPath, includeDirectories, visitedPaths, dependencies);

        std::sort(
            dependencies.begin(),
            dependencies.end(),
            [](const ShaderDependencyStamp& left, const ShaderDependencyStamp& right)
            {
                return left.Path.wstring() < right.Path.wstring();
            });

        return dependencies;
    }

    std::string BuildShaderCacheMetadata(
        const std::string& cacheKey,
        const std::vector<ShaderDependencyStamp>& dependencies)
    {
        std::ostringstream metadata;
        metadata << ShaderCacheVersion << "\n";
        metadata << "key=" << cacheKey << "\n";
        metadata << "dependencies=" << dependencies.size() << "\n";
        for (const ShaderDependencyStamp& dependency : dependencies)
        {
            metadata
                << HexEncode(NarrowString(dependency.Path.wstring()))
                << "|" << dependency.Size
                << "|" << dependency.WriteTime
                << "\n";
        }

        return metadata.str();
    }

    bool TryLoadShaderFromCache(
        const std::filesystem::path& bytecodePath,
        const std::filesystem::path& metadataPath,
        const std::string& expectedMetadata,
        std::vector<std::uint8_t>& outBytecode)
    {
        namespace fs = std::filesystem;

        std::error_code existsError;
        if (!fs::exists(bytecodePath, existsError) || existsError
            || !fs::exists(metadataPath, existsError) || existsError)
        {
            return false;
        }

        const std::string actualMetadata = ReadTextFile(metadataPath);
        if (actualMetadata != expectedMetadata)
        {
            return false;
        }

        outBytecode = ReadBinaryFile(bytecodePath.wstring());
        return !outBytecode.empty();
    }

    void StoreShaderInCache(
        const std::filesystem::path& bytecodePath,
        const std::filesystem::path& metadataPath,
        const std::string& metadata,
        const std::vector<std::uint8_t>& bytecode)
    {
        try
        {
            WriteBinaryFile(bytecodePath, bytecode);

            std::ofstream metadataFile(metadataPath, std::ios::binary | std::ios::trunc);
            if (!metadataFile.is_open())
            {
                throw std::runtime_error("Failed to open the shader cache metadata file.");
            }

            metadataFile << metadata;
        }
        catch (const std::exception& exception)
        {
            OutputDebugStringA("Failed to write shader cache entry: ");
            OutputDebugStringA(exception.what());
            OutputDebugStringA("\n");
        }
    }

    std::vector<std::uint8_t> CompileShaderBlobWithDxc(const ShaderCompileRequest& request, const std::wstring& resolvedPath)
    {
        namespace fs = std::filesystem;

        const fs::path bundledDxcPath = ResolveBundledDxcExecutablePath();
        const fs::path resolvedShaderPath = fs::path(resolvedPath);
        const std::wstring outputPath = MakeTemporaryFilePath(L".dxil");

        std::vector<std::wstring> arguments =
        {
            resolvedPath,
            L"-T", request.TargetProfile,
            L"-Fo", outputPath,
            L"-I", resolvedShaderPath.parent_path().wstring(),
            // Not -WX: SM5 shaders now route through dxc too (see CompileShaderBytecode),
            // and dxc is stricter than the legacy D3DCompiler they used to compile under -
            // e.g. it flags AgxTonemap.hlsl's implicit vector truncation as a warning that
            // -WX would promote to a hard compile failure, even though that shader has
            // always compiled (and worked) fine under the old compiler. Warnings still
            // print via compilerOutput below; just not fatal, matching the leniency this
            // codebase already had for every SM5 shader before this change.
            L"-all_resources_bound",
#if defined(PTERO_DEBUG)
            L"-Zi",
            L"-Qembed_debug",
            L"-Od",
#else
            L"-O3",
#endif
        };

        for (const std::wstring& includeDirectory : request.IncludeDirectories)
        {
            if (!includeDirectory.empty())
            {
                arguments.push_back(L"-I");
                arguments.push_back(includeDirectory);
            }
        }

        for (const std::wstring& define : request.Defines)
        {
            if (!define.empty())
            {
                arguments.push_back(L"-D");
                arguments.push_back(define);
            }
        }

        // DXIL library targets such as lib_6_3 do not use a single entry point.
        // Only pass -E when the caller supplied one so the same compiler path can
        // build both regular shaders and exported DXR libraries from source.
        if (!request.EntryPoint.empty())
        {
            arguments.push_back(L"-E");
            arguments.push_back(request.EntryPoint);
        }

        DWORD exitCode = 0;
        const std::string compilerOutput = ExecuteProcessAndCaptureOutput(
            bundledDxcPath,
            arguments,
            resolvedShaderPath.parent_path(),
            exitCode);

        if (!compilerOutput.empty())
        {
            OutputDebugStringA(compilerOutput.c_str());
        }

        if (exitCode != 0)
        {
            std::error_code removeError;
            fs::remove(outputPath, removeError);
            throw std::runtime_error(
                compilerOutput.empty()
                    ? "The bundled dxc.exe compiler failed without diagnostic output."
                    : compilerOutput);
        }

        std::vector<std::uint8_t> bytecode = ReadBinaryFile(outputPath);
        std::error_code removeError;
        fs::remove(outputPath, removeError);
        return bytecode;
    }

    // Every shader (SM5 and SM6 profiles alike) compiles through the bundled dxc.exe.
    // SM5 targets used to go through the legacy system D3DCompiler (d3dcompiler_47.dll)
    // instead, on the theory that it widened compatibility to machines without the full
    // DXC + DXIL runtime pair - but a packaged build already ships dxcompiler.dll/dxil.dll
    // (and now dxc.exe itself, see ReleaseGame.cpp) for the SM6 shaders regardless, making
    // that reasoning moot, and D3DCompileFromFile's legacy compiler turned out to have a
    // latent bug: it hangs for 20-35s then crashes with an access violation on at least one
    // real shader (DeferredLighting.hlsl) the very first time it has to compile it from
    // source rather than hit a warm Cache/Shaders/ entry - invisible in normal dev use
    // (the cache is always warm there) but fatal on a fresh install with an empty cache.
    // dxc compiles SM5.0/5.1 targets fine, so there is no remaining reason to keep both
    // compiler backends.
    std::vector<std::uint8_t> CompileShaderBytecode(const ShaderCompileRequest& request, const std::wstring& resolvedPath)
    {
        return CompileShaderBlobWithDxc(request, resolvedPath);
    }

    // -----------------------------------------------------------------------------------
    // Packaged games: in-process DXC over the archives
    //
    // dxc.exe can only read sources from disk, and a packaged game's shaders exist only
    // inside Shaders.ppak. So for those, the same compile runs through dxcompiler.dll
    // (shipped next to the game already) with the source handed over as a buffer and an
    // include handler that reads #includes through DataFiles - nothing is written out.
    // -----------------------------------------------------------------------------------

    using DxcCreateInstanceFn = HRESULT(__stdcall*)(REFCLSID, REFIID, LPVOID*);

    DxcCreateInstanceFn GetDxcCreateInstance()
    {
        static const DxcCreateInstanceFn createInstance = []() -> DxcCreateInstanceFn
        {
            HMODULE module = LoadLibraryW(L"dxcompiler.dll");
            return module != nullptr
                ? reinterpret_cast<DxcCreateInstanceFn>(GetProcAddress(module, "DxcCreateInstance"))
                : nullptr;
        }();
        return createInstance;
    }

    // Lives on the stack for the duration of one Compile call, so reference counting is
    // only for DXC's benefit and never frees anything.
    class DataFilesIncludeHandler final : public IDxcIncludeHandler
    {
    public:
        DataFilesIncludeHandler(IDxcUtils* utils, std::vector<std::filesystem::path> searchDirectories)
            : mUtils(utils), mSearchDirectories(std::move(searchDirectories))
        {
        }

        HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** includeSource) override
        {
            namespace fs = std::filesystem;
            if (includeSource == nullptr)
            {
                return E_POINTER;
            }
            *includeSource = nullptr;
            if (fileName == nullptr)
            {
                return E_INVALIDARG;
            }

            // DXC usually hands over a path already joined with one of the search
            // directories; the search directories are retried in case it did not.
            const fs::path requested(fileName);
            std::vector<fs::path> candidates{ requested };
            if (!requested.is_absolute())
            {
                for (const fs::path& directory : mSearchDirectories)
                {
                    candidates.push_back(directory / requested);
                }
            }

            for (const fs::path& candidate : candidates)
            {
                std::string text;
                if (!DataFiles::ReadText(candidate.lexically_normal(), text))
                {
                    continue;
                }

                ComPtr<IDxcBlobEncoding> blob;
                const HRESULT hr = mUtils->CreateBlob(text.data(), static_cast<UINT32>(text.size()), DXC_CP_UTF8, &blob);
                if (FAILED(hr))
                {
                    return hr;
                }
                *includeSource = blob.Detach();
                return S_OK;
            }

            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (object == nullptr)
            {
                return E_POINTER;
            }
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler))
            {
                *object = static_cast<IDxcIncludeHandler*>(this);
                AddRef();
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++mReferences; }
        ULONG STDMETHODCALLTYPE Release() override { return --mReferences; }

    private:
        IDxcUtils* mUtils = nullptr;
        std::vector<std::filesystem::path> mSearchDirectories;
        ULONG mReferences = 1;
    };

    std::vector<std::uint8_t> CompileShaderBlobInProcess(const ShaderCompileRequest& request, const std::wstring& resolvedPath)
    {
        namespace fs = std::filesystem;

        const DxcCreateInstanceFn createInstance = GetDxcCreateInstance();
        if (createInstance == nullptr)
        {
            throw std::runtime_error("dxcompiler.dll could not be loaded to compile a packaged shader.");
        }

        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        if (FAILED(createInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))
            || FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        {
            throw std::runtime_error("dxcompiler.dll could not create a compiler instance.");
        }

        std::string sourceText;
        if (!DataFiles::ReadText(resolvedPath, sourceText))
        {
            throw std::runtime_error("Failed to read the packaged HLSL source: " + NarrowString(resolvedPath));
        }

        // Relative include directories (the renderers fall back to "Data\\Shaders\\" when
        // no Data folder exists on disk) are relative to the game's own folder.
        const fs::path moduleDirectory = GetCurrentModuleDirectory();
        const auto absoluteDirectory = [&](const std::wstring& directory)
        {
            const fs::path path(directory);
            return (path.is_absolute() ? path : moduleDirectory / path).lexically_normal();
        };

        // dxc.exe quietly promotes pre-6.0 profiles ("Promoting older shader model profile
        // to 6.0 version"); the library interface rejects them outright as invalid. Many of
        // the engine's shaders still ask for vs/ps/cs_5_0, so promote them the same way.
        std::wstring targetProfile = request.TargetProfile;
        if (targetProfile.size() >= 4)
        {
            const size_t majorPosition = targetProfile.find(L'_');
            if (majorPosition != std::wstring::npos && majorPosition + 1 < targetProfile.size()
                && targetProfile[majorPosition + 1] >= L'0' && targetProfile[majorPosition + 1] < L'6')
            {
                targetProfile = targetProfile.substr(0, majorPosition) + L"_6_0";
            }
        }

        const fs::path sourceDirectory = fs::path(resolvedPath).parent_path();
        std::vector<fs::path> searchDirectories{ sourceDirectory };
        std::vector<std::wstring> arguments
        {
            resolvedPath,
            L"-T", targetProfile,
            L"-I", sourceDirectory.wstring(),
            // Same options as the dxc.exe path above, for the same reasons.
            L"-all_resources_bound",
#if defined(PTERO_DEBUG)
            L"-Zi",
            L"-Qembed_debug",
            L"-Od",
#else
            L"-O3",
#endif
        };

        for (const std::wstring& includeDirectory : request.IncludeDirectories)
        {
            if (!includeDirectory.empty())
            {
                const fs::path directory = absoluteDirectory(includeDirectory);
                searchDirectories.push_back(directory);
                arguments.push_back(L"-I");
                arguments.push_back(directory.wstring());
            }
        }

        for (const std::wstring& define : request.Defines)
        {
            if (!define.empty())
            {
                arguments.push_back(L"-D");
                arguments.push_back(define);
            }
        }

        if (!request.EntryPoint.empty())
        {
            arguments.push_back(L"-E");
            arguments.push_back(request.EntryPoint);
        }

        std::vector<LPCWSTR> argumentPointers;
        argumentPointers.reserve(arguments.size());
        for (const std::wstring& argument : arguments)
        {
            argumentPointers.push_back(argument.c_str());
        }

        DxcBuffer source{};
        source.Ptr = sourceText.data();
        source.Size = sourceText.size();
        source.Encoding = DXC_CP_UTF8;

        DataFilesIncludeHandler includeHandler(utils.Get(), std::move(searchDirectories));
        ComPtr<IDxcResult> result;
        HRESULT hr = compiler->Compile(
            &source,
            argumentPointers.data(),
            static_cast<UINT32>(argumentPointers.size()),
            &includeHandler,
            IID_PPV_ARGS(&result));

        HRESULT status = hr;
        if (SUCCEEDED(hr) && result)
        {
            result->GetStatus(&status);
        }

        std::string diagnostics;
        if (result)
        {
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr))
                && errors && errors->GetStringLength() > 0)
            {
                diagnostics.assign(errors->GetStringPointer(), errors->GetStringLength());
                OutputDebugStringA(diagnostics.c_str());
            }
        }

        if (FAILED(status))
        {
            throw std::runtime_error(diagnostics.empty()
                ? "dxcompiler.dll failed to compile " + NarrowString(resolvedPath) + " without diagnostic output."
                : diagnostics);
        }

        ComPtr<IDxcBlob> bytecode;
        if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&bytecode), nullptr)) || !bytecode)
        {
            throw std::runtime_error("dxcompiler.dll produced no bytecode for " + NarrowString(resolvedPath) + ".");
        }

        return CopyBlobBytes(bytecode->GetBufferPointer(), bytecode->GetBufferSize());
    }
}

bool DX12Shader::Compile(const ShaderCompileRequest& request)
{
    try
    {
        mLastErrorMessage.clear();
        mLoadedFromCache = false;
        mSourcePath = ResolveShaderPath(request.FilePath);

        const std::string cacheKey = BuildShaderCacheKey(request, mSourcePath);
        const std::filesystem::path cacheDirectory = GetShaderCacheDirectory(mSourcePath);
        const std::filesystem::path bytecodeCachePath = cacheDirectory / (cacheKey + ".dxil");
        const std::filesystem::path metadataCachePath = cacheDirectory / (cacheKey + ".meta");
        const std::vector<ShaderDependencyStamp> dependencies = BuildShaderDependencySnapshot(request, mSourcePath);
        const std::string expectedMetadata = BuildShaderCacheMetadata(cacheKey, dependencies);

        std::lock_guard<std::mutex> cacheLock(gShaderCacheMutex);
        if (TryLoadShaderFromCache(bytecodeCachePath, metadataCachePath, expectedMetadata, mBytecode))
        {
            mLoadedFromCache = true;
            return true;
        }

        mBytecode = IsPackagedPath(mSourcePath)
            ? CompileShaderBlobInProcess(request, mSourcePath)
            : CompileShaderBytecode(request, mSourcePath);
        mLoadedFromCache = false;
        StoreShaderInCache(bytecodeCachePath, metadataCachePath, expectedMetadata, mBytecode);
        return true;
    }
    catch (const std::exception& exception)
    {
        mLastErrorMessage = exception.what();
        OutputDebugStringA("DX12Shader::Compile failed: ");
        OutputDebugStringA(mLastErrorMessage.c_str());
        OutputDebugStringA("\n");
        mSourcePath.clear();
        mBytecode.clear();
        mLoadedFromCache = false;
        return false;
    }
    catch (...)
    {
        mLastErrorMessage = "Shader compilation failed with an unknown error.";
        OutputDebugStringA("DX12Shader::Compile failed with an unknown error.\n");
        mSourcePath.clear();
        mBytecode.clear();
        mLoadedFromCache = false;
        return false;
    }
}
