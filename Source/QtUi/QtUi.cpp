#include "QtUi.h"
#pragma warning(push)
#pragma warning(disable : 4996) // Qt 6.11 overrides its own deprecated event hook.
#include <QtWidgets/QtWidgets>
#include <QtGui/QPainter>
#include <QtGui/QStandardItemModel>
#include <QtCore/QElapsedTimer>
#pragma warning(pop)
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <algorithm>
#include <cstdarg>
#include <cfloat>
#include <map>
#include <memory>
#include <vector>

namespace
{
QString label(const char *s)
{
    return QString::fromUtf8(s ? s : "").section("##", 0, 0);
}
QColor color(UiU32 c)
{
    return QColor(c & 255, (c >> 8) & 255, (c >> 16) & 255, (c >> 24) & 255);
}
QColor color(UiVec4 c)
{
    return QColor::fromRgbF(std::clamp(c.x, 0.f, 1.f), std::clamp(c.y, 0.f, 1.f), std::clamp(c.z, 0.f, 1.f),
                            std::clamp(c.w, 0.f, 1.f));
}
QString format(const char *f, va_list args)
{
    return QString::vasprintf(f, args);
}
struct Node
{
    QPointer<QWidget> widget;
    QPointer<QAction> action;
    int seen = 0;
    bool fired = false, edited = false, closed = false, expanded = false, populated = false;
    // Frames of content-driven sizing still owed to a freshly opened auto-resizing window.
    int autoSize = 0;
    // A framed tool window, which measures its content again every time it is opened.
    bool autoFit = false;
    // Smallest size content sizing may settle on, taken from whatever the caller asked for
    // with SetNextWindowSize. A panel that opens roomy on purpose keeps its room.
    QSize sizeFloor;
    QVariant value;
    // Last known cell, so reconciling a widget costs no layout search.
    QPointer<QGridLayout> cell;
    int row = -1, col = -1;
    // Takes the whole row of its grid rather than one column (section bodies).
    bool span = false;
};
// What a scope was opened for. Sections are the indented bodies that collapsing headers
// and tree nodes put their content in; they exist only to indent and are otherwise
// invisible to the caller: they share their parent's key and serial counter, so a
// control keeps its identity whether or not it sits in one.
enum class Section
{
    None,
    Header,
    Tree
};
constexpr int kSectionIndent = 16;
struct Scope
{
    QString key;
    QWidget *body = nullptr;
    QMenu *menu = nullptr;
    QMenuBar *bar = nullptr;
    QGridLayout *grid = nullptr;
    QTabWidget *tabs = nullptr;
    int row = 0, col = 0, serial = 0, header = 0;
    bool same = false, table = false;
    Node *node = nullptr;
    Section section = Section::None;
    // Size of the ID stack when a header section opened. A header only runs until the
    // next header at the same ID depth, or until the ID it was pushed under is popped.
    std::size_t depth = 0;
};
// Wheel notches collected over the scene viewport since the renderer last drained them.
// The renderer polls input with GetAsyncKeyState, which has nothing to report a wheel
// with, so the one place that does see the wheel - Qt's event delivery - banks it here.
int viewportWheelAngle = 0;
class Surface : public QWidget
{
  public:
    using QWidget::QWidget;
    // Only the scene viewport feeds the camera; the debug-image surfaces do not.
    bool banksWheelForCamera = false;
    QPaintEngine *paintEngine() const override
    {
        return nullptr;
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
    }
    void wheelEvent(QWheelEvent *event) override
    {
        if (!banksWheelForCamera)
        {
            QWidget::wheelEvent(event);
            return;
        }
        viewportWheelAngle += event->angleDelta().y();
        event->accept();
    }
};
class Overlay : public QWidget
{
  public:
    // Commands accumulate during the frame; EndFrame hands them to "painted". paintEvent
    // runs asynchronously, so it must read a list NewFrame will not clear underneath it.
    std::vector<std::function<void(QPainter &)>> commands, painted;
    explicit Overlay(QWidget *owner)
        : QWidget(owner,
                  Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-mapToGlobal(QPoint(0, 0)));
        for (auto &command : painted)
            command(p);
    }
};
// Diagnostic: Qt repaints and relayouts are driven by its own posted events, not by
// WM_PAINT, so they are invisible from the Win32 pump even though that is where they are
// paid for. This counts them per frame and names whoever is churning the most.
class EventCounter : public QObject
{
  public:
    unsigned updates = 0, layouts = 0, paints = 0;
    QHash<QString, unsigned> painters;
    // Snapshot of the last complete loop iteration. The host pump runs before NewFrame,
    // so the interesting events land outside the render call and have to be carried over.
    unsigned lastUpdates = 0, lastLayouts = 0, lastPaints = 0;
    QHash<QString, unsigned> lastPainters;
    // Every event type seen, so the thing *triggering* the repaints can be named rather
    // than guessed at: a FontChange or StyleChange arriving each frame points straight at
    // an unguarded setter, where a Resize points at a layout that will not settle.
    QHash<int, unsigned> types, lastTypes;

    void reset()
    {
        lastUpdates = updates;
        lastLayouts = layouts;
        lastPaints = paints;
        lastPainters = painters;
        lastTypes = types;
        updates = layouts = paints = 0;
        painters.clear();
        types.clear();
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        ++types[int(event->type())];
        switch (event->type())
        {
        case QEvent::UpdateRequest:
            ++updates;
            break;
        case QEvent::LayoutRequest:
            ++layouts;
            break;
        case QEvent::Paint: {
            ++paints;
            const QString name = watched->objectName().isEmpty()
                                     ? QString::fromUtf8(watched->metaObject()->className())
                                     : watched->objectName();
            ++painters[name];
            break;
        }
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
};
EventCounter *events = nullptr;
std::unique_ptr<QApplication> app;
QMainWindow *shell = nullptr;
Surface *surface = nullptr;
bool standaloneGame = false;
bool gameFullscreen = false;
WINDOWPLACEMENT gamePlacement{sizeof(WINDOWPLACEMENT)};
LONG_PTR gameWindowStyle = 0;
bool gameCloseRequested = false;
bool gameFullscreenRequested = false;
bool playWindowActive = false;
// A play session running inside the editor viewport. It owns input and the viewport
// image, but not the window: the editor keeps its menus, panels and swap chain, which
// is exactly what separates this mode from the Play window.
bool viewportGame = false;
class PlayWindow : public QWidget {
protected:
    void closeEvent(QCloseEvent* event) override { gameCloseRequested=true; event->ignore(); }
};
PlayWindow* playWindow = nullptr;
Surface* playSurface = nullptr;
Surface* ActiveSurface() { return playWindowActive ? playSurface : surface; }
Overlay *overlay = nullptr;
HWND host = nullptr;
bool saveLayout = true;
std::map<QString, std::unique_ptr<Node>> nodes;
std::vector<Scope> scopes;
std::vector<QString> ids;
// Key prefixes of menus whose contents were not walked this frame.
std::vector<QString> skippedMenus;
std::vector<bool> disabled;
std::vector<std::pair<int, UiVec4>> colors;
std::vector<std::pair<int, UiVec2>> styles;
std::vector<float> widths;
std::map<UiTextureID, QPixmap> icons;
std::vector<QtUi::TextureView> textureViews;
Node *last = nullptr;
bool lastChanged = false;
QtUiIO io;
QtUiStyle style;
QtUiViewport viewport;
UiDrawList drawList;
QElapsedTimer timer;
qint64 lastTime = 0;
qint64 uiFrameStart = 0;
float uiFrameMs = 0;
float uiEventMs = 0;
int frame = 0;
bool mouseDown[3]{}, mouseClicked[3]{}, mouseReleased[3]{}, mouseDouble[3]{};
bool keys[256]{}, pressed[256]{};
qint64 clickTime[3]{};
UiVec2 nextSize{}, nextPos{}, nextPivot{}, minSize{}, maxSize{};
float nextAlpha = 1, nextWidth = 0;
bool positionSet = false;
QString key(const char *name)
{
    QString result = scopes.empty() ? QString() : scopes.back().key;
    for (const auto &id : ids)
        result += "/id:" + id;
    return result + "/" + QString::fromUtf8(name ? name : "");
}
Node &node(const QString &id)
{
    auto &n = nodes[id];
    if (!n)
        n = std::make_unique<Node>();
    n->seen = frame;
    return *n;
}
bool enabled()
{
    return std::none_of(disabled.begin(), disabled.end(), [](bool d) { return d; });
}
// Set when any menu action fires. Activating an item also closes its menu, so the frame
// that would deliver the click is exactly the frame BeginMenu wants to skip; without this
// the pending "fired" flag sits unconsumed until the user opens the menu a second time.
bool actionFired = false;
// A spin box stores its value and bounds rounded to the precision it displays, so a raw
// comparison against the caller's float differs every frame for anything carrying more
// digits than the format string shows.
double roundToDecimals(double v, int decimals)
{
    const double scale = std::pow(10.0, double(std::clamp(decimals, 0, 9)));
    return std::round(v * scale) / scale;
}
bool nearlySame(double a, double b)
{
    return std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}
// Resolution of a floating-point slider track. Fine enough that a sweep feels continuous,
// coarse enough that the thumb still lands on round numbers.
constexpr int kSliderSteps = 1000;
int sliderPosition(double value, double low, double high)
{
    if (!(high > low))
        return 0;
    const double t = (std::clamp(value, low, high) - low) / (high - low);
    return int(std::lround(t * kSliderSteps));
}
bool take(Node &n)
{
    bool result = n.fired;
    n.fired = false;
    last = &n;
    lastChanged = result;
    return result;
}
QGridLayout *grid(QWidget *w)
{
    if (auto *g = qobject_cast<QGridLayout *>(w->layout()))
        return g;
    auto *g = new QGridLayout(w);
    g->setContentsMargins(8, 8, 8, 8);
    g->setSpacing(6);
    g->setAlignment(Qt::AlignTop);
    return g;
}
void push(QWidget *w, const QString &id, Node *n = nullptr)
{
    Scope s;
    s.key = id;
    s.body = w;
    s.grid = grid(w);
    s.node = n;
    scopes.push_back(s);
}
QString serial(const char *type)
{
    return key((QString(type) + QString::number(scopes.back().serial++)).toUtf8().constData());
}
void place(Node &n)
{
    if (!n.widget || scopes.empty())
        return;
    Scope &s = scopes.back();
    if (s.menu)
    {
        if (!n.action)
        {
            auto *a = new QWidgetAction(s.menu);
            a->setDefaultWidget(n.widget);
            s.menu->addAction(a);
            n.action = a;
        }
        n.action->setVisible(true);
    }
    else if (s.grid)
    {
        const int row = s.same ? std::max(0, s.row - 1) : s.row;
        if (!s.same)
            s.col = 0;
        // Trust the remembered cell instead of QGridLayout::indexOf, which scans the
        // whole layout: at one scan per widget per frame that is quadratic in panel size.
        if (n.cell != s.grid || n.row != row || n.col != s.col)
        {
            s.grid->removeWidget(n.widget);
            s.grid->addWidget(n.widget, row, s.col, 1, n.span ? -1 : 1);
            n.cell = s.grid;
            n.row = row;
            n.col = s.col;
        }
        ++s.col;
        if (!s.same)
            ++s.row;
        s.same = false;
    }
    n.widget->setEnabled(enabled());
    if (nextWidth > 0)
    {
        n.widget->setMaximumWidth(int(nextWidth));
        nextWidth = 0;
    }
    else if (!widths.empty() && widths.back() > 0)
        n.widget->setMaximumWidth(int(widths.back()));
    else
        n.widget->setMaximumWidth(QWIDGETSIZE_MAX);
    n.widget->show();
    last = &n;
    lastChanged = false;
}
template <class T> T *control(Node &n)
{
    if (!n.widget)
        n.widget = new T;
    auto *w = static_cast<T *>(n.widget.data());
    place(n);
    return w;
}
// The innermost scope that is not a section: the window, child, popup or tab the caller
// actually opened, which is what window-level queries are about.
Scope &owner()
{
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
        if (it->section == Section::None)
            return *it;
    return scopes.front();
}
void openSection(const QString &id, Section kind)
{
    Scope &parent = scopes.back();
    if (!parent.grid || parent.menu || parent.table)
        return;
    // Placing the body would make it the "last item", but whoever called TreeNode or
    // CollapsingHeader asks IsItemClicked and BeginPopupContextItem about the header.
    Node *const header = last;
    const bool headerChanged = lastChanged;
    Node &n = node(id + "#section");
    if (!n.widget)
    {
        n.widget = new QWidget;
        grid(n.widget)->setContentsMargins(kSectionIndent, 0, 0, 0);
        n.span = true;
    }
    place(n);
    last = header;
    lastChanged = headerChanged;
    Scope s;
    s.key = parent.key;
    s.body = n.widget;
    s.grid = grid(n.widget);
    s.serial = parent.serial;
    s.section = kind;
    s.depth = ids.size();
    scopes.push_back(s);
}
void closeSection()
{
    const int serial = scopes.back().serial;
    scopes.pop_back();
    if (!scopes.empty())
        scopes.back().serial = serial;
}
// Header sections run until the next header at the same ID depth or shallower.
void closeHeaders(std::size_t depth)
{
    while (!scopes.empty() && scopes.back().section == Section::Header && scopes.back().depth >= depth)
        closeSection();
}
// One entry per open TreeNode: the scope count once its body (if any) was pushed.
struct TreeLevel
{
    std::size_t scopeCount;
    bool body;
};
std::vector<TreeLevel> trees;
template <class T> T *labeledControl(Node &n, const char *name)
{
    if (!n.widget)
    {
        n.widget = new QWidget;
        n.widget->setProperty("uiControl", true);
        auto *layout = new QGridLayout(n.widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto *caption = new QLabel(label(name));
        caption->setObjectName("caption");
        caption->setWordWrap(true);
        caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        caption->setMinimumWidth(52);
        auto *value = new T;
        value->setObjectName("value");
        value->setAccessibleName(label(name));
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        value->setMinimumWidth(64);
        layout->addWidget(caption, 0, 0);
        layout->addWidget(value, 0, 1);
        layout->setColumnStretch(0, 1);
        layout->setColumnStretch(1, 1);
        caption->setVisible(!label(name).isEmpty());
    }
    place(n);
    return n.widget->findChild<T *>("value");
}
// A bounded numeric field: a slider to sweep the range next to a spin box for exact entry.
//
// The spin box stays authoritative. The slider only proposes a value, so its coarse
// integer resolution never becomes the precision the setting is stored at - typing 0.125
// into the box keeps 0.125 even though the slider cannot land on it exactly.
template <class T> T *labeledSlider(Node &n, const char *name, QSlider *&outTrack)
{
    if (!n.widget)
    {
        n.widget = new QWidget;
        n.widget->setProperty("uiControl", true);
        auto *layout = new QGridLayout(n.widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        auto *caption = new QLabel(label(name));
        caption->setObjectName("caption");
        caption->setWordWrap(true);
        caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        caption->setMinimumWidth(52);
        auto *track = new QSlider(Qt::Horizontal);
        track->setObjectName("track");
        track->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        track->setMinimumWidth(60);
        auto *value = new T;
        value->setObjectName("value");
        value->setAccessibleName(label(name));
        // Fixed rather than Ignored: the box has to stay wide enough to read while the
        // slider takes whatever width is left, which is what keeps this usable in the
        // narrow docks as well as in the roomy settings dialogs.
        value->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        value->setMinimumWidth(72);
        value->setMaximumWidth(104);
        layout->addWidget(caption, 0, 0);
        layout->addWidget(track, 0, 1);
        layout->addWidget(value, 0, 2);
        layout->setColumnStretch(0, 1);
        layout->setColumnStretch(1, 2);
        layout->setColumnStretch(2, 0);
        caption->setVisible(!label(name).isEmpty());
    }
    place(n);
    outTrack = n.widget->findChild<QSlider *>("track");
    return n.widget->findChild<T *>("value");
}
// Several Qt setters do real work on every call regardless of whether the value changed:
// setStyleSheet repolishes the entire style, setIcon invalidates geometry, setToolTip
// posts an event. An immediate-mode facade re-asserts all of them every frame, so each
// one has to be guarded or the editor pays for a full restyle per widget per frame.
void applyStyleSheet(QWidget *w, const QString &sheet)
{
    if (w && w->styleSheet() != sheet)
        w->setStyleSheet(sheet);
}
void applyToolTip(QWidget *w, const QString &tip)
{
    if (w && w->toolTip() != tip)
        w->setToolTip(tip);
}
void textWidget(const QString &text, bool wrap = false, const QColor &tint = QColor())
{
    Node &n = node(serial("text"));
    auto *w = control<QLabel>(n);
    if (w->text() != text)
        w->setText(text);
    // Guard every one of these. QLabel::setWordWrap has no early-out: it calls
    // updateLabel() unconditionally, which invalidated and repainted every label in the
    // editor on every frame. That is ruinous for the translucent HUD, where each repaint
    // pushes the whole layered-window surface through DWM.
    if (w->wordWrap() != wrap)
        w->setWordWrap(wrap);
    if (w->textFormat() != Qt::PlainText)
        w->setTextFormat(Qt::PlainText);
    if (w->textInteractionFlags() != Qt::TextSelectableByMouse)
        w->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const QColor tone = tint.isValid() ? tint : QColor("#f2ebe3");
    if (w->palette().color(QPalette::WindowText) != tone)
    {
        QPalette p = w->palette();
        p.setColor(QPalette::WindowText, tone);
        w->setPalette(p);
    }
}
QWidget *makeBody(QWidget *parent, bool scroll)
{
    auto *body = new QWidget;
    if (scroll)
    {
        auto *area = new QScrollArea(parent);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setWidget(body);
        auto *layout = new QVBoxLayout(parent);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(area);
    }
    else
    {
        auto *layout = new QVBoxLayout(parent);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(body);
    }
    return body;
}
QWidget *bodyFor(Node &n)
{
    return n.widget->findChild<QWidget *>("uiBody", Qt::FindChildrenRecursively);
}
// Windows only puts a taskbar button on a window that nothing owns. Every panel here is
// owned by the editor shell so that it stays above it, which would leave a minimized one
// with nowhere to click to bring it back - the reason minimize used to be left off the
// frame altogether. WS_EX_APPWINDOW asks for the button anyway, so minimize behaves the
// way it does everywhere else. The shell reads this when the window is first shown, so it
// has to be set while the window is still hidden.
void addTaskbarButton(QWidget *w)
{
    const HWND handle = reinterpret_cast<HWND>(w->winId());
    if (!handle)
        return;
    const LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    if (!(style & WS_EX_APPWINDOW))
        SetWindowLongPtrW(handle, GWL_EXSTYLE, style | WS_EX_APPWINDOW);
}
// A dock torn off the shell is a Qt::Tool window, and Windows draws nothing but a close
// box on a tool window's caption however the style bits are set - so a floated panel could
// be dragged around but never maximized. Make it an ordinary window instead.
//
// Re-asserted rather than connected to topLevelChanged once: Qt rebuilds these flags from
// scratch whenever it changes the dock's window state, which includes every drag of an
// already-floating panel, not only the moment it leaves the shell. Never while a mouse
// button is down - setWindowFlags destroys and recreates the native window, which would
// drop a drag in progress.
void keepDockManageable(QWidget *w)
{
    auto *dock = qobject_cast<QDockWidget *>(w);
    if (!dock || !dock->isFloating() || QApplication::mouseButtons() != Qt::NoButton)
        return;
    const Qt::WindowFlags flags = dock->windowFlags();
    if ((flags & Qt::WindowType_Mask) == Qt::Window && (flags & Qt::WindowMaximizeButtonHint))
        return;
    dock->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                         Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint |
                         Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    addTaskbarButton(dock);
    dock->show();
}
// Size an auto-resizing window to its content, clamped to the screen.
//
// The content sits inside a QScrollArea, whose size hint deliberately does not track the
// widget it holds - that is the whole point of a scroll area - so the dialog cannot simply
// be told to hug its own layout; doing that collapses it onto the scroll area's minimal
// hint. Measure the inner body instead, clamp to the available screen, and leave the
// scroll area to handle whatever still does not fit. This runs for a few frames because
// size hints only settle once the children exist and the style has polished them.
void autoSizeWindow(Node &n)
{
    --n.autoSize;
    QWidget *w = n.widget;
    if (!w || !w->isVisible())
        return;
    // A window the user has maximized has been given its size, and one that has been
    // minimized is not being looked at; keep counting the frames down but do not fight it.
    if (w->isMaximized() || w->isFullScreen() || w->isMinimized())
        return;
    QWidget *body = bodyFor(n);
    if (!body)
        return;
    if (QLayout *bodyLayout = body->layout())
        bodyLayout->activate();

    // What the caller asked for is a floor, not the answer: content larger than that
    // still grows the window, content smaller than it leaves the window as asked.
    const QSize content = body->sizeHint().expandedTo(n.sizeFloor);
    if (content.isEmpty())
        return;

    const QScreen *screen = w->screen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const int chrome = std::max(0, w->frameGeometry().height() - w->height());
    const int maxWidth = int(available.width() * 0.9);
    const int maxHeight = int(available.height() * 0.9) - chrome;

    const int barWidth = QApplication::style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    int width = content.width();
    int height = content.height();
    if (height > maxHeight)
    {
        // A vertical scroll bar is about to appear; widen so it does not eat the content.
        height = maxHeight;
        width += barWidth;
    }
    if (width > maxWidth)
    {
        // Same the other way round: a horizontal bar takes a strip off the bottom.
        width = maxWidth;
        height = std::min(height + barWidth, maxHeight);
    }

    if (w->size() != QSize(width, height))
        w->resize(width, height);

    // A window that just grew can end up hanging off the screen; pull it back on.
    const QRect frame = w->frameGeometry();
    QPoint shift;
    if (frame.bottom() > available.bottom())
        shift.setY(available.bottom() - frame.bottom());
    if (frame.right() > available.right())
        shift.setX(available.right() - frame.right());
    if (!shift.isNull())
        w->move(w->pos() + shift);
}
void dimensions(QWidget *w, bool fresh)
{
    if (fresh && nextSize.x > 0 && nextSize.y > 0)
        w->resize(int(nextSize.x), int(nextSize.y));
    if (minSize.x > 0 && minSize.y > 0)
        w->setMinimumSize(int(minSize.x), int(minSize.y));
    if (maxSize.x > 0 && maxSize.y > 0)
        w->setMaximumSize(int(std::min(maxSize.x, float(QWIDGETSIZE_MAX))),
                          int(std::min(maxSize.y, float(QWIDGETSIZE_MAX))));
    // Both of these are per-frame calls on a top-level window. QWidget::setWindowOpacity
    // has no early-out either, so it reapplied the layered-window attributes every frame,
    // and an unconditional move() is a SetWindowPos that forces a recomposite.
    // Moving a maximized window un-maximizes it on Windows, so leave one be.
    // Only the undecorated overlays position themselves every frame anyway.
    if (positionSet && !w->isMaximized() && !w->isFullScreen())
    {
        const QPoint target(int(nextPos.x - w->width() * nextPivot.x),
                            int(nextPos.y - w->height() * nextPivot.y));
        if (w->pos() != target)
            w->move(target);
    }
    if (!qFuzzyCompare(w->windowOpacity(), qreal(nextAlpha)))
        w->setWindowOpacity(nextAlpha);
    nextSize = {};
    minSize = {};
    maxSize = {};
    positionSet = false;
    nextAlpha = 1;
}
} // namespace

namespace QtUi
{
bool Initialize(HWND owner, bool persistLayout)
{
    host = owner;
    saveLayout = persistLayout && !standaloneGame;
    frame = 0;
    if (!qApp)
    {
        static int argc = 1;
        static char name[] = "Ptero Editor";
        static char *argv[] = {name, nullptr};
        app = std::make_unique<QApplication>(argc, argv);
    }
    if (!events)
    {
        events = new EventCounter;
        qApp->installEventFilter(events);
    }
    QApplication::setStyle("Fusion");
    QApplication::setQuitOnLastWindowClosed(false);
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    const BOOL darkTitle = TRUE;
    DwmSetWindowAttribute(host, 20, &darkTitle, sizeof(darkTitle));
    QCoreApplication::setOrganizationName("Pterosoft");
    QCoreApplication::setApplicationName("Ptero Editor");
    QPalette p;
    p.setColor(QPalette::Window, QColor("#191a1c"));
    p.setColor(QPalette::WindowText, QColor("#f2ebe3"));
    p.setColor(QPalette::Base, QColor("#111214"));
    p.setColor(QPalette::AlternateBase, QColor("#222326"));
    p.setColor(QPalette::Text, QColor("#f2ebe3"));
    p.setColor(QPalette::Button, QColor("#292a2d"));
    p.setColor(QPalette::ButtonText, QColor("#f2ebe3"));
    p.setColor(QPalette::Highlight, QColor("#c73d0d"));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, QColor("#292a2d"));
    p.setColor(QPalette::ToolTipText, QColor("#f2ebe3"));
    p.setColor(QPalette::Link, QColor("#db521a"));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor("#797a7d"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#797a7d"));
    QApplication::setPalette(p);
    // The editor writes in the system UI font - the one Windows puts in a title bar - at
    // the size the system asks for. It used to load Playfair Display out of Data/Fonts,
    // but a display serif at 11 pixels is the wrong tool for panels that are mostly dense
    // labels and numbers, and it never matched the window chrome around it. Playfair is
    // still the HUD's font: RmlUiRenderer loads it for the documents under Data/UI, which
    // is game content and nothing to do with the editor's own chrome.
    //
    // The class overrides are set to the same font deliberately. Qt takes a menu's font
    // from the system's menu metrics rather than from the general UI font, and the two
    // are not guaranteed to agree.
    const QFont uiFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    QApplication::setFont(uiFont);
    for (const char *type : {"QMenuBar", "QMenu", "QDockWidget", "QToolTip"})
        QApplication::setFont(uiFont, type);
    qApp->setStyleSheet(
        "QMainWindow::separator { background:#333438; width:5px; height:5px; }"
        "QDockWidget::title { background:#222326; padding:6px; border-bottom:1px solid #393a3e; }"
        "QPushButton, QToolButton { padding:4px 8px; border:1px solid #424348; border-radius:4px; background:#292a2d; }"
        "QPushButton:hover,QToolButton:hover { border-color:#db521a; }"
        "QPushButton:checked,QToolButton:checked { background:#793014; border-color:#c73d0d; }"
        "QLineEdit,QAbstractSpinBox,QComboBox { padding:3px; border:1px solid #424348; border-radius:3px; "
        "background:#111214; }"
        "QLineEdit:focus,QAbstractSpinBox:focus,QComboBox:focus { border-color:#c73d0d; }"
        "QTabBar::tab { padding:7px 12px; background:#222326; } QTabBar::tab:selected { border-bottom:2px solid "
        "#c73d0d; }"
        "QMenu::item:selected { background:#793014; } QProgressBar::chunk { background:#c73d0d; }");
    shell = new QMainWindow;
    // Qt must create the shell as a child of the host itself. Setting the native parent
    // before the HWND exists marks the platform window "embedded": Qt then keeps its
    // geometry in host-client coordinates and resolves mapToGlobal through ClientToScreen
    // instead of a stale top-level position. Reparenting behind Qt's back leaves every
    // screen coordinate (overlay placement, popups, gizmo hit tests) offset, and lets
    // Qt-driven relayouts shove the shell around inside the host.
    shell->setProperty("_q_embedded_native_parent_handle", QVariant::fromValue(reinterpret_cast<WId>(host)));
    shell->setWindowFlags(Qt::FramelessWindowHint);
    shell->setObjectName("PteroQtShell");
    shell->setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks);
    auto *center = new QWidget;
    auto *layout = new QVBoxLayout(center);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *bar = new QMenuBar(center);
    bar->setFont(QApplication::font());
    bar->setObjectName("viewportMenu");
    layout->addWidget(bar);
    if (standaloneGame) bar->hide();
    surface = new Surface(center);
    surface->banksWheelForCamera = true;
    surface->setObjectName("SceneViewport");
    surface->setAttribute(Qt::WA_NativeWindow);
    surface->setAttribute(Qt::WA_PaintOnScreen);
    surface->setAttribute(Qt::WA_NoSystemBackground);
    surface->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(surface, 1);
    shell->setCentralWidget(center);
    shell->createWinId();
    RECT rc{};
    GetClientRect(host, &rc);
    ResizeHost(rc.right, rc.bottom);
    shell->show();
    overlay = new Overlay(shell);
    timer.start();
    lastTime = 0;
    return true;
}
void Shutdown()
{
    delete playWindow; playWindow=nullptr; playSurface=nullptr; playWindowActive=false;
    viewportGame=false;
    if (shell && saveLayout)
    {
        QSettings settings;
        settings.setValue("layout/qt-v1", shell->saveState(1));
    }
    delete overlay;
    overlay = nullptr;
    delete shell;
    shell = nullptr;
    surface = nullptr;
    viewportWheelAngle = 0;
    nodes.clear();
    scopes.clear();
    ids.clear();
    icons.clear();
    textureViews.clear();
    disabled.clear();
    colors.clear();
    styles.clear();
    widths.clear();
    last = nullptr;
    app.reset();
    host = nullptr;
}
HWND HostHandle()
{
    return host;
}
void *ShellWidget()
{
    // Handed out as void* so QtUi.h stays free of Qt types for the engine sources that
    // include it. Tool windows built directly on Qt - the node graph editor - parent
    // themselves to this so they inherit the editor's palette and stay above it.
    return shell;
}
float FramebufferScale()
{
    return ActiveSurface() ? static_cast<float>(ActiveSurface()->devicePixelRatioF()) : 1.0f;
}
float EventMilliseconds()
{
    return uiEventMs;
}
const char *EventTypes()
{
    // Top event types of the last loop iteration, named via QEvent's meta-enum. Paint,
    // UpdateRequest and LayoutRequest are reported separately, so leave them out here.
    static QByteArray summary;
    summary = "-";
    if (!events)
        return summary.constData();
    const QMetaEnum meta = QMetaEnum::fromType<QEvent::Type>();
    QList<std::pair<unsigned, int>> ranked;
    for (auto it = events->lastTypes.constBegin(); it != events->lastTypes.constEnd(); ++it)
        if (it.key() != int(QEvent::Paint) && it.key() != int(QEvent::UpdateRequest) &&
            it.key() != int(QEvent::LayoutRequest))
            ranked.append({it.value(), it.key()});
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    summary.clear();
    for (int i = 0; i < ranked.size() && i < 3; ++i)
    {
        const char *key = meta.valueToKey(ranked[i].second);
        summary += (i ? " " : "") + (key ? QByteArray(key) : QByteArray::number(ranked[i].second)) + "x" +
                   QByteArray::number(ranked[i].first);
    }
    if (summary.isEmpty())
        summary = "-";
    return summary.constData();
}
void EventCounts(unsigned &updates, unsigned &layouts, unsigned &paints, const char *&topPainter,
                 unsigned &topPainterCount)
{
    static QByteArray name;
    updates = layouts = paints = topPainterCount = 0;
    name = "-";
    if (events)
    {
        updates = events->lastUpdates;
        layouts = events->lastLayouts;
        paints = events->lastPaints;
        for (auto it = events->lastPainters.constBegin(); it != events->lastPainters.constEnd(); ++it)
            if (it.value() > topPainterCount)
            {
                topPainterCount = it.value();
                name = it.key().toUtf8();
            }
    }
    topPainter = name.constData();
}
float FrameMilliseconds()
{
    return uiFrameMs;
}
HWND ViewportHandle()
{
    return ActiveSurface() ? reinterpret_cast<HWND>(ActiveSurface()->winId()) : nullptr;
}
void OpenGameWindow(bool separateWindow, const char *title)
{
    if (standaloneGame) { if (surface) surface->setFocus(); return; }
    gameCloseRequested=false; gameFullscreenRequested=false;
    if (!separateWindow) {
        // Nothing to open: the editor's own surface keeps presenting and its panels stay
        // enabled. Only input ownership moves, so focus the viewport - otherwise the
        // first key the player presses would go to whichever panel was last clicked.
        viewportGame=true;
        if (surface) { surface->setFocus(); }
        return;
    }
    if (playWindowActive) return;
    if (!playWindow) {
        playWindow=new PlayWindow;
        auto* layout=new QVBoxLayout(playWindow);
        layout->setContentsMargins(0,0,0,0);
        playSurface=new Surface(playWindow);
        playSurface->setAttribute(Qt::WA_NativeWindow);
        playSurface->setAttribute(Qt::WA_PaintOnScreen);
        playSurface->setAttribute(Qt::WA_NoSystemBackground);
        playSurface->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(playSurface);
        // Both native surfaces stay alive. DXGI switches at a frame boundary.
        playSurface->winId();
    }
    playWindowActive=true;
    // Retitled on every open rather than at construction: the name comes from the game
    // module, and a rebuilt Game.dll may well report a different one.
    playWindow->setWindowTitle(title && *title ? QString::fromUtf8(title) : QStringLiteral("Play"));
    // Freeze retained editor controls while the renderer belongs to the game.
    shell->setEnabled(false);
    playWindow->showMaximized();
    playWindow->activateWindow(); playSurface->setFocus();
}
void CloseGameWindow()
{
    if (standaloneGame) { if (host) PostMessageW(host, WM_CLOSE, 0, 0); return; }
    viewportGame=false; gameFullscreenRequested=false;
    if (!playWindowActive) return;
    playWindowActive=false; gameCloseRequested=false;
    // Keep the HWND alive while DXGI retains its swap chain.
    playWindow->hide(); shell->setEnabled(true);
    SetForegroundWindow(host); surface->setFocus();
}
void ToggleGameFullscreen()
{
    // The editor owns the in-viewport fullscreen mode, so record the wish and let it act.
    if (viewportGame) { gameFullscreenRequested=true; return; }
    if (playWindowActive) {
        if (playWindow->isFullScreen()) playWindow->showMaximized();
        else playWindow->showFullScreen();
        return;
    }
    if (!standaloneGame || !host) return;
    if (!gameFullscreen) {
        gameWindowStyle = GetWindowLongPtrW(host, GWL_STYLE);
        GetWindowPlacement(host, &gamePlacement);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(host, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        SetWindowLongPtrW(host, GWL_STYLE, gameWindowStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(host, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right-monitor.rcMonitor.left, monitor.rcMonitor.bottom-monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(host, GWL_STYLE, gameWindowStyle);
        SetWindowPlacement(host, &gamePlacement);
        SetWindowPos(host, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    gameFullscreen = !gameFullscreen;
}
void SetStandaloneGame(bool enabled) { standaloneGame = enabled; }
bool IsStandaloneGame() { return standaloneGame; }
bool IsGameWindowOpen() { return standaloneGame || playWindowActive; }
bool IsGamePlaying() { return standaloneGame || playWindowActive || viewportGame; }
bool GameWindowHasFocus()
{
    // Playing inside the viewport, the editor window *is* the game window. Text fields
    // still win: typing a name into the Properties panel must not also move the player.
    if (viewportGame)
        return host && GetAncestor(GetForegroundWindow(), GA_ROOT)==GetAncestor(host, GA_ROOT) &&
               !io.WantTextInput;
    HWND gameHost=playWindowActive ? reinterpret_cast<HWND>(playWindow->winId()) : host;
    return IsGameWindowOpen() && gameHost && GetAncestor(GetForegroundWindow(), GA_ROOT)==gameHost;
}
bool ConsumeGameCloseRequest() { bool result=gameCloseRequested; gameCloseRequested=false; return result; }
bool ConsumeGameFullscreenRequest() { bool result=gameFullscreenRequested; gameFullscreenRequested=false; return result; }
BOOL fileDialog(OPENFILENAMEA *request, bool save)
{
    QStringList filters;
    for (const char *entry = request->lpstrFilter; entry && *entry;)
    {
        QString title = QString::fromLocal8Bit(entry);
        entry += strlen(entry) + 1;
        if (!*entry)
            break;
        QString pattern = QString::fromLocal8Bit(entry).replace(';', ' ');
        entry += strlen(entry) + 1;
        filters.append(title + " (" + pattern + ")");
    }
    QFileDialog dialog(shell, QString::fromLocal8Bit(request->lpstrTitle ? request->lpstrTitle
                                                     : save              ? "Save"
                                                                         : "Open"));
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setNameFilters(filters);
    dialog.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    const bool multiple = !save && (request->Flags & OFN_ALLOWMULTISELECT);
    dialog.setFileMode(save ? QFileDialog::AnyFile : multiple ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);
    if (request->lpstrInitialDir)
        dialog.setDirectory(QString::fromLocal8Bit(request->lpstrInitialDir));
    if (request->lpstrFile && *request->lpstrFile)
        dialog.selectFile(QString::fromLocal8Bit(request->lpstrFile));
    if (request->lpstrDefExt)
        dialog.setDefaultSuffix(QString::fromLocal8Bit(request->lpstrDefExt));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return FALSE;
    const auto files = dialog.selectedFiles();
    QByteArray result;
    if (multiple && files.size() > 1)
    {
        result = QDir::toNativeSeparators(QFileInfo(files.front()).absolutePath()).toLocal8Bit();
        result.append('\0');
        for (const auto &path : files)
        {
            result.append(QFileInfo(path).fileName().toLocal8Bit());
            result.append('\0');
        }
    }
    else
    {
        result = QDir::toNativeSeparators(files.front()).toLocal8Bit();
        result.append('\0');
    }
    result.append('\0');
    if (std::size_t(result.size()) > request->nMaxFile)
        return FALSE;
    memcpy(request->lpstrFile, result.constData(), result.size());
    return TRUE;
}
BOOL OpenFileName(OPENFILENAMEA *request)
{
    return fileDialog(request, false);
}
BOOL SaveFileName(OPENFILENAMEA *request)
{
    return fileDialog(request, true);
}
void ResizeHost(unsigned w, unsigned h)
{
    if (shell)
    {
        // Pin the shell to the host client area. Position matters as much as size:
        // the embedded window's origin is the host's client origin, so anything but
        // (0, 0) leaves an unpainted gutter the host never erases.
        const qreal dpr = shell->devicePixelRatioF();
        shell->setGeometry(0, 0, std::max(1, int(w / dpr)), std::max(1, int(h / dpr)));
    }
}
bool CameraInputAllowed()
{
    if (!surface || QApplication::activeModalWidget() || QApplication::activePopupWidget())
        return false;
    POINT p{};
    GetCursorPos(&p);
    RECT r{};
    GetWindowRect(ViewportHandle(), &r);
    // QApplication::widgetAt walks every top-level window and hit-tests widget by widget,
    // which is far too expensive to run once per frame. The viewport is a native window,
    // so asking Windows which HWND is under the cursor answers the same question directly.
    const HWND hitWindow = WindowFromPoint(p);
    const HWND viewportWindow = ViewportHandle();
    return (hitWindow == viewportWindow || IsChild(viewportWindow, hitWindow)) &&
           (GameWindowHasFocus() || GetAncestor(GetForegroundWindow(), GA_ROOT) == GetAncestor(host, GA_ROOT)) && PtInRect(&r, p) &&
           !io.WantTextInput;
}
bool KeyboardCameraInputAllowed()
{
    if (IsGamePlaying()) return GameWindowHasFocus();
    // The renderer reads WASD with GetAsyncKeyState, which reports the physical key
    // regardless of who owns the keyboard focus - so without this the editor camera flew
    // around while the user was typing in another application entirely. Unlike
    // CameraInputAllowed this does not require the cursor to be over the viewport, so a
    // fly-through still works when the pointer drifts off it mid-drag.
    if (QApplication::activeModalWidget())
        return false;
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(host, GA_ROOT))
        return false;
    return !io.WantTextInput;
}
float ConsumeViewportWheelDelta()
{
    // One detent is 120 eighths-of-a-degree; report notches so callers do not have to know
    // that. Draining on every read keeps wheel input from piling up while it is unused.
    const float notches = float(viewportWheelAngle) / 120.f;
    viewportWheelAngle = 0;
    return notches;
}
void NewFrame()
{
    uiFrameStart = timer.nsecsElapsed();
    if (events)
        events->reset();
    ++frame;
    scopes.clear();
    ids.clear();
    trees.clear();
    skippedMenus.clear();
    textureViews.clear();
    overlay->commands.clear();
    const qint64 eventsStart = timer.nsecsElapsed();
    // Qt's own work - layout requests, style polish, deferred deletes - is only serviced
    // here; Win32 paints go through the host message pump instead. processEvents returns
    // as soon as the queue drains, and in steady state it is empty, so this ceiling costs
    // nothing on an idle frame. It only bounds a burst, and opening a window *is* a burst:
    // with a 2 ms slice that work was spread over hundreds of frames, which is what made
    // a window take a visible moment to appear. One frame's worth lets a burst clear
    // immediately while still capping the damage if something floods the queue.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
    uiEventMs = float(double(timer.nsecsElapsed() - eventsStart) / 1.0e6);
    const qint64 now = timer.elapsed();
    io.DeltaTime = float(now - lastTime) / 1000.f;
    lastTime = now;
    io.Framerate = io.DeltaTime > 0 ? 1 / io.DeltaTime : 0;
    const QPoint mouse = QCursor::pos();
    io.MousePos = {float(mouse.x()), float(mouse.y())};
    const bool active = GameWindowHasFocus() || (!playWindowActive && GetAncestor(GetForegroundWindow(), GA_ROOT) == GetAncestor(host, GA_ROOT));
    for (int k = 0; k < 256; ++k)
    {
        bool down = active && (GetAsyncKeyState(k) & 0x8000);
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
            mouseDouble[i] = clickTime[i] > 0 && now - clickTime[i] < QApplication::doubleClickInterval();
            clickTime[i] = now;
        }
        mouseDown[i] = down;
    }
    io.KeyCtrl = keys[VK_CONTROL];
    io.KeyShift = keys[VK_SHIFT];
    QWidget *focus = QApplication::focusWidget();
    io.WantTextInput = focus && (qobject_cast<QLineEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus) ||
                                 qobject_cast<QTextEdit *>(focus));
    viewport.WorkSize = {float(shell->width()), float(shell->height())};
    const QPoint origin = shell->mapToGlobal(QPoint(0, 0));
    viewport.WorkPos = {float(origin.x()), float(origin.y())};
    io.DisplaySize = viewport.WorkSize;
}
void EndFrame()
{
    // A skipped menu's contents were deliberately not walked, so keep the whole subtree
    // alive. Keys are hierarchical and "nodes" is ordered, so each subtree is one range.
    for (const QString &prefix : skippedMenus)
        for (auto it = nodes.lower_bound(prefix); it != nodes.end() && it->first.startsWith(prefix); ++it)
            it->second->seen = frame;
    for (auto &[id, n] : nodes)
        if (!playWindowActive && n->seen != frame)
        {
            if (n->widget)
                n->widget->hide();
            if (n->action)
                n->action->setVisible(false);
        }
    if (frame == 2)
    {
        QList<QDockWidget *> left, right;
        for (auto *d : shell->findChildren<QDockWidget *>())
        {
            auto area = shell->dockWidgetArea(d);
            if (area == Qt::LeftDockWidgetArea)
                left.append(d);
            if (area == Qt::RightDockWidgetArea)
                right.append(d);
        }
        if (!left.empty())
            shell->resizeDocks(left, QList<int>(left.size(), int(shell->width() * .19)), Qt::Horizontal);
        if (!right.empty())
            shell->resizeDocks(right, QList<int>(right.size(), int(shell->width() * .154)), Qt::Horizontal);
        if (saveLayout)
        {
            QSettings settings;
            shell->restoreState(settings.value("layout/qt-v1").toByteArray(), 1);
        }
    }
    // The overlay is a translucent layered window: each repaint costs a full-size ARGB
    // rasterization plus a DWM composite, so leave it hidden unless the frame actually
    // produced draw commands, and only move it when the viewport really moved.
    overlay->painted.swap(overlay->commands);
    if (!IsGameWindowOpen() && !overlay->painted.empty() && IsWindowVisible(host) && !IsIconic(host) && surface->isVisible())
    {
        const QRect bounds(surface->mapToGlobal(QPoint(0, 0)), surface->size());
        if (overlay->geometry() != bounds)
            overlay->setGeometry(bounds);
        overlay->show();
        overlay->update();
    }
    else if (!overlay->isHidden())
        overlay->hide();
    std::fill(std::begin(pressed), std::end(pressed), false);
    std::fill(std::begin(mouseClicked), std::end(mouseClicked), false);
    std::fill(std::begin(mouseReleased), std::end(mouseReleased), false);
    std::fill(std::begin(mouseDouble), std::end(mouseDouble), false);
    // Release controls belonging to old selections/folders after a grace period.
    // Container nodes remain stable; signals use QObject contexts and disconnect
    // automatically when the corresponding leaf widget/action is destroyed.
    // Nothing becomes collectable until it has gone unseen for 300 frames, so sweeping
    // every frame just pays for a dynamic_cast per node per frame for nothing.
    for (auto it = nodes.begin(); !playWindowActive && frame % 60 == 0 && it != nodes.end();)
    {
        auto &n = *it->second;
        const bool leaf = n.widget && (!n.widget->layout() || n.widget->property("uiControl").toBool()) &&
                          !qobject_cast<QScrollArea *>(n.widget.data()) && !qobject_cast<QMenu *>(n.widget.data()) &&
                          !qobject_cast<QTabWidget *>(n.widget.data()) && !dynamic_cast<Surface *>(n.widget.data());
        if (frame - n.seen > 300 && leaf)
        {
            delete n.action;
            delete n.widget;
            if (last == &n)
                last = nullptr;
            it = nodes.erase(it);
        }
        else
            ++it;
    }
    actionFired = false;
    uiFrameMs = float(double(timer.nsecsElapsed() - uiFrameStart) / 1.0e6);
}
void RegisterIcon(UiTextureID id, const wchar_t *path)
{
    icons[id] = QPixmap(QString::fromWCharArray(path));
}
const TextureView *TextureViews(std::size_t &count)
{
    count = textureViews.size();
    return textureViews.data();
}
bool Begin(const char *name, bool *open, int flags)
{
    const QString id = "window/" + QString::fromUtf8(name);
    Node &n = node(id);
    const bool fresh = !n.widget;
    if (QString::fromUtf8(name) == "Viewport")
    {
        n.widget = shell->centralWidget();
        Scope s;
        s.key = id;
        s.body = n.widget;
        s.node = &n;
        scopes.push_back(s);
        nextSize = {};
        minSize = {};
        maxSize = {};
        positionSet = false;
        return true;
    }
    if (fresh)
    {
        const QString title = label(name);
        QWidget *body = nullptr;
        if (title == "Toolbar")
        {
            auto *toolbar = new QToolBar(title, shell);
            toolbar->setObjectName(name);
            toolbar->setMovable(true);
            shell->addToolBar(Qt::TopToolBarArea, toolbar);
            body = new QWidget;
            body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            toolbar->addWidget(body);
            grid(body)->setAlignment(Qt::AlignTop | Qt::AlignLeft);
            n.widget = toolbar;
        }
        // Only the panels that are part of the permanent working set dock. Level Explorer,
        // Audio Manager and Resource Debug are opened to answer a question and closed
        // again, so they get a floating tool window like every other auxiliary editor;
        // tabbing them into a dock buried the panel the user actually keeps there.
        //
        // Resource Debug is the clearest case: it is consulted while chasing a frame-time
        // problem, and docking it tabbed it over Properties - so reading the timings meant
        // losing the inspector for whatever was being profiled.
        //
        // Console is in the permanent set for the opposite reason: it is where the
        // engine reports what it is doing, so it has to be somewhere the user can
        // leave open. It tabs beside Components on the left.
        else if (title == "Components" || title == "Properties" || title == "Console")
        {
            auto *dock = new QDockWidget(title, shell);
            dock->setFont(QApplication::font());
            dock->setObjectName(name);
            auto *page = new QWidget;
            body = makeBody(page, true);
            dock->setWidget(page);
            n.widget = dock;
            const auto area = (title == "Properties") ? Qt::RightDockWidgetArea : Qt::LeftDockWidgetArea;
            QDockWidget *previous = nullptr;
            for (auto *d : shell->findChildren<QDockWidget *>())
                if (d != dock && shell->dockWidgetArea(d) == area)
                {
                    previous = d;
                    break;
                }
            shell->addDockWidget(area, dock);
            if (previous)
                shell->tabifyDockWidget(previous, dock);
            QObject::connect(dock->toggleViewAction(), &QAction::triggered, dock,
                             [&n](bool visible) { n.closed = !visible; });
        }
        else
        {
            // Deliberately not a Qt::Tool window. Asking for the maximize hint on one
            // achieves nothing: a tool window gets WS_EX_TOOLWINDOW, and Windows draws
            // only a close box on that caption whatever style bits are set - which is why
            // every panel here was stuck at the size it opened at, maximize hint or no.
            // An ordinary window frame carries the full caption, so the buttons appear and
            // double-clicking the title bar works.
            //
            // Minimize comes with it. It was left off before because a tool window has no
            // taskbar button, so a minimized panel was somewhere the user could not get it
            // back from; addTaskbarButton() below is what removes that objection.
            auto *dialog = new QDialog(shell,
                Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint |
                Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
            dialog->setWindowTitle(title);
            dialog->setObjectName(name);
            body = makeBody(dialog, (flags & QtUiWindowFlags_NoDecoration) == 0);
            n.widget = dialog;
            if (flags & QtUiWindowFlags_NoDecoration)
            {
                // Undecorated HUD overlays carry no size of their own, so a roomy default
                // would blanket the viewport. Hug the content instead, the way the
                // immediate-mode caller expects - these are not windows the user resizes,
                // so the layout can own the size outright. One that asked for a size is
                // given it by dimensions() below and left alone.
                if (nextSize.x <= 0)
                    dialog->layout()->setSizeConstraint(QLayout::SetFixedSize);
            }
            else
            {
                // Every framed window opens at the size of what it holds, not at a guess.
                // The old fallback was a flat 720x640, which left the small panels mostly
                // empty and clipped the wide ones behind a scroll bar; AlwaysAutoResize was
                // the exception when it should have been the rule. A size the caller asked
                // for survives as the floor - see Node::sizeFloor.
                //
                // Sized from its content in End(), once that content actually exists.
                // Deliberately not SetFixedSize: that also makes the window unresizable,
                // which leaves the user stuck if the measurement is wrong.
                if (nextSize.x > 0)
                    n.sizeFloor.setWidth(int(nextSize.x));
                if (nextSize.y > 0)
                    n.sizeFloor.setHeight(int(nextSize.y));
                n.autoFit = true;
                n.autoSize = 6;
                addTaskbarButton(dialog);
            }
            QObject::connect(dialog, &QDialog::finished, dialog, [&n] { n.closed = true; });
            if (flags & QtUiWindowFlags_NoDecoration)
                dialog->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
            if (flags & QtUiWindowFlags_NoMouseInputs)
                dialog->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
        body->setObjectName("uiBody");
        grid(body);
    }
    if (n.closed)
    {
        if (open)
            *open = false;
        n.closed = false;
    }
    const bool visible = !open || *open;
    dimensions(n.widget, fresh);
    keepDockManageable(n.widget);
    // A window coming back on screen is measured again: what it holds now is not
    // necessarily what it held when the user last closed it. Reopening also undoes a
    // minimize - the menu is the one route back to a panel that was minimized and then
    // switched off, and it would otherwise tick the item without showing anything.
    if (visible && n.autoFit && !fresh && !n.widget->isVisible())
    {
        if (n.widget->isMinimized())
            n.widget->setWindowState(n.widget->windowState() & ~Qt::WindowMinimized);
        n.autoSize = 6;
    }
    n.widget->setVisible(visible);
    push(bodyFor(n), id, &n);
    return visible;
}
void End()
{
    // A header's section has no end call of its own; the scope around it ends it.
    while (!scopes.empty() && scopes.back().section != Section::None)
        closeSection();
    if (scopes.empty())
        return;
    // The content was built between Begin and End, so this is the first point at which
    // an auto-resizing window can actually be measured.
    if (Node *n = scopes.back().node; n != nullptr && n->autoSize > 0)
        autoSizeWindow(*n);
    scopes.pop_back();
    // Tree nodes left open inside the scope just closed.
    while (!trees.empty() && trees.back().scopeCount > scopes.size())
        trees.pop_back();
}
bool BeginChild(const char *name, UiVec2 size, bool, int)
{
    const QString id = key(name);
    Node &n = node(id);
    if (!n.widget)
    {
        auto *area = new QScrollArea;
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        auto *body = new QWidget;
        body->setObjectName("uiBody");
        area->setWidget(body);
        n.widget = area;
    }
    place(n);
    if (size.x > 0)
        n.widget->setFixedWidth(int(size.x));
    if (size.y > 0)
        n.widget->setFixedHeight(int(size.y));
    push(bodyFor(n), id, &n);
    return true;
}
void EndChild()
{
    End();
}
bool BeginMainMenuBar()
{
    Scope s;
    s.key = "menu";
    s.bar = shell->menuBar();
    // Once only: with a stylesheet active QWidget::setFont calls saveWidgetFont before it
    // compares anything, so re-asserting the same font every frame is real work.
    if (s.bar->font() != QApplication::font())
        s.bar->setFont(QApplication::font());
    scopes.push_back(s);
    return true;
}
void EndMainMenuBar()
{
    End();
}
bool BeginMenuBar()
{
    Scope s;
    s.key = key("menubar");
    s.bar = shell->findChild<QMenuBar *>("viewportMenu");
    scopes.push_back(s);
    return true;
}
void EndMenuBar()
{
    End();
}
bool BeginMenu(const char *name, bool available)
{
    QString id = key(name);
    Node &n = node(id);
    auto &parent = scopes.back();
    if (!n.widget)
    {
        auto *menu = new QMenu(label(name), shell);
        menu->setFont(QApplication::font());
        n.widget = menu;
        if (parent.menu)
            parent.menu->addMenu(menu);
        else if (parent.bar)
            parent.bar->addMenu(menu);
        n.action = menu->menuAction();
    }
    n.action->setVisible(true);
    n.action->setEnabled(available && enabled());
    auto *menu = static_cast<QMenu *>(n.widget.data());
    // Reconciling a closed menu every frame is pure waste: the caller walks hundreds of
    // actions and embedded controls for a popup nobody can see, which is what the
    // immediate-mode original avoids by reporting closed menus as closed. Keep the items
    // that already exist alive (EndFrame marks the subtree seen) so the menu is ready the
    // instant it opens, and refresh it while it is on screen.
    // Walk every menu on a frame where an action fired, even the closed ones, so the
    // item that fired gets a chance to report it.
    if (n.populated && !menu->isVisible() && !actionFired)
    {
        skippedMenus.push_back(id + "/");
        return false;
    }
    n.populated = true;
    Scope s;
    s.key = id;
    s.menu = menu;
    s.node = &n;
    scopes.push_back(s);
    return true;
}
void EndMenu()
{
    End();
}
bool MenuItem(const char *name, const char *shortcut, bool checked, bool available)
{
    Node &n = node(key(name));
    if (!n.action)
    {
        n.action = new QAction(label(name), shell);
        auto &s = scopes.back();
        if (s.menu)
            s.menu->addAction(n.action);
        else if (s.bar)
            s.bar->addAction(n.action);
        QObject::connect(n.action, &QAction::triggered, n.action, [&n] { n.fired = true; actionFired = true; });
    }
    n.action->setText(label(name) + (shortcut ? "\t" + QString(shortcut) : QString()));
    n.action->setCheckable(checked || n.action->isCheckable());
    n.action->setChecked(checked);
    n.action->setEnabled(available && enabled());
    n.action->setVisible(true);
    return take(n);
}
bool MenuItem(const char *name, const char *shortcut, bool *checked, bool available)
{
    bool changed = MenuItem(name, shortcut, *checked, available);
    if (last && last->action)
        last->action->setCheckable(true);
    if (changed)
        *checked = !*checked;
    return changed;
}
bool Button(const char *name, UiVec2 size)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    auto *w = control<QPushButton>(n);
    w->setText(label(name));
    if (fresh)
    {
        // QPushButton turns on autoDefault inside a QDialog, and Begin builds every
        // non-docked panel as one. QAbstractSpinBox ignores Return once it has interpreted
        // its text, so the key travels on to the dialog and activates its first
        // autoDefault button: pressing Enter to commit a number in the Material Editor was
        // also clicking "New", which reset the material being edited. Nothing here is ever
        // a dialog's accept button, so no QtUi button should answer Return.
        w->setAutoDefault(false);
        w->setDefault(false);
        QObject::connect(w, &QPushButton::clicked, w, [&n] { n.fired = true; });
    }
    w->setCheckable(!colors.empty() && colors.back().first == QtUiCol_Button);
    w->setChecked(w->isCheckable());
    if (size.x > 0)
        w->setMinimumWidth(int(size.x));
    if (size.y > 0)
        w->setMinimumHeight(int(size.y));
    return take(n);
}
bool SmallButton(const char *n)
{
    return Button(n);
}
bool Checkbox(const char *name, bool *value)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    auto *w = control<QCheckBox>(n);
    w->setText(label(name));
    if (fresh)
        QObject::connect(w, &QCheckBox::clicked, w, [&n](bool v) {
            n.value = v;
            n.fired = true;
        });
    bool changed = take(n);
    if (changed)
        *value = n.value.toBool();
    QSignalBlocker blocker(w);
    w->setChecked(*value);
    return changed;
}
bool RadioButton(const char *name, bool active)
{
    bool changed = Button(name);
    auto *w = static_cast<QPushButton *>(last->widget.data());
    w->setCheckable(true);
    w->setChecked(active);
    return changed;
}
bool Selectable(const char *name, bool selected, int, UiVec2 size)
{
    if (!scopes.empty() && scopes.back().menu)
        return MenuItem(name, nullptr, selected);
    bool changed = Button(name, size);
    auto *w = static_cast<QPushButton *>(last->widget.data());
    w->setCheckable(true);
    w->setChecked(selected);
    applyStyleSheet(w, "text-align:left;");
    return changed;
}
bool headerButton(const char *name, int flags)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    auto *w = control<QToolButton>(n);
    w->setText(label(name));
    w->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (fresh)
    {
        n.expanded = (flags & QtUiTreeNodeFlags_DefaultOpen) != 0;
        QObject::connect(w, &QToolButton::clicked, w, [&n] {
            n.expanded = !n.expanded;
            n.fired = true;
        });
    }
    w->setArrowType(n.expanded ? Qt::DownArrow : Qt::RightArrow);
    take(n);
    return n.expanded;
}
// Everything up to the next header at this ID depth goes in the header's indented section,
// so a settings panel reads as groups rather than one flat column of controls.
bool CollapsingHeader(const char *name, int flags)
{
    closeHeaders(ids.size());
    const bool expanded = headerButton(name, flags);
    if (expanded)
        openSection(key(name), Section::Header);
    return expanded;
}
bool TreeNodeEx(const char *name, int flags)
{
    if (flags & QtUiTreeNodeFlags_Leaf)
    {
        Selectable(name, (flags & QtUiTreeNodeFlags_Selected) != 0);
        PushID(name);
        trees.push_back({scopes.size(), false});
        return true;
    }
    const bool expanded = headerButton(name, flags);
    if (expanded)
    {
        const QString id = key(name);
        PushID(name);
        const std::size_t before = scopes.size();
        openSection(id, Section::Tree);
        trees.push_back({scopes.size(), scopes.size() > before});
    }
    return expanded;
}
bool TreeNode(const char *name)
{
    return TreeNodeEx(name);
}
void TreePop()
{
    if (!trees.empty())
    {
        const TreeLevel level = trees.back();
        trees.pop_back();
        // Header sections opened inside the tree body end with it.
        while (scopes.size() > level.scopeCount && scopes.back().section == Section::Header)
            closeSection();
        if (level.body && scopes.size() == level.scopeCount && scopes.back().section == Section::Tree)
            closeSection();
    }
    PopID();
}
bool TreeNodeEx(const void *id, int flags, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    QString name = format(f, a);
    va_end(a);
    name += "##" + QString::number(reinterpret_cast<quintptr>(id));
    return TreeNodeEx(name.toUtf8().constData(), flags);
}
bool TreeNodeEx(const char *id, int flags, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    QString name = format(f, a);
    va_end(a);
    name += "##" + QString::fromUtf8(id);
    return TreeNodeEx(name.toUtf8().constData(), flags);
}
bool InputTextSubmit(const char *name, char *value, std::size_t capacity,
                     const UiCompletion *completions, int completionCount)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    auto *w = labeledControl<QLineEdit>(n, name);
    applyToolTip(w, label(name));
    w->setMaxLength(int(capacity ? capacity - 1 : 0));
    if (fresh)
    {
        // Only Return reports. A console line is meaningless half-typed, so the
        // textEdited signal InputText uses would fire a command per keystroke.
        QObject::connect(w, &QLineEdit::returnPressed, w, [&n, w] {
            n.value = w->text();
            n.fired = true;
            w->clear();
        });
    }

    if (completions && completionCount > 0)
    {
        // The list is fixed once the engine has registered its cvars, so it is
        // built from a signature rather than rebuilt every keystroke: setModel
        // would also drop the custom popup, and repopulating under the cursor
        // makes the drop-down flicker while typing.
        quint64 signature = 1469598103934665603ull;
        for (int i = 0; i < completionCount; ++i)
            for (const char *text : {completions[i].Text, completions[i].Detail})
                for (const char *c = text ? text : ""; *c; ++c)
                    signature = (signature ^ quint64(quint8(*c))) * 1099511628211ull;
        signature ^= quint64(completionCount);

        auto *completer = w->completer();
        if (!completer || completer->property("pteroSignature").toULongLong() != signature)
        {
            auto *model = new QStandardItemModel(0, 2, w);
            for (int i = 0; i < completionCount; ++i)
            {
                auto *text = new QStandardItem(QString::fromUtf8(completions[i].Text ? completions[i].Text : ""));
                auto *detail = new QStandardItem(QString::fromUtf8(completions[i].Detail ? completions[i].Detail : ""));
                text->setEditable(false);
                detail->setEditable(false);
                detail->setForeground(QApplication::palette().brush(QPalette::Disabled, QPalette::Text));
                model->appendRow({text, detail});
            }

            auto *replacement = new QCompleter(model, w);
            model->setParent(replacement);
            replacement->setCaseSensitivity(Qt::CaseInsensitive);
            // Substring, not prefix: "specular" should find
            // "rtgi.specular.enabled" without the user recalling the group.
            replacement->setFilterMode(Qt::MatchContains);
            replacement->setCompletionMode(QCompleter::PopupCompletion);
            replacement->setCompletionColumn(0);
            replacement->setMaxVisibleItems(14);

            // A tree view rather than the default list, so the hint column can
            // sit beside the name without ever being inserted into the field.
            auto *popup = new QTreeView;
            popup->setRootIsDecorated(false);
            popup->setHeaderHidden(true);
            popup->setSelectionBehavior(QAbstractItemView::SelectRows);
            popup->setAllColumnsShowFocus(true);
            popup->setEditTriggers(QAbstractItemView::NoEditTriggers);
            popup->setUniformRowHeights(true);
            // setPopup must follow setModel, which resets it, and precede
            // setCompleter, which is what wires the key handling up.
            replacement->setPopup(popup);
            // QCompleter sizes the popup from the field and the completion
            // column, which would clip the hint column off the right edge.
            // setGeometry cannot go below a minimum width, so this survives it.
            popup->setMinimumWidth(560);
            popup->setColumnWidth(0, 280);

            replacement->setProperty("pteroSignature", QVariant::fromValue(signature));
            w->setCompleter(replacement);
        }
    }
    else if (w->completer())
    {
        w->setCompleter(nullptr);
    }

    const bool submitted = take(n);
    if (submitted && capacity)
    {
        QByteArray bytes = n.value.toString().toUtf8();
        const auto len = std::min(capacity - 1, std::size_t(bytes.size()));
        memcpy(value, bytes.constData(), len);
        value[len] = 0;
    }
    return submitted;
}
void TextView(const char *name, const char *html, UiVec2 size, bool scrollToBottom)
{
    Node &n = node(key(name));
    const bool fresh = !n.widget;
    auto *w = control<QTextEdit>(n);
    if (fresh)
    {
        w->setReadOnly(true);
        w->setLineWrapMode(QTextEdit::NoWrap);
        w->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        // Content must never drive the panel wider: a single long log line would
        // otherwise push the dock open a little further every frame.
        w->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        w->setMinimumSize(80, 60);
    }
    if (size.y > 0)
        w->setMinimumHeight(int(size.y));

    // Re-parsing the document costs real time, so only do it when the text has
    // actually changed - which for a quiet log is never.
    const QString incoming = QString::fromUtf8(html ? html : "");
    if (n.value.toString() != incoming)
    {
        const QScrollBar *bar = w->verticalScrollBar();
        const bool wasAtBottom = bar == nullptr || bar->value() >= bar->maximum() - 4;
        w->setHtml(incoming);
        n.value = incoming;
        if (scrollToBottom && wasAtBottom)
            w->verticalScrollBar()->setValue(w->verticalScrollBar()->maximum());
    }
}
bool InputText(const char *name, char *value, std::size_t capacity, int)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    auto *w = labeledControl<QLineEdit>(n, name);
    applyToolTip(w, label(name));
    w->setMaxLength(int(capacity ? capacity - 1 : 0));
    if (fresh)
        QObject::connect(w, &QLineEdit::textEdited, w, [&n](const QString &v) {
            n.value = v;
            n.fired = true;
        });
    bool changed = take(n);
    if (changed && capacity)
    {
        QByteArray bytes = n.value.toString().toUtf8();
        const auto len = std::min(capacity - 1, std::size_t(bytes.size()));
        memcpy(value, bytes.constData(), len);
        value[len] = 0;
    }
    if (!w->hasFocus() && w->text() != QString::fromUtf8(value))
    {
        QSignalBlocker b(w);
        w->setText(QString::fromUtf8(value));
    }
    return changed;
}
bool DragFloat(const char *name, float *value, float step, float low, float high, const char *fmt, int)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    // A finite range is what makes a slider meaningful. Unbounded fields - transforms, and
    // everything routed through InputFloat - keep the plain spin box, because there is no
    // span for a track to represent.
    QSlider *track = nullptr;
    auto *w = (low < high) ? labeledSlider<QDoubleSpinBox>(n, name, track)
                           : labeledControl<QDoubleSpinBox>(n, name);
    int decimals = 3;
    if (fmt)
    {
        const char *dot = strchr(fmt, '.');
        if (dot && dot[1] >= '0' && dot[1] <= '9')
            decimals = dot[1] - '0';
    }
    // Guard every one of these. QDoubleSpinBox::setDecimals has no early-out: it calls
    // setRange() and setValue() unconditionally, and QAbstractSpinBox::setRange clears the
    // cached size hints and calls updateGeometry(). Re-applying settings that never change
    // therefore invalidated the layout once per spin box per frame, so a panel full of
    // numeric fields cost more per frame than rendering the scene did.
    const double minimum = low < high ? low : -1e12;
    const double maximum = low < high ? high : 1e12;
    const double singleStep = step > 0 ? step : .1;
    {
        QSignalBlocker b(w);
        if (w->decimals() != decimals)
            w->setDecimals(decimals);
        if (!nearlySame(w->minimum(), roundToDecimals(minimum, decimals)) ||
            !nearlySame(w->maximum(), roundToDecimals(maximum, decimals)))
            w->setRange(minimum, maximum);
        if (!nearlySame(w->singleStep(), singleStep))
            w->setSingleStep(singleStep);
    }
    if (w->keyboardTracking())
        w->setKeyboardTracking(false);
    if (fresh)
        QObject::connect(w, &QDoubleSpinBox::valueChanged, w, [&n](double v) {
            n.value = v;
            n.fired = true;
        });
    if (track)
    {
        // The track is integral, so it carries a 0..kSliderSteps position and the real
        // range rides along as properties. Both are guarded: a dynamic property posts a
        // change event, and re-asserting one every frame for every slider is exactly the
        // per-frame cost the rest of this file goes out of its way to avoid.
        if (track->maximum() != kSliderSteps)
        {
            QSignalBlocker b(track);
            track->setRange(0, kSliderSteps);
            track->setSingleStep(kSliderSteps / 100);
            track->setPageStep(kSliderSteps / 10);
        }
        if (!nearlySame(track->property("uiLow").toDouble(), minimum))
            track->setProperty("uiLow", minimum);
        if (!nearlySame(track->property("uiHigh").toDouble(), maximum))
            track->setProperty("uiHigh", maximum);
        if (fresh)
            QObject::connect(track, &QSlider::valueChanged, track, [&n, track](int position) {
                const double lo = track->property("uiLow").toDouble();
                const double hi = track->property("uiHigh").toDouble();
                n.value = lo + (hi - lo) * (double(position) / double(kSliderSteps));
                n.fired = true;
            });
    }
    bool changed = take(n);
    if (changed)
        *value = float(n.value.toDouble());
    // setValue is not free either: it repaints the editor even when handed a value that
    // rounds to what is already displayed.
    if (!w->hasFocus() && !nearlySame(w->value(), roundToDecimals(*value, decimals)))
    {
        QSignalBlocker b(w);
        w->setValue(*value);
    }
    // Never fight the thumb the user is holding: the round trip through the spin box's
    // decimals would otherwise snap it back a fraction of a step every frame.
    if (track && !track->isSliderDown())
    {
        const int position = sliderPosition(*value, minimum, maximum);
        if (track->value() != position)
        {
            QSignalBlocker b(track);
            track->setValue(position);
        }
    }
    return changed;
}
bool InputFloat(const char *n, float *v, float step, float, const char *f, int flags)
{
    return DragFloat(n, v, step, 0, 0, f, flags);
}
bool DragInt(const char *name, int *value, float step, int low, int high, const char *, int)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    QSlider *track = nullptr;
    auto *w = (low < high) ? labeledSlider<QSpinBox>(n, name, track) : labeledControl<QSpinBox>(n, name);
    const int minimum = low < high ? low : INT_MIN;
    const int maximum = low < high ? high : INT_MAX;
    const int singleStep = std::max(1, int(step));
    {
        QSignalBlocker b(w);
        if (w->minimum() != minimum || w->maximum() != maximum)
            w->setRange(minimum, maximum);
        if (w->singleStep() != singleStep)
            w->setSingleStep(singleStep);
    }
    if (track)
    {
        // Integer sliders map one step per value, so no scaling is needed and the track
        // can be the authority on its own range.
        if (track->minimum() != minimum || track->maximum() != maximum)
        {
            QSignalBlocker b(track);
            track->setRange(minimum, maximum);
        }
        if (track->singleStep() != singleStep)
            track->setSingleStep(singleStep);
        if (fresh)
            QObject::connect(track, &QSlider::valueChanged, track, [&n](int v) {
                n.value = v;
                n.fired = true;
            });
    }
    if (w->keyboardTracking())
        w->setKeyboardTracking(false);
    if (fresh)
        QObject::connect(w, &QSpinBox::valueChanged, w, [&n](int v) {
            n.value = v;
            n.fired = true;
        });
    bool changed = take(n);
    if (changed)
        *value = n.value.toInt();
    if (!w->hasFocus() && w->value() != *value)
    {
        QSignalBlocker b(w);
        w->setValue(*value);
    }
    if (track && !track->isSliderDown() && track->value() != *value)
    {
        QSignalBlocker b(track);
        track->setValue(*value);
    }
    return changed;
}
bool InputInt(const char *n, int *v, int step, int, int flags)
{
    return DragInt(n, v, float(step), 0, 0, "%d", flags);
}
bool SliderFloat(const char *n, float *v, float low, float high, const char *f, int flags)
{
    return DragFloat(n, v, (high - low) / 100, low, high, f, flags);
}
bool SliderInt(const char *n, int *v, int low, int high, const char *f, int flags)
{
    return DragInt(n, v, 1, low, high, f, flags);
}
bool DragFloat3(const char *name, float *v, float step, float low, float high, const char *f, int flags)
{
    TextUnformatted(label(name).toUtf8().constData());
    PushID(name);
    bool changed = false;
    const char *axes[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i)
        changed = DragFloat(axes[i], v + i, step, low, high, f, flags) || changed;
    PopID();
    return changed;
}
bool DragFloatRange2(const char *name, float *a, float *b, float step, float low, float high, const char *f,
                     const char *f2, int flags)
{
    PushID(name);
    TextUnformatted(name);
    bool c = DragFloat("Min", a, step, low, high, f, flags);
    c = DragFloat("Max", b, step, low, high, f2 ? f2 : f, flags) || c;
    PopID();
    return c;
}
bool editColor(const char *name, float *v, int count, int flags)
{
    PushID(name);
    bool changed = Button(name);
    Node *button = last;
    QColor current = QColor::fromRgbF(std::clamp(v[0], 0.f, 1.f), std::clamp(v[1], 0.f, 1.f),
                                      std::clamp(v[2], 0.f, 1.f), count == 4 ? std::clamp(v[3], 0.f, 1.f) : 1);
    applyStyleSheet(button->widget, "border-left:8px solid " + current.name() + ";");
    if (changed)
    {
        QColor selected =
            QColorDialog::getColor(current, shell, label(name),
                                   count == 4 ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
        changed = selected.isValid();
        if (changed)
        {
            v[0] = selected.redF();
            v[1] = selected.greenF();
            v[2] = selected.blueF();
            if (count == 4)
                v[3] = selected.alphaF();
        }
    }
    if (flags & QtUiColorEditFlags_HDR)
    {
        const char *channels[] = {"R", "G", "B", "A"};
        for (int i = 0; i < count; ++i)
            changed = DragFloat(channels[i], v + i, .01f, 0, 0, "%.3f") || changed;
    }
    PopID();
    return changed;
}
bool ColorEdit3(const char *n, float *v, int f)
{
    return editColor(n, v, 3, f);
}
bool ColorEdit4(const char *n, float *v, int f)
{
    return editColor(n, v, 4, f);
}
bool Combo(const char *name, int *value, const char *const *items, int count, int)
{
    Node &n = node(key(name));
    bool fresh = !n.widget;
    auto *w = labeledControl<QComboBox>(n, name);
    applyToolTip(w, label(name));
    if (fresh)
        QObject::connect(w, &QComboBox::activated, w, [&n](int v) {
            n.value = v;
            n.fired = true;
        });
    bool changed = take(n);
    if (changed)
        *value = n.value.toInt();
    QSignalBlocker blocker(w);
    bool rebuild = w->count() != count;
    for (int i = 0; i < count && !rebuild; ++i)
        rebuild = w->itemText(i) != QString::fromUtf8(items[i]);
    if (rebuild)
    {
        w->clear();
        for (int i = 0; i < count; ++i)
            w->addItem(QString::fromUtf8(items[i]));
    }
    w->setCurrentIndex(*value);
    return changed;
}
bool Combo(const char *name, int *v, const char *items, int height)
{
    std::vector<const char *> list;
    for (auto p = items; *p; p += strlen(p) + 1)
        list.push_back(p);
    return Combo(name, v, list.data(), int(list.size()), height);
}
bool BeginCombo(const char *name, const char *preview, int)
{
    QString id = key(name);
    Node &n = node(id);
    bool fresh = !n.widget;
    auto *button = control<QToolButton>(n);
    button->setText(label(name) + ": " + label(preview));
    button->setPopupMode(QToolButton::InstantPopup);
    if (fresh)
        button->setMenu(new QMenu(button));
    Scope s;
    s.key = id + "/options";
    s.menu = button->menu();
    scopes.push_back(s);
    return true;
}
void EndCombo()
{
    End();
}
bool BeginTabBar(const char *name, int)
{
    QString id = key(name);
    Node &n = node(id);
    auto *tabs = control<QTabWidget>(n);
    Scope s;
    s.key = id;
    s.tabs = tabs;
    scopes.push_back(s);
    return true;
}
void EndTabBar()
{
    End();
}
bool BeginTabItem(const char *name, bool *open, int)
{
    if (open && !*open)
        return false;
    Scope &parent = scopes.back();
    QString id = key(name);
    Node &n = node(id);
    if (!n.widget)
    {
        n.widget = new QWidget;
        parent.tabs->addTab(n.widget, label(name));
    }
    if (parent.tabs->currentWidget() != n.widget)
        return false;
    push(n.widget, id, &n);
    return true;
}
void EndTabItem()
{
    End();
}
bool BeginTable(const char *name, int, int, UiVec2, float)
{
    QString id = key(name);
    Node &n = node(id);
    auto *body = control<QWidget>(n);
    push(body, id, &n);
    scopes.back().table = true;
    return true;
}
void EndTable()
{
    End();
}
void TableSetupColumn(const char *name, int, float, unsigned)
{
    Scope &s = scopes.back();
    s.same = s.header > 0;
    s.col = s.header++;
    TextUnformatted(name);
}
void TableHeadersRow()
{
}
void TableNextRow(int, float)
{
    Scope &s = scopes.back();
    s.same = false;
    s.col = 0;
}
bool TableSetColumnIndex(int col)
{
    Scope &s = scopes.back();
    s.same = col > 0;
    s.col = col;
    return true;
}
void OpenPopup(const char *name)
{
    node("popup/" + QString(name)).expanded = true;
}
bool BeginPopupModal(const char *name, bool *open, int flags)
{
    Node &state = node("popup/" + QString(name));
    if (!state.expanded)
        return false;
    bool visible = true;
    if (!Begin(name, &visible, flags))
    {
        End();
        state.expanded = false;
        if (open)
            *open = false;
        return false;
    }
    scopes.back().key = "popup/" + QString(name);
    if (auto *d = qobject_cast<QDialog *>(scopes.back().node->widget.data()))
        d->setWindowModality(Qt::WindowModal);
    return true;
}
bool contextPopup(const char *name, QWidget *target)
{
    QString id = key(name ? name : "context");
    Node &n = node(id);
    if (!n.widget)
        n.widget = new QMenu(shell);
    auto *menu = static_cast<QMenu *>(n.widget.data());
    if (target && !target->property(("ctx:" + id).toUtf8()).toBool())
    {
        target->setProperty(("ctx:" + id).toUtf8(), true);
        target->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(target, &QWidget::customContextMenuRequested, menu,
                         [target, menu](const QPoint &pos) { menu->popup(target->mapToGlobal(pos)); });
    }
    Scope s;
    s.key = id;
    s.menu = menu;
    s.node = &n;
    scopes.push_back(s);
    return true;
}
bool BeginPopupContextItem(const char *name, int)
{
    return contextPopup(name, last ? last->widget.data() : nullptr);
}
bool BeginPopupContextWindow(const char *name, int)
{
    return contextPopup(name, owner().body);
}
void CloseCurrentPopup()
{
    if (scopes.empty())
        return;
    auto &s = owner();
    if (s.menu)
        s.menu->close();
    if (s.node && s.node->widget)
        s.node->widget->hide();
    if (s.key.startsWith("popup/"))
        node(s.key).expanded = false;
}
void EndPopup()
{
    End();
}
void BeginDisabled(bool d)
{
    disabled.push_back(d);
}
void EndDisabled()
{
    if (!disabled.empty())
        disabled.pop_back();
}
void BeginGroup()
{
    QString id = serial("group");
    Node &n = node(id);
    auto *w = control<QWidget>(n);
    push(w, id, &n);
}
void EndGroup()
{
    End();
}
void PushID(const char *id)
{
    ids.push_back(QString::fromUtf8(id));
}
void PushID(int id)
{
    ids.push_back(QString::number(id));
}
void PopID()
{
    if (!ids.empty())
        ids.pop_back();
    // A header opened under the ID just popped has ended; what follows belongs outside it.
    closeHeaders(ids.size() + 1);
}
void PushStyleColor(int c, UiVec4 v)
{
    colors.emplace_back(c, v);
}
void PopStyleColor(int count)
{
    while (count-- > 0 && !colors.empty())
        colors.pop_back();
}
void PushStyleVar(int c, UiVec2 v)
{
    UiVec2 *field = c == QtUiStyleVar_WindowPadding ? &style.WindowPadding
                    : c == QtUiStyleVar_ItemSpacing ? &style.ItemSpacing
                                                    : &style.FramePadding;
    styles.emplace_back(c, *field);
    *field = v;
}
void PopStyleVar(int count)
{
    while (count-- > 0 && !styles.empty())
    {
        auto [c, v] = styles.back();
        styles.pop_back();
        (c == QtUiStyleVar_WindowPadding ? style.WindowPadding
         : c == QtUiStyleVar_ItemSpacing ? style.ItemSpacing
                                         : style.FramePadding) = v;
    }
}
void PushItemWidth(float w)
{
    widths.push_back(w);
}
void PopItemWidth()
{
    if (!widths.empty())
        widths.pop_back();
}
void SetNextItemWidth(float w)
{
    nextWidth = w;
}
void SameLine(float, float)
{
    if (!scopes.empty())
        scopes.back().same = true;
}
void Spacing()
{
    Dummy({0, 6});
}
void Dummy(UiVec2 size)
{
    Node &n = node(serial("space"));
    auto *w = control<QWidget>(n);
    w->setFixedSize(int(std::max(0.f, size.x)), int(std::max(0.f, size.y)));
}
void Separator()
{
    Node &n = node(serial("separator"));
    if (scopes.back().menu)
    {
        if (!n.action)
            n.action = scopes.back().menu->addSeparator();
        n.action->setVisible(true);
        return;
    }
    auto *w = control<QFrame>(n);
    w->setFrameShape(QFrame::HLine);
}
void SeparatorText(const char *s)
{
    Separator();
    TextUnformatted(s);
}
void Text(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    textWidget(format(f, a));
    va_end(a);
}
void TextWrapped(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    textWidget(format(f, a), true);
    va_end(a);
}
void TextDisabled(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    textWidget(format(f, a), false, QColor("#959699"));
    va_end(a);
}
void TextColored(UiVec4 c, const char *f, ...)
{
    va_list a;
    va_start(a, f);
    textWidget(format(f, a), false, color(c));
    va_end(a);
}
void BulletText(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    textWidget(QString::fromUtf8("\xe2\x80\xa2 ") + format(f, a));
    va_end(a);
}
void TextUnformatted(const char *s, const char *end)
{
    textWidget(end ? QString::fromUtf8(s, int(end - s)) : QString::fromUtf8(s));
}
void SetTooltip(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    if (last && last->widget)
        applyToolTip(last->widget, format(f, a));
    va_end(a);
}
void SetItemTooltip(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    if (last && last->widget)
        applyToolTip(last->widget, format(f, a));
    va_end(a);
}
void ProgressBar(float fraction, UiVec2, const char *text)
{
    Node &n = node(serial("progress"));
    auto *w = control<QProgressBar>(n);
    w->setRange(0, 1000);
    w->setValue(int(std::clamp(fraction, 0.f, 1.f) * 1000));
    w->setFormat(text ? QString::fromUtf8(text) : "%p%");
}
void Image(UiTextureID texture, UiVec2 size, UiVec2, UiVec2)
{
    if (scopes.back().key == "window/Viewport")
        return;
    Node &n = node(serial("image"));
    if (!n.widget)
    {
        auto *w = new Surface;
        w->setAttribute(Qt::WA_NativeWindow);
        w->setAttribute(Qt::WA_PaintOnScreen);
        w->setAttribute(Qt::WA_NoSystemBackground);
        n.widget = w;
    }
    place(n);
    n.widget->setMinimumSize(std::max(1, int(size.x)), std::max(1, int(size.y)));
    if (n.widget->isVisible())
        textureViews.push_back({reinterpret_cast<HWND>(n.widget->winId()), texture,
                                unsigned(n.widget->width() * n.widget->devicePixelRatioF()),
                                unsigned(n.widget->height() * n.widget->devicePixelRatioF())});
}
bool ImageButton(const char *name, UiTextureID id, UiVec2 size)
{
    bool clicked = Button(name);
    auto *w = static_cast<QPushButton *>(last->widget.data());
    auto it = icons.find(id);
    if (it != icons.end() && !it->second.isNull())
    {
        // setIcon has no early-out: it clears the size hint, then calls update() and
        // updateGeometry(), so re-applying it every frame invalidated the toolbar layout
        // every frame. The pixmap for a given id never changes, so apply it once.
        if (w->property("uiIcon").toULongLong() != static_cast<unsigned long long>(id))
        {
            w->setIcon(QIcon(it->second));
            w->setProperty("uiIcon", static_cast<unsigned long long>(id));
        }
        w->setIconSize(QSize(int(size.x), int(size.y)));
        w->setText("");
        w->setFixedSize(int(size.x + 8), int(size.y + 8));
        applyStyleSheet(w, "padding:3px;");
    }
    applyToolTip(w, label(name));
    return clicked;
}
bool IsItemHovered()
{
    return last && last->widget && last->widget->underMouse();
}
bool IsItemClicked(int button)
{
    if (!last)
        return false;
    bool fired = last->fired;
    last->fired = false;
    return fired || lastChanged || (IsItemHovered() && IsMouseClicked(button));
}
bool IsItemDeactivatedAfterEdit()
{
    return lastChanged;
}
bool IsWindowHovered(int)
{
    return !scopes.empty() && scopes.back().key == "window/Viewport"
               ? CameraInputAllowed()
               : !scopes.empty() && owner().body && owner().body->underMouse();
}
bool IsKeyPressed(int k, bool)
{
    return k >= 0 && k < 256 && pressed[k] && !QApplication::activeModalWidget();
}
bool IsMouseDown(int b)
{
    return b >= 0 && b < 3 && mouseDown[b];
}
bool IsMouseClicked(int b)
{
    return b >= 0 && b < 3 && mouseClicked[b];
}
bool IsMouseReleased(int b)
{
    return b >= 0 && b < 3 && mouseReleased[b];
}
bool IsMouseDoubleClicked(int b)
{
    return b >= 0 && b < 3 && mouseDouble[b];
}
QtUiIO &GetIO()
{
    return io;
}
QtUiStyle &GetStyle()
{
    return style;
}
UiVec4 GetStyleColorVec4(int c)
{
    return c == QtUiCol_Text ? UiVec4(.95f, .92f, .89f, 1) : UiVec4(.78f, .24f, .05f, 1);
}
QtUiViewport *GetMainViewport()
{
    return &viewport;
}
UiDrawList *GetWindowDrawList()
{
    return &drawList;
}
double GetTime()
{
    return timer.elapsed() / 1000.;
}
UiVec2 GetContentRegionAvail()
{
    const bool scene = scopes.back().key == "window/Viewport";
    // Measured on the owning window body: a section is only as tall as what is already in
    // it, so sizing a child from its height would feed back on itself.
    QWidget *w = scene ? ActiveSurface() : owner().body;
    int indent = 0;
    for (auto it = scopes.rbegin(); it != scopes.rend() && it->section != Section::None; ++it)
        indent += kSectionIndent;
    const int margin = scene ? 0 : 16;
    return w ? UiVec2(float(std::max(1, w->width() - margin - indent)), float(std::max(1, w->height() - margin)))
             : UiVec2(640, 480);
}
UiVec2 GetCursorScreenPos()
{
    QWidget *w = scopes.back().key == "window/Viewport" ? ActiveSurface() : scopes.back().body;
    QPoint p = w->mapToGlobal(QPoint(0, 0));
    return {float(p.x()), float(p.y())};
}
float GetCursorPosX()
{
    return 0;
}
void SetCursorPosX(float x)
{
    if (!scopes.empty() && scopes.back().grid)
        scopes.back().grid->setContentsMargins(int(std::max(0.f, x)), 4, 4, 4);
}
float GetFrameHeightWithSpacing()
{
    return static_cast<float>(QFontMetrics(QApplication::font()).height() + 14);
}
void SetNextWindowSize(UiVec2 s, int)
{
    nextSize = s;
}
void SetNextWindowPos(UiVec2 p, int, UiVec2 pivot)
{
    nextPos = p;
    nextPivot = pivot;
    positionSet = true;
}
void SetNextWindowSizeConstraints(UiVec2 min, UiVec2 max)
{
    minSize = min;
    maxSize = max;
}
void SetNextWindowViewport(unsigned)
{
}
void SetNextWindowBgAlpha(float a)
{
    nextAlpha = a;
}
void SetItemDefaultFocus()
{
    if (last && last->widget && last->widget->isVisible() && !QApplication::focusWidget())
        last->widget->setFocus();
}
QScrollArea *currentScroll()
{
    if (scopes.empty())
        return nullptr;
    QWidget *w = scopes.back().body;
    while (w)
    {
        if (auto *a = qobject_cast<QScrollArea *>(w))
            return a;
        w = w->parentWidget();
    }
    return nullptr;
}
float GetScrollY()
{
    auto *a = currentScroll();
    return a ? float(a->verticalScrollBar()->value()) : 0;
}
float GetScrollMaxY()
{
    auto *a = currentScroll();
    return a ? float(a->verticalScrollBar()->maximum()) : 0;
}
void SetScrollHereY(float ratio)
{
    auto *a = currentScroll();
    if (a)
        a->verticalScrollBar()->setValue(int(a->verticalScrollBar()->maximum() * ratio));
}
} // namespace QtUi

void UiDrawList::AddLine(UiVec2 a, UiVec2 b, UiU32 c, float width)
{
    overlay->commands.push_back([=](QPainter &p) {
        p.setPen(QPen(color(c), width));
        p.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    });
}
void UiDrawList::AddRect(UiVec2 a, UiVec2 b, UiU32 c, float radius, int, float width)
{
    overlay->commands.push_back([=](QPainter &p) {
        p.setPen(QPen(color(c), width));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(QPointF(a.x, a.y), QPointF(b.x, b.y)), radius, radius);
    });
}
void UiDrawList::AddRectFilled(UiVec2 a, UiVec2 b, UiU32 c, float radius)
{
    overlay->commands.push_back([=](QPainter &p) {
        p.setPen(Qt::NoPen);
        p.setBrush(color(c));
        p.drawRoundedRect(QRectF(QPointF(a.x, a.y), QPointF(b.x, b.y)), radius, radius);
    });
}
void UiDrawList::AddTriangle(UiVec2 a, UiVec2 b, UiVec2 c, UiU32 tint, float width)
{
    overlay->commands.push_back([=](QPainter &p) {
        p.setPen(QPen(color(tint), width));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF{QPointF(a.x, a.y), QPointF(b.x, b.y), QPointF(c.x, c.y)});
    });
}
void UiDrawList::AddTriangleFilled(UiVec2 a, UiVec2 b, UiVec2 c, UiU32 tint)
{
    overlay->commands.push_back([=](QPainter &p) {
        p.setPen(Qt::NoPen);
        p.setBrush(color(tint));
        p.drawPolygon(QPolygonF{QPointF(a.x, a.y), QPointF(b.x, b.y), QPointF(c.x, c.y)});
    });
}
void UiDrawList::AddCircle(UiVec2 a, float r, UiU32 c, int, float width)
{
    overlay->commands.push_back([=](QPainter &p) {
        p.setPen(QPen(color(c), width));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(a.x, a.y), r, r);
    });
}
void UiDrawList::AddCircleFilled(UiVec2 a, float r, UiU32 c, int)
{
    overlay->commands.push_back([=](QPainter &p) {
        p.setPen(Qt::NoPen);
        p.setBrush(color(c));
        p.drawEllipse(QPointF(a.x, a.y), r, r);
    });
}
void UiDrawList::AddPolyline(const UiVec2 *points, int count, UiU32 c, int flags, float width)
{
    for (int i = 1; i < count; ++i)
        AddLine(points[i - 1], points[i], c, width);
    if (flags && count > 1)
        AddLine(points[count - 1], points[0], c, width);
}
void UiDrawList::AddImage(UiTextureID id, UiVec2 a, UiVec2 b, UiVec2, UiVec2, UiU32 tint)
{
    auto it = icons.find(id);
    if (it == icons.end())
        return;
    QPixmap image = it->second;
    overlay->commands.push_back([=](QPainter &p) {
        p.save();
        p.setOpacity(float(tint >> 24) / 255);
        p.drawPixmap(QRectF(QPointF(a.x, a.y), QPointF(b.x, b.y)), image, QRectF(image.rect()));
        p.restore();
    });
}
