#include "pch.h"
#include "DX12ShaderCompiler.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
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
                if (fs::exists(candidatePath))
                {
                    return fs::weakly_canonical(candidatePath);
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

    std::wstring ResolveShaderPath(const std::wstring& shaderPath)
    {
        namespace fs = std::filesystem;

        const fs::path requestedPath(shaderPath);
        const fs::path requestedFileName = requestedPath.filename();
        if (requestedPath.is_absolute() && fs::exists(requestedPath))
        {
            return requestedPath.wstring();
        }

        const fs::path moduleDirectory = GetCurrentModuleDirectory();
        if (!moduleDirectory.empty())
        {
            const fs::path candidateInOutput = moduleDirectory / requestedPath;
            if (fs::exists(candidateInOutput))
            {
                return candidateInOutput.wstring();
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
        if (fs::exists(candidateFromWorkingDirectory))
        {
            return fs::weakly_canonical(candidateFromWorkingDirectory).wstring();
        }

        if (fs::exists(requestedPath))
        {
            return fs::weakly_canonical(requestedPath).wstring();
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
            L"-WX",
            L"-all_resources_bound",
#if defined(_DEBUG)
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

    std::vector<std::uint8_t> CompileShaderBlobWithD3DCompile(const ShaderCompileRequest& request, const std::wstring& resolvedPath)
    {
        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> shaderBlob;
        ComPtr<ID3DBlob> errorBlob;
        const HRESULT compileResult = D3DCompileFromFile(
            resolvedPath.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            NarrowString(request.EntryPoint).c_str(),
            NarrowString(request.TargetProfile).c_str(),
            compileFlags,
            0,
            &shaderBlob,
            &errorBlob);

        if (errorBlob && errorBlob->GetBufferSize() > 0)
        {
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }

        DX12_THROW_IF_FAILED(compileResult);
        return CopyBlobBytes(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
    }

    bool UsesShaderModel5Profile(const std::wstring& targetProfile)
    {
        return targetProfile.find(L"_5_") != std::wstring::npos;
    }

    std::vector<std::uint8_t> CompileShaderBytecode(const ShaderCompileRequest& request, const std::wstring& resolvedPath)
    {
        // The editor's test cube does not need Shader Model 6 features. Prefer the
        // system compiler for Shader Model 5 targets so the scene renderer works on
        // machines that do not ship the full DXC + DXIL runtime pair.
        if (UsesShaderModel5Profile(request.TargetProfile))
        {
            return CompileShaderBlobWithD3DCompile(request, resolvedPath);
        }

        return CompileShaderBlobWithDxc(request, resolvedPath);
    }
}

bool DX12Shader::Compile(const ShaderCompileRequest& request)
{
    try
    {
        mLastErrorMessage.clear();
        mSourcePath = ResolveShaderPath(request.FilePath);
        mBytecode = CompileShaderBytecode(request, mSourcePath);
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
        return false;
    }
    catch (...)
    {
        mLastErrorMessage = "Shader compilation failed with an unknown error.";
        OutputDebugStringA("DX12Shader::Compile failed with an unknown error.\n");
        mSourcePath.clear();
        mBytecode.clear();
        return false;
    }
}
