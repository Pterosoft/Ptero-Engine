// GameLauncher - the minimal standalone host for a packaged game build.
//
// This is a trimmed copy of EditorMain.cpp's bootstrap (window creation, loading
// Renderer_DX12.dll dynamically, driving the render loop) with all editor chrome removed:
// no menu, no accelerators, no About box, no "unsaved changes" prompt, no splash screen.
// It always runs in the standalone-game path that EditorMain.cpp only takes when launched
// with "--game <level>" - see RendererDX12_ConfigureStandaloneGame in DX12RendererAPI.cpp.
//
// Deliberately duplicated rather than shared with EditorMain.cpp/Ptero-Engine.vcxproj, so
// changes here can never destabilize the editor's own entry point.
//
// Before touching Renderer_DX12.dll at all, it mounts the packaged Content\*.ppak files
// (built by the editor's Release menu). Nothing is unpacked: the archives are indexed
// once, and each asset is decrypted into memory when an engine module asks for it,
// through the PteroData_* functions this exe exports (see System/DataFiles.h, which is
// how Renderer_DX12, Audio and Video find them). The game's Data root is the virtual
// "<exe dir>\Data", which never exists on disk.

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include "AudioManager.h"
#include "System/PackageFormat.h"
#include "System/PackagedDataApi.h"
#include "System/PackagingKeyObfuscation.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Matches the IDs the editor's resource patcher writes into a copy of
    // GameLauncherTemplate.exe when it builds a game (see the Release menu's EXE step).
    constexpr int kGameIconResourceId = 100;
    constexpr int kPackagingKeyResourceId = 200;

    using RendererInitializeFn = bool(__stdcall*)(HWND);
    using RendererRenderFn = bool(__stdcall*)();
    using RendererResizeFn = bool(__stdcall*)(UINT, UINT);
    using RendererHandleWindowMessageFn = bool(__stdcall*)(HWND, UINT, WPARAM, LPARAM);
    using RendererShutdownFn = void(__stdcall*)();
    using RendererConfirmCloseFn = bool(__stdcall*)(HWND);
    using RendererGetLastErrorFn = const char* (__stdcall*)();
    using RendererProgressFn = void(__stdcall*)(const wchar_t* message);
    using RendererSetProgressCallbackFn = void(__stdcall*)(RendererProgressFn);
    using RendererSetAudioManagerFn = void(__stdcall*)(AudioManager*);
    using RendererSetLoopTimingsFn = void(__stdcall*)(float, float, unsigned, unsigned);
    using RendererConfigureStandaloneGameFn = void(__stdcall*)(const wchar_t*);

    AudioManager gAudioManager;

    HMODULE gRendererModule = nullptr;
    RendererInitializeFn gRendererInitialize = nullptr;
    RendererRenderFn gRendererRender = nullptr;
    RendererResizeFn gRendererResize = nullptr;
    RendererHandleWindowMessageFn gRendererHandleWindowMessage = nullptr;
    RendererShutdownFn gRendererShutdown = nullptr;
    RendererConfirmCloseFn gRendererConfirmClose = nullptr;
    RendererGetLastErrorFn gRendererGetLastError = nullptr;
    RendererSetProgressCallbackFn gRendererSetProgressCallback = nullptr;
    RendererSetAudioManagerFn gRendererSetAudioManager = nullptr;
    RendererSetLoopTimingsFn gRendererSetLoopTimings = nullptr;

    bool gRendererReady = false;
    bool gIsClosing = false;
    HWND gMainWindowHandle = nullptr;
    std::wstring gWindowTitle = L"Ptero Game";

    const wchar_t* kWindowClassName = L"PteroGameLauncherWindowClass";

    void ReportFatalError(const std::wstring& message)
    {
        MessageBoxW(gMainWindowHandle, message.c_str(), L"Game startup", MB_OK | MB_ICONERROR);
    }

    fs::path GetExecutableDirectory()
    {
        wchar_t modulePath[MAX_PATH * 4] = {};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        return fs::path(modulePath).parent_path();
    }

    // Reads the packaging key the editor embedded as an RT_RCDATA resource when it built
    // this exe. A raw, unpackaged GameLauncherTemplate.exe has no such resource, which is
    // the signal to refuse to run rather than limp along unable to decrypt anything.
    bool ReadEmbeddedPackagingKey(Packaging::Key& outKey)
    {
        HRSRC resourceHandle = FindResourceW(nullptr, MAKEINTRESOURCE(kPackagingKeyResourceId), RT_RCDATA);
        if (resourceHandle == nullptr)
            return false;

        HGLOBAL resourceData = LoadResource(nullptr, resourceHandle);
        if (resourceData == nullptr)
            return false;

        const DWORD size = SizeofResource(nullptr, resourceHandle);
        if (size != outKey.size())
            return false;

        const auto* bytes = static_cast<const unsigned char*>(LockResource(resourceData));
        if (bytes == nullptr)
            return false;

        Packaging::Key obfuscated{};
        std::memcpy(obfuscated.data(), bytes, obfuscated.size());
        outKey = Packaging::ObfuscateKey(obfuscated);
        return true;
    }

    std::wstring ReadGameTitle(const fs::path& exeDirectory)
    {
        std::ifstream configFile(exeDirectory / L"game.cfg");
        if (!configFile)
            return L"Ptero Game";

        std::string firstLine;
        std::getline(configFile, firstLine);
        while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == '\n'))
            firstLine.pop_back();
        if (firstLine.empty())
            return L"Ptero Game";

        const int required = MultiByteToWideChar(CP_UTF8, 0, firstLine.c_str(), -1, nullptr, 0);
        if (required <= 0)
            return L"Ptero Game";
        std::wstring wide(static_cast<size_t>(required) - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, firstLine.c_str(), -1, wide.data(), required);
        return wide;
    }

    // ---------------------------------------------------------------------------------
    // Mounted content
    // ---------------------------------------------------------------------------------

    struct MountedFile
    {
        const Packaging::PackageArchive* Archive = nullptr;
        size_t Index = 0;
        std::string Path;   // "Textures/Foo.dds", as stored (original casing)
    };

    Packaging::Key gContentKey{};
    std::vector<std::unique_ptr<Packaging::PackageArchive>> gArchives;
    std::unordered_map<std::string, MountedFile> gMountedFiles;   // keyed by lower-case path
    std::set<std::string> gMountedDirectories;                    // lower-case, every prefix

    std::string LowerPath(const char* path)
    {
        std::string lowered = path != nullptr ? path : "";
        for (char& c : lowered)
        {
            if (c == '\\')
                c = '/';
            else if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        while (!lowered.empty() && lowered.back() == '/')
            lowered.pop_back();
        return lowered;
    }

    // Indexes every Content\*.ppak. Each archive holds one Data\ subfolder, named after
    // the archive (Textures.ppak -> "Textures/..."). A missing Content folder is not an
    // error here; the renderer reports the level it then cannot find.
    bool MountContent(const fs::path& exeDirectory, std::wstring& outError)
    {
        const fs::path contentDirectory = exeDirectory / L"Content";
        std::error_code error;
        if (!fs::is_directory(contentDirectory, error))
            return true;

        for (const auto& entry : fs::directory_iterator(contentDirectory, error))
        {
            if (!entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), L".ppak") != 0)
                continue;

            auto archive = std::make_unique<Packaging::PackageArchive>();
            std::string openError;
            if (!archive->Open(entry.path().wstring(), openError))
            {
                outError = L"Failed to open " + entry.path().filename().wstring() + L": ";
                outError += std::wstring(openError.begin(), openError.end());
                return false;
            }

            const std::string folder = entry.path().stem().string();
            const std::vector<std::string>& paths = archive->Paths();
            for (size_t i = 0; i < paths.size(); ++i)
            {
                MountedFile file{ archive.get(), i, folder + "/" + paths[i] };
                std::string key = LowerPath(file.Path.c_str());
                for (size_t slash = key.find('/'); slash != std::string::npos; slash = key.find('/', slash + 1))
                    gMountedDirectories.insert(key.substr(0, slash));
                gMountedFiles[std::move(key)] = std::move(file);
            }
            gArchives.push_back(std::move(archive));
        }
        gMountedDirectories.insert(std::string());
        return true;
    }

    void CleanupRenderer()
    {
        if (gRendererReady && gRendererShutdown)
            gRendererShutdown();

        if (gRendererModule)
        {
            FreeLibrary(gRendererModule);
            gRendererModule = nullptr;
        }
        gRendererReady = false;
    }

    LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CLOSE:
            if (gRendererReady && gRendererConfirmClose && !gRendererConfirmClose(hWnd))
                return 0;
            gIsClosing = true;
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        if (gRendererReady && gRendererHandleWindowMessage && gRendererHandleWindowMessage(hWnd, message, wParam, lParam))
            return 1;

        switch (message)
        {
        case WM_ERASEBKGND:
            return gRendererReady ? 1 : DefWindowProc(hWnd, message, wParam, lParam);
        case WM_SIZE:
            if (gRendererReady && gRendererResize && wParam != SIZE_MINIMIZED)
                gRendererResize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paintStruct;
            HDC deviceContext = BeginPaint(hWnd, &paintStruct);
            static HBRUSH backgroundBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(deviceContext, &paintStruct.rcPaint, backgroundBrush);
            EndPaint(hWnd, &paintStruct);
            return 0;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
}

// The packaged-data API every engine DLL resolves on this exe (see PackagedDataApi.h).
extern "C"
{
    __declspec(dllexport) long long __cdecl PteroData_FileSize(const char* relativePath)
    {
        const auto it = gMountedFiles.find(LowerPath(relativePath));
        return it == gMountedFiles.end() ? -1 : static_cast<long long>(it->second.Archive->PlaintextSize(it->second.Index));
    }

    __declspec(dllexport) bool __cdecl PteroData_ReadFile(const char* relativePath, void* buffer, unsigned long long size)
    {
        const auto it = gMountedFiles.find(LowerPath(relativePath));
        if (it == gMountedFiles.end())
            return false;

        std::string error;
        if (!it->second.Archive->Read(it->second.Index, gContentKey, buffer, size, error))
        {
            OutputDebugStringA(("[GameLauncher] " + error + "\n").c_str());
            return false;
        }
        return true;
    }

    __declspec(dllexport) bool __cdecl PteroData_IsDirectory(const char* relativePath)
    {
        return gMountedDirectories.count(LowerPath(relativePath)) != 0;
    }

    __declspec(dllexport) void __cdecl PteroData_List(const char* directory, bool recursive,
                                                      PteroDataListCallback callback, void* context)
    {
        if (callback == nullptr)
            return;

        const std::string prefix = LowerPath(directory);
        const std::string withSlash = prefix.empty() ? std::string() : prefix + "/";
        for (const auto& [key, file] : gMountedFiles)
        {
            if (key.compare(0, withSlash.size(), withSlash) != 0)
                continue;
            if (!recursive && key.find('/', withSlash.size()) != std::string::npos)
                continue;
            callback(file.Path.c_str(), context);
        }
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // Editor.exe gets COM initialized for free via Qt's QApplication; this host has no Qt
    // at all on the standalone path, but Renderer_DX12.dll's shader compiler still falls
    // back to the legacy D3DCompiler for SM5 shaders (see DX12ShaderCompiler.cpp), whose
    // default #include handler (D3D_COMPILE_STANDARD_FILE_INCLUDE) relies on COM being
    // initialized on the calling thread - without it, D3DCompileFromFile hangs rather than
    // failing cleanly. Must happen before RendererDX12_Initialize.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const fs::path exeDirectory = GetExecutableDirectory();

    if (!ReadEmbeddedPackagingKey(gContentKey))
    {
        ReportFatalError(L"This executable was not built by the editor's Release menu "
            L"(no packaging key embedded). Use Release > Build Game in the editor instead "
            L"of running the raw launcher template directly.");
        return 1;
    }

    std::wstring mountError;
    if (!MountContent(exeDirectory, mountError))
    {
        ReportFatalError(L"Could not open game content.\n\n" + mountError);
        return 1;
    }

    // Decrypt the level once up front: a key that does not match the archives would
    // otherwise only surface as a pile of "missing asset" errors deep inside the renderer.
    {
        constexpr const char* kLevel = "Levels/Farkle.json";
        const long long levelSize = PteroData_FileSize(kLevel);
        std::vector<unsigned char> levelBytes(levelSize > 0 ? static_cast<size_t>(levelSize) : 0);
        if (levelSize < 0 || !PteroData_ReadFile(kLevel, levelBytes.data(), levelBytes.size()))
        {
            ReportFatalError(L"The game content is missing or damaged (Content\\Levels.ppak could not be read). "
                L"Rebuild the game with Release > Build Game in the editor.");
            return 1;
        }
    }

    gWindowTitle = ReadGameTitle(exeDirectory);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = hInstance;
    windowClass.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(kGameIconResourceId));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClassName;
    RegisterClassExW(&windowClass);

    HWND hWnd = CreateWindowW(kWindowClassName, gWindowTitle.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd)
        return 1;
    gMainWindowHandle = hWnd;

    // Plain module name: Windows searches this exe's own directory first (see
    // GameHost.cpp's LoadModule for the same reasoning with Game.dll).
    gRendererModule = LoadLibraryW(L"Renderer_DX12.dll");
    if (!gRendererModule)
    {
        ReportFatalError(L"Could not load Renderer_DX12.dll.");
        DestroyWindow(hWnd);
        return 1;
    }

    gRendererInitialize = reinterpret_cast<RendererInitializeFn>(GetProcAddress(gRendererModule, "RendererDX12_Initialize"));
    gRendererRender = reinterpret_cast<RendererRenderFn>(GetProcAddress(gRendererModule, "RendererDX12_Render"));
    gRendererResize = reinterpret_cast<RendererResizeFn>(GetProcAddress(gRendererModule, "RendererDX12_Resize"));
    gRendererHandleWindowMessage = reinterpret_cast<RendererHandleWindowMessageFn>(GetProcAddress(gRendererModule, "RendererDX12_HandleWindowMessage"));
    gRendererShutdown = reinterpret_cast<RendererShutdownFn>(GetProcAddress(gRendererModule, "RendererDX12_Shutdown"));
    gRendererConfirmClose = reinterpret_cast<RendererConfirmCloseFn>(GetProcAddress(gRendererModule, "RendererDX12_ConfirmClose"));
    gRendererGetLastError = reinterpret_cast<RendererGetLastErrorFn>(GetProcAddress(gRendererModule, "RendererDX12_GetLastError"));
    gRendererSetProgressCallback = reinterpret_cast<RendererSetProgressCallbackFn>(GetProcAddress(gRendererModule, "RendererDX12_SetProgressCallback"));
    gRendererSetAudioManager = reinterpret_cast<RendererSetAudioManagerFn>(GetProcAddress(gRendererModule, "RendererDX12_SetAudioManager"));
    gRendererSetLoopTimings = reinterpret_cast<RendererSetLoopTimingsFn>(GetProcAddress(gRendererModule, "RendererDX12_SetLoopTimings"));
    auto configureStandaloneGame = reinterpret_cast<RendererConfigureStandaloneGameFn>(GetProcAddress(gRendererModule, "RendererDX12_ConfigureStandaloneGame"));

    if (!gRendererInitialize || !gRendererRender || !gRendererResize || !gRendererShutdown || !configureStandaloneGame)
    {
        ReportFatalError(L"Renderer_DX12.dll is missing required exports. Rebuild the engine and try again.");
        CleanupRenderer();
        DestroyWindow(hWnd);
        return 1;
    }

    const fs::path levelPath = exeDirectory / L"Data" / L"Levels" / L"Farkle.json";
    configureStandaloneGame(levelPath.c_str());

    if (!gRendererInitialize(hWnd))
    {
        const char* rendererError = gRendererGetLastError ? gRendererGetLastError() : nullptr;
        std::wstring message = L"Failed to initialize the renderer.";
        if (rendererError && rendererError[0])
        {
            message += L"\n\n";
            message += std::wstring(rendererError, rendererError + strlen(rendererError));
        }
        ReportFatalError(message);
        CleanupRenderer();
        DestroyWindow(hWnd);
        return 1;
    }
    gRendererReady = true;

    std::string audioError;
    if (!gAudioManager.Initialize(audioError))
        OutputDebugStringA(("Audio initialization failed: " + audioError + "\n").c_str());
    if (gRendererSetAudioManager)
        gRendererSetAudioManager(&gAudioManager);

    // RendererDX12_Initialize has already put the window into the player's saved display
    // mode (borderless over the monitor by default); maximizing here would undo that.
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    MSG msg{};
    while (!gIsClosing)
    {
        bool processedMessage = false;
        const auto pumpStart = std::chrono::steady_clock::now();
        unsigned messageCount = 0;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            processedMessage = true;
            ++messageCount;
            if (msg.message == WM_QUIT)
            {
                gIsClosing = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        const float pumpMilliseconds =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - pumpStart).count();

        if (gIsClosing)
            break;

        if (gRendererReady && gRendererRender)
        {
            const auto audioStart = std::chrono::steady_clock::now();
            gAudioManager.Update();
            const float audioMilliseconds =
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
            if (gRendererSetLoopTimings)
                gRendererSetLoopTimings(pumpMilliseconds, audioMilliseconds, messageCount, 0);
            gRendererRender();
        }
        else if (!processedMessage)
        {
            WaitMessage();
        }
    }

    // Let the GPU drain the frames still in flight before the process goes. Unlike the
    // editor, whose swap chain lives on a Qt child window, the game's swap chain is bound
    // to the top-level window itself, so the window is deliberately not destroyed here
    // either: tearing it down under a live swap chain (with Streamline/DLSS or FSR frame
    // generation hooked into present) is exactly the kind of exit that crashes inside
    // the driver. Process termination releases both once nothing is running any more.
    using WaitForGpuFn = bool(__stdcall*)();
    if (gRendererModule != nullptr)
    {
        if (auto waitForGpu = reinterpret_cast<WaitForGpuFn>(GetProcAddress(gRendererModule, "DX12Context_WaitForGPU")))
            waitForGpu();
    }

    // Hard process termination, same rationale as EditorMain.cpp: some FMOD plugins raise
    // a fatal-exit exception under ExitProcess(), and renderer/audio teardown can block.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
