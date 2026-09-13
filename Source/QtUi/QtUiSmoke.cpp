// Standalone interaction test for the native Qt layer (no GPU or engine assets).
#include "QtUi.h"
#include <QtWidgets/QtWidgets>
#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    HWND host = CreateWindowExW(0, L"STATIC", L"Ptero Qt smoke test", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 800, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(host);
    assert(QtUi::Initialize(host, false));
    bool checked = false, properties = true;
    float value = 1;
    int menuClicks = 0;
    auto render = [&] {
        QtUi::NewFrame();
        if (QtUi::BeginMainMenuBar())
        {
            if (QtUi::BeginMenu("File"))
            {
                if (QtUi::MenuItem("New"))
                    ++menuClicks;
                QtUi::EndMenu();
            }
            QtUi::EndMainMenuBar();
        }
        QtUi::Begin("Viewport");
        QtUi::End();
        QtUi::Begin("Toolbar");
        QtUi::Button("Select");
        QtUi::SameLine();
        QtUi::Button("Move");
        QtUi::End();
        QtUi::Begin("Components");
        QtUi::Button("Geometry");
        QtUi::End();
        if (properties)
        {
            QtUi::Begin("Properties", &properties);
            QtUi::Checkbox("Enabled", &checked);
            QtUi::DragFloat("Intensity", &value, .1f, 0, 10);
            QtUi::End();
        }
        QtUi::EndFrame();
        QCoreApplication::processEvents();
    };
    render();
    render();
    QCheckBox *box = nullptr;
    QDoubleSpinBox *spin = nullptr;
    QMainWindow *main = nullptr;
    for (QWidget *w : QApplication::allWidgets())
    {
        if (auto *c = qobject_cast<QCheckBox *>(w); c && c->text() == "Enabled")
            box = c;
        if (auto *c = qobject_cast<QDoubleSpinBox *>(w); c && c->accessibleName() == "Intensity")
            spin = c;
        if (auto *c = qobject_cast<QMainWindow *>(w))
            main = c;
    }
    assert(box && spin && main);
    assert(QApplication::palette().color(QPalette::Window).lightness() < 40);
    assert(QApplication::palette().color(QPalette::Highlight) == QColor("#c73d0d"));
    assert(QApplication::font().family().contains("Playfair"));
    box->click();
    spin->setValue(3.5);
    render();
    assert(checked && value == 3.5f);
    render();
    assert(checked && value == 3.5f); // edits are consumed exactly once
    for (auto *action : main->findChildren<QAction *>())
        if (action->text() == "New")
            action->trigger();
    render();
    render();
    assert(menuClicks == 1);
    assert(GetAncestor(QtUi::ViewportHandle(), GA_ROOT) == host);
    QtUi::ResizeHost(1024, 700);
    render();
    assert(main->width() > 0 && main->width() <= 1024);

    // The shell must sit exactly on the host's client origin. An embedded window Qt
    // still believes is top-level drifts by its phantom frame margin, leaving gutters
    // the host never repaints (stale menu rows, unpainted rectangles).
    const HWND shellWindow = reinterpret_cast<HWND>(main->winId());
    assert(GetParent(shellWindow) == host);
    RECT shellRect{};
    GetWindowRect(shellWindow, &shellRect);
    POINT shellOrigin{shellRect.left, shellRect.top};
    ScreenToClient(host, &shellOrigin);
    assert(shellOrigin.x == 0 && shellOrigin.y == 0);

    // ...and Qt's own screen coordinates must agree with Win32, since the overlay,
    // popups and gizmo hit tests are all placed through mapToGlobal.
    const QPoint qtOrigin = main->mapToGlobal(QPoint(0, 0));
    POINT win32Origin{0, 0};
    ClientToScreen(shellWindow, &win32Origin);
    const qreal shellDpr = main->devicePixelRatioF();
    assert(std::abs(std::lround(qtOrigin.x() * shellDpr) - win32Origin.x) <= 1);
    assert(std::abs(std::lround(qtOrigin.y() * shellDpr) - win32Origin.y) <= 1);
    properties = false;
    render();
    properties = true;
    render();
    assert(box->isVisible());
    const auto image = main->grab();
    assert(image.save("Cache/qt-ui-smoke.png"));
    QtUi::Shutdown();
    DestroyWindow(host);
    std::puts("Qt UI smoke: passed");
}
