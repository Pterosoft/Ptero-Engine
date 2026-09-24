#include "pch.h"
#include "GameConsoleWindow.h"

#include "System/CVar.h"
#include "System/PteroLog.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kWindowClass[] = L"PteroGameConsoleWindow";
    constexpr int kLogControlId = 1;
    constexpr int kInputControlId = 2;
    constexpr std::size_t kMaxDisplayedLines = 400;
    // Rebuilding the log text re-lays-out the whole edit control, so it is throttled
    // rather than done on every frame a line arrives.
    constexpr std::chrono::milliseconds kRefreshInterval{ 150 };

    constexpr COLORREF kBackground = RGB(17, 18, 20);
    constexpr COLORREF kInputBackground = RGB(34, 35, 38);
    constexpr COLORREF kText = RGB(214, 221, 229);

    HWND gWindow = nullptr;
    HWND gLog = nullptr;
    HWND gInput = nullptr;
    HWND gHost = nullptr;
    HFONT gFont = nullptr;
    HBRUSH gBackgroundBrush = nullptr;
    HBRUSH gInputBrush = nullptr;
    WNDPROC gInputDefaultProc = nullptr;
    bool gEscapePressed = false;
    std::uint64_t gShownTotal = ~0ull;
    std::chrono::steady_clock::time_point gLastRefresh{};
    RECT gLastPlacement{};

    std::wstring Wide(const std::string& text)
    {
        if (text.empty())
            return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
        return out;
    }

    std::string Utf8(const std::wstring& text)
    {
        if (text.empty())
            return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    void RefreshLog(bool force)
    {
        const std::uint64_t total = PteroLog::TotalCount();
        const auto now = std::chrono::steady_clock::now();
        if (!force && (total == gShownTotal || now - gLastRefresh < kRefreshInterval))
            return;
        gShownTotal = total;
        gLastRefresh = now;

        std::vector<PteroLog::Entry> entries;
        PteroLog::Snapshot(entries, kMaxDisplayedLines);

        std::wstring text;
        text.reserve(entries.size() * 96);
        for (const PteroLog::Entry& entry : entries)
        {
            if (entry.MessageLevel < PteroLog::Level::Info)
                continue;
            char prefix[64] = {};
            std::snprintf(prefix, sizeof(prefix), "%8.2f  %-5s  %-12s  ",
                          entry.TimeSeconds, PteroLog::LevelName(entry.MessageLevel), entry.Category.c_str());
            text += Wide(prefix);
            text += Wide(entry.Message);
            text += L"\r\n";
        }

        SendMessageW(gLog, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(gLog, text.c_str());
        const int length = GetWindowTextLengthW(gLog);
        SendMessageW(gLog, EM_SETSEL, length, length);
        SendMessageW(gLog, EM_SCROLLCARET, 0, 0);
        SendMessageW(gLog, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(gLog, nullptr, TRUE);
    }

    void RunInputLine()
    {
        const int length = GetWindowTextLengthW(gInput);
        std::wstring line(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(gInput, line.data(), length + 1);
        line.resize(static_cast<size_t>(length));
        SetWindowTextW(gInput, L"");
        if (line.empty())
            return;

        CVar::Execute(Utf8(line));
        RefreshLog(true);
    }

    LRESULT CALLBACK InputProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_KEYDOWN && wParam == VK_RETURN)
        {
            RunInputLine();
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
        {
            gEscapePressed = true;
            return 0;
        }
        // Enter/Escape are handled on key-down; the tilde is the toggle key, never text.
        if (message == WM_CHAR && (wParam == L'\r' || wParam == 27 || wParam == L'`' || wParam == L'~'))
            return 0;
        return CallWindowProcW(gInputDefaultProc, window, message, wParam, lParam);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), kText);
            SetBkColor(reinterpret_cast<HDC>(wParam), kInputBackground);
            return reinterpret_cast<LRESULT>(gInputBrush);
        case WM_CTLCOLORSTATIC:   // the read-only log
            SetTextColor(reinterpret_cast<HDC>(wParam), kText);
            SetBkColor(reinterpret_cast<HDC>(wParam), kBackground);
            return reinterpret_cast<LRESULT>(gBackgroundBrush);
        case WM_SIZE:
        {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            constexpr int kInputHeight = 26;
            constexpr int kPadding = 6;
            MoveWindow(gLog, kPadding, kPadding, width - 2 * kPadding, height - kInputHeight - 3 * kPadding, TRUE);
            MoveWindow(gInput, kPadding, height - kInputHeight - kPadding, width - 2 * kPadding, kInputHeight, TRUE);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE && gInput)
                SetFocus(gInput);
            return 0;
        case WM_CLOSE:
            gEscapePressed = true;
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    bool Create(HWND host)
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        gBackgroundBrush = CreateSolidBrush(kBackground);
        gInputBrush = CreateSolidBrush(kInputBackground);
        windowClass.hbrBackground = gBackgroundBrush;
        windowClass.lpszClassName = kWindowClass;
        RegisterClassExW(&windowClass);

        // Owned by the game window, so it stays above it and minimizes with it, but is
        // its own top-level window: while it has the keyboard, the game (which checks
        // for its own window being in the foreground) stops reacting to typing.
        gWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Console", WS_POPUP | WS_BORDER,
                                  0, 0, 800, 300, host, nullptr, instance, nullptr);
        if (!gWindow)
            return false;

        gLog = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                               0, 0, 10, 10, gWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLogControlId)), instance, nullptr);
        gInput = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 0, 0, 10, 10, gWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputControlId)), instance, nullptr);

        gFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(gLog, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), FALSE);
        SendMessageW(gInput, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), FALSE);
        // The edit control's default text limit (32K characters) is too small for a log.
        SendMessageW(gLog, EM_SETLIMITTEXT, 0, 0);

        gInputDefaultProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(gInput, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(InputProc)));

        gHost = host;
        return true;
    }

    // Top 40% of the host's client area, like a Quake console.
    void Dock()
    {
        RECT client{};
        GetClientRect(gHost, &client);
        POINT origin{ 0, 0 };
        ClientToScreen(gHost, &origin);
        const int width = client.right - client.left;
        const int height = (std::max)(160, static_cast<int>((client.bottom - client.top) * 0.4f));
        const RECT placement{ origin.x, origin.y, origin.x + width, origin.y + height };
        if (EqualRect(&placement, &gLastPlacement))
            return;
        gLastPlacement = placement;
        SetWindowPos(gWindow, nullptr, placement.left, placement.top, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

namespace GameConsoleWindow
{
    bool Update(HWND host, bool visible)
    {
        if (host == nullptr)
            return visible;

        if (gEscapePressed)
        {
            gEscapePressed = false;
            visible = false;
        }

        if (!visible)
        {
            if (gWindow && IsWindowVisible(gWindow))
            {
                ShowWindow(gWindow, SW_HIDE);
                SetForegroundWindow(gHost);
            }
            return false;
        }

        if (!gWindow && !Create(host))
            return false;

        Dock();
        if (!IsWindowVisible(gWindow))
        {
            RefreshLog(true);
            ShowWindow(gWindow, SW_SHOW);
            SetForegroundWindow(gWindow);
            SetFocus(gInput);
        }
        RefreshLog(false);
        return true;
    }

    bool HasFocus()
    {
        return gWindow && IsWindowVisible(gWindow) && GetForegroundWindow() == gWindow;
    }

    void Shutdown()
    {
        if (gWindow)
            DestroyWindow(gWindow);
        gWindow = gLog = gInput = gHost = nullptr;
        if (gFont) DeleteObject(gFont);
        if (gBackgroundBrush) DeleteObject(gBackgroundBrush);
        if (gInputBrush) DeleteObject(gInputBrush);
        gFont = nullptr;
        gBackgroundBrush = gInputBrush = nullptr;
        gLastPlacement = {};
        gShownTotal = ~0ull;
    }
}
