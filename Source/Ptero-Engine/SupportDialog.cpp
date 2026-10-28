#include "framework.h"
#include "SupportDialog.h"

#include <windowsx.h>
#include <wincodec.h>
#include <shellapi.h>
#include <filesystem>
#include <string>

#pragma comment(lib, "windowscodecs.lib")

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
namespace
{
    constexpr wchar_t kClassName[]  = L"PteroSupportWnd";
    constexpr wchar_t kPatreonUrl[] = L"https://www.patreon.com/cw/pterosoftstudio";

    // How long Continue stays locked after the dialog opens. Opening the Patreon page
    // unlocks it immediately - nobody should have to wait after they already clicked.
    constexpr ULONGLONG kLockMilliseconds = 5000;
    constexpr UINT_PTR  kTickTimer        = 1;

    // Layout, authored at 96 DPI like the splash screen.
    constexpr int kBaseW    = 800;
    constexpr int kBaseH    = 420;
    constexpr int kArtW     = 300;   // orange artwork panel on the left
    constexpr int kContentX = kArtW + 44;
    constexpr int kMarginR  = 40;

    // Palette from the website's styles.css.
    constexpr COLORREF kBg          = RGB(0x12, 0x13, 0x13);   // --bg
    constexpr COLORREF kOrange      = RGB(0xc7, 0x3d, 0x0c);   // --orange
    constexpr COLORREF kOrangeHover = RGB(0xa9, 0x30, 0x09);   // .button-primary:hover
    constexpr COLORREF kAccent      = RGB(0xf2, 0x7b, 0x50);   // --accent
    constexpr COLORREF kInk         = RGB(0xf3, 0xf1, 0xed);   // --ink
    constexpr COLORREF kMuted       = RGB(0xa8, 0xaa, 0xa7);   // --muted
    constexpr COLORREF kLine        = RGB(0x34, 0x36, 0x35);   // --line
    constexpr COLORREF kEyebrow     = RGB(0xd3, 0xd2, 0xcc);   // .hero .eyebrow
    constexpr COLORREF kDarkHover   = RGB(0x29, 0x2d, 0x29);   // .button-dark:hover
    constexpr COLORREF kDivider     = RGB(0x62, 0x67, 0x5f);   // .engine-strip separator

    enum class Button { None, Patreon, Continue };

    HWND       g_Hwnd       = nullptr;
    HWND       g_Owner      = nullptr;
    HINSTANCE  g_hInstance  = nullptr;
    UINT       g_Dpi        = USER_DEFAULT_SCREEN_DPI;
    HBITMAP    g_Art        = nullptr;
    int        g_ArtW       = 0;
    int        g_ArtH       = 0;
    ULONGLONG  g_UnlockAt   = 0;
    Button     g_Hover      = Button::None;
    Button     g_Pressed    = Button::None;
    bool       g_Tracking   = false;
    std::wstring g_FontPath;   // Playfair, registered privately for this process

    HFONT g_EyebrowFont = nullptr;
    HFONT g_HeadingFont = nullptr;
    HFONT g_AccentFont  = nullptr;
    HFONT g_BodyFont    = nullptr;
    HFONT g_ButtonFont  = nullptr;
    HFONT g_NoteFont    = nullptr;

    int Scaled(int value)
    {
        return MulDiv(value, static_cast<int>(g_Dpi), USER_DEFAULT_SCREEN_DPI);
    }

    bool IsLocked()
    {
        return GetTickCount64() < g_UnlockAt;
    }

    // -----------------------------------------------------------------------
    // Data/Media lookup, walking upward from the executable like the splash does.
    // -----------------------------------------------------------------------
    std::filesystem::path FindMediaFile(const wchar_t* fileName)
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return {};

        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
        while (!dir.empty())
        {
            std::filesystem::path candidate = dir / L"Data" / L"Media" / fileName;
            if (std::filesystem::exists(candidate))
                return candidate;

            std::filesystem::path parent = dir.parent_path();
            if (parent == dir)
                break;
            dir = parent;
        }
        return {};
    }

    // Decodes a PNG through WIC into a top-down 32-bit DIB.
    HBITMAP LoadPngBitmap(const std::filesystem::path& imagePath, int& outW, int& outH)
    {
        if (imagePath.empty())
            return nullptr;

        IWICImagingFactory* factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWICImagingFactory, reinterpret_cast<void**>(&factory))))
            return nullptr;

        HBITMAP result = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        if (SUCCEEDED(factory->CreateDecoderFromFilename(imagePath.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                            nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        {
            UINT w = 0, h = 0;
            converter->GetSize(&w, &h);

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = static_cast<LONG>(w);
            bmi.bmiHeader.biHeight      = -static_cast<LONG>(h);
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void* bits = nullptr;
            result = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (result && bits && SUCCEEDED(converter->CopyPixels(nullptr, w * 4, w * 4 * h, static_cast<BYTE*>(bits))))
            {
                outW = static_cast<int>(w);
                outH = static_cast<int>(h);
            }
            else if (result)
            {
                DeleteObject(result);
                result = nullptr;
            }
        }

        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        factory->Release();
        return result;
    }

    // -----------------------------------------------------------------------
    // Fonts. Sizes are the website's CSS pixel sizes; negative heights ask GDI for the
    // em size rather than the cell height, which is what CSS means by font-size.
    // -----------------------------------------------------------------------
    HFONT MakeFont(const wchar_t* face, int pixels, int weight)
    {
        LOGFONTW lf{};
        lf.lfHeight         = -Scaled(pixels);
        lf.lfWeight         = weight;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfOutPrecision   = OUT_TT_PRECIS;
        lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
        lf.lfQuality        = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        wcscpy_s(lf.lfFaceName, face);
        return CreateFontIndirectW(&lf);
    }

    void DestroyFonts()
    {
        for (HFONT* font : { &g_EyebrowFont, &g_HeadingFont, &g_AccentFont, &g_BodyFont, &g_ButtonFont, &g_NoteFont })
        {
            if (*font)
            {
                DeleteObject(*font);
                *font = nullptr;
            }
        }
    }

    void RebuildFonts()
    {
        DestroyFonts();
        // The site sets its italic-free accent in Playfair; Georgia is its own fallback.
        const wchar_t* accentFace = g_FontPath.empty() ? L"Georgia" : L"Playfair Display Medium";
        g_EyebrowFont = MakeFont(L"Segoe UI Semibold", 12, FW_SEMIBOLD);
        g_HeadingFont = MakeFont(L"Segoe UI", 40, FW_NORMAL);
        g_AccentFont  = MakeFont(accentFace, 40, FW_MEDIUM);
        g_BodyFont    = MakeFont(L"Segoe UI", 15, FW_NORMAL);
        g_ButtonFont  = MakeFont(L"Segoe UI Semibold", 14, FW_SEMIBOLD);
        g_NoteFont    = MakeFont(L"Segoe UI", 12, FW_NORMAL);
    }

    // -----------------------------------------------------------------------
    // Layout
    // -----------------------------------------------------------------------
    RECT PatreonRect()
    {
        const int x = Scaled(kContentX);
        const int y = Scaled(296);
        return { x, y, x + Scaled(212), y + Scaled(52) };
    }

    // Wide enough for "Continue to editor" plus its arrow, ending on the right margin.
    RECT ContinueRect()
    {
        const RECT patreon = PatreonRect();
        const int x = patreon.right + Scaled(12);
        return { x, patreon.top, Scaled(kBaseW - kMarginR), patreon.bottom };
    }

    Button HitTest(POINT pt)
    {
        const RECT patreon = PatreonRect();
        if (PtInRect(&patreon, pt))
            return Button::Patreon;
        const RECT cont = ContinueRect();
        if (PtInRect(&cont, pt))
            return Button::Continue;
        return Button::None;
    }

    // -----------------------------------------------------------------------
    // Painting
    // -----------------------------------------------------------------------
    void Fill(HDC dc, const RECT& rect, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    void Frame(HDC dc, const RECT& rect, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        FrameRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    void DrawLine(HDC dc, HFONT font, COLORREF color, int x, int y, const wchar_t* text, int extraSpacing = 0)
    {
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetTextCharacterExtra(dc, extraSpacing);
        TextOutW(dc, x, y, text, static_cast<int>(wcslen(text)));
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, oldFont);
    }

    // Draws the orange artwork panel: the emblem and wordmark cut out of the wide splash
    // image, fitted whole into the panel. The splash background is the same orange as
    // the panel, so the margins around it blend in.
    void PaintArt(HDC dc, int height)
    {
        const RECT panel = { 0, 0, Scaled(kArtW), height };
        Fill(dc, panel, kOrange);
        if (!g_Art || g_ArtW <= 0 || g_ArtH <= 0)
            return;

        // Emblem + "PTERO-ENGINE" occupy roughly this part of the 1920x1080 splash;
        // expressed as fractions so a re-exported splash at another size still works.
        const int srcX = g_ArtW * 27 / 100;
        const int srcY = g_ArtH * 6 / 100;
        const int srcW = g_ArtW * 46 / 100;
        const int srcH = g_ArtH * 86 / 100;

        const int maxW = panel.right - Scaled(48);
        const int maxH = height - Scaled(64);
        int dstW = maxW;
        int dstH = MulDiv(dstW, srcH, srcW);
        if (dstH > maxH)
        {
            dstH = maxH;
            dstW = MulDiv(dstH, srcW, srcH);
        }

        HDC artDC = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(artDC, g_Art);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, (panel.right - dstW) / 2, (height - dstH) / 2, dstW, dstH,
                   artDC, srcX, srcY, srcW, srcH, SRCCOPY);
        SelectObject(artDC, old);
        DeleteDC(artDC);
    }

    void PaintButton(HDC dc, RECT rect, Button which)
    {
        const bool hovered = g_Hover == which;
        const bool locked = which == Button::Continue && IsLocked();

        // .button:hover lifts by 2px on the website.
        if (hovered && !locked)
            OffsetRect(&rect, 0, -Scaled(2));

        const wchar_t* label = nullptr;
        const wchar_t* glyph = nullptr;
        COLORREF textColor = kInk;
        wchar_t countdown[32] = {};

        if (which == Button::Patreon)
        {
            Fill(dc, rect, hovered ? kOrangeHover : kOrange);
            label = L"Support on Patreon";
            glyph = L"\x2197";
            textColor = RGB(255, 255, 255);
        }
        else if (locked)
        {
            Frame(dc, rect, kLine);
            const ULONGLONG remaining = g_UnlockAt - GetTickCount64();
            swprintf_s(countdown, L"Continue in %llu", (remaining + 999) / 1000);
            label = countdown;
            textColor = kMuted;

            // Thin accent bar along the bottom edge fills up as the lock runs out.
            const double progress = 1.0 - static_cast<double>(remaining) / static_cast<double>(kLockMilliseconds);
            const int barW = static_cast<int>((rect.right - rect.left) * (progress < 0.0 ? 0.0 : progress));
            Fill(dc, { rect.left, rect.bottom - Scaled(2), rect.left + barW, rect.bottom }, kOrange);
        }
        else
        {
            if (hovered)
                Fill(dc, rect, kDarkHover);
            Frame(dc, rect, hovered ? kDivider : kLine);
            label = L"Continue to editor";
            glyph = L"\x2192";
        }

        HGDIOBJ oldFont = SelectObject(dc, g_ButtonFont);
        SetTextColor(dc, textColor);
        RECT textRect = { rect.left + Scaled(22), rect.top, rect.right - Scaled(22), rect.bottom };
        DrawTextW(dc, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (glyph)
            DrawTextW(dc, glyph, -1, &textRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
    }

    void Paint(HDC dc, int width, int height)
    {
        Fill(dc, { 0, 0, width, height }, kBg);
        PaintArt(dc, height);
        SetBkMode(dc, TRANSPARENT);

        const int x = Scaled(kContentX);

        // Eyebrow: short orange rule, then letter-spaced caps (.eyebrow, .orange-rule).
        const int eyebrowY = Scaled(54);
        Fill(dc, { x, eyebrowY + Scaled(9), x + Scaled(28), eyebrowY + Scaled(11) }, kAccent);
        DrawLine(dc, g_EyebrowFont, kEyebrow, x + Scaled(42), eyebrowY,
                 L"INDEPENDENTLY BUILT. STILL EVOLVING.", Scaled(2));

        // Headline from the site's support section, second line in the Playfair accent.
        DrawLine(dc, g_HeadingFont, kInk, x, Scaled(88), L"Be part of", -Scaled(1));
        DrawLine(dc, g_AccentFont, kAccent, x, Scaled(134), L"what comes next.", -Scaled(1));

        HGDIOBJ oldFont = SelectObject(dc, g_BodyFont);
        SetTextColor(dc, kMuted);
        RECT bodyRect = { x, Scaled(206), width - Scaled(kMarginR), Scaled(280) };
        DrawTextW(dc,
                  L"Ptero-Engine is built independently by Pterosoft Studio. "
                  L"Follow along and support its continued development on Patreon.",
                  -1, &bodyRect, DT_LEFT | DT_WORDBREAK);
        SelectObject(dc, oldFont);

        PaintButton(dc, PatreonRect(), Button::Patreon);
        PaintButton(dc, ContinueRect(), Button::Continue);

        DrawLine(dc, g_NoteFont, kMuted, x, Scaled(372), L"Thank you for building with Ptero-Engine.");

        Frame(dc, { 0, 0, width, height }, kLine);
    }

    void OnPaint(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        HDC bufferDC = CreateCompatibleDC(hdc);
        HBITMAP buffer = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        HGDIOBJ oldBitmap = SelectObject(bufferDC, buffer);

        Paint(bufferDC, client.right, client.bottom);
        BitBlt(hdc, 0, 0, client.right, client.bottom, bufferDC, 0, 0, SRCCOPY);

        SelectObject(bufferDC, oldBitmap);
        DeleteObject(buffer);
        DeleteDC(bufferDC);
        EndPaint(hwnd, &ps);
    }

    // -----------------------------------------------------------------------
    // Actions
    // -----------------------------------------------------------------------
    void Dismiss()
    {
        // Re-enable the owner first so Windows hands activation back to the editor
        // instead of whichever application happens to be behind it.
        if (g_Owner)
        {
            EnableWindow(g_Owner, TRUE);
            SetForegroundWindow(g_Owner);
        }
        DestroyWindow(g_Hwnd);
    }

    void Activate(Button which)
    {
        if (which == Button::Patreon)
        {
            ShellExecuteW(g_Hwnd, L"open", kPatreonUrl, nullptr, nullptr, SW_SHOWNORMAL);
            g_UnlockAt = 0;
            InvalidateRect(g_Hwnd, nullptr, FALSE);
        }
        else if (which == Button::Continue && !IsLocked())
        {
            Dismiss();
        }
    }

    void SetHover(Button hover)
    {
        if (g_Hover == hover)
            return;
        g_Hover = hover;
        InvalidateRect(g_Hwnd, nullptr, FALSE);
    }

    // -----------------------------------------------------------------------
    // Window procedure
    // -----------------------------------------------------------------------
    LRESULT CALLBACK SupportWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
        case WM_PAINT:
            OnPaint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
        {
            // Borderless, so anywhere that is not a button drags the window.
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            return HitTest(pt) == Button::None ? HTCAPTION : HTCLIENT;
        }
        case WM_NCLBUTTONDBLCLK:
            return 0;   // a fake caption must not maximize
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT)
            {
                const bool clickable = g_Hover == Button::Patreon || (g_Hover == Button::Continue && !IsLocked());
                SetCursor(LoadCursor(nullptr, clickable ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
        {
            if (!g_Tracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                g_Tracking = TrackMouseEvent(&tme) != FALSE;
            }
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetHover(HitTest(pt));
            return 0;
        }
        case WM_MOUSELEAVE:
            g_Tracking = false;
            SetHover(Button::None);
            return 0;
        case WM_LBUTTONDOWN:
        {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            g_Pressed = HitTest(pt);
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP:
        {
            ReleaseCapture();
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const Button released = HitTest(pt);
            const Button pressed = g_Pressed;
            g_Pressed = Button::None;
            if (released == pressed)
                Activate(released);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == VK_RETURN)
                Activate(Button::Continue);
            return 0;
        case WM_TIMER:
            if (wParam == kTickTimer)
            {
                if (!IsLocked())
                    KillTimer(hwnd, kTickTimer);
                const RECT cont = ContinueRect();
                RECT dirty = { cont.left, cont.top - Scaled(2), cont.right, cont.bottom };
                InvalidateRect(hwnd, &dirty, FALSE);
            }
            return 0;
        case WM_CLOSE:
            // Alt+F4 and friends are ignored while Continue is still locked.
            if (!IsLocked())
                Dismiss();
            return 0;
        case WM_DPICHANGED:
        {
            g_Dpi = HIWORD(wParam);
            RebuildFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kTickTimer);
            if (g_Owner)
                EnableWindow(g_Owner, TRUE);
            DestroyFonts();
            if (g_Art)
            {
                DeleteObject(g_Art);
                g_Art = nullptr;
            }
            if (!g_FontPath.empty())
            {
                RemoveFontResourceExW(g_FontPath.c_str(), FR_PRIVATE, nullptr);
                g_FontPath.clear();
            }
            g_Hwnd = nullptr;
            // Deliberately no PostQuitMessage: this window shares the editor's message loop.
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // Same per-thread opt-in the splash screen uses. The editor itself is DPI unaware,
    // so without this the dialog would be bitmap-stretched and blurry above 100%.
    // A window keeps the awareness it was created with, so the context only needs to be
    // switched around CreateWindowEx and is restored straight after.
    DPI_AWARENESS_CONTEXT SetThreadDpiContext(DPI_AWARENESS_CONTEXT context)
    {
        using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto setContext = user32 ? reinterpret_cast<SetThreadDpiAwarenessContextFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"))) : nullptr;
        return setContext ? setContext(context) : nullptr;
    }

    UINT WindowDpi(HWND hwnd)
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto getDpi = user32 ? reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow"))) : nullptr;
        const UINT dpi = getDpi ? getDpi(hwnd) : 0;
        return dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace SupportDialog
{
    void Show(HINSTANCE hInstance, HWND owner)
    {
        if (g_Hwnd)
            return;

        g_hInstance = hInstance;
        g_Owner     = owner;

        g_Art = LoadPngBitmap(FindMediaFile(L"ptero-engine_wide.png"), g_ArtW, g_ArtH);

        const std::filesystem::path fontPath = FindMediaFile(L"PlayfairDisplay-Medium.ttf");
        if (!fontPath.empty() && AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) > 0)
            g_FontPath = fontPath.wstring();

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DROPSHADOW;
        wc.lpfnWndProc   = SupportWndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        const DPI_AWARENESS_CONTEXT previousContext = SetThreadDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // Centre on the editor's monitor. Created at zero size first so the window's own
        // DPI - known only once it exists on that monitor - can decide the real size.
        HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(monitor, &monitorInfo))
            monitorInfo.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };

        g_Hwnd = CreateWindowExW(0, kClassName, L"Support Ptero-Engine", WS_POPUP,
                                 monitorInfo.rcWork.left, monitorInfo.rcWork.top, 0, 0,
                                 owner, nullptr, hInstance, nullptr);

        if (previousContext)
            SetThreadDpiContext(previousContext);

        if (!g_Hwnd)
        {
            if (g_Art)
            {
                DeleteObject(g_Art);
                g_Art = nullptr;
            }
            if (!g_FontPath.empty())
            {
                RemoveFontResourceExW(g_FontPath.c_str(), FR_PRIVATE, nullptr);
                g_FontPath.clear();
            }
            return;
        }

        g_Dpi = WindowDpi(g_Hwnd);
        RebuildFonts();

        const int wndW  = Scaled(kBaseW);
        const int wndH  = Scaled(kBaseH);
        const int workW = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        const int workH = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
        SetWindowPos(g_Hwnd, HWND_TOP,
                     monitorInfo.rcWork.left + (workW - wndW) / 2,
                     monitorInfo.rcWork.top + (workH - wndH) / 2,
                     wndW, wndH, SWP_NOACTIVATE);

        g_UnlockAt = GetTickCount64() + kLockMilliseconds;
        g_Hover    = Button::None;
        g_Pressed  = Button::None;
        g_Tracking = false;
        SetTimer(g_Hwnd, kTickTimer, 50, nullptr);

        // Owned popup over a disabled owner: the editor finishes appearing behind it but
        // takes no input until the dialog is dismissed.
        EnableWindow(owner, FALSE);
        ShowWindow(g_Hwnd, SW_SHOW);
        SetForegroundWindow(g_Hwnd);
        SetFocus(g_Hwnd);
    }
}
