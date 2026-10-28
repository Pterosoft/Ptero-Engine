#include "GameSplash.h"

#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <thread>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    // Must match ReleaseGame.cpp's kSplashImageResourceId.
    constexpr int kSplashImageResourceId = 300;
    // Never more of the screen than this, whatever size the image was authored at.
    constexpr double kMaxScreenShare = 0.7;
    constexpr wchar_t kClassName[] = L"PteroGameSplashWindow";

    HINSTANCE gInstance = nullptr;
    std::thread gThread;
    std::atomic<HWND> gWindow{ nullptr };
    // Set by the splash thread once it has either shown the window or given up.
    std::atomic<bool> gStarted{ false };

    // The PNG the editor embedded, or {nullptr, 0} when there is none.
    std::pair<const BYTE*, DWORD> FindSplashResource()
    {
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCE(kSplashImageResourceId), RT_RCDATA);
        HGLOBAL data = resource ? LoadResource(nullptr, resource) : nullptr;
        const BYTE* bytes = data ? static_cast<const BYTE*>(LockResource(data)) : nullptr;
        const DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
        return { bytes, bytes ? size : 0 };
    }

    // Per-thread, like the editor's splash (see SplashScreen.cpp): without it an unaware
    // thread sees a scaled-down desktop and centres the window in the wrong place.
    void MakeThreadDpiAware()
    {
        using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        {
            auto setContext = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
            if (setContext != nullptr)
                setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    UINT MonitorDpi(HMONITOR monitor)
    {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        UINT dpi = USER_DEFAULT_SCREEN_DPI;
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
        {
            auto getDpi = reinterpret_cast<GetDpiForMonitorFn>(
                reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")));
            UINT dpiX = 0, dpiY = 0;
            if (getDpi != nullptr && SUCCEEDED(getDpi(monitor, 0, &dpiX, &dpiY)) && dpiX != 0)
                dpi = dpiX;
            FreeLibrary(shcore);
        }
        return dpi;
    }

    // Decodes the PNG and scales it to the screen as a premultiplied BGRA DIB, which is
    // what UpdateLayeredWindow blends with. Null on any failure.
    HBITMAP DecodeSplash(const BYTE* bytes, DWORD size, const RECT& workArea, UINT dpi, int& outWidth, int& outHeight)
    {
        IWICImagingFactory* factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_IWICImagingFactory, reinterpret_cast<void**>(&factory))))
            return nullptr;

        HBITMAP bitmap = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* converter = nullptr;

        UINT imageWidth = 0, imageHeight = 0;
        if (SUCCEEDED(factory->CreateStream(&stream))
            && SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), size))
            && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
            && SUCCEEDED(decoder->GetFrame(0, &frame))
            && SUCCEEDED(frame->GetSize(&imageWidth, &imageHeight))
            && imageWidth > 0 && imageHeight > 0)
        {
            // Authored for a 100% display: grow with the monitor's scaling, but never past
            // kMaxScreenShare of the work area.
            const double workWidth = static_cast<double>(workArea.right - workArea.left);
            const double workHeight = static_cast<double>(workArea.bottom - workArea.top);
            const double scale = (std::min)({ static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI,
                kMaxScreenShare * workWidth / imageWidth, kMaxScreenShare * workHeight / imageHeight });
            const UINT width = (std::max)(1u, static_cast<UINT>(imageWidth * scale + 0.5));
            const UINT height = (std::max)(1u, static_cast<UINT>(imageHeight * scale + 0.5));

            if (SUCCEEDED(factory->CreateBitmapScaler(&scaler))
                && SUCCEEDED(scaler->Initialize(frame, width, height, WICBitmapInterpolationModeHighQualityCubic))
                && SUCCEEDED(factory->CreateFormatConverter(&converter))
                && SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
            {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = static_cast<LONG>(width);
                info.bmiHeader.biHeight = -static_cast<LONG>(height);   // top-down
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bitmap != nullptr && (bits == nullptr
                    || FAILED(converter->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(bits)))))
                {
                    DeleteObject(bitmap);
                    bitmap = nullptr;
                }
                outWidth = static_cast<int>(width);
                outHeight = static_cast<int>(height);
            }
        }

        if (converter) converter->Release();
        if (scaler) scaler->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        factory->Release();
        return bitmap;
    }

    LRESULT CALLBACK SplashWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_NCHITTEST:
            // Clicks pass straight through to whatever is behind; nothing here is interactive.
            return HTTRANSPARENT;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    void SplashThread(const BYTE* bytes, DWORD size)
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        MakeThreadDpiAware();

        const POINT origin{ 0, 0 };
        HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
            monitorInfo.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };

        int width = 0, height = 0;
        HBITMAP bitmap = DecodeSplash(bytes, size, monitorInfo.rcWork, MonitorDpi(monitor), width, height);
        if (bitmap == nullptr)
        {
            OutputDebugStringA("[GameLauncher] The embedded splash image could not be decoded; starting without it.\n");
            gStarted = true;
            CoUninitialize();
            return;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = SplashWndProc;
        windowClass.hInstance = gInstance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_APPSTARTING);
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass);

        const RECT& work = monitorInfo.rcWork;
        POINT position{ work.left + ((work.right - work.left) - width) / 2, work.top + ((work.bottom - work.top) - height) / 2 };
        // Tool window: no taskbar button of its own; the game's window gets that.
        HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"",
            WS_POPUP, position.x, position.y, width, height, nullptr, nullptr, gInstance, nullptr);
        if (window != nullptr)
        {
            HDC screen = GetDC(nullptr);
            HDC memory = CreateCompatibleDC(screen);
            HGDIOBJ previous = SelectObject(memory, bitmap);
            SIZE windowSize{ width, height };
            POINT source{ 0, 0 };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(window, screen, &position, &windowSize, memory, &source, 0, &blend, ULW_ALPHA);
            SelectObject(memory, previous);
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);

            ShowWindow(window, SW_SHOWNOACTIVATE);
            gWindow = window;
        }
        gStarted = true;

        MSG message{};
        while (window != nullptr && GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        gWindow = nullptr;
        DeleteObject(bitmap);
        UnregisterClassW(kClassName, gInstance);
        CoUninitialize();
    }
}

namespace GameSplash
{
    bool Show(HINSTANCE instance)
    {
        if (gThread.joinable())
            return true;
        const auto [bytes, size] = FindSplashResource();
        if (bytes == nullptr || size == 0)
            return false;

        gInstance = instance;
        gStarted = false;
        gThread = std::thread(SplashThread, bytes, size);
        // Up before the main thread starts the heavy work, so it is seen at once. Bounded,
        // so a splash that cannot start never delays the game.
        for (int i = 0; i < 100 && !gStarted; ++i)
            Sleep(10);
        return gWindow.load() != nullptr;
    }

    void Close()
    {
        if (!gThread.joinable())
            return;
        // The thread may still be decoding; wait for it to get as far as a window or none.
        for (int i = 0; i < 300 && !gStarted; ++i)
            Sleep(10);
        if (HWND window = gWindow.load())
            PostMessageW(window, WM_CLOSE, 0, 0);
        gThread.join();
    }
}
