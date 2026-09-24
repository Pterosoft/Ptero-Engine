#include "VideoPlayerWindow.h"

#include "VideoPlayer.h"

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")

namespace
{
    constexpr UINT WM_APP_OPEN = WM_APP + 1;
    constexpr UINT_PTR kModalTickTimer = 1;
    constexpr wchar_t kClassName[] = L"PteroVideoPlayerWindow";
    constexpr double kSkipSeconds = 5.0;
    // Scrubbing seeks at most this often; each seek restarts decoding from a key frame.
    constexpr double kScrubInterval = 0.04;

    // Matches the editor's dark Qt theme.
    constexpr COLORREF kVideoBackground = RGB(0, 0, 0);
    constexpr COLORREF kBarBackground = RGB(0x2b, 0x2d, 0x31);
    constexpr COLORREF kButtonHover = RGB(0x3c, 0x3f, 0x45);
    constexpr COLORREF kButtonPressed = RGB(0x4a, 0x4d, 0x55);
    constexpr COLORREF kTrack = RGB(0x4a, 0x4d, 0x55);
    constexpr COLORREF kAccent = RGB(0x3d, 0x8b, 0xfd);
    constexpr COLORREF kText = RGB(0xdc, 0xdd, 0xde);
    constexpr COLORREF kDimText = RGB(0x9a, 0xa0, 0xa6);

    // Shared between the window thread and callers of Open/Shutdown.
    std::mutex gMutex;
    HANDLE gThread = nullptr;
    HWND gWindow = nullptr;
    std::wstring gPendingPath;
    bool gShutdownRequested = false;

    enum class Hit
    {
        None,
        Video,
        Play,
        Stop,
        Loop,
        Volume,
        Timeline,
        Open
    };

    std::wstring FormatTime(double seconds)
    {
        const int total = static_cast<int>((std::max)(seconds, 0.0));
        wchar_t buffer[32];
        if (total >= 3600)
        {
            swprintf_s(buffer, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
        }
        else
        {
            swprintf_s(buffer, L"%02d:%02d", total / 60, total % 60);
        }

        return buffer;
    }

    std::wstring Widen(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
        return result;
    }

    bool Contains(const RECT& rect, POINT point)
    {
        return PtInRect(&rect, point) != FALSE;
    }

    void FillSolid(HDC dc, const RECT& rect, COLORREF color)
    {
        SetDCBrushColor(dc, color);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    void FillRounded(HDC dc, const RECT& rect, COLORREF color, int radius)
    {
        SetDCBrushColor(dc, color);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        // RoundRect excludes the right and bottom edges when there is no pen.
        RoundRect(dc, rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius * 2, radius * 2);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }

    void FillPolygon(HDC dc, const POINT* points, int count, COLORREF color)
    {
        SetDCBrushColor(dc, color);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, count);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }

    void DrawTextIn(HDC dc, const std::wstring& text, RECT rect, COLORREF color, UINT format)
    {
        SetTextColor(dc, color);
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format | DT_SINGLELINE | DT_NOPREFIX);
    }

    HICON LoadEditorIcon(int size)
    {
        // Borrow the host executable's first icon, so the window is recognisably the editor's.
        HMODULE exe = GetModuleHandleW(nullptr);
        LPWSTR found = nullptr;
        EnumResourceNamesW(
            exe, RT_GROUP_ICON,
            [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR parameter) -> BOOL {
                *reinterpret_cast<LPWSTR*>(parameter) = IS_INTRESOURCE(name) ? name : _wcsdup(name);
                return FALSE;
            },
            reinterpret_cast<LONG_PTR>(&found));

        if (found == nullptr)
        {
            return nullptr;
        }

        HICON icon = static_cast<HICON>(LoadImageW(exe, found, IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        if (!IS_INTRESOURCE(found))
        {
            free(found);
        }

        return icon;
    }

    class PlayerWindow
    {
    public:
        HWND Handle = nullptr;

        void Create(const std::wstring& initialPath)
        {
            RegisterWindowClass();

            // Open on the monitor the user is working on, sized for its DPI. The thread
            // is per-monitor aware, so these are physical pixels.
            POINT cursor{};
            GetCursorPos(&cursor);
            HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{ sizeof(info) };
            GetMonitorInfoW(monitor, &info);
            UINT dpiX = 96, dpiY = 96;
            GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
            mDpi = dpiX;

            const RECT& work = info.rcWork;
            const int workWidth = work.right - work.left;
            const int workHeight = work.bottom - work.top;
            const int width = (std::min)(Scale(1024), workWidth * 9 / 10);
            const int height = (std::min)(Scale(660), workHeight * 9 / 10);

            Handle = CreateWindowExW(0, kClassName, L"Video Player", WS_OVERLAPPEDWINDOW,
                                     work.left + (workWidth - width) / 2, work.top + (workHeight - height) / 2, width,
                                     height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
            if (Handle == nullptr)
            {
                return;
            }

            const BOOL dark = TRUE;
            // DWMWA_USE_IMMERSIVE_DARK_MODE, spelled as a number for older SDK headers.
            DwmSetWindowAttribute(Handle, 20, &dark, sizeof(dark));

            if (HICON big = LoadEditorIcon(GetSystemMetricsForDpi(SM_CXICON, mDpi)))
            {
                SendMessageW(Handle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
            }

            if (HICON smallIcon = LoadEditorIcon(GetSystemMetricsForDpi(SM_CXSMICON, mDpi)))
            {
                SendMessageW(Handle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
            }

            ShowWindow(Handle, SW_SHOWNORMAL);
            SetForegroundWindow(Handle);

            QueryPerformanceFrequency(&mFrequency);
            QueryPerformanceCounter(&mLastTick);

            if (!initialPath.empty())
            {
                Load(initialPath);
            }
        }

        // Body of the thread's message loop: waits briefly for input, then advances the
        // player. Returns false once the window has gone.
        bool Pump()
        {
            const bool busy = mPlayer.IsPlaying() || mScrubbing;
            MsgWaitForMultipleObjectsEx(0, nullptr, busy ? 4 : 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    return false;
                }

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            Tick();
            return true;
        }

    private:
        VideoPlayer mPlayer;
        UINT mDpi = 96;
        HFONT mFont = nullptr;

        RECT mVideo{}, mBar{}, mPlay{}, mStop{}, mLoop{}, mVolume{}, mTimeline{}, mTimeText{}, mOpen{};
        float mVolumeLevel = 1.0f;
        bool mMuted = false;
        Hit mHover = Hit::None;
        Hit mPressed = Hit::None;
        bool mTrackingLeave = false;

        bool mScrubbing = false;
        bool mResumeAfterScrub = false;
        bool mScrubPending = false;
        double mScrubTarget = 0.0;
        double mLastScrubSeek = 0.0;

        bool mLoopPreference = true;
        std::wstring mMessage = L"Open a .webm video to play it.";

        std::uint64_t mShownSerial = 0;
        double mShownTime = -1.0;
        VideoPlayer::State mShownState = VideoPlayer::State::Closed;

        LARGE_INTEGER mFrequency{};
        LARGE_INTEGER mLastTick{};
        double mClock = 0.0;

        HDC mBackDc = nullptr;
        HBITMAP mBackBitmap = nullptr;
        HGDIOBJ mBackOldBitmap = nullptr;
        int mBackWidth = 0;
        int mBackHeight = 0;

        int Scale(int value) const
        {
            return MulDiv(value, static_cast<int>(mDpi), 96);
        }

        static void RegisterWindowClass()
        {
            static bool registered = false;
            if (registered)
            {
                return;
            }

            WNDCLASSEXW windowClass{ sizeof(windowClass) };
                        windowClass.lpfnWndProc = &PlayerWindow::StaticProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = kClassName;
            registered = RegisterClassExW(&windowClass) != 0;
        }

        static LRESULT CALLBACK StaticProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            PlayerWindow* self = nullptr;
            if (message == WM_NCCREATE)
            {
                self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                self->Handle = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            else
            {
                self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }

            return self != nullptr ? self->Proc(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
        }

        LRESULT Proc(UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_CREATE:
                CreateFonts();
                return 0;

            case WM_SIZE:
                Layout();
                InvalidateRect(Handle, nullptr, FALSE);
                return 0;

            case WM_GETMINMAXINFO:
            {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
                limits->ptMinTrackSize = { Scale(520), Scale(320) };
                return 0;
            }

            case WM_DPICHANGED:
            {
                mDpi = HIWORD(wParam);
                CreateFonts();
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(Handle, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                InvalidateRect(Handle, nullptr, FALSE);
                return 0;
            }

            case WM_ENTERSIZEMOVE:
                // Moving or resizing runs a modal loop that starves Pump(); a timer keeps
                // the video running meanwhile.
                SetTimer(Handle, kModalTickTimer, 10, nullptr);
                return 0;

            case WM_EXITSIZEMOVE:
                KillTimer(Handle, kModalTickTimer);
                return 0;

            case WM_TIMER:
                if (wParam == kModalTickTimer)
                {
                    Tick();
                }
                return 0;

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT:
                Paint();
                return 0;

            case WM_MOUSEMOVE:
                OnMouseMove({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
                return 0;

            case WM_MOUSEWHEEL:
                ChangeVolume(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 0.05f : -0.05f);
                return 0;

            case WM_MOUSELEAVE:
                mTrackingLeave = false;
                SetHover(Hit::None);
                return 0;

            case WM_LBUTTONDOWN:
                OnMouseDown({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
                return 0;

            case WM_LBUTTONUP:
                OnMouseUp({ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) });
                return 0;

            case WM_CAPTURECHANGED:
                if (mScrubbing)
                {
                    EndScrub();
                }
                mPressed = Hit::None;
                return 0;

            case WM_KEYDOWN:
                OnKey(static_cast<UINT>(wParam));
                return 0;

            case WM_APP_OPEN:
            {
                std::wstring path;
                {
                    std::lock_guard<std::mutex> lock(gMutex);
                    path.swap(gPendingPath);
                }

                if (!path.empty())
                {
                    Load(path);
                }

                if (IsIconic(Handle))
                {
                    ShowWindow(Handle, SW_RESTORE);
                }

                SetForegroundWindow(Handle);
                return 0;
            }

            case WM_CLOSE:
                DestroyWindow(Handle);
                return 0;

            case WM_DESTROY:
                mPlayer.Close();
                ReleaseBackBuffer();
                if (mFont != nullptr)
                {
                    DeleteObject(mFont);
                    mFont = nullptr;
                }
                {
                    std::lock_guard<std::mutex> lock(gMutex);
                    gWindow = nullptr;
                }
                PostQuitMessage(0);
                return 0;

            default:
                break;
            }

            return DefWindowProcW(Handle, message, wParam, lParam);
        }

        void CreateFonts()
        {
            if (mFont != nullptr)
            {
                DeleteObject(mFont);
            }

            mFont = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                L"Segoe UI");
        }

        void Layout()
        {
            RECT client{};
            GetClientRect(Handle, &client);

            const int barHeight = Scale(48);
            const int padding = Scale(10);
            const int button = Scale(32);
            const int gap = Scale(4);

            mBar = { 0, (std::max)(client.bottom - barHeight, 0L), client.right, client.bottom };
            mVideo = { 0, 0, client.right, mBar.top };

            const int top = mBar.top + (barHeight - button) / 2;
            mPlay = { padding, top, padding + button, top + button };
            mStop = { mPlay.right + gap, top, mPlay.right + gap + button, top + button };
            mLoop = { mStop.right + gap, top, mStop.right + gap + Scale(56), top + button };
            mVolume = { mLoop.right + gap, top, mLoop.right + gap + Scale(56), top + button };
            mOpen = { client.right - padding - Scale(64), top, client.right - padding, top + button };
            mTimeText = { mOpen.left - Scale(12) - Scale(120), top, mOpen.left - Scale(12), top + button };
            mTimeline = { mVolume.right + Scale(14), top,
                          (std::max)(mTimeText.left - Scale(14), mVolume.right + Scale(14)), top + button };
        }

        Hit HitTest(POINT point) const
        {
            if (Contains(mPlay, point)) return Hit::Play;
            if (Contains(mStop, point)) return Hit::Stop;
            if (Contains(mLoop, point)) return Hit::Loop;
            if (Contains(mVolume, point)) return Hit::Volume;
            if (Contains(mTimeline, point)) return Hit::Timeline;
            if (Contains(mOpen, point)) return Hit::Open;
            if (Contains(mVideo, point)) return Hit::Video;
            return Hit::None;
        }

        void SetHover(Hit hit)
        {
            if (hit != mHover)
            {
                mHover = hit;
                InvalidateRect(Handle, &mBar, FALSE);
            }
        }

        void OnMouseMove(POINT point)
        {
            if (!mTrackingLeave)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, Handle, 0 };
                mTrackingLeave = TrackMouseEvent(&track) != FALSE;
            }

            SetHover(HitTest(point));
            if (mScrubbing)
            {
                ScrubTo(point.x);
            }
        }

        void OnMouseDown(POINT point)
        {
            SetFocus(Handle);
            const Hit hit = HitTest(point);
            if (hit == Hit::Timeline && mPlayer.IsOpen())
            {
                mScrubbing = true;
                mResumeAfterScrub = mPlayer.IsPlaying();
                mPlayer.Pause();
                SetCapture(Handle);
                ScrubTo(point.x);
                FlushScrub();
                return;
            }

            mPressed = hit;
            if (hit != Hit::None)
            {
                SetCapture(Handle);
                InvalidateRect(Handle, &mBar, FALSE);
            }
        }

        void OnMouseUp(POINT point)
        {
            if (mScrubbing)
            {
                ScrubTo(point.x);
                ReleaseCapture();  // ends the scrub through WM_CAPTURECHANGED
                return;
            }

            const Hit pressed = mPressed;
            mPressed = Hit::None;
            if (GetCapture() == Handle)
            {
                ReleaseCapture();
            }

            if (pressed != Hit::None && pressed == HitTest(point))
            {
                Activate(pressed);
            }

            InvalidateRect(Handle, &mBar, FALSE);
        }

        void Activate(Hit hit)
        {
            switch (hit)
            {
            case Hit::Video:
            case Hit::Play:
                TogglePlay();
                break;
            case Hit::Stop:
                mPlayer.Stop();
                break;
            case Hit::Loop:
                mLoopPreference = !mLoopPreference;
                mPlayer.SetLooping(mLoopPreference);
                break;
            case Hit::Volume:
                mMuted = !mMuted;
                ApplyVolume();
                break;
            case Hit::Open:
                PromptForFile();
                break;
            default:
                break;
            }

            InvalidateRect(Handle, nullptr, FALSE);
        }

        void OnKey(UINT key)
        {
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            switch (key)
            {
            case VK_SPACE:
                TogglePlay();
                break;
            case VK_LEFT:
                mPlayer.Seek(mPlayer.GetTime() - kSkipSeconds);
                break;
            case VK_RIGHT:
                mPlayer.Seek(mPlayer.GetTime() + kSkipSeconds);
                break;
            case VK_HOME:
                mPlayer.Seek(0.0);
                break;
            case 'L':
                Activate(Hit::Loop);
                break;
            case 'M':
                Activate(Hit::Volume);
                break;
            case VK_UP:
                ChangeVolume(0.05f);
                break;
            case VK_DOWN:
                ChangeVolume(-0.05f);
                break;
            case 'O':
                if (control)
                {
                    PromptForFile();
                }
                break;
            case VK_ESCAPE:
                PostMessageW(Handle, WM_CLOSE, 0, 0);
                break;
            default:
                return;
            }

            InvalidateRect(Handle, nullptr, FALSE);
        }

        void ApplyVolume()
        {
            mPlayer.SetVolume(mMuted ? 0.0f : mVolumeLevel);
        }

        void ChangeVolume(float delta)
        {
            mVolumeLevel = std::clamp(mVolumeLevel + delta, 0.0f, 1.0f);
            // Turning it up is also how you unmute.
            if (delta > 0.0f)
            {
                mMuted = false;
            }

            ApplyVolume();
            InvalidateRect(Handle, &mBar, FALSE);
        }

        void TogglePlay()
        {
            if (mPlayer.IsPlaying())
            {
                mPlayer.Pause();
            }
            else
            {
                mPlayer.Play();
            }
        }

        void ScrubTo(int x)
        {
            const int width = (std::max)(static_cast<int>(mTimeline.right - mTimeline.left), 1);
            const double fraction = std::clamp(static_cast<double>(x - mTimeline.left) / width, 0.0, 1.0);
            mScrubTarget = fraction * mPlayer.GetDuration();
            mScrubPending = true;
            if (mClock - mLastScrubSeek >= kScrubInterval)
            {
                FlushScrub();
            }
        }

        void FlushScrub()
        {
            if (mScrubPending)
            {
                mPlayer.Seek(mScrubTarget);
                mScrubPending = false;
                mLastScrubSeek = mClock;
                InvalidateRect(Handle, &mBar, FALSE);
            }
        }

        void EndScrub()
        {
            FlushScrub();
            mScrubbing = false;
            if (mResumeAfterScrub)
            {
                mPlayer.Play();
            }

            InvalidateRect(Handle, nullptr, FALSE);
        }

        void PromptForFile()
        {
            wchar_t buffer[MAX_PATH * 4] = {};
            std::wstring initialDirectory;
            if (!mPlayer.GetPath().empty())
            {
                initialDirectory = std::filesystem::path(mPlayer.GetPath()).parent_path().wstring();
            }
            else
            {
                // Nothing open yet: start where imported videos live.
                wchar_t modulePath[MAX_PATH * 4] = {};
                GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
                std::error_code error;
                for (std::filesystem::path directory = std::filesystem::path(modulePath).parent_path();
                     !directory.empty() && directory != directory.parent_path(); directory = directory.parent_path())
                {
                    if (std::filesystem::is_directory(directory / L"Data", error))
                    {
                        initialDirectory = (directory / L"Data").wstring();
                        break;
                    }
                }
            }

            OPENFILENAMEW dialog{ sizeof(dialog) };
            dialog.hwndOwner = Handle;
            dialog.lpstrFilter = L"WebM Video (*.webm)\0*.webm\0All Files (*.*)\0*.*\0";
            dialog.lpstrFile = buffer;
            dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
            dialog.lpstrInitialDir = initialDirectory.empty() ? nullptr : initialDirectory.c_str();
            // NOCHANGEDIR: the editor resolves Data paths against its working directory.
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
            const bool resume = mPlayer.IsPlaying();
            mPlayer.Pause();
            if (GetOpenFileNameW(&dialog))
            {
                Load(buffer);
            }
            else if (resume)
            {
                mPlayer.Play();
            }

            // The dialog's modal loop stopped Pump(); do not count that time as playback.
            QueryPerformanceCounter(&mLastTick);
        }

        void Load(const std::wstring& path)
        {
            const std::wstring name = std::filesystem::path(path).filename().wstring();
            if (!mPlayer.Open(path))
            {
                mMessage = L"Could not play " + name + L":\n" + Widen(mPlayer.GetLastError());
                SetWindowTextW(Handle, (L"Video Player - " + name).c_str());
                InvalidateRect(Handle, nullptr, FALSE);
                return;
            }

            mMessage.clear();
            mPlayer.SetLooping(mLoopPreference);
            ApplyVolume();
            mPlayer.Play();

            wchar_t details[128];
            const double fps = mPlayer.GetFrameRate();
            if (fps > 0.0)
            {
                swprintf_s(details, L"  (%dx%d, %S, %.3g fps)", mPlayer.GetWidth(), mPlayer.GetHeight(),
                           mPlayer.GetCodecName(), fps);
            }
            else
            {
                swprintf_s(details, L"  (%dx%d, %S)", mPlayer.GetWidth(), mPlayer.GetHeight(), mPlayer.GetCodecName());
            }

            SetWindowTextW(Handle, (L"Video Player - " + name + details).c_str());
            QueryPerformanceCounter(&mLastTick);
            InvalidateRect(Handle, nullptr, FALSE);
        }

        void Tick()
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            double elapsed = static_cast<double>(now.QuadPart - mLastTick.QuadPart) / static_cast<double>(mFrequency.QuadPart);
            mLastTick = now;
            // A long stall (a breakpoint, a blocked thread) should pause the video, not
            // make it jump ahead.
            elapsed = (std::min)(elapsed, 0.25);
            mClock += elapsed;

            if (mScrubbing && mClock - mLastScrubSeek >= kScrubInterval)
            {
                FlushScrub();
            }

            mPlayer.Update(elapsed);

            VideoPlayer::Frame frame;
            if (mPlayer.GetFrame(frame) && frame.Serial != mShownSerial)
            {
                InvalidateRect(Handle, &mVideo, FALSE);
            }

            // The time readout only changes visibly ten times a second.
            const double shownTime = std::floor(mPlayer.GetTime() * 10.0);
            if (shownTime != mShownTime || mPlayer.GetState() != mShownState)
            {
                InvalidateRect(Handle, &mBar, FALSE);
            }
        }

        void EnsureBackBuffer(HDC windowDc, int width, int height)
        {
            if (mBackDc != nullptr && width == mBackWidth && height == mBackHeight)
            {
                return;
            }

            ReleaseBackBuffer();
            mBackDc = CreateCompatibleDC(windowDc);
            mBackBitmap = CreateCompatibleBitmap(windowDc, (std::max)(width, 1), (std::max)(height, 1));
            mBackOldBitmap = SelectObject(mBackDc, mBackBitmap);
            mBackWidth = width;
            mBackHeight = height;
        }

        void ReleaseBackBuffer()
        {
            if (mBackDc != nullptr)
            {
                SelectObject(mBackDc, mBackOldBitmap);
                DeleteObject(mBackBitmap);
                DeleteDC(mBackDc);
            }

            mBackDc = nullptr;
            mBackBitmap = nullptr;
            mBackOldBitmap = nullptr;
            mBackWidth = mBackHeight = 0;
        }

        void Paint()
        {
            PAINTSTRUCT paint{};
            HDC windowDc = BeginPaint(Handle, &paint);
            RECT client{};
            GetClientRect(Handle, &client);
            EnsureBackBuffer(windowDc, client.right, client.bottom);

            HDC dc = mBackDc;
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ oldFont = SelectObject(dc, mFont);

            PaintVideo(dc);
            PaintBar(dc);

            SelectObject(dc, oldFont);
            BitBlt(windowDc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left,
                   paint.rcPaint.bottom - paint.rcPaint.top, dc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
            EndPaint(Handle, &paint);
        }

        void PaintVideo(HDC dc)
        {
            FillSolid(dc, mVideo, kVideoBackground);

            VideoPlayer::Frame frame;
            if (!mMessage.empty() || !mPlayer.GetFrame(frame))
            {
                RECT text = mVideo;
                InflateRect(&text, -Scale(24), -Scale(24));
                SetTextColor(dc, kDimText);
                RECT measure = text;
                DrawTextW(dc, mMessage.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
                const int textHeight = measure.bottom - measure.top;
                text.top += ((text.bottom - text.top) - textHeight) / 2;
                DrawTextW(dc, mMessage.c_str(), -1, &text, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
                return;
            }

            const int areaWidth = mVideo.right - mVideo.left;
            const int areaHeight = mVideo.bottom - mVideo.top;
            if (areaWidth <= 0 || areaHeight <= 0)
            {
                return;
            }

            // Letterbox: the whole picture, centred, aspect preserved.
            int width = areaWidth;
            int height = static_cast<int>(static_cast<long long>(areaWidth) * frame.Height / frame.Width);
            if (height > areaHeight)
            {
                height = areaHeight;
                width = static_cast<int>(static_cast<long long>(areaHeight) * frame.Width / frame.Height);
            }

            const int x = mVideo.left + (areaWidth - width) / 2;
            const int y = mVideo.top + (areaHeight - height) / 2;

            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = frame.Width;
            bitmap.bmiHeader.biHeight = -frame.Height;  // top-down
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, x, y, width, height, 0, 0, frame.Width, frame.Height, frame.Pixels, &bitmap,
                          DIB_RGB_COLORS, SRCCOPY);
            mShownSerial = frame.Serial;
        }

        COLORREF ButtonColor(Hit hit) const
        {
            if (mPressed == hit)
            {
                return kButtonPressed;
            }

            return mHover == hit ? kButtonHover : kBarBackground;
        }

        void PaintBar(HDC dc)
        {
            FillSolid(dc, mBar, kBarBackground);
            const int radius = Scale(4);
            const bool open = mPlayer.IsOpen();
            const COLORREF iconColor = open ? kText : kDimText;

            // Play / pause.
            FillRounded(dc, mPlay, ButtonColor(Hit::Play), radius);
            {
                const int cx = (mPlay.left + mPlay.right) / 2;
                const int cy = (mPlay.top + mPlay.bottom) / 2;
                const int s = Scale(7);
                if (mPlayer.IsPlaying() || (mScrubbing && mResumeAfterScrub))
                {
                    const RECT left{ cx - s + Scale(1), cy - s, cx - Scale(2), cy + s };
                    const RECT right{ cx + Scale(2), cy - s, cx + s - Scale(1), cy + s };
                    FillSolid(dc, left, iconColor);
                    FillSolid(dc, right, iconColor);
                }
                else
                {
                    const POINT triangle[3] = { { cx - s + Scale(2), cy - s }, { cx - s + Scale(2), cy + s },
                                                { cx + s, cy } };
                    FillPolygon(dc, triangle, 3, iconColor);
                }
            }

            // Stop.
            FillRounded(dc, mStop, ButtonColor(Hit::Stop), radius);
            {
                const int cx = (mStop.left + mStop.right) / 2;
                const int cy = (mStop.top + mStop.bottom) / 2;
                const int s = Scale(6);
                FillSolid(dc, RECT{ cx - s, cy - s, cx + s, cy + s }, iconColor);
            }

            // Loop toggle: filled with the accent colour while on.
            FillRounded(dc, mLoop, mLoopPreference ? kAccent : ButtonColor(Hit::Loop), radius);
            DrawTextIn(dc, L"Loop", mLoop, mLoopPreference ? RGB(255, 255, 255) : kText, DT_CENTER | DT_VCENTER);

            // Volume: click to mute, wheel or Up/Down to change. A clip whose soundtrack
            // could not be decoded says so instead.
            const bool hasAudio = mPlayer.HasAudio();
            FillRounded(dc, mVolume, mMuted && hasAudio ? kButtonPressed : ButtonColor(Hit::Volume), radius);
            wchar_t volumeLabel[32];
            if (!open || !hasAudio)
            {
                wcscpy_s(volumeLabel, L"No audio");
            }
            else if (mMuted)
            {
                wcscpy_s(volumeLabel, L"Muted");
            }
            else
            {
                swprintf_s(volumeLabel, L"%d%%", static_cast<int>(mVolumeLevel * 100.0f + 0.5f));
            }

            DrawTextIn(dc, volumeLabel, mVolume, hasAudio && open ? kText : kDimText, DT_CENTER | DT_VCENTER);

            // Timeline.
            const double duration = mPlayer.GetDuration();
            const double time = mScrubbing ? mScrubTarget : mPlayer.GetTime();
            const double fraction = duration > 0.0 ? std::clamp(time / duration, 0.0, 1.0) : 0.0;
            const int trackHeight = Scale(mHover == Hit::Timeline || mScrubbing ? 6 : 4);
            const int trackTop = (mTimeline.top + mTimeline.bottom - trackHeight) / 2;
            const RECT track{ mTimeline.left, trackTop, mTimeline.right, trackTop + trackHeight };
            FillRounded(dc, track, kTrack, trackHeight / 2);
            const int knobX = track.left + static_cast<int>(fraction * (track.right - track.left));
            if (open && knobX > track.left)
            {
                FillRounded(dc, RECT{ track.left, track.top, knobX, track.bottom }, kAccent, trackHeight / 2);
            }

            if (open)
            {
                const int knob = Scale(mHover == Hit::Timeline || mScrubbing ? 7 : 6);
                const int cy = (track.top + track.bottom) / 2;
                FillRounded(dc, RECT{ knobX - knob, cy - knob, knobX + knob, cy + knob }, RGB(255, 255, 255), knob);
            }

            // Time readout.
            const std::wstring timeText = FormatTime(time) + L" / " + FormatTime(duration);
            DrawTextIn(dc, timeText, mTimeText, open ? kText : kDimText, DT_RIGHT | DT_VCENTER);

            // Open.
            FillRounded(dc, mOpen, ButtonColor(Hit::Open), radius);
            DrawTextIn(dc, L"Open...", mOpen, kText, DT_CENTER | DT_VCENTER);

            mShownTime = std::floor(mPlayer.GetTime() * 10.0);
            mShownState = mPlayer.GetState();
        }
    };

    DWORD WINAPI WindowThread(void*)
    {
        // Editor.exe declares no DPI awareness, so without this every size here would be
        // in scaled pixels while the window is placed in physical ones.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        std::wstring initialPath;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            initialPath.swap(gPendingPath);
        }

        PlayerWindow window;
        window.Create(initialPath);
        if (window.Handle == nullptr)
        {
            return 1;
        }

        {
            std::lock_guard<std::mutex> lock(gMutex);
            gWindow = window.Handle;
            // Anything that arrived while the window was being created: a Shutdown, or
            // another Open that could not post to a window that did not exist yet.
            if (gShutdownRequested)
            {
                PostMessageW(gWindow, WM_CLOSE, 0, 0);
            }
            else if (!gPendingPath.empty())
            {
                PostMessageW(gWindow, WM_APP_OPEN, 0, 0);
            }
        }

        while (window.Pump())
        {
        }

        return 0;
    }
}

namespace VideoPlayerWindow
{
    bool Open(const std::wstring& path)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gPendingPath = path;
        gShutdownRequested = false;

        if (gThread != nullptr && WaitForSingleObject(gThread, 0) == WAIT_TIMEOUT)
        {
            // Still starting up (it will pick up the pending path itself) or already
            // running (it needs telling).
            if (gWindow != nullptr)
            {
                PostMessageW(gWindow, WM_APP_OPEN, 0, 0);
            }

            return true;
        }

        if (gThread != nullptr)
        {
            CloseHandle(gThread);
            gThread = nullptr;
        }

        gThread = CreateThread(nullptr, 0, &WindowThread, nullptr, 0, nullptr);
        return gThread != nullptr;
    }

    void Shutdown()
    {
        HANDLE thread = nullptr;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gShutdownRequested = true;
            if (gWindow != nullptr)
            {
                PostMessageW(gWindow, WM_CLOSE, 0, 0);
            }

            thread = gThread;
            gThread = nullptr;
        }

        if (thread != nullptr)
        {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
    }
}
