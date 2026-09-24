// QtUiHeadless - the QtUi API with no Qt behind it, for the packaged game's renderer.
//
// A shipped game has no editor chrome, so it does not ship Qt: Renderer_DX12's game
// runtime flavour (PteroGameRuntime=true in Renderer_DX12.vcxproj) compiles this file in
// place of QtUi.cpp. Qt cannot simply be delay-loaded instead - the renderer imports Qt
// data symbols (staticMetaObject), which the linker refuses to delay.
//
// What a standalone game actually uses from QtUi is kept and implemented on Win32: the
// host window, per-frame mouse/keyboard state, the "Viewport" rectangle the game UI maps
// the cursor into, and the display-mode/fullscreen handling. Every editor widget call is
// a no-op that reports "nothing happened", so the editor code compiled into the same DLL
// runs through harmlessly if it is ever reached.

#include "QtUi.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <string>

namespace
{
HWND host = nullptr;
bool standaloneGame = false;
bool gameFullscreen = false;
WINDOWPLACEMENT gamePlacement{sizeof(WINDOWPLACEMENT)};
LONG_PTR gameWindowStyle = 0;
bool gameDisplayChanged = false;
wchar_t gameDisplayDevice[CCHDEVICENAME]{};
bool gameCloseRequested = false;
bool gameFullscreenRequested = false;

QtUiIO io;
QtUiStyle style;
QtUiViewport viewport;
UiDrawList drawList;

bool keys[256]{};
bool pressed[256]{};
bool mouseDown[3]{};
bool mouseClicked[3]{};
bool mouseReleased[3]{};
bool mouseDouble[3]{};
std::chrono::steady_clock::time_point clickTime[3]{};

const auto startTime = std::chrono::steady_clock::now();
auto lastFrameTime = startTime;
float uiFrameMs = 0;

// Depth of Begin("Viewport") scopes; any other window is never "open" here.
int viewportScopes = 0;

bool HostIsForeground()
{
    return host && GetAncestor(GetForegroundWindow(), GA_ROOT) == GetAncestor(host, GA_ROOT);
}

// Host client area in screen coordinates.
RECT HostClientScreenRect()
{
    RECT rect{};
    if (!host)
        return rect;
    GetClientRect(host, &rect);
    POINT origin{0, 0};
    ClientToScreen(host, &origin);
    OffsetRect(&rect, origin.x, origin.y);
    return rect;
}

bool CoverGameMonitor()
{
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    if (!gameFullscreen) {
        gameWindowStyle = GetWindowLongPtrW(host, GWL_STYLE);
        GetWindowPlacement(host, &gamePlacement);
    }
    SetWindowLongPtrW(host, GWL_STYLE, gameWindowStyle & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(host, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
        monitor.rcMonitor.right-monitor.rcMonitor.left, monitor.rcMonitor.bottom-monitor.rcMonitor.top,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    gameFullscreen = true;
    return true;
}

void RestoreGameDisplay()
{
    if (!gameDisplayChanged) return;
    ChangeDisplaySettingsExW(gameDisplayDevice, nullptr, nullptr, 0, nullptr);
    gameDisplayChanged = false;
}

void UncoverGameMonitor()
{
    RestoreGameDisplay();
    if (!gameFullscreen) return;
    SetWindowLongPtrW(host, GWL_STYLE, gameWindowStyle);
    SetWindowPlacement(host, &gamePlacement);
    SetWindowPos(host, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    gameFullscreen = false;
}
} // namespace

void UiDrawList::AddLine(UiVec2, UiVec2, UiU32, float) {}
void UiDrawList::AddRect(UiVec2, UiVec2, UiU32, float, int, float) {}
void UiDrawList::AddRectFilled(UiVec2, UiVec2, UiU32, float) {}
void UiDrawList::AddTriangle(UiVec2, UiVec2, UiVec2, UiU32, float) {}
void UiDrawList::AddTriangleFilled(UiVec2, UiVec2, UiVec2, UiU32) {}
void UiDrawList::AddCircle(UiVec2, float, UiU32, int, float) {}
void UiDrawList::AddCircleFilled(UiVec2, float, UiU32, int) {}
void UiDrawList::AddPolyline(const UiVec2 *, int, UiU32, int, float) {}
void UiDrawList::AddImage(UiTextureID, UiVec2, UiVec2, UiVec2, UiVec2, UiU32) {}

namespace QtUi
{
bool Initialize(HWND owner, bool)
{
    host = owner;
    return true;
}
void Shutdown()
{
    RestoreGameDisplay();
    host = nullptr;
}
void NewFrame()
{
    const auto frameStart = std::chrono::steady_clock::now();
    io.DeltaTime = std::chrono::duration<float>(frameStart - lastFrameTime).count();
    lastFrameTime = frameStart;
    io.Framerate = io.DeltaTime > 0 ? 1 / io.DeltaTime : 0;

    POINT cursor{};
    GetCursorPos(&cursor);
    io.MousePos = {float(cursor.x), float(cursor.y)};

    const bool active = HostIsForeground();
    for (int k = 0; k < 256; ++k)
    {
        const bool down = active && (GetAsyncKeyState(k) & 0x8000);
        pressed[k] = pressed[k] || (down && !keys[k]);
        keys[k] = down;
    }
    const int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
    for (int i = 0; i < 3; ++i)
    {
        const bool down = keys[buttons[i]];
        const bool clicked = down && !mouseDown[i];
        mouseClicked[i] = mouseClicked[i] || clicked;
        mouseReleased[i] = mouseReleased[i] || (!down && mouseDown[i]);
        if (clicked)
        {
            mouseDouble[i] = frameStart - clickTime[i] < std::chrono::milliseconds(GetDoubleClickTime());
            clickTime[i] = frameStart;
        }
        mouseDown[i] = down;
    }
    io.KeyCtrl = keys[VK_CONTROL];
    io.KeyShift = keys[VK_SHIFT];
    io.WantTextInput = false;

    const RECT client = HostClientScreenRect();
    viewport.WorkPos = {float(client.left), float(client.top)};
    viewport.WorkSize = {float(client.right - client.left), float(client.bottom - client.top)};
    io.DisplaySize = viewport.WorkSize;
    viewportScopes = 0;
    uiFrameMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
}
void EndFrame()
{
    std::fill(std::begin(pressed), std::end(pressed), false);
    std::fill(std::begin(mouseClicked), std::end(mouseClicked), false);
    std::fill(std::begin(mouseReleased), std::end(mouseReleased), false);
    std::fill(std::begin(mouseDouble), std::end(mouseDouble), false);
}
HWND ViewportHandle() { return host; }
void SetStandaloneGame(bool enabled) { standaloneGame = enabled; }
bool IsStandaloneGame() { return standaloneGame; }
void OpenGameWindow(bool, const char *) {}
void CloseGameWindow()
{
    RestoreGameDisplay();
    if (host) PostMessageW(host, WM_CLOSE, 0, 0);
}
void ToggleGameFullscreen()
{
    if (!host) return;
    if (gameFullscreen) UncoverGameMonitor();
    else CoverGameMonitor();
}
bool SetGameDisplayMode(int mode, unsigned width, unsigned height)
{
    if (!host) return false;
    if (mode == 1) { RestoreGameDisplay(); return CoverGameMonitor(); }
    if (mode == 2) {
        MONITORINFOEXW monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
        DEVMODEW display{};
        display.dmSize = sizeof(display);
        display.dmPelsWidth = width;
        display.dmPelsHeight = height;
        display.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (ChangeDisplaySettingsExW(monitor.szDevice, &display, nullptr, CDS_FULLSCREEN, nullptr) != DISP_CHANGE_SUCCESSFUL) {
            RestoreGameDisplay();
            CoverGameMonitor();
            return false;
        }
        wcscpy_s(gameDisplayDevice, monitor.szDevice);
        gameDisplayChanged = true;
        return CoverGameMonitor();
    }
    UncoverGameMonitor();
    ShowWindow(host, SW_RESTORE);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const DWORD windowStyle = static_cast<DWORD>(GetWindowLongPtrW(host, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(host, GWL_EXSTYLE));
    RECT frame{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&frame, windowStyle, GetMenu(host) != nullptr, exStyle);
    const RECT& work = monitor.rcWork;
    const int frameWidth = (std::min)(int(frame.right - frame.left), int(work.right - work.left));
    const int frameHeight = (std::min)(int(frame.bottom - frame.top), int(work.bottom - work.top));
    SetWindowPos(host, nullptr, work.left + (work.right - work.left - frameWidth) / 2,
        work.top + (work.bottom - work.top - frameHeight) / 2, frameWidth, frameHeight,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    return true;
}
bool GameWindowHasFocus() { return standaloneGame && HostIsForeground(); }
bool IsGameWindowOpen() { return standaloneGame; }
bool IsGamePlaying() { return standaloneGame; }
bool ConsumeGameCloseRequest() { bool result=gameCloseRequested; gameCloseRequested=false; return result; }
bool ConsumeGameFullscreenRequest() { bool result=gameFullscreenRequested; gameFullscreenRequested=false; return result; }
HWND HostHandle() { return host; }
void *ShellWidget() { return nullptr; }
float FramebufferScale() { return 1.0f; }
float FrameMilliseconds() { return uiFrameMs; }
float EventMilliseconds() { return 0.0f; }
const char *EventTypes() { return "-"; }
void EventCounts(unsigned &updates, unsigned &layouts, unsigned &paints, const char *&topPainter,
                 unsigned &topPainterCount)
{
    updates = layouts = paints = topPainterCount = 0;
    topPainter = "-";
}
void ResizeHost(unsigned, unsigned) {}
BOOL OpenFileName(OPENFILENAMEA *) { return FALSE; }
BOOL SaveFileName(OPENFILENAMEA *) { return FALSE; }
bool CameraInputAllowed() { return GameWindowHasFocus(); }
bool KeyboardCameraInputAllowed() { return GameWindowHasFocus(); }
float ConsumeViewportWheelDelta() { return 0.0f; }
void RegisterIcon(UiTextureID, const wchar_t *) {}
// No editor chrome in a packaged game, so no styles either.
const std::vector<StyleEntry> &AvailableStyles(bool)
{
    static const std::vector<StyleEntry> none;
    return none;
}
bool LoadStyle(const char *) { return false; }
bool ReloadStyle() { return false; }
const char *CurrentStyleFile() { return ""; }
const char *StyleError() { return ""; }
const char *StylesDirectory() { return ""; }
bool DuplicateStyle(const char *) { return false; }
float StyleMetric(const char *, float fallback) { return fallback; }
void SetNextItemIcon(const char *) {}
bool IconButton(const char *, const char *) { return false; }
const TextureView *TextureViews(std::size_t &count)
{
    count = 0;
    return nullptr;
}

// Only the scene viewport exists: the game UI overlay asks for its rectangle so it can map
// the cursor into UI pixels. Every other window is closed.
bool Begin(const char *name, bool *, int)
{
    if (name && std::strcmp(name, "Viewport") == 0)
    {
        ++viewportScopes;
        return true;
    }
    return false;
}
void End()
{
    if (viewportScopes > 0) --viewportScopes;
}
bool BeginChild(const char *, UiVec2, bool, int) { return false; }
void EndChild() {}
bool BeginMainMenuBar() { return false; }
void EndMainMenuBar() {}
bool BeginMenuBar() { return false; }
void EndMenuBar() {}
bool BeginMenu(const char *, bool) { return false; }
void EndMenu() {}
bool MenuItem(const char *, const char *, bool, bool) { return false; }
bool MenuItem(const char *, const char *, bool *, bool) { return false; }
bool Button(const char *, UiVec2) { return false; }
bool SmallButton(const char *) { return false; }
bool Checkbox(const char *, bool *) { return false; }
bool RadioButton(const char *, bool) { return false; }
bool Selectable(const char *, bool, int, UiVec2) { return false; }
bool CollapsingHeader(const char *, int) { return false; }
bool TreeNode(const char *) { return false; }
bool TreeNodeEx(const char *, int) { return false; }
void TreePop() {}
bool TreeNodeEx(const void *, int, const char *, ...) { return false; }
bool TreeNodeEx(const char *, int, const char *, ...) { return false; }
bool InputText(const char *, char *, std::size_t, int) { return false; }
bool InputTextSubmit(const char *, char *, std::size_t, const UiCompletion *, int) { return false; }
void TextView(const char *, const char *, UiVec2, bool) {}
bool InputInt(const char *, int *, int, int, int) { return false; }
bool InputFloat(const char *, float *, float, float, const char *, int) { return false; }
bool DragFloat(const char *, float *, float, float, float, const char *, int) { return false; }
bool DragFloat3(const char *, float *, float, float, float, const char *, int) { return false; }
bool DragFloatRange2(const char *, float *, float *, float, float, float, const char *, const char *, int) { return false; }
bool DragInt(const char *, int *, float, int, int, const char *, int) { return false; }
bool SliderFloat(const char *, float *, float, float, const char *, int) { return false; }
bool SliderInt(const char *, int *, int, int, const char *, int) { return false; }
bool ColorEdit3(const char *, float *, int) { return false; }
bool ColorEdit4(const char *, float *, int) { return false; }
bool Combo(const char *, int *, const char *const *, int, int) { return false; }
bool Combo(const char *, int *, const char *, int) { return false; }
bool BeginCombo(const char *, const char *, int) { return false; }
void EndCombo() {}
bool BeginTabBar(const char *, int) { return false; }
void EndTabBar() {}
bool BeginTabItem(const char *, bool *, int) { return false; }
void EndTabItem() {}
bool BeginTable(const char *, int, int, UiVec2, float) { return false; }
void EndTable() {}
void TableSetupColumn(const char *, int, float, unsigned) {}
void TableHeadersRow() {}
void TableNextRow(int, float) {}
bool TableSetColumnIndex(int) { return false; }
void OpenPopup(const char *) {}
bool BeginPopupModal(const char *, bool *, int) { return false; }
bool BeginPopupContextItem(const char *, int) { return false; }
bool BeginPopupContextWindow(const char *, int) { return false; }
void CloseCurrentPopup() {}
void EndPopup() {}
void BeginDisabled(bool) {}
void EndDisabled() {}
void BeginGroup() {}
void EndGroup() {}
void PushID(const char *) {}
void PushID(int) {}
void PopID() {}
void PushStyleColor(int, UiVec4) {}
void PopStyleColor(int) {}
void PushStyleVar(int, UiVec2) {}
void PopStyleVar(int) {}
void PushItemWidth(float) {}
void PopItemWidth() {}
void SetNextItemWidth(float) {}
void SameLine(float, float) {}
void Spacing() {}
void Dummy(UiVec2) {}
void Separator() {}
void SeparatorText(const char *) {}
void Text(const char *, ...) {}
void TextWrapped(const char *, ...) {}
void TextDisabled(const char *, ...) {}
void TextColored(UiVec4, const char *, ...) {}
void BulletText(const char *, ...) {}
void TextUnformatted(const char *, const char *) {}
void SetTooltip(const char *, ...) {}
void SetItemTooltip(const char *, ...) {}
void ProgressBar(float, UiVec2, const char *) {}
void Image(UiTextureID, UiVec2, UiVec2, UiVec2) {}
bool ImageButton(const char *, UiTextureID, UiVec2) { return false; }
bool IsItemHovered() { return false; }
bool IsItemClicked(int) { return false; }
bool IsItemDeactivatedAfterEdit() { return false; }
bool IsWindowHovered(int)
{
    if (viewportScopes == 0 || !HostIsForeground())
        return false;
    POINT cursor{};
    GetCursorPos(&cursor);
    const RECT client = HostClientScreenRect();
    return PtInRect(&client, cursor) != FALSE && WindowFromPoint(cursor) == host;
}
bool IsKeyPressed(int k, bool) { return k >= 0 && k < 256 && pressed[k]; }
bool IsMouseDown(int b) { return b >= 0 && b < 3 && mouseDown[b]; }
bool IsMouseClicked(int b) { return b >= 0 && b < 3 && mouseClicked[b]; }
bool IsMouseReleased(int b) { return b >= 0 && b < 3 && mouseReleased[b]; }
bool IsMouseDoubleClicked(int b) { return b >= 0 && b < 3 && mouseDouble[b]; }
QtUiIO &GetIO() { return io; }
QtUiStyle &GetStyle() { return style; }
UiVec4 GetStyleColorVec4(int c)
{
    return c == QtUiCol_Text ? UiVec4(.95f, .92f, .89f, 1) : UiVec4(.78f, .24f, .05f, 1);
}
QtUiViewport *GetMainViewport() { return &viewport; }
UiDrawList *GetWindowDrawList() { return &drawList; }
double GetTime()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
}
UiVec2 GetContentRegionAvail()
{
    const RECT client = HostClientScreenRect();
    return UiVec2(float((std::max)(1L, client.right - client.left)), float((std::max)(1L, client.bottom - client.top)));
}
UiVec2 GetCursorScreenPos()
{
    const RECT client = HostClientScreenRect();
    return UiVec2(float(client.left), float(client.top));
}
float GetCursorPosX() { return 0; }
void SetCursorPosX(float) {}
float GetFrameHeightWithSpacing() { return 28.0f; }
void SetNextWindowSize(UiVec2, int) {}
void SetNextWindowPos(UiVec2, int, UiVec2) {}
void SetNextWindowSizeConstraints(UiVec2, UiVec2) {}
void SetNextWindowViewport(unsigned) {}
void SetNextWindowBgAlpha(float) {}
void SetItemDefaultFocus() {}
float GetScrollY() { return 0; }
float GetScrollMaxY() { return 0; }
void SetScrollHereY(float) {}
} // namespace QtUi
