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
    constexpr int   kSplashW     = 700;
    constexpr int   kSplashH     = 420;
    constexpr int   kStatusBarH  = 52;   // height reserved below the image for status text
    constexpr COLORREF kBgColor  = RGB(18, 18, 22);
    constexpr COLORREF kTextColor = RGB(220, 210, 200);
    constexpr wchar_t kClassName[] = L"PteroSplashWnd";

    HWND              g_Hwnd       = nullptr;
    HBITMAP           g_Bitmap     = nullptr;   // splash image converted to a GDI DIB
    int               g_ImgW       = 0;
    int               g_ImgH       = 0;
    std::atomic<bool> g_Running    { false };
    std::thread       g_Thread;
    std::mutex        g_StatusMutex;
    std::wstring      g_StatusText;
    HINSTANCE         g_hInstance  = nullptr;

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

    // -----------------------------------------------------------------------
    // WM_PAINT handler – draws the logo bitmap and status text.
    // -----------------------------------------------------------------------
    void OnPaint(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        // -- background fill --
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        HBRUSH bgBrush = CreateSolidBrush(kBgColor);
        FillRect(hdc, &clientRect, bgBrush);
        DeleteObject(bgBrush);

        // -- splash image (stretched to fill the image area above the status bar) --
        if (g_Bitmap && g_ImgW > 0 && g_ImgH > 0)
        {
            const int imgAreaH = clientRect.bottom - kStatusBarH;
            HDC memDC = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(memDC, g_Bitmap);
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, 0, 0, clientRect.right, imgAreaH,
                       memDC, 0, 0, g_ImgW, g_ImgH, SRCCOPY);
            SelectObject(memDC, old);
            DeleteDC(memDC);
        }

        // -- status bar area (slightly lighter background) --
        RECT statusRect = { 0, clientRect.bottom - kStatusBarH, clientRect.right, clientRect.bottom };
        HBRUSH statusBrush = CreateSolidBrush(RGB(12, 12, 16));
        FillRect(hdc, &statusRect, statusBrush);
        DeleteObject(statusBrush);

        // -- thin separator line --
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 40, 20));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, 0, statusRect.top, nullptr);
        LineTo(hdc, clientRect.right, statusRect.top);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);

        // -- status text --
        std::wstring statusCopy;
        {
            std::lock_guard<std::mutex> lock(g_StatusMutex);
            statusCopy = g_StatusText;
        }
        if (!statusCopy.empty())
        {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, kTextColor);

            // Use the default system font, slightly larger via a log-font.
            LOGFONTW lf{};
            lf.lfHeight         = 16;
            lf.lfWeight         = FW_NORMAL;
            lf.lfCharSet        = DEFAULT_CHARSET;
            lf.lfOutPrecision   = OUT_DEFAULT_PRECIS;
            lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
            lf.lfQuality        = CLEARTYPE_QUALITY;
            lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
            wcscpy_s(lf.lfFaceName, L"Segoe UI");
            HFONT font = CreateFontIndirectW(&lf);
            HGDIOBJ oldFont = SelectObject(hdc, font);

            // Left-pad the text and vertically center it in the status bar.
            RECT textRect = { 14, statusRect.top + 4, statusRect.right - 8, statusRect.bottom - 4 };
            DrawTextW(hdc, statusCopy.c_str(), -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(hdc, oldFont);
            DeleteObject(font);
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

        // Load the bitmap once so we have the dimensions before creating the window.
        g_Bitmap = LoadSplashBitmap(g_ImgW, g_ImgH);

        // Decide window size: use native image aspect ratio if available,
        // otherwise fall back to the fixed size constants.
        int wndW = kSplashW;
        int wndH = kSplashH;
        if (g_ImgW > 0 && g_ImgH > 0)
        {
            // Keep the splash width fixed, derive height from the image aspect ratio
            // and add room for the status bar below.
            const float aspect = static_cast<float>(g_ImgH) / static_cast<float>(g_ImgW);
            wndH = static_cast<int>(kSplashW * aspect) + kStatusBarH;
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

        // Center the splash on the primary monitor.
        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        const int posX    = (screenW - wndW) / 2;
        const int posY    = (screenH - wndH) / 2;

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

        if (g_Bitmap)
        {
            DeleteObject(g_Bitmap);
            g_Bitmap = nullptr;
        }
        g_Hwnd    = nullptr;
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

        // Request a repaint on the splash window from whatever thread calls this.
        if (g_Hwnd)
            InvalidateRect(g_Hwnd, nullptr, FALSE);
    }

    void Close()
    {
        if (g_Hwnd)
        {
            PostMessageW(g_Hwnd, WM_CLOSE, 0, 0);
        }

        if (g_Thread.joinable())
            g_Thread.join();

        UnregisterClassW(kClassName, g_hInstance);
    }
}
