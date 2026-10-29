// Application settings, stored with QSettings (INI format) and shared by all
// windows. Every change is announced through changed(key).
#pragma once

#include <QKeySequence>
#include <QObject>
#include <QVariant>

class QSettings;

namespace vn {

namespace key {
// Editor
inline constexpr char fontFamily[] = "editor/fontFamily"; // "" = built-in monospace
inline constexpr char fontSize[] = "editor/fontSize";     // points; 0 = system default
inline constexpr char lineNumbers[] = "editor/lineNumbers";
inline constexpr char highlightCurrentLine[] = "editor/highlightCurrentLine";
inline constexpr char drawWhitespace[] = "editor/drawWhitespace";
inline constexpr char tabWidth[] = "editor/tabWidth";
inline constexpr char insertSpaces[] = "editor/insertSpaces";
inline constexpr char wordWrap[] = "editor/wordWrap";
inline constexpr char multiCursorModifier[] = "editor/multiCursorModifier"; // "alt" | "ctrl"
// Markdown
inline constexpr char defaultMode[] = "markdown/defaultMode"; // "source" | "split" | "visual"
inline constexpr char highlightActiveBlock[] = "markdown/highlightActiveBlock";
inline constexpr char breaks[] = "markdown/breaks";
inline constexpr char linkify[] = "markdown/linkify";
inline constexpr char typographer[] = "markdown/typographer";
inline constexpr char frontMatter[] = "markdown/frontMatter"; // "hide" | "code" | "table"
inline constexpr char renderMath[] = "markdown/renderMath";
inline constexpr char mathMacros[] = "markdown/mathMacros"; // QVariantMap command -> expansion
inline constexpr char mermaidTheme[] = "markdown/mermaidTheme"; // "auto" | "base" | "default" | ...
inline constexpr char pasteImageFolder[] = "markdown/pasteImageFolder";
inline constexpr char updateLinksOnPaste[] = "markdown/updateLinksOnPaste";
inline constexpr char validateLinks[] = "markdown/validateLinks";
// Files
inline constexpr char restoreUnsaved[] = "files/restoreUnsaved";
inline constexpr char restoreSession[] = "files/restoreSession";
inline constexpr char autoSave[] = "files/autoSave"; // "off" | "afterDelay" | "onFocusChange"
inline constexpr char newFileLanguage[] = "files/newFileLanguage"; // "markdown" | "plaintext"
inline constexpr char encoding[] = "files/encoding"; // "utf8" | "utf8bom" | "utf16le"
inline constexpr char lineEnding[] = "files/lineEnding"; // "auto" | "lf" | "crlf"
// View
inline constexpr char statusBar[] = "view/statusBar";
inline constexpr char outline[] = "view/outline";
inline constexpr char zoom[] = "view/zoom"; // percent
// Shortcut overrides live under "shortcuts/<command id>".
inline constexpr char shortcutsGroup[] = "shortcuts/";
} // namespace key

class Settings : public QObject
{
    Q_OBJECT

public:
    static Settings &instance();

    QVariant value(const char *key) const;
    void setValue(const char *key, const QVariant &value);
    // Restores the default (removes the stored value).
    void reset(const char *key);
    QVariant defaultValue(const char *key) const;

    bool flag(const char *key) const { return value(key).toBool(); }
    int number(const char *key) const { return value(key).toInt(); }
    QString text(const char *key) const { return value(key).toString(); }

    // Shortcuts: an empty list means the command's default.
    QList<QKeySequence> shortcutOverride(const QString &commandId) const;
    bool hasShortcutOverride(const QString &commandId) const;
    void setShortcutOverride(const QString &commandId, const QList<QKeySequence> &keys);
    void resetShortcut(const QString &commandId);
    void resetAllShortcuts();

    // Most recently opened files, newest first.
    QStringList recentFiles() const;
    void addRecentFile(const QString &path);
    void removeRecentFile(const QString &path);
    void clearRecentFiles();

    // For tests: use a separate file.
    void useFile(const QString &path);

Q_SIGNALS:
    // key is a settings key, or "shortcuts/<id>" for shortcut changes.
    void changed(const QString &key);

private:
    Settings();
    QSettings *m_store;
};

} // namespace vn
