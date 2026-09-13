#include "framework.h"
#include "SplashScreen.h"

#include <wincodec.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>

#pragma comment(lib, "windowscodecs.lib")

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
namespace
{
    // Authored at 96 DPI and scaled to the monitor the splash lands on, so it keeps its
    // intended physical size instead of shrinking to 4/5 of it on a 125% display.
    constexpr int   kSplashBaseW    = 700;
    constexpr int   kSplashBaseH    = 420;
    constexpr int   kStatusBarBaseH = 52;   // height reserved below the image for status text
    constexpr COLORREF kBgColor  = RGB(18, 18, 22);
    constexpr COLORREF kTextColor = RGB(220, 210, 200);
    constexpr wchar_t kClassName[] = L"PteroSplashWnd";

    HWND              g_Hwnd       = nullptr;
    HBITMAP           g_Bitmap     = nullptr;   // splash image converted to a GDI DIB
    HBITMAP           g_BufferBmp  = nullptr;   // off-screen double-buffer
    HDC               g_BufferDC   = nullptr;   // off-screen buffer DC
    int               g_ImgW       = 0;
    int               g_ImgH       = 0;
    int               g_WndW       = 0;
    int               g_WndH       = 0;
    std::atomic<bool> g_Running    { false };
    std::thread       g_Thread;
    std::mutex        g_StatusMutex;
    std::wstring      g_StatusText;
    HINSTANCE         g_hInstance  = nullptr;
    UINT              g_Dpi        = USER_DEFAULT_SCREEN_DPI;
    int               g_StatusBarH = kStatusBarBaseH;

    // Converts a metric authored at 96 DPI to the splash monitor's pixels.
    int Scaled(int value)
    {
        return MulDiv(value, static_cast<int>(g_Dpi), USER_DEFAULT_SCREEN_DPI);
    }

    // -----------------------------------------------------------------------
    // Win32 reports desktop metrics in whichever coordinate space the calling thread's
    // DPI awareness implies, while CreateWindowEx always consumes physical pixels here.
    // The editor ships no dpiAware manifest entry, so an unaware thread sees the desktop
    // scaled down - 1536x864 on a 1920x1080 display at 125% - and centring against that
    // lands the window up and to the left. Opting this thread in makes both sides agree.
    // The awareness context is per-thread, so the splash thread doing this cannot disturb
    // the Qt setup that runs on the main thread later.
    // -----------------------------------------------------------------------
    void MakeThreadDpiAware()
    {
        using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);

        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr)
            return;

        auto setContext = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
        if (setContext != nullptr)
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    // Effective DPI of the given monitor, or 96 when the OS is too old to say.
    UINT MonitorDpi(HMONITOR monitor)
    {
        // MDT_EFFECTIVE_DPI; typed loosely so this file does not need shellscalingapi.h
        // or a link-time dependency on shcore.
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

        UINT dpi = USER_DEFAULT_SCREEN_DPI;
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
        {
            auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(
                reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
            if (getDpi != nullptr)
            {
                UINT dpiX = 0, dpiY = 0;
                if (SUCCEEDED(getDpi(monitor, 0, &dpiX, &dpiY)) && dpiX != 0)
                    dpi = dpiX;
            }
            FreeLibrary(shcore);
        }

        return dpi;
    }

    // -----------------------------------------------------------------------
    // Locate the splash image by walking upward from the executable directory.
    // -----------------------------------------------------------------------
    std::filesystem::path FindSplashImage()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return {};

        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
        while (!dir.empty())
        {
            std::filesystem::path candidate = dir / L"Data" / L"Media" / L"ptero-engine_wide.png";
            if (std::filesystem::exists(candidate))
                return candidate;

            std::filesystem::path parent = dir.parent_path();
            if (parent == dir)
                break;
            dir = parent;
        }
        return {};
    }

    // -----------------------------------------------------------------------
    // Load the PNG via WIC and convert it to a GDI HBITMAP (BGRA, top-down).
    // -----------------------------------------------------------------------
    HBITMAP LoadSplashBitmap(int& outW, int& outH)
    {
        const std::filesystem::path imagePath = FindSplashImage();
        if (imagePath.empty())
            return nullptr;

        IWICImagingFactory* factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWICImagingFactory, reinterpret_cast<void**>(&factory))))
            return nullptr;

        IWICBitmapDecoder* decoder = nullptr;
        HBITMAP result = nullptr;

        if (SUCCEEDED(factory->CreateDecoderFromFilename(imagePath.c_str(), nullptr,
                                                          GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                          &decoder)))
        {
            IWICBitmapFrameDecode* frame = nullptr;
            if (SUCCEEDED(decoder->GetFrame(0, &frame)))
            {
                IWICFormatConverter* converter = nullptr;
                if (SUCCEEDED(factory->CreateFormatConverter(&converter)))
                {
                    if (SUCCEEDED(converter->Initialize(frame,
                                                        GUID_WICPixelFormat32bppBGRA,
                                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                                        WICBitmapPaletteTypeCustom)))
                    {
                        UINT w = 0, h = 0;
                        converter->GetSize(&w, &h);

                        // Create a device-independent DIB so we can paint it without a DC.
                        BITMAPINFO bmi{};
                        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                        bmi.bmiHeader.biWidth       = static_cast<LONG>(w);
                        bmi.bmiHeader.biHeight      = -static_cast<LONG>(h); // top-down
                        bmi.bmiHeader.biPlanes      = 1;
                        bmi.bmiHeader.biBitCount    = 32;
                        bmi.bmiHeader.biCompression = BI_RGB;

                        void* bits = nullptr;
                        HDC screenDC = GetDC(nullptr);
                        result = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
                        ReleaseDC(nullptr, screenDC);

                        if (result && bits)
                        {
                            const UINT stride = w * 4;
                            const UINT bufferSize = stride * h;
                            converter->CopyPixels(nullptr, stride, bufferSize,
                                                  static_cast<BYTE*>(bits));
                            outW = static_cast<int>(w);
                            outH = static_cast<int>(h);
                        }
                    }
                    converter->Release();
                }
                frame->Release();
            }
            decoder->Release();
        }
        factory->Release();
        return result;
    }

    // ---------------------------------------------------------------------------
    // Create or resize the off-screen double-buffer.
    // ---------------------------------------------------------------------------
    void EnsureBuffer(int w, int h)
    {
        if (g_BufferDC && g_WndW == w && g_WndH == h)
            return;

        if (g_BufferDC)
        {
            DeleteDC(g_BufferDC);
            g_BufferDC = nullptr;
        }
        if (g_BufferBmp)
        {
            DeleteObject(g_BufferBmp);
            g_BufferBmp = nullptr;
        }

        HDC screenDC = GetDC(nullptr);
        g_BufferDC = CreateCompatibleDC(screenDC);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        g_BufferBmp = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screenDC);

        if (g_BufferBmp)
            SelectObject(g_BufferDC, g_BufferBmp);

        g_WndW = w;
        g_WndH = h;
    }

    // ---------------------------------------------------------------------------
    // Build the static part of the buffer (background + image).
    // ---------------------------------------------------------------------------
    void BuildStaticBuffer()
    {
        if (!g_BufferDC || g_WndW == 0 || g_WndH == 0)
            return;

        RECT clientRect = { 0, 0, g_WndW, g_WndH };

        // -- background fill --
        HBRUSH bgBrush = CreateSolidBrush(kBgColor);
        FillRect(g_BufferDC, &clientRect, bgBrush);
        DeleteObject(bgBrush);

        // -- splash image (stretched to fill the image area above the status bar) --
        if (g_Bitmap && g_ImgW > 0 && g_ImgH > 0)
        {
            const int imgAreaH = g_WndH - g_StatusBarH;
            HDC memDC = CreateCompatibleDC(nullptr);
            HGDIOBJ old = SelectObject(memDC, g_Bitmap);
            SetStretchBltMode(g_BufferDC, HALFTONE);
            StretchBlt(g_BufferDC, 0, 0, g_WndW, imgAreaH,
                        memDC, 0, 0, g_ImgW, g_ImgH, SRCCOPY);
            SelectObject(memDC, old);
            DeleteDC(memDC);
        }

        // -- status bar background --
        RECT statusRect = { 0, g_WndH - g_StatusBarH, g_WndW, g_WndH };
        HBRUSH statusBrush = CreateSolidBrush(RGB(12, 12, 16));
        FillRect(g_BufferDC, &statusRect, statusBrush);
        DeleteObject(statusBrush);

        // -- thin separator line --
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 40, 20));
        HGDIOBJ oldPen = SelectObject(g_BufferDC, pen);
        MoveToEx(g_BufferDC, 0, statusRect.top, nullptr);
        LineTo(g_BufferDC, g_WndW, statusRect.top);
        SelectObject(g_BufferDC, oldPen);
        DeleteObject(pen);
    }

    // ---------------------------------------------------------------------------
    // Update only the status text area in the buffer.
    // ---------------------------------------------------------------------------
    void UpdateStatusTextInBuffer()
    {
        if (!g_BufferDC || g_WndW == 0 || g_WndH == 0)
            return;

        RECT statusRect = { 0, g_WndH - g_StatusBarH, g_WndW, g_WndH };

        // Clear the text area first (fill with status bar color)
        HBRUSH statusBrush = CreateSolidBrush(RGB(12, 12, 16));
        FillRect(g_BufferDC, &statusRect, statusBrush);
        DeleteObject(statusBrush);

        // Redraw separator line
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 40, 20));
        HGDIOBJ oldPen = SelectObject(g_BufferDC, pen);
        MoveToEx(g_BufferDC, 0, statusRect.top, nullptr);
        LineTo(g_BufferDC, g_WndW, statusRect.top);
        SelectObject(g_BufferDC, oldPen);
        DeleteObject(pen);

        // Draw the text
        std::wstring statusCopy;
        {
            std::lock_guard<std::mutex> lock(g_StatusMutex);
            statusCopy = g_StatusText;
        }

        SetBkMode(g_BufferDC, TRANSPARENT);
        SetTextColor(g_BufferDC, kTextColor);

        LOGFONTW lf{};
        lf.lfHeight         = Scaled(16);
        lf.lfWeight         = FW_NORMAL;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfOutPrecision   = OUT_DEFAULT_PRECIS;
        lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
        lf.lfQuality        = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT font = CreateFontIndirectW(&lf);
        HGDIOBJ oldFont = SelectObject(g_BufferDC, font);

        RECT textRect = { Scaled(14), statusRect.top + Scaled(4),
                          statusRect.right - Scaled(8), statusRect.bottom - Scaled(4) };
        DrawTextW(g_BufferDC, statusCopy.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(g_BufferDC, oldFont);
        DeleteObject(font);
    }

    // ---------------------------------------------------------------------------
    // WM_PAINT handler – blit the off-screen buffer to screen.
    // ---------------------------------------------------------------------------
    void OnPaint(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (g_BufferDC && g_WndW > 0 && g_WndH > 0)
        {
            BitBlt(hdc, 0, 0, g_WndW, g_WndH, g_BufferDC, 0, 0, SRCCOPY);
        }

        EndPaint(hwnd, &ps);
    }

    // -----------------------------------------------------------------------
    // Window procedure for the splash window.
    // -----------------------------------------------------------------------
    LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_PAINT:
            OnPaint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1; // handled in WM_PAINT
        case WM_USER + 1: // UpdateStatus request from other threads
            UpdateStatusTextInBuffer();
            if (g_WndW > 0 && g_WndH > 0)
            {
                RECT statusRect = { 0, g_WndH - g_StatusBarH, g_WndW, g_WndH };
                InvalidateRect(hwnd, &statusRect, FALSE);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    // -----------------------------------------------------------------------
    // Thread entry: creates the splash window and runs its message loop.
    // -----------------------------------------------------------------------
    void SplashThreadProc()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        MakeThreadDpiAware();

        // Load the bitmap once so we have the dimensions before creating the window.
        g_Bitmap = LoadSplashBitmap(g_ImgW, g_ImgH);

        // The splash always goes on the primary monitor, so its DPI decides the size and
        // its work area decides the position. GetMonitorInfo reports the same coordinate
        // space CreateWindowEx consumes, which GetSystemMetrics does not guarantee.
        const POINT origin{ 0, 0 };
        HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        g_Dpi = MonitorDpi(monitor);
        g_StatusBarH = Scaled(kStatusBarBaseH);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            monitorInfo.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        }

        // Decide window size: use native image aspect ratio if available,
        // otherwise fall back to the fixed size constants.
        int wndW = Scaled(kSplashBaseW);
        int wndH = Scaled(kSplashBaseH);
        if (g_ImgW > 0 && g_ImgH > 0)
        {
            // Keep the splash width fixed, derive height from the image aspect ratio
            // and add room for the status bar below.
            const float aspect = static_cast<float>(g_ImgH) / static_cast<float>(g_ImgW);
            wndH = static_cast<int>(wndW * aspect) + g_StatusBarH;
        }

        // Register the window class.
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = SplashWndProc;
        wc.hInstance     = g_hInstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        // Centre on the work area rather than the whole monitor so the splash is not
        // pushed under the taskbar, and offset by the work area's own origin so a
        // secondary monitor sitting left of the primary cannot skew it.
        const int workW = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        const int workH = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
        const int posX  = monitorInfo.rcWork.left + (workW - wndW) / 2;
        const int posY  = monitorInfo.rcWork.top + (workH - wndH) / 2;

        // WS_POPUP + no caption = borderless; WS_EX_TOPMOST keeps it in front.
        g_Hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED,
            kClassName, L"Ptero-Engine",
            WS_POPUP,
            posX, posY, wndW, wndH,
            nullptr, nullptr, g_hInstance, nullptr);

        if (g_Hwnd)
        {
            // Fully opaque but with alpha channel support (needed for WS_EX_LAYERED).
            SetLayeredWindowAttributes(g_Hwnd, 0, 255, LWA_ALPHA);

            // Create the off-screen double-buffer.
            EnsureBuffer(wndW, wndH);
            BuildStaticBuffer();

            ShowWindow(g_Hwnd, SW_SHOW);
            UpdateWindow(g_Hwnd);
        }

        g_Running = true;

        // Run the message loop until WM_QUIT.
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Clean up buffer resources.
        if (g_BufferDC)
        {
            DeleteDC(g_BufferDC);
            g_BufferDC = nullptr;
        }
        if (g_BufferBmp)
        {
            DeleteObject(g_BufferBmp);
            g_BufferBmp = nullptr;
        }

        if (g_Bitmap)
        {
            DeleteObject(g_Bitmap);
            g_Bitmap = nullptr;
        }
        g_Hwnd    = nullptr;
        g_WndW    = 0;
        g_WndH    = 0;
        g_Running = false;

        CoUninitialize();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace SplashScreen
{
    void Show(HINSTANCE hInstance)
    {
        g_hInstance = hInstance;
        g_Running   = false;
        g_Thread    = std::thread(SplashThreadProc);

        // Wait briefly until the window is actually created before returning so
        // the caller can immediately call UpdateStatus().
        for (int i = 0; i < 200 && !g_Running; ++i)
            Sleep(10);
    }

    void UpdateStatus(const wchar_t* message)
    {
        {
            std::lock_guard<std::mutex> lock(g_StatusMutex);
            g_StatusText = message ? message : L"";
        }

        // Post a message to the splash window so the splash thread handles
        // the buffer update. This is thread-safe since only the splash thread
        // accesses g_BufferDC.
        if (g_Hwnd)
            PostMessageW(g_Hwnd, WM_USER + 1, 0, 0);
    }

    void Close()
    {
        if (g_Hwnd)
        {
            PostMessageW(g_Hwnd, WM_CLOSE, 0, 0);
        }

        if (g_Thread.joinable())
            g_Thread.join();

        // Clean up the off-screen buffer.
        if (g_BufferDC)
        {
            DeleteDC(g_BufferDC);
            g_BufferDC = nullptr;
        }
        if (g_BufferBmp)
        {
            DeleteObject(g_BufferBmp);
            g_BufferBmp = nullptr;
        }

        UnregisterClassW(kClassName, g_hInstance);
    }
}
