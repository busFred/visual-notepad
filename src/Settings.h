// Application settings, stored with QSettings (INI format) and shared by all
// windows. Every change is announced through changed(key).
#pragma once

#include <QColor>
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
inline constexpr char detectIndentation[] = "editor/detectIndentation"; // per file, from its content
inline constexpr char wordWrap[] = "editor/wordWrap";
inline constexpr char lineSpacing[] = "editor/lineSpacing"; // multiple of the font height
inline constexpr char autoClose[] = "editor/autoClose";     // brackets and quotes
inline constexpr char autoSurround[] = "editor/autoSurround"; // wrap selections when typing a bracket or quote
inline constexpr char multiCursorModifier[] = "editor/multiCursorModifier"; // "alt" | "ctrl"
// Markdown
inline constexpr char defaultMode[] = "markdown/defaultMode"; // "source" | "split" | "visual"
inline constexpr char highlightActiveBlock[] = "markdown/highlightActiveBlock";
inline constexpr char breaks[] = "markdown/breaks";
inline constexpr char linkify[] = "markdown/linkify";
inline constexpr char typographer[] = "markdown/typographer";
inline constexpr char frontMatter[] = "markdown/frontMatter"; // "hide" | "code" | "table"
inline constexpr char textFontFamily[] = "markdown/textFontFamily"; // "" = system UI font
inline constexpr char textFontSize[] = "markdown/textFontSize";     // points; 0 = UI size x 1.15
inline constexpr char textLineHeight[] = "markdown/textLineHeight"; // multiple of the font height
inline constexpr char renderMath[] = "markdown/renderMath";
inline constexpr char mathMacros[] = "markdown/mathMacros"; // QVariantMap command -> expansion
inline constexpr char mermaidTheme[] = "markdown/mermaidTheme"; // "auto" | "base" | "default" | ...
inline constexpr char remoteImages[] = "markdown/remoteImages"; // "ask" | "never"
inline constexpr char pasteImageFolder[] = "markdown/pasteImageFolder";
inline constexpr char updateLinksOnPaste[] = "markdown/updateLinksOnPaste";
inline constexpr char validateLinks[] = "markdown/validateLinks";
// Files
inline constexpr char restoreUnsaved[] = "files/restoreUnsaved";
inline constexpr char restoreSession[] = "files/restoreSession";
inline constexpr char rememberRecent[] = "files/rememberRecent";
inline constexpr char autoSave[] = "files/autoSave"; // "off" | "afterDelay" | "onFocusChange"
inline constexpr char newFileLanguage[] = "files/newFileLanguage"; // "markdown" | "plaintext"
inline constexpr char encoding[] = "files/encoding"; // "utf8" | "utf8bom" | "utf16le"
inline constexpr char guessEncoding[] = "files/guessEncoding";
inline constexpr char legacyEncoding[] = "files/legacyEncoding"; // codec name; "" = automatic (system)
inline constexpr char lineEnding[] = "files/lineEnding"; // "auto" | "lf" | "crlf"
// View
inline constexpr char statusBar[] = "view/statusBar";
inline constexpr char outline[] = "view/outline";
inline constexpr char zoom[] = "view/zoom"; // percent
inline constexpr char theme[] = "view/theme"; // "modern" | "standard" | "classic"
// Not a preference: announced through changed() when the list changes.
inline constexpr char recentFiles[] = "state/recentFiles";
// Shortcut overrides live under "shortcuts/<command id>".
inline constexpr char shortcutsGroup[] = "shortcuts/";
// Colour overrides live under "colors/<theme id>/<role>" as "#RRGGBB".
inline constexpr char colorsGroup[] = "colors/";
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

    // Colour overrides for one theme, by role name (see theme::roleName).
    // colorOverride returns an invalid colour when the role is not overridden.
    QColor colorOverride(const QString &themeId, const QString &role) const;
    void setColorOverride(const QString &themeId, const QString &role, const QColor &color);
    void resetColorOverride(const QString &themeId, const QString &role);
    QStringList overriddenColors(const QString &themeId) const;

    // Most recently opened files, newest first. Nothing is recorded while
    // rememberRecent is off, and turning it off clears the list.
    QStringList recentFiles() const;
    void addRecentFile(const QString &path);
    void removeRecentFile(const QString &path);
    void clearRecentFiles();

    // While alive, changed() is held back and then emitted once per distinct
    // key, after all writes: related keys (font family and size) are all in
    // place when listeners react. Nests.
    class ChangeBatch
    {
    public:
        ChangeBatch() { ++Settings::instance().m_batchDepth; }
        ~ChangeBatch() { Settings::instance().endBatch(); }
        Q_DISABLE_COPY(ChangeBatch)
    };

    // For tests: use a separate file.
    void useFile(const QString &path);

Q_SIGNALS:
    // key is a settings key, or "shortcuts/<id>" for shortcut changes
    // and "colors/<theme>/<role>" for colour overrides.
    void changed(const QString &key);

private:
    Settings();
    void announce(const QString &changedKey);
    void endBatch();
    int m_batchDepth = 0;
    QStringList m_batched;
    // Renames values written by older versions.
    void migrate();
    QSettings *m_store;
};

} // namespace vn
