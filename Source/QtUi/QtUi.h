#pragma once
#include "UiTypes.h"
#include <cstddef>
#include <string>
#include <windows.h>
#include <commdlg.h>

using QtUiWindowFlags = int;
using QtUiTreeNodeFlags = int;
enum
{
    QtUiWindowFlags_None = 0,
    QtUiWindowFlags_NoCollapse = 1,
    QtUiWindowFlags_NoScrollbar = 2,
    QtUiWindowFlags_NoScrollWithMouse = 4,
    QtUiWindowFlags_NoBringToFrontOnFocus = 8,
    QtUiWindowFlags_MenuBar = 16,
    QtUiWindowFlags_AlwaysAutoResize = 32,
    QtUiWindowFlags_NoDecoration = 64,
    QtUiWindowFlags_NoDocking = 128,
    QtUiWindowFlags_NoFocusOnAppearing = 256,
    QtUiWindowFlags_NoMouseInputs = 512,
    QtUiWindowFlags_NoMove = 1024,
    QtUiWindowFlags_NoNav = 2048,
    QtUiWindowFlags_NoSavedSettings = 4096
};
enum
{
    QtUiTreeNodeFlags_DefaultOpen = 1,
    QtUiTreeNodeFlags_Leaf = 2,
    QtUiTreeNodeFlags_OpenOnArrow = 4,
    QtUiTreeNodeFlags_OpenOnDoubleClick = 8,
    QtUiTreeNodeFlags_Selected = 16,
    QtUiTreeNodeFlags_SpanLabelWidth = 32
};
enum
{
    QtUiCond_Always,
    QtUiCond_Appearing,
    QtUiCond_FirstUseEver
};
enum
{
    QtUiCol_Text,
    QtUiCol_Button,
    QtUiCol_ButtonActive
};
enum
{
    QtUiStyleVar_WindowPadding,
    QtUiStyleVar_ItemSpacing,
    QtUiStyleVar_FramePadding
};
enum
{
    QtUiColorEditFlags_Float = 1,
    QtUiColorEditFlags_HDR = 2
};
enum
{
    QtUiTableFlags_Borders = 1,
    QtUiTableFlags_RowBg = 2,
    QtUiTableFlags_SizingStretchProp = 4,
    QtUiTableColumnFlags_WidthFixed = 8
};
enum
{
    QtUiPopupFlags_MouseButtonRight = 1,
    QtUiPopupFlags_NoOpenOverItems = 2,
    QtUiSelectableFlags_AllowDoubleClick = 4
};
enum
{
    QtUiMouseButton_Left,
    QtUiMouseButton_Right,
    QtUiMouseButton_Middle
};
enum
{
    QtUiKey_1 = '1',
    QtUiKey_2 = '2',
    QtUiKey_3 = '3',
    QtUiKey_4 = '4',
    QtUiKey_C = 'C',
    QtUiKey_Delete = VK_DELETE,
    QtUiKey_F11 = VK_F11,
    QtUiKey_N = 'N',
    QtUiKey_O = 'O',
    QtUiKey_S = 'S',
    QtUiKey_V = 'V',
    QtUiKey_Y = 'Y',
    QtUiKey_Z = 'Z'
};
// One row of a field's type-ahead list. Both pointers must outlive the call.
struct UiCompletion
{
    const char *Text = nullptr;     // inserted when the row is chosen
    const char *Detail = nullptr;   // shown beside it, never inserted
};
struct QtUiIO
{
    UiVec2 MousePos, DisplaySize;
    bool KeyCtrl = false, KeyShift = false, WantTextInput = false;
    float DeltaTime = 0, Framerate = 0;
};
struct QtUiStyle
{
    UiVec2 ItemSpacing{6, 6}, WindowPadding{8, 8}, FramePadding{6, 4};
};
struct QtUiViewport
{
    unsigned ID = 1;
    UiVec2 WorkSize, WorkPos;
};
struct UiDrawList
{
    void AddLine(UiVec2, UiVec2, UiU32, float = 1);
    void AddRect(UiVec2, UiVec2, UiU32, float = 0, int = 0, float = 1);
    void AddRectFilled(UiVec2, UiVec2, UiU32, float = 0);
    void AddTriangle(UiVec2, UiVec2, UiVec2, UiU32, float = 1);
    void AddTriangleFilled(UiVec2, UiVec2, UiVec2, UiU32);
    void AddCircle(UiVec2, float, UiU32, int = 0, float = 1);
    void AddCircleFilled(UiVec2, float, UiU32, int = 0);
    void AddPolyline(const UiVec2 *, int, UiU32, int, float);
    void AddImage(UiTextureID, UiVec2, UiVec2, UiVec2 = {0, 0}, UiVec2 = {1, 1}, UiU32 = 0xffffffff);
};

// Retained Qt controls reconcile these declarations each frame. Signals queue values,
// never pointers into transient engine data. The caller consumes edits synchronously.
namespace QtUi
{
bool Initialize(HWND host, bool persistLayout = true);
void Shutdown();
void NewFrame();
void EndFrame();
HWND ViewportHandle();
void SetStandaloneGame(bool enabled);
bool IsStandaloneGame();
// Puts a play session on screen. With separateWindow the game gets its own top-level
// window and the editor's controls are frozen behind it; without it the game plays
// inside the editor viewport and every panel stays live, which is the mode to use while
// still editing. `title` names the separate window and is ignored otherwise.
void OpenGameWindow(bool separateWindow, const char *title);
void CloseGameWindow();
void ToggleGameFullscreen();
// Player display setting: 0 windowed with a width x height client area, 1 borderless
// over the monitor, 2 fullscreen with the monitor switched to width x height. Only the
// standalone game changes the monitor's mode; the editor's Play window treats 2 as
// borderless and in-viewport play keeps the editor's window. False when the request
// was not honoured as asked (a failed mode switch falls back to borderless).
bool SetGameDisplayMode(int mode, unsigned width, unsigned height);
// True when the game owns the keyboard: the window hosting it is in the foreground and
// no text field is taking input. In-viewport play answers for the editor window.
bool GameWindowHasFocus();
// True only while a surface other than the editor viewport owns presentation - the Play
// window, or a standalone build. Callers use it to decide whether the editor's own
// chrome and swap chain are still the ones in play, so in-viewport sessions are
// deliberately not counted here.
bool IsGameWindowOpen();
// True while a play session is running in any mode, including inside the viewport.
bool IsGamePlaying();
bool ConsumeGameCloseRequest();
// In-viewport play borrows the editor's own fullscreen viewport mode, which the editor
// owns. A game asking for fullscreen therefore raises a request here for the editor to
// consume, rather than QtUi resizing a window that is not the game's.
bool ConsumeGameFullscreenRequest();
HWND HostHandle();
// The editor's QMainWindow, as void* so this header stays Qt-free. Null before
// Initialize; used by tool windows that are written against Qt directly.
void *ShellWidget();
float FramebufferScale();
float FrameMilliseconds();
float EventMilliseconds();
const char *EventTypes();
void EventCounts(unsigned &updates, unsigned &layouts, unsigned &paints, const char *&topPainter,
                 unsigned &topPainterCount);
void ResizeHost(unsigned, unsigned);
BOOL OpenFileName(OPENFILENAMEA *);
BOOL SaveFileName(OPENFILENAMEA *);
bool CameraInputAllowed();
// True when the editor owns the keyboard: it is the foreground window and no text field
// has focus. Gate polled movement keys on this so they cannot drive the camera while the
// user is in another application or typing into a panel.
bool KeyboardCameraInputAllowed();
// Mouse-wheel notches banked over the scene viewport since the last call, positive when
// scrolled away from the user. Reading drains the accumulator.
float ConsumeViewportWheelDelta();
void RegisterIcon(UiTextureID, const wchar_t *);
struct TextureView
{
    HWND Window;
    UiTextureID Texture;
    unsigned Width, Height;
};
const TextureView *TextureViews(std::size_t &count);
bool Begin(const char *, bool * = nullptr, int = 0);
void End();
bool BeginChild(const char *, UiVec2 = {}, bool = false, int = 0);
void EndChild();
bool BeginMainMenuBar();
void EndMainMenuBar();
bool BeginMenuBar();
void EndMenuBar();
bool BeginMenu(const char *, bool = true);
void EndMenu();
bool MenuItem(const char *, const char * = nullptr, bool = false, bool = true);
bool MenuItem(const char *, const char *, bool *, bool = true);
bool Button(const char *, UiVec2 = {});
bool SmallButton(const char *);
bool Checkbox(const char *, bool *);
bool RadioButton(const char *, bool);
bool Selectable(const char *, bool = false, int = 0, UiVec2 = {});
bool CollapsingHeader(const char *, int = 0);
bool TreeNode(const char *);
bool TreeNodeEx(const char *, int = 0);
void TreePop();
bool TreeNodeEx(const void *, int, const char *, ...);
bool TreeNodeEx(const char *, int, const char *, ...);
bool InputText(const char *, char *, std::size_t, int = 0);
// Text field that reports only when Return is pressed, then clears itself.
// Console-style entry: the caller receives the line once, complete.
//
// `completions` attaches a type-ahead list: as the user types, matching entries
// drop down beneath the field the way a game console does. Matching is
// case-insensitive and by substring, not prefix, because a half-remembered name
// is the usual reason to want the list at all. Only `Text` is ever inserted;
// `Detail` sits beside it as a hint.
bool InputTextSubmit(const char *, char *, std::size_t,
                     const UiCompletion * = nullptr, int completionCount = 0);
// Read-only rich-text view backed by a single widget. A run of Text() calls
// retains one widget per line, which a log pane cannot afford; this stays one
// widget however many lines it shows. The document is only re-parsed when the
// html actually differs from what is on screen.
void TextView(const char *, const char *, UiVec2 = {}, bool scrollToBottom = false);
bool InputInt(const char *, int *, int = 1, int = 100, int = 0);
bool InputFloat(const char *, float *, float = 0, float = 0, const char * = "%.3f", int = 0);
bool DragFloat(const char *, float *, float = 1, float = 0, float = 0, const char * = "%.3f", int = 0);
bool DragFloat3(const char *, float *, float = 1, float = 0, float = 0, const char * = "%.3f", int = 0);
bool DragFloatRange2(const char *, float *, float *, float = 1, float = 0, float = 0, const char * = "%.3f",
                     const char * = nullptr, int = 0);
bool DragInt(const char *, int *, float = 1, int = 0, int = 0, const char * = "%d", int = 0);
bool SliderFloat(const char *, float *, float, float, const char * = "%.3f", int = 0);
bool SliderInt(const char *, int *, int, int, const char * = "%d", int = 0);
bool ColorEdit3(const char *, float *, int = 0);
bool ColorEdit4(const char *, float *, int = 0);
bool Combo(const char *, int *, const char *const *, int, int = -1);
bool Combo(const char *, int *, const char *, int = -1);
bool BeginCombo(const char *, const char *, int = 0);
void EndCombo();
bool BeginTabBar(const char *, int = 0);
void EndTabBar();
bool BeginTabItem(const char *, bool * = nullptr, int = 0);
void EndTabItem();
bool BeginTable(const char *, int, int = 0, UiVec2 = {}, float = 0);
void EndTable();
void TableSetupColumn(const char *, int = 0, float = 0, unsigned = 0);
void TableHeadersRow();
void TableNextRow(int = 0, float = 0);
bool TableSetColumnIndex(int);
void OpenPopup(const char *);
bool BeginPopupModal(const char *, bool * = nullptr, int = 0);
bool BeginPopupContextItem(const char * = nullptr, int = 1);
bool BeginPopupContextWindow(const char * = nullptr, int = 1);
void CloseCurrentPopup();
void EndPopup();
void BeginDisabled(bool = true);
void EndDisabled();
void BeginGroup();
void EndGroup();
void PushID(const char *);
void PushID(int);
void PopID();
void PushStyleColor(int, UiVec4);
void PopStyleColor(int = 1);
void PushStyleVar(int, UiVec2);
void PopStyleVar(int = 1);
void PushItemWidth(float);
void PopItemWidth();
void SetNextItemWidth(float);
void SameLine(float = 0, float = -1);
void Spacing();
void Dummy(UiVec2);
void Separator();
void SeparatorText(const char *);
void Text(const char *, ...);
void TextWrapped(const char *, ...);
void TextDisabled(const char *, ...);
void TextColored(UiVec4, const char *, ...);
void BulletText(const char *, ...);
void TextUnformatted(const char *, const char * = nullptr);
void SetTooltip(const char *, ...);
void SetItemTooltip(const char *, ...);
void ProgressBar(float, UiVec2 = {}, const char * = nullptr);
void Image(UiTextureID, UiVec2, UiVec2 = {0, 0}, UiVec2 = {1, 1});
bool ImageButton(const char *, UiTextureID, UiVec2);
bool IsItemHovered();
bool IsItemClicked(int = 0);
bool IsItemDeactivatedAfterEdit();
bool IsWindowHovered(int = 0);
bool IsKeyPressed(int, bool = true);
bool IsMouseDown(int);
bool IsMouseClicked(int);
bool IsMouseReleased(int);
bool IsMouseDoubleClicked(int);
QtUiIO &GetIO();
QtUiStyle &GetStyle();
UiVec4 GetStyleColorVec4(int);
QtUiViewport *GetMainViewport();
UiDrawList *GetWindowDrawList();
double GetTime();
UiVec2 GetContentRegionAvail();
UiVec2 GetCursorScreenPos();
float GetCursorPosX();
void SetCursorPosX(float);
float GetFrameHeightWithSpacing();
void SetNextWindowSize(UiVec2, int = 0);
void SetNextWindowPos(UiVec2, int = 0, UiVec2 = {});
void SetNextWindowSizeConstraints(UiVec2, UiVec2);
void SetNextWindowViewport(unsigned);
void SetNextWindowBgAlpha(float);
void SetItemDefaultFocus();
float GetScrollY();
float GetScrollMaxY();
void SetScrollHereY(float = 0.5f);
} // namespace QtUi
