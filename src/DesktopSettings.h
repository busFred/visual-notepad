// Settings of GTK-based desktops (GNOME, Cinnamon, MATE, Budgie…) that our
// bundled Qt can't read itself: it has no GTK platform theme, and its generic
// GNOME theme hard-codes the font "Sans Serif 9" and leaves the cursor size
// to libxcb-cursor's guess.
#pragma once

#include <QFont>
#include <QString>
#include <QStringList>

#include <optional>

namespace vn::desktop {

// A font from a Pango description: "[FAMILY-LIST] [STYLE-OPTIONS] [SIZE]",
// e.g. "Segoe UI 10", "Noto Sans, Sans Bold Italic 11" or "Ubuntu 13px"
// (https://docs.gtk.org/Pango/type_func.FontDescription.from_string.html).
struct FontDescription
{
    QStringList families;
    qreal size = 0; // 0 if absent
    bool pixels = false;
    QFont::Weight weight = QFont::Normal;
    bool italic = false;
};
FontDescription parseFontDescription(const QString &text);

// The value of `key` in the desktop's interface schema (org.cinnamon.desktop.
// interface, org.mate.interface or org.gnome.desktop.interface), unquoted;
// empty off Linux, on KDE or LXQt (where Qt reads the settings itself), or if
// gsettings is unavailable. Usable before QApplication exists.
QString interfaceSetting(const char *key);

// The desktop's cursor size in pixels, or 0.
int cursorSize();

// The desktop's UI font, if it can be read and its family is installed.
// Needs a QGuiApplication.
std::optional<QFont> uiFont();

} // namespace vn::desktop
