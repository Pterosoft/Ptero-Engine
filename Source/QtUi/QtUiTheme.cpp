#include "QtUiTheme.h"
#include "QtUi.h"
#pragma warning(push)
#pragma warning(disable : 4996)
#include <QtWidgets/QtWidgets>
#include <QtGui/QIconEngine>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtCore/QtMath>
#pragma warning(pop)
#include <dwmapi.h>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr qreal kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------
// Built-in defaults. These are "Ptero Dark"; a style file only has to name what it changes,
// and a token it leaves out keeps the value given here.
// ---------------------------------------------------------------------------------------
const std::pair<const char *, const char *> kDefaultColors[] = {
    {"window", "#1a1a1c"},        // main window, menu bar, toolbar
    {"panel", "#1f1f22"},         // dock contents
    {"header", "#252528"},        // dock titles, section headers, table headers
    {"base", "#131315"},          // text fields, lists
    {"alternateBase", "#1c1c1f"}, // alternating rows
    {"button", "#27272a"},
    {"buttonHover", "#303034"},
    {"buttonPressed", "#1d1d20"},
    {"border", "#333337"},
    {"borderStrong", "#48484d"},
    {"separator", "#2a2a2e"},
    {"text", "#e8e6e3"},
    {"textDim", "#9a9a9f"},
    {"textDisabled", "#626267"},
    {"accent", "#f26b1d"},
    {"accentHover", "#ff8638"},
    {"accentPressed", "#d45810"},
    {"accentSoft", "#f26b1d2e"}, // checked buttons, selected tabs
    {"accentText", "#ffffff"},   // text on an accent background
    {"checkedText", "#ffffff"},  // text on a checked button (accentSoft)
    {"menu", "#222225"},
    {"menuBorder", "#3a3a3e"},
    {"tooltip", "#2b2b2f"},
    {"scrollbar", "#3a3a3f"},
    {"scrollbarHover", "#56565c"},
    {"icon", "#d9d9dc"},
    {"iconActive", "#ffffff"},   // highlighted menu items, focused buttons
    {"iconChecked", "#f26b1d"},  // checked toolbar and component buttons
    {"iconSelected", "#ffffff"}, // on a selection background
    {"iconDisabled", "#5a5a5f"},
};
const std::pair<const char *, double> kDefaultMetrics[] = {
    {"radius", 4},          {"radiusSmall", 3},    {"iconSize", 18},     {"menuIconSize", 16},
    {"toolButtonSize", 36}, {"toolIconSize", 22},  {"listButtonHeight", 24},
    {"indicatorSize", 16},  {"scrollbarWidth", 10}, {"splitterWidth", 4},
};

// The stylesheet. @name is replaced by the style's token of that name: colors as colors,
// metrics as pixel lengths, and @checkImage and @arrowUpImage / Down / Right by
// images rendered from the icon folder in the style's colors.
//
// Deliberately no "color" on QWidget or QLabel: QtUi tints individual labels through
// their palette (TextColored), and a stylesheet color would override every one of them.
// Text color comes from the application palette, which the style also sets.
const char *const kSheet = R"QSS(
QMainWindow, QDialog { background: @window; }
QMainWindow::separator { background: @window; width: @splitterWidth; height: @splitterWidth; }
QMainWindow::separator:hover { background: @accent; }
QToolTip { background: @tooltip; color: @text; border: 1px solid @menuBorder; border-radius: @radiusSmall; padding: 4px 6px; }

QMenuBar { background: @window; border: none; padding: 2px 4px; }
QMenuBar::item { background: transparent; color: @text; padding: 5px 10px; border-radius: @radiusSmall; }
QMenuBar::item:selected { background: @buttonHover; }
QMenuBar::item:pressed { background: @accent; color: @accentText; }
QMenuBar::item:disabled { color: @textDisabled; }

QMenu { background: @menu; border: 1px solid @menuBorder; border-radius: @radius; padding: 4px; }
QMenu::item { background: transparent; color: @text; padding: 6px 28px 6px 8px; border-radius: @radiusSmall; }
QMenu::item:selected { background: @accent; color: @accentText; }
QMenu::item:disabled { color: @textDisabled; background: transparent; }
QMenu::icon { padding-left: 8px; }
QMenu::separator { height: 1px; background: @separator; margin: 4px 6px; }
QMenu::indicator { width: @indicatorSize; height: @indicatorSize; margin-left: 8px; border-radius: @radiusSmall; }
QMenu::indicator:non-exclusive:unchecked { border: 1px solid @borderStrong; background: @base; }
QMenu::indicator:non-exclusive:checked { border: 1px solid @accent; background: @accent; image: url(@checkImage); }
QMenu::indicator:exclusive:checked { border: 1px solid @accent; background: @accent; image: url(@checkImage); }
QMenu::right-arrow { image: url(@arrowRightImage); width: 12px; height: 12px; margin-right: 6px; }

QToolBar { background: @window; border: none; border-bottom: 1px solid @separator; padding: 6px; spacing: 4px; }
QToolBar::handle { background: @accent; width: 3px; margin: 6px 8px 6px 2px; border-radius: 1px; }
QToolBar::separator { background: @border; width: 1px; margin: 6px 8px; }

QDockWidget { color: @text; }
QDockWidget::title { background: @panel; padding: 8px 10px; border-bottom: 1px solid @separator; text-align: left; }
QDockWidget QWidget#uiBody { background: @panel; }
QDockWidget QScrollArea, QDockWidget QAbstractScrollArea > QWidget#qt_scrollarea_viewport { background: @panel; border: none; }

QPushButton { background: @button; color: @text; border: 1px solid @border; border-radius: @radius; padding: 5px 12px; icon-size: @iconSize; }
QPushButton:hover { background: @buttonHover; border-color: @borderStrong; }
QPushButton:pressed { background: @buttonPressed; }
QPushButton:checked { background: @accentSoft; border-color: @accent; color: @checkedText; }
QPushButton:checked:hover { border-color: @accentHover; }
QPushButton:disabled { color: @textDisabled; border-color: @separator; }
QPushButton[uiSelectable="true"] { text-align: left; padding: 6px 10px; min-height: @listButtonHeight; }
QPushButton[uiSelectable="true"]:!checked { background: @header; }
QPushButton[uiSelectable="true"]:!checked:hover { background: @buttonHover; }
QPushButton[uiIconButton="true"] { padding: 0px; icon-size: @toolIconSize; }
QPushButton[uiIconButton="true"]:checked { border: 2px solid @accent; }

QToolButton { background: @button; color: @text; border: 1px solid @border; border-radius: @radius; padding: 4px 8px; }
QToolButton:hover { background: @buttonHover; border-color: @borderStrong; }
QToolButton:checked { background: @accentSoft; border-color: @accent; }
QToolButton[uiHeader="true"] { background: @header; border: none; border-radius: @radiusSmall; padding: 6px 6px; font-weight: 600; }
QToolButton[uiHeader="true"]:hover { background: @buttonHover; }

QLineEdit, QAbstractSpinBox, QComboBox, QTextEdit, QPlainTextEdit {
    background: @base; color: @text; border: 1px solid @border; border-radius: @radiusSmall; padding: 3px 6px;
    selection-background-color: @accent; selection-color: @accentText; }
QLineEdit:hover, QAbstractSpinBox:hover, QComboBox:hover { border-color: @borderStrong; }
QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QTextEdit:focus, QPlainTextEdit:focus { border-color: @accent; }
QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled { color: @textDisabled; border-color: @separator; }
QAbstractSpinBox { padding-right: 18px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { subcontrol-origin: border; width: 16px; border: none;
    background: transparent; }
QAbstractSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: @radiusSmall; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: @radiusSmall; }
QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { background: @buttonHover; }
QAbstractSpinBox::up-arrow { image: url(@arrowUpImage); width: 10px; height: 10px; }
QAbstractSpinBox::down-arrow { image: url(@arrowDownImage); width: 10px; height: 10px; }
QComboBox { padding-right: 22px; }
QComboBox::drop-down { border: none; width: 20px; subcontrol-origin: padding; subcontrol-position: center right; }
QComboBox::down-arrow { image: url(@arrowDownImage); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: @menu; border: 1px solid @menuBorder; outline: 0; padding: 2px;
    selection-background-color: @accent; selection-color: @accentText; }

QCheckBox, QRadioButton { spacing: 8px; }
QCheckBox::indicator, QRadioButton::indicator { width: @indicatorSize; height: @indicatorSize; border: 1px solid @borderStrong; background: @base; }
QCheckBox::indicator { border-radius: @radiusSmall; }
QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(@checkImage); }
QRadioButton::indicator:checked { background: @accent; border: 4px solid @base; }
QCheckBox::indicator:disabled { background: @separator; border-color: @separator; }

QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::handle:horizontal { background: @text; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: @accentHover; }
QSlider::handle:horizontal:disabled { background: @textDisabled; }
QSlider::sub-page:horizontal:disabled { background: @borderStrong; }

QTabWidget::pane { border: 1px solid @border; border-radius: @radius; background: @panel; top: -1px; }
QTabBar::tab { background: @window; color: @textDim; padding: 7px 16px; border: none; border-top: 2px solid transparent; }
QTabBar::tab:hover { background: @buttonHover; color: @text; }
QTabBar::tab:selected { background: @accentSoft; color: @text; border-top: 2px solid @accent; }

QTreeView, QListView, QTableView { background: @base; alternate-background-color: @alternateBase; border: 1px solid @border;
    border-radius: @radiusSmall; outline: 0; selection-background-color: @accent; selection-color: @accentText; }
QTreeView::item, QListView::item { padding: 3px 2px; }
QTreeView::item:hover, QListView::item:hover { background: @buttonHover; }
QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: @accent; color: @accentText; }
QHeaderView::section { background: @header; color: @textDim; padding: 5px 8px; border: none;
    border-right: 1px solid @border; border-bottom: 1px solid @border; }

QScrollArea { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: @scrollbarWidth; margin: 0; }
QScrollBar:horizontal { background: transparent; height: @scrollbarWidth; margin: 0; }
QScrollBar::handle:vertical { background: @scrollbar; min-height: 24px; border-radius: 3px; margin: 2px; }
QScrollBar::handle:horizontal { background: @scrollbar; min-width: 24px; border-radius: 3px; margin: 2px; }
QScrollBar::handle:hover { background: @scrollbarHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QProgressBar { background: @base; color: @text; border: 1px solid @border; border-radius: @radiusSmall; text-align: center; }
QProgressBar::chunk { background: @accent; border-radius: 2px; }
QFrame[frameShape="4"], QFrame[frameShape="5"] { background: @separator; border: none; max-height: 1px; }
QGroupBox { border: 1px solid @border; border-radius: @radius; margin-top: 14px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: @textDim; }
)QSS";

// ---------------------------------------------------------------------------------------
// SVG icons.
//
// The bundled Qt is qtbase alone, so there is no QtSvg to read the icon files with. Stroke
// icons such as Lucide's only use a handful of shapes, and QPainterPath already has every
// primitive they need, so they are read here instead. Supported: path (all commands, arcs
// included), line, rect, circle, ellipse, polyline and polygon, with fill and stroke
// inherited from enclosing elements. Transforms, gradients and text are not.
// ---------------------------------------------------------------------------------------
struct SvgShape
{
    QPainterPath path;
    bool fill = false, stroke = true;
};
struct SvgIcon
{
    QRectF viewBox{0, 0, 24, 24};
    qreal strokeWidth = 2;
    Qt::PenCapStyle cap = Qt::RoundCap;
    Qt::PenJoinStyle join = Qt::RoundJoin;
    std::vector<SvgShape> shapes;
};

class PathReader
{
  public:
    explicit PathReader(const QString &text) : s(text), n(int(text.size())) {}
    QPainterPath read()
    {
        QPainterPath p;
        QPointF cur, start, lastCubic, lastQuad;
        QChar cmd, previous;
        for (;;)
        {
            skip();
            if (i >= n)
                break;
            if (s[i].isLetter())
                cmd = s[i++];
            else if (cmd.isNull())
                break; // numbers with no command before them
            const bool rel = cmd.isLower();
            const QChar c = cmd.toUpper();
            const QPointF origin = rel ? cur : QPointF();
            qreal a, b;
            if (c == 'M')
            {
                if (!pair(a, b))
                    break;
                cur = start = origin + QPointF(a, b);
                p.moveTo(cur);
                // Further pairs after a moveto are implicit linetos.
                cmd = rel ? QChar('l') : QChar('L');
            }
            else if (c == 'L')
            {
                if (!pair(a, b))
                    break;
                cur = origin + QPointF(a, b);
                p.lineTo(cur);
            }
            else if (c == 'H')
            {
                if (!number(a))
                    break;
                cur.setX(rel ? cur.x() + a : a);
                p.lineTo(cur);
            }
            else if (c == 'V')
            {
                if (!number(a))
                    break;
                cur.setY(rel ? cur.y() + a : a);
                p.lineTo(cur);
            }
            else if (c == 'C' || c == 'S')
            {
                QPointF c1;
                if (c == 'C')
                {
                    if (!pair(a, b))
                        break;
                    c1 = origin + QPointF(a, b);
                }
                else
                    c1 = (previous == 'C' || previous == 'S') ? 2 * cur - lastCubic : cur;
                qreal x2, y2;
                if (!pair(x2, y2) || !pair(a, b))
                    break;
                const QPointF c2 = origin + QPointF(x2, y2);
                cur = origin + QPointF(a, b);
                p.cubicTo(c1, c2, cur);
                lastCubic = c2;
            }
            else if (c == 'Q' || c == 'T')
            {
                QPointF c1;
                if (c == 'Q')
                {
                    if (!pair(a, b))
                        break;
                    c1 = origin + QPointF(a, b);
                }
                else
                    c1 = (previous == 'Q' || previous == 'T') ? 2 * cur - lastQuad : cur;
                if (!pair(a, b))
                    break;
                cur = origin + QPointF(a, b);
                p.quadTo(c1, cur);
                lastQuad = c1;
            }
            else if (c == 'A')
            {
                qreal rx, ry, rotation;
                bool large, sweep;
                if (!number(rx) || !number(ry) || !number(rotation) || !flag(large) || !flag(sweep) || !pair(a, b))
                    break;
                const QPointF end = origin + QPointF(a, b);
                arc(p, cur, end, rx, ry, rotation, large, sweep);
                cur = end;
            }
            else if (c == 'Z')
            {
                p.closeSubpath();
                cur = start;
                // Nothing may follow a closepath but another command.
                previous = c;
                cmd = QChar();
                continue;
            }
            else
                break; // unknown command
            previous = c;
        }
        return p;
    }

  private:
    const QString &s;
    int n, i = 0;

    void skip()
    {
        while (i < n && (s[i].isSpace() || s[i] == ','))
            ++i;
    }
    bool number(qreal &out)
    {
        skip();
        const int begin = i;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            ++i;
        bool digits = false, dot = false;
        while (i < n)
        {
            if (s[i].isDigit())
                digits = true;
            else if (s[i] == '.' && !dot)
                dot = true;
            else
                break;
            ++i;
        }
        if (digits && i < n && (s[i] == 'e' || s[i] == 'E'))
        {
            const int mark = i++;
            if (i < n && (s[i] == '+' || s[i] == '-'))
                ++i;
            if (i < n && s[i].isDigit())
                while (i < n && s[i].isDigit())
                    ++i;
            else
                i = mark;
        }
        if (!digits)
        {
            i = begin;
            return false;
        }
        out = s.mid(begin, i - begin).toDouble();
        return true;
    }
    bool pair(qreal &x, qreal &y)
    {
        return number(x) && number(y);
    }
    // Arc flags may be written with no separator at all: "a2 2 0 014 0".
    bool flag(bool &out)
    {
        skip();
        if (i < n && (s[i] == '0' || s[i] == '1'))
        {
            out = s[i++] == '1';
            return true;
        }
        return false;
    }
    // SVG endpoint arc to cubic Béziers (SVG 1.1 appendix F.6.5).
    static void arc(QPainterPath &p, QPointF p0, QPointF p1, qreal rx, qreal ry, qreal degrees, bool large,
                    bool sweep)
    {
        if (p0 == p1)
            return;
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx == 0 || ry == 0)
        {
            p.lineTo(p1);
            return;
        }
        const qreal phi = qDegreesToRadians(degrees), cs = std::cos(phi), sn = std::sin(phi);
        const qreal dx = (p0.x() - p1.x()) / 2, dy = (p0.y() - p1.y()) / 2;
        const qreal x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
        const qreal lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1)
        {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const qreal num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const qreal den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        qreal coef = den > 0 ? std::sqrt(std::max<qreal>(0, num / den)) : 0;
        if (large == sweep)
            coef = -coef;
        const qreal cxp = coef * rx * y1 / ry, cyp = -coef * ry * x1 / rx;
        const qreal cx = cs * cxp - sn * cyp + (p0.x() + p1.x()) / 2;
        const qreal cy = sn * cxp + cs * cyp + (p0.y() + p1.y()) / 2;
        const auto angle = [](qreal ux, qreal uy, qreal vx, qreal vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const qreal theta = angle(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        qreal delta = angle((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && delta > 0)
            delta -= 2 * kPi;
        else if (sweep && delta < 0)
            delta += 2 * kPi;
        const int segments = std::max(1, int(std::ceil(std::abs(delta) / (kPi / 2) - 1e-9)));
        const qreal step = delta / segments, t = 4.0 / 3.0 * std::tan(step / 4);
        const auto map = [&](qreal x, qreal y) {
            return QPointF(cx + cs * rx * x - sn * ry * y, cy + sn * rx * x + cs * ry * y);
        };
        for (int k = 0; k < segments; ++k)
        {
            const qreal a1 = theta + k * step, a2 = a1 + step;
            const qreal c1 = std::cos(a1), s1 = std::sin(a1), c2 = std::cos(a2), s2 = std::sin(a2);
            p.cubicTo(map(c1 - t * s1, s1 + t * c1), map(c2 + t * s2, s2 - t * c2), map(c2, s2));
        }
    }
};

std::vector<qreal> numberList(const QString &text)
{
    std::vector<qreal> out;
    static const QRegularExpression token(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)");
    for (auto it = token.globalMatch(text); it.hasNext();)
        out.push_back(it.next().captured(0).toDouble());
    return out;
}

std::shared_ptr<SvgIcon> readSvg(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return nullptr;
    auto icon = std::make_shared<SvgIcon>();
    // Paint state inherited down the element tree: fill, stroke.
    struct Paint
    {
        bool fill = false, stroke = false;
    };
    std::vector<Paint> stack{Paint{true, false}}; // SVG's own defaults: black fill, no stroke
    QXmlStreamReader xml(&f);
    const auto paintFrom = [](const QXmlStreamAttributes &attrs, Paint inherited) {
        if (attrs.hasAttribute("fill"))
            inherited.fill = attrs.value("fill") != QLatin1String("none");
        if (attrs.hasAttribute("stroke"))
            inherited.stroke = attrs.value("stroke") != QLatin1String("none");
        return inherited;
    };
    while (!xml.atEnd())
    {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::EndElement)
        {
            if (stack.size() > 1)
                stack.pop_back();
            continue;
        }
        if (token != QXmlStreamReader::StartElement)
            continue;
        const auto attrs = xml.attributes();
        const auto num = [&](const char *name, qreal fallback = 0) {
            bool ok = false;
            const qreal v = attrs.value(QLatin1String(name)).toDouble(&ok);
            return ok ? v : fallback;
        };
        const Paint paint = paintFrom(attrs, stack.back());
        stack.push_back(paint);
        const QStringView tag = xml.name();
        if (tag == QLatin1String("svg"))
        {
            const auto box = numberList(attrs.value("viewBox").toString());
            if (box.size() == 4 && box[2] > 0 && box[3] > 0)
                icon->viewBox = QRectF(box[0], box[1], box[2], box[3]);
            else if (num("width") > 0 && num("height") > 0)
                icon->viewBox = QRectF(0, 0, num("width"), num("height"));
            icon->strokeWidth = num("stroke-width", 1);
            const auto cap = attrs.value("stroke-linecap");
            icon->cap = cap == QLatin1String("round")    ? Qt::RoundCap
                        : cap == QLatin1String("square") ? Qt::SquareCap
                                                         : Qt::FlatCap;
            const auto join = attrs.value("stroke-linejoin");
            icon->join = join == QLatin1String("round")   ? Qt::RoundJoin
                         : join == QLatin1String("bevel") ? Qt::BevelJoin
                                                          : Qt::MiterJoin;
            continue;
        }
        SvgShape shape;
        shape.fill = paint.fill;
        shape.stroke = paint.stroke;
        if (tag == QLatin1String("path"))
        {
            const QString d = attrs.value("d").toString();
            shape.path = PathReader(d).read();
        }
        else if (tag == QLatin1String("line"))
        {
            shape.path.moveTo(num("x1"), num("y1"));
            shape.path.lineTo(num("x2"), num("y2"));
        }
        else if (tag == QLatin1String("rect"))
        {
            const QRectF r(num("x"), num("y"), num("width"), num("height"));
            qreal rx = num("rx", -1), ry = num("ry", -1);
            if (rx < 0)
                rx = ry;
            if (ry < 0)
                ry = rx;
            if (rx > 0)
                shape.path.addRoundedRect(r, rx, ry);
            else
                shape.path.addRect(r);
        }
        else if (tag == QLatin1String("circle"))
            shape.path.addEllipse(QPointF(num("cx"), num("cy")), num("r"), num("r"));
        else if (tag == QLatin1String("ellipse"))
            shape.path.addEllipse(QPointF(num("cx"), num("cy")), num("rx"), num("ry"));
        else if (tag == QLatin1String("polyline") || tag == QLatin1String("polygon"))
        {
            const auto points = numberList(attrs.value("points").toString());
            for (std::size_t k = 0; k + 1 < points.size(); k += 2)
                k == 0 ? shape.path.moveTo(points[k], points[k + 1]) : shape.path.lineTo(points[k], points[k + 1]);
            if (tag == QLatin1String("polygon"))
                shape.path.closeSubpath();
        }
        else
            continue;
        if (!shape.path.isEmpty())
            icon->shapes.push_back(std::move(shape));
    }
    if (xml.hasError() || icon->shapes.empty())
        return nullptr;
    return icon;
}

void paintSvg(QPainter &p, const SvgIcon &icon, const QRectF &target, const QColor &color)
{
    const QRectF &box = icon.viewBox;
    const qreal scale = std::min(target.width() / box.width(), target.height() / box.height());
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(target.center());
    p.scale(scale, scale);
    p.translate(-box.center());
    const QPen pen(color, icon.strokeWidth, Qt::SolidLine, icon.cap, icon.join);
    for (const SvgShape &shape : icon.shapes)
    {
        if (shape.fill)
            p.fillPath(shape.path, color);
        if (shape.stroke)
            p.strokePath(shape.path, pen);
    }
    p.restore();
}

// ---------------------------------------------------------------------------------------
// Active style
// ---------------------------------------------------------------------------------------
struct Style
{
    QString file; // absolute path; empty for the built-in defaults
    QString name, description;
    bool dark = true;
    QString fontFamily;
    qreal fontPointSize = 0;
    QString iconFolder = "Icons/Editor";
    std::map<QString, QString> colors; // as written in the file
    std::map<QString, double> metrics;
    QString extraSheet;
};
Style active;
int generation = 0;
HWND hostWindow = nullptr;
QByteArray errorText, currentFileText, stylesDirText;
QFileSystemWatcher *watcher = nullptr;
bool reloadQueued = false;
std::vector<QtUi::StyleEntry> entries;
bool entriesScanned = false;
std::map<QString, std::shared_ptr<SvgIcon>> svgCache; // null entry: looked for, not found
std::map<QString, QPixmap> pngCache;
QHash<QString, QPixmap> renderCache;
std::map<QString, QIcon> iconCache;
// Every color token of the active style as a QColor. A token set to something that is not
// a color (a gradient) resolves to its built-in value, which always is one.
std::map<std::string, QColor, std::less<>> resolvedColors;


QString dataDirectory()
{
    static QString cached;
    if (!cached.isEmpty())
        return cached;
    // Walk up from the executable, the way the renderer finds Data/Icons: Editor.exe runs
    // from Binaries/, with Data/ beside it one level up.
    QDir dir(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 6; ++depth)
    {
        if (QFileInfo(dir.filePath("Data/Styles")).isDir() || QFileInfo(dir.filePath("Data/Icons")).isDir())
            return cached = QDir::cleanPath(dir.filePath("Data"));
        if (!dir.cdUp())
            break;
    }
    return cached = QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).filePath("Data"));
}
QString stylesDirectory()
{
    return QDir(dataDirectory()).filePath("Styles");
}
QString iconDirectory()
{
    const QString folder = active.iconFolder;
    return QDir::isAbsolutePath(folder) ? folder : QDir(dataDirectory()).filePath(folder);
}

// "#rgb", "#rrggbb", "#rrggbbaa" (CSS order - alpha last), "rgb(r,g,b)", "rgba(r,g,b,a)" with a
// from 0 to 1 or as a percentage, or a named color. Invalid when it is none of those,
// which is how raw QSS values (gradients) are told apart from colors.
QColor parseColor(const QString &text)
{
    const QString v = text.trimmed();
    if (v.startsWith('#') && v.size() == 9)
    {
        bool ok = false;
        const uint rgba = v.mid(1).toUInt(&ok, 16);
        if (ok)
            return QColor((rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255);
    }
    static const QRegularExpression fn(
        R"(^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*(?:,\s*([\d.]+)(%?)\s*)?\)$)",
        QRegularExpression::CaseInsensitiveOption);
    if (const auto m = fn.match(v); m.hasMatch())
    {
        qreal alpha = 1;
        if (m.hasCaptured(4) && !m.captured(4).isEmpty())
            alpha = m.captured(4).toDouble() / (m.captured(5).isEmpty() ? 1.0 : 100.0);
        return QColor(std::clamp(m.captured(1).toInt(), 0, 255), std::clamp(m.captured(2).toInt(), 0, 255),
                      std::clamp(m.captured(3).toInt(), 0, 255), std::clamp(int(std::lround(alpha * 255)), 0, 255));
    }
    return QColor::fromString(v);
}
// QSS reads "#aarrggbb" through QColor, so that is the one unambiguous way to hand it alpha.
QString qssColor(const QColor &c)
{
    return c.name(c.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
}

QColor iconColor(QIcon::Mode mode, QIcon::State state)
{
    const char *token = mode == QIcon::Disabled   ? "iconDisabled"
                        : mode == QIcon::Selected ? "iconSelected"
                        : state == QIcon::On      ? "iconChecked"
                        : mode == QIcon::Active   ? "iconActive"
                                                  : "icon";
    return QtUiTheme::Color(token);
}

const SvgIcon *svgFor(const QString &name)
{
    auto it = svgCache.find(name);
    if (it == svgCache.end())
        it = svgCache.emplace(name, readSvg(QDir(iconDirectory()).filePath(name + ".svg"))).first;
    return it->second.get();
}
const QPixmap *pngFor(const QString &name)
{
    auto it = pngCache.find(name);
    if (it == pngCache.end())
        it = pngCache.emplace(name, QPixmap(QDir(iconDirectory()).filePath(name + ".png"))).first;
    return it->second.isNull() ? nullptr : &it->second;
}

// Paints by name from whatever the active style is at paint time, so a style switch
// recolors - or swaps the icon set of - every widget already holding one of these.
class ThemeIconEngine : public QIconEngine
{
  public:
    explicit ThemeIconEngine(QString iconName) : name(std::move(iconName)) {}
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        // A PNG beside the SVG wins, for anyone who would rather draw their icons.
        if (const QPixmap *png = pngFor(name))
            painter->drawPixmap(rect, *png);
        else if (const SvgIcon *svg = svgFor(name))
            paintSvg(*painter, *svg, rect, iconColor(mode, state));
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        // Styles ask for the pixmap on every paint of every button; render each variant once.
        const QString cacheKey = QStringLiteral("%1|%2x%3|%4|%5|%6")
                                     .arg(name)
                                     .arg(size.width())
                                     .arg(size.height())
                                     .arg(int(mode))
                                     .arg(int(state))
                                     .arg(scale);
        if (auto it = renderCache.constFind(cacheKey); it != renderCache.constEnd())
            return *it;
        QPixmap pm(size * scale);
        pm.fill(Qt::transparent);
        pm.setDevicePixelRatio(scale);
        {
            QPainter p(&pm);
            paint(&p, QRect(QPoint(), size), mode, state);
        }
        renderCache.insert(cacheKey, pm);
        return pm;
    }
    QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override
    {
        return size;
    }
    bool isNull() override
    {
        return !pngFor(name) && !svgFor(name);
    }
    QString iconName() override
    {
        return name;
    }
    QString key() const override
    {
        return QStringLiteral("PteroTheme");
    }
    QIconEngine *clone() const override
    {
        return new ThemeIconEngine(name);
    }

  private:
    QString name;
};

// Stylesheet images have to be files. Render the few the sheet needs into the cache folder,
// named per generation so Qt's own image cache cannot serve the previous style's colors.
QString renderImage(const QString &icon, const char *colorToken, int size)
{
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("styles");
    QDir().mkpath(dir);
    const QString file = QDir(dir).filePath(QStringLiteral("%1-%2.png").arg(icon).arg(generation));
    const SvgIcon *svg = svgFor(icon);
    if (!svg)
        return QString();
    constexpr int kScale = 2; // sharp on high-DPI screens; the sheet sizes it down
    QImage image(size * kScale, size * kScale, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter p(&image);
        paintSvg(p, *svg, QRectF(0, 0, image.width(), image.height()), QtUiTheme::Color(colorToken));
    }
    return image.save(file) ? QDir::fromNativeSeparators(file) : QString();
}
void removeRenderedImages()
{
    const QDir dir(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("styles"));
    for (const QString &file : dir.entryList({"*.png"}, QDir::Files))
        QFile::remove(dir.filePath(file));
}

QString buildSheet()
{
    std::map<QString, QString> tokens;
    for (const auto &[key, value] : active.colors)
    {
        const QColor c = parseColor(value);
        tokens[key] = c.isValid() ? qssColor(c) : value; // not a color: raw QSS, e.g. a gradient
    }
    for (const auto &[key, value] : active.metrics)
        tokens[key] = QString::number(value) + "px";
    tokens["checkImage"] = renderImage("check", "accentText", 16);
    tokens["arrowUpImage"] = renderImage("chevron-up", "icon", 16);
    tokens["arrowDownImage"] = renderImage("chevron-down", "icon", 16);
    tokens["arrowRightImage"] = renderImage("chevron-right", "icon", 16);

    const QString source = QString::fromUtf8(kSheet) + "\n" + active.extraSheet;
    static const QRegularExpression token(R"(@([A-Za-z_][A-Za-z0-9_]*))");
    QString out;
    out.reserve(source.size() + 1024);
    qsizetype at = 0;
    for (auto it = token.globalMatch(source); it.hasNext();)
    {
        const auto m = it.next();
        out += QStringView(source).mid(at, m.capturedStart() - at);
        const auto found = tokens.find(m.captured(1));
        out += found != tokens.end() ? found->second : m.captured(0);
        at = m.capturedEnd();
    }
    out += QStringView(source).mid(at);
    return out;
}

void resolveColors()
{
    resolvedColors.clear();
    for (const auto &[key, value] : kDefaultColors)
        resolvedColors[key] = parseColor(QString::fromLatin1(value));
    for (const auto &[key, value] : active.colors)
        if (const QColor c = parseColor(value); c.isValid())
            resolvedColors[key.toStdString()] = c;
}

void apply()
{
    ++generation;
    svgCache.clear();
    pngCache.clear();
    renderCache.clear();
    iconCache.clear();
    removeRenderedImages();
    resolveColors();

    const auto c = [](const char *token) { return QtUiTheme::Color(token); };
    QPalette p;
    p.setColor(QPalette::Window, c("window"));
    p.setColor(QPalette::WindowText, c("text"));
    p.setColor(QPalette::Base, c("base"));
    p.setColor(QPalette::AlternateBase, c("alternateBase"));
    p.setColor(QPalette::Text, c("text"));
    p.setColor(QPalette::Button, c("button"));
    p.setColor(QPalette::ButtonText, c("text"));
    p.setColor(QPalette::BrightText, c("accentText"));
    p.setColor(QPalette::Highlight, c("accent"));
    p.setColor(QPalette::HighlightedText, c("accentText"));
    p.setColor(QPalette::ToolTipBase, c("tooltip"));
    p.setColor(QPalette::ToolTipText, c("text"));
    p.setColor(QPalette::PlaceholderText, c("textDim"));
    p.setColor(QPalette::Link, c("accent"));
    p.setColor(QPalette::LinkVisited, c("accentHover"));
    p.setColor(QPalette::Light, c("borderStrong"));
    p.setColor(QPalette::Midlight, c("borderStrong"));
    p.setColor(QPalette::Mid, c("border"));
    p.setColor(QPalette::Dark, c("separator"));
    p.setColor(QPalette::Shadow, active.dark ? QColor(0, 0, 0) : c("borderStrong"));
    for (const auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        p.setColor(QPalette::Disabled, role, c("textDisabled"));
    QApplication::setPalette(p);

    // The editor writes in the system UI font by default - the one Windows puts in a title
    // bar. The class overrides are set too because Qt takes a menu's font from the system's
    // menu metrics rather than from the general UI font, and the two need not agree.
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    if (!active.fontFamily.isEmpty())
        font.setFamilies({active.fontFamily});
    if (active.fontPointSize > 0)
        font.setPointSizeF(active.fontPointSize);
    QApplication::setFont(font);
    for (const char *type : {"QMenuBar", "QMenu", "QDockWidget", "QToolTip"})
        QApplication::setFont(font, type);

    qApp->styleHints()->setColorScheme(active.dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    if (hostWindow)
    {
        const BOOL darkTitle = active.dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hostWindow, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkTitle, sizeof(darkTitle));
    }
    qApp->setStyleSheet(buildSheet());
}

QString resolveStyleFile(const QString &file)
{
    if (file.isEmpty())
        return QString();
    if (QDir::isAbsolutePath(file))
        return QDir::cleanPath(file);
    QString name = file;
    if (!name.endsWith(".style", Qt::CaseInsensitive))
        name += ".style";
    return QDir::cleanPath(QDir(stylesDirectory()).filePath(name));
}

Style defaults()
{
    Style s;
    s.name = "Built-in";
    for (const auto &[key, value] : kDefaultColors)
        s.colors[key] = value;
    for (const auto &[key, value] : kDefaultMetrics)
        s.metrics[key] = value;
    return s;
}

bool readStyle(const QString &path, Style &out, QString &error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        error = QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), f.errorString());
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
    {
        error = QStringLiteral("%1: %2 at offset %3")
                    .arg(QFileInfo(path).fileName(), parse.errorString())
                    .arg(parse.offset);
        return false;
    }
    const QJsonObject root = doc.object();
    Style s = defaults();
    s.file = path;
    s.name = root.value("name").toString(QFileInfo(path).completeBaseName());
    s.description = root.value("description").toString();
    s.dark = root.value("dark").toBool(true);
    const QJsonObject font = root.value("font").toObject();
    s.fontFamily = font.value("family").toString();
    s.fontPointSize = font.value("pointSize").toDouble(0);
    const QJsonObject icons = root.value("icons").toObject();
    if (icons.contains("folder"))
        s.iconFolder = icons.value("folder").toString();
    // Icon colors may sit with the icons, where a style author will look for them.
    for (auto it = icons.begin(); it != icons.end(); ++it)
        if (it.key() != "folder" && it.value().isString())
        {
            const QString key = it.key() == "color" ? QStringLiteral("icon")
                                                    : "icon" + it.key().left(1).toUpper() + it.key().mid(1);
            s.colors[key] = it.value().toString();
        }
    const QJsonObject colors = root.value("colors").toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it)
        if (it.value().isString())
            s.colors[it.key()] = it.value().toString();
    const QJsonObject metrics = root.value("metrics").toObject();
    for (auto it = metrics.begin(); it != metrics.end(); ++it)
        if (it.value().isDouble())
            s.metrics[it.key()] = it.value().toDouble();
    const QJsonValue sheet = root.value("stylesheet");
    if (sheet.isString())
        s.extraSheet = sheet.toString();
    else if (sheet.isArray())
        for (const QJsonValue &line : sheet.toArray())
            s.extraSheet += line.toString() + "\n";
    out = std::move(s);
    return true;
}

void watch(const QString &path)
{
    if (!watcher)
        return;
    if (!watcher->files().isEmpty())
        watcher->removePaths(watcher->files());
    if (!path.isEmpty())
        watcher->addPath(path);
}

// Relative to the styles folder when it is inside it, so a moved project keeps its choice.
QString settingValue(const QString &path)
{
    const QString relative = QDir(stylesDirectory()).relativeFilePath(path);
    return relative.startsWith("..") ? path : relative;
}
} // namespace

namespace QtUiTheme
{
void Initialize(HWND host)
{
    hostWindow = host;
    stylesDirText = QDir::toNativeSeparators(stylesDirectory()).toUtf8();
    if (!watcher)
    {
        watcher = new QFileSystemWatcher;
        // Editors save by writing a new file and renaming it over the old one, which drops
        // the watch and can fire before the new file is complete; wait a moment, reload,
        // and watch the path again.
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged, watcher, [](const QString &) {
            if (reloadQueued)
                return;
            reloadQueued = true;
            QTimer::singleShot(250, watcher, [] {
                reloadQueued = false;
                QtUi::ReloadStyle();
            });
        });
    }
    const QString saved = QSettings().value("editor/style", "Ptero Dark.style").toString();
    if (!QtUi::LoadStyle(saved.toUtf8().constData()) && !QtUi::LoadStyle("Ptero Dark.style"))
    {
        // No usable style on disk: run on the built-in one rather than unstyled.
        const QByteArray reason = errorText;
        active = defaults();
        apply();
        errorText = reason;
    }
}
void Shutdown()
{
    // Pixmaps and the watcher must not outlive the QApplication.
    delete watcher;
    watcher = nullptr;
    svgCache.clear();
    pngCache.clear();
    renderCache.clear();
    iconCache.clear();
}
QColor Color(const char *token)
{
    // Labels ask for their color every frame, so this is a plain lookup into colors
    // resolved once per apply(), without building a QString for the key.
    const auto it = resolvedColors.find(std::string_view(token));
    return it != resolvedColors.end() ? it->second : QColor();
}
int Metric(const char *token, int fallback)
{
    const auto it = active.metrics.find(QString::fromLatin1(token));
    return it != active.metrics.end() ? int(std::lround(it->second)) : fallback;
}
QIcon Icon(const QString &name)
{
    if (name.isEmpty())
        return QIcon();
    auto it = iconCache.find(name);
    if (it == iconCache.end())
    {
        const bool exists = pngFor(name) || svgFor(name);
        it = iconCache.emplace(name, exists ? QIcon(new ThemeIconEngine(name)) : QIcon()).first;
    }
    return it->second;
}
int Generation()
{
    return generation;
}
} // namespace QtUiTheme

namespace QtUi
{
const std::vector<StyleEntry> &AvailableStyles(bool rescan)
{
    if (entriesScanned && !rescan)
        return entries;
    entriesScanned = true;
    entries.clear();
    const QDir dir(stylesDirectory());
    for (const QFileInfo &info : dir.entryInfoList({"*.style"}, QDir::Files, QDir::Name | QDir::IgnoreCase))
    {
        StyleEntry entry;
        entry.File = info.fileName().toStdString();
        entry.Name = info.completeBaseName().toStdString();
        QFile f(info.filePath());
        if (f.open(QIODevice::ReadOnly))
        {
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            if (root.contains("name"))
                entry.Name = root.value("name").toString().toStdString();
            entry.Description = root.value("description").toString().toStdString();
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}
bool LoadStyle(const char *file)
{
    const QString path = resolveStyleFile(QString::fromUtf8(file ? file : ""));
    if (path.isEmpty())
    {
        errorText = "No style file given.";
        return false;
    }
    Style loaded;
    QString error;
    if (!readStyle(path, loaded, error))
    {
        errorText = error.toUtf8();
        return false;
    }
    active = std::move(loaded);
    errorText.clear();
    currentFileText = QFileInfo(path).fileName().toUtf8();
    apply();
    watch(path);
    QSettings().setValue("editor/style", settingValue(path));
    return true;
}
bool ReloadStyle()
{
    if (active.file.isEmpty())
        return false;
    // Keep the path: LoadStyle replaces "active" and the string would go with it.
    const QByteArray path = active.file.toUtf8();
    return LoadStyle(path.constData());
}
const char *CurrentStyleFile()
{
    return currentFileText.constData();
}
const char *StyleError()
{
    return errorText.constData();
}
const char *StylesDirectory()
{
    return stylesDirText.constData();
}
bool DuplicateStyle(const char *name)
{
    QString title = QString::fromUtf8(name ? name : "").trimmed();
    // Keep it a plain file name: no folders, nothing Windows refuses.
    static const QRegularExpression unsafe(R"([\\/:*?"<>|])");
    title.remove(unsafe);
    if (title.isEmpty())
    {
        errorText = "Give the new style a name.";
        return false;
    }
    const QString target = QDir(stylesDirectory()).filePath(title + ".style");
    if (QFileInfo::exists(target))
    {
        errorText = QStringLiteral("%1.style already exists.").arg(title).toUtf8();
        return false;
    }
    QByteArray text;
    if (!active.file.isEmpty())
    {
        QFile source(active.file);
        if (source.open(QIODevice::ReadOnly))
            text = source.readAll();
    }
    if (text.isEmpty())
    {
        // Built-in style: write every token out, so the copy is a complete starting point.
        QJsonObject colors, metrics;
        for (const auto &[key, value] : active.colors)
            colors[key] = value;
        for (const auto &[key, value] : active.metrics)
            metrics[key] = value;
        QJsonObject root{{"name", title}, {"dark", active.dark}, {"colors", colors}, {"metrics", metrics}};
        text = QJsonDocument(root).toJson(QJsonDocument::Indented);
    }
    else
    {
        // Rename in the text rather than through QJsonDocument, which would sort the keys
        // and throw away the author's layout.
        static const QRegularExpression field(R"("name"\s*:\s*"(?:[^"\\]|\\.)*")");
        const QString jsonName = QString::fromUtf8(QJsonDocument(QJsonArray{title}).toJson(QJsonDocument::Compact));
        const QString quoted = jsonName.mid(1, jsonName.size() - 2); // strip the array brackets
        QString body = QString::fromUtf8(text);
        const auto m = field.match(body);
        if (m.hasMatch())
            body.replace(m.capturedStart(), m.capturedLength(), "\"name\": " + quoted);
        text = body.toUtf8();
    }
    QDir().mkpath(stylesDirectory());
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly) || out.write(text) != text.size())
    {
        errorText = QStringLiteral("Could not write %1: %2").arg(target, out.errorString()).toUtf8();
        return false;
    }
    out.close();
    AvailableStyles(true);
    return LoadStyle(target.toUtf8().constData());
}
float StyleMetric(const char *token, float fallback)
{
    const auto it = active.metrics.find(QString::fromLatin1(token));
    return it != active.metrics.end() ? float(it->second) : fallback;
}
} // namespace QtUi
