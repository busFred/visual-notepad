// Application settings, stored with QSettings (INI format) and shared by all
// windows. Every change is announced through changed(key).
#pragma once

#include <QColor>
#include <QKeySequence>
#include <QObject>
#include <QVariant>

class QSettings;

namespace vn {

// Valid range of key::pictureCacheMiB (values outside it are clamped on read)
// and its default.
inline constexpr int pictureCacheMinMiB = 16;
inline constexpr int pictureCacheMaxMiB = 2048;
inline constexpr int pictureCacheDefaultMiB = 128;

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
// Line height as VS Code counts it: a multiple of the font size in pixels (the
// line is round(size x factor) high), not of the font's own line height, which
// is 1.2x the size for some fonts and 1.33x for others. Replaces
// "editor/lineSpacing" (below), which Settings converts on first use.
inline constexpr char lineSpacing[] = "editor/lineHeightFactor";
inline constexpr char legacyLineSpacing[] = "editor/lineSpacing"; // old: multiple of the font's line height
inline constexpr char autoClose[] = "editor/autoClose";     // brackets and quotes
inline constexpr char autoSurround[] = "editor/autoSurround"; // wrap selections when typing a bracket or quote
inline constexpr char multiCursorModifier[] = "editor/multiCursorModifier"; // "alt" | "ctrl"
// Markdown
inline constexpr char defaultMode[] = "markdown/defaultMode"; // "source" | "split" | "visual"
inline constexpr char highlightActiveBlock[] = "markdown/highlightActiveBlock";
inline constexpr char syncAnchor[] = "markdown/syncAnchor"; // where Source and Visual line up: "top" | "middle"
inline constexpr char blocksStartCollapsed[] = "markdown/blocksStartCollapsed"; // tables, figures, equations, code
// Read mode (Visual mode, locked): where a tab starts; its bar changes the tab only.
inline constexpr char readColumns[] = "markdown/readColumns";       // 0 = as many as fit (once, when a tab opens), else a count
inline constexpr char readChars[] = "markdown/readChars";           // characters per column, 50 to 100
inline constexpr char readKeptLines[] = "markdown/readKeptLines";   // lines kept on a page turn, 0 to 5
inline constexpr char breaks[] = "markdown/breaks";
inline constexpr char linkify[] = "markdown/linkify";
inline constexpr char typographer[] = "markdown/typographer";
inline constexpr char frontMatter[] = "markdown/frontMatter"; // "hide" | "code" | "table"
inline constexpr char textFontFamily[] = "markdown/textFontFamily"; // "" = system UI font
inline constexpr char textFontSize[] = "markdown/textFontSize";     // points; 0 = UI size x 1.15
// As lineSpacing, for the Visual view's text: a multiple of the text font size.
inline constexpr char textLineHeight[] = "markdown/textLineHeightFactor";
inline constexpr char legacyTextLineHeight[] = "markdown/textLineHeight"; // old: multiple of the font's line height
inline constexpr char renderMath[] = "markdown/renderMath";
inline constexpr char mathMacros[] = "markdown/mathMacros"; // QVariantMap command -> expansion
inline constexpr char mermaidTheme[] = "markdown/mermaidTheme"; // "auto" | "base" | "default" | ...
inline constexpr char remoteImages[] = "markdown/remoteImages"; // "ask" | "never"
inline constexpr char pasteImageFolder[] = "markdown/pasteImageFolder";
inline constexpr char updateLinksOnPaste[] = "markdown/updateLinksOnPaste";
inline constexpr char validateLinks[] = "markdown/validateLinks";
// Performance
inline constexpr char pictureCacheMiB[] = "performance/pictureCacheMiB"; // see pictureCacheMinMiB/MaxMiB
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
inline constexpr char lastTabClosed[] = "view/lastTabClosed"; // "keep" | "close"
inline constexpr char externalFilesWindow[] = "view/externalFilesWindow"; // "active" | "new"
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
    // The file the settings are kept in, and writing it now (QSettings
    // otherwise writes when it sees fit).
    QString fileName() const;
    void sync();

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
    void makePrivate();
    // Turns the old line spacings (multiples of the font's line height) into
    // the new ones (multiples of the font size) the first time either is read.
    // It needs the fonts, so it cannot run when the settings are made.
    void migrateLineSpacing() const;
    mutable bool m_spacingMigrated = false;
    void migrate();
    QSettings *m_store;
};

} // namespace vn
