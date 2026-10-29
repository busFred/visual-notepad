// Colour themes: two built-in sets of colours plus per-theme user overrides.
#pragma once

#include <QColor>
#include <QFont>
#include <QObject>

class QApplication;

namespace KSyntaxHighlighting {
class Repository;
class Theme;
} // namespace KSyntaxHighlighting

namespace vn::theme {

enum class Role {
    // Chrome
    menuBar,
    menu, // Popup background
    menuHover,
    menuHoverText,
    tabStrip,
    statusBar,
    activeTab,
    inactiveTab,
    inactiveTabText,
    sideBar,
    sideBarHover,
    panelTitle,
    divider,
    surface, // Preferences page and tooltips
    button, // Push buttons
    buttonHover,
    // Content
    editor,
    text,
    selection,
    selectionText,
    accent,
    accentBorder,
    switchOn,
    activeBlock,
    currentLine,
    codeBlock,
    gutterText,
    gripDots,
    mutedText,
    Count
};

struct ThemeInfo
{
    QString id;    // Stored in view/theme
    QString label; // Shown to the user
};

// Built-in themes; the first is the default.
QList<ThemeInfo> themes();
// The theme selected by view/theme; unknown ids fall back to the default.
QString currentThemeId();
QString themeLabel(const QString &themeId);

// Name used in settings keys ("colors/<theme>/<role>") and the style sheet.
QString roleName(Role role);
// Colour of the current theme, including the user's overrides.
QColor color(Role role);
// Built-in colour of a theme, ignoring overrides.
QColor defaultColor(const QString &themeId, Role role);

// Announces that colours changed. apply() emits it after the palette and
// style sheet are replaced; widgets that cache colours or rendered output
// connect to it.
class Notifier : public QObject
{
    Q_OBJECT

Q_SIGNALS:
    void changed();
};
Notifier &notifier();

// While alive, setting changes do not re-apply the theme; it is applied once
// when the last blocker is destroyed. For writing many overrides at once.
class UpdateBlocker
{
public:
    UpdateBlocker();
    ~UpdateBlocker();
    UpdateBlocker(const UpdateBlocker &) = delete;
    UpdateBlocker &operator=(const UpdateBlocker &) = delete;
};

// Sets the Fusion style, palette and style sheet on the application, and keeps
// them current when view/theme or a colour override changes.
void apply(QApplication &app);

// Monospace font for editors.
QFont editorFont();
// Proportional font for rendered Markdown text, before editor zoom.
QFont textFont();

// Shared syntax definitions, including the app's own highlighting theme.
KSyntaxHighlighting::Repository &syntaxRepository();
KSyntaxHighlighting::Theme syntaxTheme();

} // namespace vn::theme
