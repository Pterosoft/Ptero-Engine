// Ptero-Engine.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "EditorMain.h"
#include "SplashScreen.h"
#include "AudioManager.h"
#include <string>
#include <sstream>
#include <chrono>

static AudioManager gAudioManager;

typedef bool(__stdcall* RendererDX12InitializeFn)(HWND);
typedef bool(__stdcall* RendererDX12RenderFn)();
typedef bool(__stdcall* RendererDX12ResizeFn)(UINT, UINT);
typedef bool(__stdcall* RendererDX12HandleWindowMessageFn)(HWND, UINT, WPARAM, LPARAM);
typedef void(__stdcall* RendererDX12ShutdownFn)();
typedef const char* (__stdcall* RendererDX12GetLastErrorFn)();
typedef void(__stdcall* RendererDX12ProgressFn)(const wchar_t* message);
typedef void(__stdcall* RendererDX12SetProgressCallbackFn)(RendererDX12ProgressFn callback);

typedef void(__stdcall* RendererDX12SetAudioManagerFn)(AudioManager* audioManager);
typedef void(__stdcall* RendererDX12SetLoopTimingsFn)(float pumpMilliseconds, float audioMilliseconds, unsigned messageCount, unsigned paintMessageCount);

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name

HMODULE gRendererModule = nullptr;
RendererDX12InitializeFn gRendererInitialize = nullptr;
RendererDX12RenderFn gRendererRender = nullptr;
RendererDX12ResizeFn gRendererResize = nullptr;
RendererDX12HandleWindowMessageFn gRendererHandleWindowMessage = nullptr;
RendererDX12ShutdownFn gRendererShutdown = nullptr;
RendererDX12GetLastErrorFn gRendererGetLastError = nullptr;
RendererDX12SetProgressCallbackFn gRendererSetProgressCallback = nullptr;
RendererDX12SetAudioManagerFn gRendererSetAudioManager = nullptr;
RendererDX12SetLoopTimingsFn gRendererSetLoopTimings = nullptr;
bool gRendererReady = false;
bool gIsClosing = false;
bool gRendererFailureReported = false;
bool gRendererEnteredRenderLoop = false;
bool gRendererPresentedFirstFrame = false;
HWND gMainWindowHandle = nullptr;

void BeginApplicationShutdown(HWND hWnd)
{
    gIsClosing = true;

    if (hWnd != nullptr)
    {
        ShowWindow(hWnd, SW_HIDE);
        EnableWindow(hWnd, FALSE);
    }
}

void UpdateMainWindowTitle(const wchar_t* statusSuffix);
void ReportRendererFailure(HWND hWnd, const char* fallbackMessage);
void CleanupRenderer();

void PrepareInitialMainWindowFrame(HWND hWnd)
{
    if (!gRendererReady || !gRendererRender || gRendererPresentedFirstFrame)
    {
        return;
    }

    SplashScreen::UpdateStatus(L"Preparing initial frame...");
    UpdateMainWindowTitle(L"[preparing initial frame]");

    // Make the editor window exist behind the splash screen so DXGI can bind the
    // swap chain to the real HWND and the first frame can be presented before the
    // splash closes.
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hWnd);
    RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);

    gRendererEnteredRenderLoop = true;
    gAudioManager.Update();
    if (gRendererRender())
    {
        gRendererPresentedFirstFrame = true;
        UpdateMainWindowTitle(L"[initial frame ready]");
        SplashScreen::UpdateStatus(L"Editor ready.");
        return;
    }

    ReportRendererFailure(hWnd, "Renderer failed while preparing the initial frame.");
    SplashScreen::UpdateStatus(L"Failed to prepare initial frame.");
    UpdateMainWindowTitle(L"[initial frame failed]");
    ShowWindow(hWnd, SW_HIDE);
    CleanupRenderer();
}

void UpdateMainWindowTitle(const wchar_t* statusSuffix)
{
    if (gMainWindowHandle == nullptr)
    {
        return;
    }

    std::wstring titleText = szTitle;
    if (statusSuffix != nullptr && statusSuffix[0] != L'\0')
    {
        titleText += L" ";
        titleText += statusSuffix;
    }

    SetWindowTextW(gMainWindowHandle, titleText.c_str());
}

void ReportRendererFailure(HWND hWnd, const char* fallbackMessage)
{
    const char* rendererMessage = gRendererGetLastError ? gRendererGetLastError() : nullptr;
    const char* messageToShow = (rendererMessage != nullptr && rendererMessage[0] != '\0')
        ? rendererMessage
        : fallbackMessage;

    if (gMainWindowHandle != nullptr)
    {
        SetWindowTextA(gMainWindowHandle, messageToShow);
    }

    MessageBoxA(hWnd, messageToShow, "Renderer Failure", MB_OK | MB_ICONERROR);
}

void ReportRendererFailure(HWND hWnd, const std::string& fallbackMessage)
{
    ReportRendererFailure(hWnd, fallbackMessage.c_str());
}

void CleanupRenderer()
{
    // Tear down the renderer before window destruction completes so the swap chain
    // releases its HWND association while the window is still valid.
    if (gRendererReady && gRendererShutdown)
    {
        gRendererShutdown();
    }

    if (gRendererModule)
    {
        FreeLibrary(gRendererModule);
        gRendererModule = nullptr;
    }

    gRendererInitialize = nullptr;
    gRendererRender = nullptr;
    gRendererResize = nullptr;
    gRendererHandleWindowMessage = nullptr;
    gRendererShutdown = nullptr;
    gRendererGetLastError = nullptr;
    gRendererSetProgressCallback = nullptr;
    gRendererSetAudioManager = nullptr;
    gRendererReady = false;
    gRendererEnteredRenderLoop = false;
    gRendererPresentedFirstFrame = false;
}

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Show the splash screen before doing any heavyweight initialization.
    SplashScreen::Show(hInstance);
    SplashScreen::UpdateStatus(L"Initializing editor...");

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_PTEROENGINE, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        SplashScreen::Close();
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_PTEROENGINE));

    MSG msg{};

    // Drive rendering from the main loop instead of WM_PAINT so the DX12 swap chain
    // keeps presenting frames even when Windows is not issuing paint messages.
    while (!gIsClosing)
    {
        bool processedMessage = false;
        // Qt's child windows service their paint messages inside DispatchMessage, so this
        // pump - not the renderer - is where editor UI repaints are actually paid for.
        const auto pumpStart = std::chrono::steady_clock::now();
        unsigned messageCount = 0;
        unsigned paintMessageCount = 0;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            processedMessage = true;
            ++messageCount;
            // WM_PAINT means Qt is actually redrawing a window; anything else arriving in
            // bulk (mouse moves, timers, DWM traffic) points somewhere entirely different.
            if (msg.message == WM_PAINT)
            {
                ++paintMessageCount;
            }

            if (msg.message == WM_QUIT)
            {
                gIsClosing = true;
                break;
            }

            if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        const float pumpMilliseconds =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - pumpStart).count();

        if (gIsClosing)
        {
            break;
        }

        if (gRendererReady && gRendererRender)
        {
            const auto audioStart = std::chrono::steady_clock::now();
            gAudioManager.Update();
            const float audioMilliseconds =
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - audioStart).count();
            if (gRendererSetLoopTimings)
            {
                gRendererSetLoopTimings(pumpMilliseconds, audioMilliseconds, messageCount, paintMessageCount);
            }
            gRendererRender();
        }
        else if (!processedMessage)
        {
            WaitMessage();
        }
    }

    if (gMainWindowHandle != nullptr && IsWindow(gMainWindowHandle))
    {
        DestroyWindow(gMainWindowHandle);
        gMainWindowHandle = nullptr;
    }

    if (msg.message != WM_QUIT)
    {
        msg.wParam = 0;
    }

    // Use a hard process termination for application shutdown. Some third-party
    // audio plugins raise a fatal-exit exception under the debugger during
    // ExitProcess(), and renderer/audio teardown has proven to block for too long.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(msg.wParam));
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PTEROENGINE));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    // Leave background painting to the renderer so GDI doesn't keep restoring a white client area.
    wcex.hbrBackground  = nullptr;
    wcex.lpszMenuName   = 0;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   // WS_CLIPCHILDREN keeps the host from painting over the embedded Qt shell and the
   // DX12 surface, so background fills below can never flicker across the editor UI.
   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   gMainWindowHandle = hWnd;

   ShowWindow(hWnd, SW_HIDE); // Hidden until splash closes
   UpdateMainWindowTitle(L"[loading renderer dll]");

   SplashScreen::UpdateStatus(L"Loading renderer DLL...");

   // Dynamically load the renderer DLL so the editor can run even if the renderer is missing.
   gRendererModule = LoadLibraryW(L"Renderer_DX12.dll");
    if (!gRendererModule)
    {
        std::ostringstream errorBuilder;
        errorBuilder << "LoadLibraryW(\"Renderer_DX12.dll\") failed. Win32 error: " << GetLastError();
        ReportRendererFailure(hWnd, errorBuilder.str());
        SplashScreen::UpdateStatus(L"Failed to load renderer DLL.");
        UpdateMainWindowTitle(L"[renderer dll load failed]");
    }
    else
   {
        UpdateMainWindowTitle(L"[renderer dll loaded]");
        SplashScreen::UpdateStatus(L"Renderer DLL loaded. Resolving entry points...");

       // Resolve exported renderer entry points.
       gRendererInitialize = reinterpret_cast<RendererDX12InitializeFn>(GetProcAddress(gRendererModule, "RendererDX12_Initialize"));
        gRendererRender = reinterpret_cast<RendererDX12RenderFn>(GetProcAddress(gRendererModule, "RendererDX12_Render"));
        gRendererResize = reinterpret_cast<RendererDX12ResizeFn>(GetProcAddress(gRendererModule, "RendererDX12_Resize"));
        gRendererHandleWindowMessage = reinterpret_cast<RendererDX12HandleWindowMessageFn>(GetProcAddress(gRendererModule, "RendererDX12_HandleWindowMessage"));
       gRendererShutdown = reinterpret_cast<RendererDX12ShutdownFn>(GetProcAddress(gRendererModule, "RendererDX12_Shutdown"));
        gRendererGetLastError = reinterpret_cast<RendererDX12GetLastErrorFn>(GetProcAddress(gRendererModule, "RendererDX12_GetLastError"));
        gRendererSetProgressCallback = reinterpret_cast<RendererDX12SetProgressCallbackFn>(GetProcAddress(gRendererModule, "RendererDX12_SetProgressCallback"));
        gRendererSetAudioManager = reinterpret_cast<RendererDX12SetAudioManagerFn>(GetProcAddress(gRendererModule, "RendererDX12_SetAudioManager"));
        gRendererSetLoopTimings = reinterpret_cast<RendererDX12SetLoopTimingsFn>(GetProcAddress(gRendererModule, "RendererDX12_SetLoopTimings"));

        if (!(gRendererInitialize && gRendererRender && gRendererResize && gRendererShutdown))
        {
            std::ostringstream errorBuilder;
            errorBuilder
                << "Renderer_DX12.dll is missing required exports. "
                << "Initialize=" << (gRendererInitialize != nullptr)
                << ", Render=" << (gRendererRender != nullptr)
                << ", Resize=" << (gRendererResize != nullptr)
                << ", Shutdown=" << (gRendererShutdown != nullptr)
                << ", GetLastError=" << (gRendererGetLastError != nullptr);

            ReportRendererFailure(hWnd, errorBuilder.str());
            SplashScreen::UpdateStatus(L"Renderer DLL is missing required exports.");
            UpdateMainWindowTitle(L"[renderer exports missing]");
            CleanupRenderer();
        }
        else
        {
            UpdateMainWindowTitle(L"[renderer exports resolved]");
            SplashScreen::UpdateStatus(L"Initializing DirectX 12...");

            // Register progress callback so the renderer can report fine-grained steps.
            if (gRendererSetProgressCallback)
            {
                gRendererSetProgressCallback(SplashScreen::UpdateStatus);
            }

            // Initialize DX12 against the editor's main HWND.
            if (!gRendererInitialize(hWnd))
            {
                ReportRendererFailure(hWnd, "Renderer_DX12.dll loaded but failed to initialize. The editor will continue without the DX12 renderer.");
                SplashScreen::UpdateStatus(L"DirectX 12 initialization failed.");
                UpdateMainWindowTitle(L"[renderer initialization failed]");
                CleanupRenderer();
            }
            else
            {
                gRendererReady = true;
                UpdateMainWindowTitle(L"[DX12 initialized]");
                SplashScreen::UpdateStatus(L"Initializing audio...");

                std::string audioError;
                if (!gAudioManager.Initialize(audioError))
                {
                    // Audio failure is non-fatal; log it but continue.
                    OutputDebugStringA(("Audio initialization failed: " + audioError + "\n").c_str());
                }

                if (gRendererSetAudioManager)
                    gRendererSetAudioManager(&gAudioManager);

                SplashScreen::UpdateStatus(L"Editor ready.");
            }
       }
   }

   PrepareInitialMainWindowFrame(hWnd);

   // Close the splash screen and show the main window now that startup is complete.
   SplashScreen::Close();
   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   // Bring the main window to the foreground after the splash screen closes
   SetForegroundWindow(hWnd);
   SetFocus(hWnd);
   BringWindowToTop(hWnd);

   // Force the first WM_PAINT only after renderer startup finishes so the
   // initial frame comes from DX12/ImGui instead of the default white brush.
   RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);

   return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_CLOSE)
        {
            BeginApplicationShutdown(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        // Hide the window immediately so shutdown work does not appear as a frozen app.
        BeginApplicationShutdown(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    if (gRendererReady && gRendererHandleWindowMessage && gRendererHandleWindowMessage(hWnd, message, wParam, lParam))
    {
        return 1;
    }

    switch (message)
    {
    case WM_ERASEBKGND:
        // Once DX12 owns presentation, suppress the default white background erase
        // so the swap chain output is not replaced by GDI between frames.
        if (gRendererReady)
        {
            return 1;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // Parse the menu selections:
            switch (wmId)
            {
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                SendMessage(hWnd, WM_CLOSE, 0, 0);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_SIZE:
        if (gRendererReady && gRendererResize && wParam != SIZE_MINIMIZED)
        {
            // Keep the DX12 swap chain and the off-screen scene target aligned with
            // the current editor client size so the viewport renders at window resolution.
            gRendererResize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);

            // Always fill: WS_CLIPCHILDREN excludes the Qt shell and the DX12 surface, so
            // this only covers client area no child window occupies (briefly, while a
            // resize is in flight). Without it those pixels keep whatever GDI left behind.
            static HBRUSH backgroundBrush = CreateSolidBrush(RGB(0x19, 0x1a, 0x1c));
            FillRect(hdc, &ps.rcPaint, backgroundBrush);

            EndPaint(hWnd, &ps);
        }
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
