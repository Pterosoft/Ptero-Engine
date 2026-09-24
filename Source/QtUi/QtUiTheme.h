#pragma once
// Editor styles, shared by QtUi.cpp and QtUiTheme.cpp. Qt-dependent, so it stays out of
// QtUi.h, which engine sources include without Qt on their include path.
//
// A style is a JSON document with the .style extension in Data/Styles. It names colors
// and metrics; the stylesheet itself is a template in QtUiTheme.cpp that refers to them
// as @tokens, so a style can restyle the whole editor without writing any QSS. A style
// may still append its own QSS, and that may use the same tokens.
#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtCore/QString>
#include <windows.h>

namespace QtUiTheme
{
// Loads the style the user picked last time, or the default one, and applies it.
void Initialize(HWND host);
void Shutdown();
// A color token of the active style, e.g. "text" or "accent". Invalid if unknown.
QColor Color(const char *token);
// A numeric metric of the active style, e.g. "iconSize".
int Metric(const char *token, int fallback);
// An icon from the active style's icon folder, tinted by the style. Recolors itself when
// the style changes, so widgets can keep the QIcon they were given. Null if the file is
// missing, so callers can fall back to text.
QIcon Icon(const QString &name);
// Bumped every time a style is applied.
int Generation();
} // namespace QtUiTheme
