#include "Settings.h"

#include "Theme.h"

#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QSettings>

#include <algorithm>
#include <utility>

namespace vn {

namespace {

const QHash<QString, QVariant> &defaults()
{
    static const QHash<QString, QVariant> values = {
        {key::fontFamily, QString()},
        {key::fontSize, 0},
        {key::lineNumbers, true},
        {key::highlightCurrentLine, true},
        {key::drawWhitespace, false},
        {key::tabWidth, 4},
        {key::insertSpaces, true},
        {key::detectIndentation, true},
        {key::wordWrap, true},
        {key::lineSpacing, 1.35},
        {key::autoClose, true},
        {key::autoSurround, true},
        {key::multiCursorModifier, QStringLiteral("alt")},
        {key::defaultMode, QStringLiteral("split")},
        {key::highlightActiveBlock, true},
        {key::syncAnchor, QStringLiteral("top")},
        {key::blocksStartCollapsed, false},
        {key::readColumns, 0},
        {key::readChars, 75},
        {key::readKeptLines, 2},
        {key::breaks, false},
        {key::linkify, true},
        {key::typographer, false},
        {key::frontMatter, QStringLiteral("table")},
        {key::textFontFamily, QString()},
        {key::textFontSize, 0},
        {key::textLineHeight, 1.6},
        {key::renderMath, true},
        {key::mathMacros, QVariantMap()},
        {key::mermaidTheme, QStringLiteral("auto")},
        {key::remoteImages, QStringLiteral("ask")},
        {key::pasteImageFolder, QString()},
        {key::updateLinksOnPaste, true},
        {key::validateLinks, false},
        {key::pictureCacheMiB, pictureCacheDefaultMiB},
        {key::restoreUnsaved, true},
        {key::restoreSession, false},
        {key::rememberRecent, true},
        {key::autoSave, QStringLiteral("off")},
        {key::newFileLanguage, QStringLiteral("plaintext")},
        {key::encoding, QStringLiteral("utf8")},
        {key::guessEncoding, true},
        {key::legacyEncoding, QString()},
        {key::lineEnding, QStringLiteral("auto")},
        {key::statusBar, true},
        {key::outline, true},
        {key::zoom, 100},
        {key::theme, QStringLiteral("modern")},
        {key::lastTabClosed, QStringLiteral("keep")},
        {key::externalFilesWindow, QStringLiteral("active")},
    };
    return values;
}

QString shortcutKey(const QString &commandId)
{
    return QLatin1String(key::shortcutsGroup) + commandId;
}

QString colorKey(const QString &themeId, const QString &role)
{
    return QLatin1String(key::colorsGroup) + themeId + u'/' + role;
}

} // namespace

Settings &Settings::instance()
{
    static Settings settings;
    return settings;
}

Settings::Settings()
    : m_store(new QSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("visnotepad"),
                            QStringLiteral("settings"), this))
{
    makePrivate();
    migrate();
}

void Settings::useFile(const QString &path)
{
    delete m_store;
    m_store = new QSettings(path, QSettings::IniFormat, this);
    m_spacingMigrated = false;
    makePrivate();
    migrate();
}

QString Settings::fileName() const
{
    return m_store->fileName();
}

void Settings::sync()
{
    m_store->sync();
}

// The settings hold the recent files and folders, as the session does:
// readable by the user alone, like the session files (0600). The file is
// made now if missing; QSettings writes it again through a temporary file
// that takes over the permissions of the file it replaces.
void Settings::makePrivate()
{
    const QString path = m_store->fileName();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.exists() && !file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void Settings::migrate()
{
    // "Legacy Windows" became "Windows Standard", custom colours included.
    if (m_store->value(QLatin1String(key::theme)).toString() == QLatin1String("legacy"))
        m_store->setValue(QLatin1String(key::theme), QStringLiteral("standard"));
    m_store->beginGroup(QStringLiteral("colors/legacy"));
    const QStringList roles = m_store->childKeys();
    m_store->endGroup();
    for (const QString &role : roles) {
        const QString from = QStringLiteral("colors/legacy/") + role;
        const QString to = QStringLiteral("colors/standard/") + role;
        if (!m_store->contains(to))
            m_store->setValue(to, m_store->value(from));
        m_store->remove(from);
    }
}

// What the preference lists offer (see PreferencesDialog): an old value is
// moved to the nearest of these.
static constexpr double lineHeightChoices[] = {1.0, 1.2, 1.35, 1.5, 1.6, 1.8, 2.0};

void Settings::migrateLineSpacing() const
{
    if (m_spacingMigrated || !qGuiApp)
        return;
    // Set first: the fonts below read settings too.
    m_spacingMigrated = true;
    const auto convert = [this](const char *oldKey, const char *newKey, const QFont &font) {
        if (!m_store->contains(QLatin1String(oldKey)))
            return;
        const QFontMetricsF metrics(font);
        const int pixels = QFontInfo(font).pixelSize();
        // The old number counted the font's line height; the new one counts
        // its size. An old value stays the same line height on screen.
        const double wanted = m_store->value(QLatin1String(oldKey)).toDouble() * (pixels > 0 ? metrics.height() / pixels : 1.2);
        double best = lineHeightChoices[0];
        for (const double choice : lineHeightChoices) {
            if (qAbs(choice - wanted) < qAbs(best - wanted))
                best = choice;
        }
        // (A value the user already chose with this version is kept.)
        if (!m_store->contains(QLatin1String(newKey)) && best != defaults().value(QLatin1String(newKey)).toDouble())
            m_store->setValue(QLatin1String(newKey), best);
        m_store->remove(QLatin1String(oldKey));
    };
    convert(key::legacyLineSpacing, key::lineSpacing, theme::editorFont());
    convert(key::legacyTextLineHeight, key::textLineHeight, theme::textFont());
}

QVariant Settings::defaultValue(const char *key) const
{
    return defaults().value(QLatin1String(key));
}

QVariant Settings::value(const char *key) const
{
    if (!m_spacingMigrated && (qstrcmp(key, key::lineSpacing) == 0 || qstrcmp(key, key::textLineHeight) == 0))
        migrateLineSpacing();
    const QVariant fallback = defaultValue(key);
    QVariant stored = m_store->value(QLatin1String(key), fallback);
    // INI files store everything as strings; convert back to the default's type.
    if (fallback.isValid() && stored.metaType() != fallback.metaType() && stored.canConvert(fallback.metaType()))
        stored.convert(fallback.metaType());
    if (qstrcmp(key, key::pictureCacheMiB) == 0)
        return std::clamp(stored.toInt(), pictureCacheMinMiB, pictureCacheMaxMiB);
    return stored;
}

void Settings::announce(const QString &changedKey)
{
    if (m_batchDepth == 0) {
        Q_EMIT changed(changedKey);
        return;
    }
    if (!m_batched.contains(changedKey))
        m_batched.append(changedKey);
}

void Settings::endBatch()
{
    if (--m_batchDepth > 0)
        return;
    const QStringList keys = std::exchange(m_batched, {});
    for (const QString &k : keys)
        Q_EMIT changed(k);
}

void Settings::setValue(const char *key, const QVariant &value)
{
    if (this->value(key) == value)
        return;
    if (value == defaultValue(key))
        m_store->remove(QLatin1String(key));
    else
        m_store->setValue(QLatin1String(key), value);
    if (qstrcmp(key, key::rememberRecent) == 0 && !value.toBool())
        clearRecentFiles();
    announce(QLatin1String(key));
}

void Settings::reset(const char *key)
{
    if (!m_store->contains(QLatin1String(key)))
        return;
    m_store->remove(QLatin1String(key));
    announce(QLatin1String(key));
}

QList<QKeySequence> Settings::shortcutOverride(const QString &commandId) const
{
    QList<QKeySequence> keys;
    const QStringList stored = m_store->value(shortcutKey(commandId)).toStringList();
    for (const QString &text : stored) {
        // An explicit "none" entry means the user removed every shortcut.
        if (!text.isEmpty() && text != QLatin1String("none"))
            keys.append(QKeySequence(text, QKeySequence::PortableText));
    }
    return keys;
}

bool Settings::hasShortcutOverride(const QString &commandId) const
{
    return m_store->contains(shortcutKey(commandId));
}

void Settings::setShortcutOverride(const QString &commandId, const QList<QKeySequence> &keys)
{
    QStringList stored;
    for (const QKeySequence &sequence : keys)
        stored.append(sequence.toString(QKeySequence::PortableText));
    if (stored.isEmpty())
        stored.append(QStringLiteral("none"));
    m_store->setValue(shortcutKey(commandId), stored);
    announce(shortcutKey(commandId));
}

void Settings::resetShortcut(const QString &commandId)
{
    if (!m_store->contains(shortcutKey(commandId)))
        return;
    m_store->remove(shortcutKey(commandId));
    announce(shortcutKey(commandId));
}

QColor Settings::colorOverride(const QString &themeId, const QString &role) const
{
    const QColor color(m_store->value(colorKey(themeId, role)).toString());
    return color.isValid() ? color : QColor();
}

void Settings::setColorOverride(const QString &themeId, const QString &role, const QColor &color)
{
    const QString name = color.name(QColor::HexRgb);
    if (m_store->value(colorKey(themeId, role)).toString() == name)
        return;
    m_store->setValue(colorKey(themeId, role), name);
    announce(colorKey(themeId, role));
}

void Settings::resetColorOverride(const QString &themeId, const QString &role)
{
    if (!m_store->contains(colorKey(themeId, role)))
        return;
    m_store->remove(colorKey(themeId, role));
    announce(colorKey(themeId, role));
}

QStringList Settings::overriddenColors(const QString &themeId) const
{
    m_store->beginGroup(QLatin1String(key::colorsGroup) + themeId);
    const QStringList roles = m_store->childKeys();
    m_store->endGroup();
    return roles;
}

QStringList Settings::recentFiles() const
{
    return m_store->value(QLatin1String(key::recentFiles)).toStringList();
}

void Settings::addRecentFile(const QString &path)
{
    if (!flag(key::rememberRecent))
        return;
    QStringList files = recentFiles();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 15)
        files.removeLast();
    m_store->setValue(QLatin1String(key::recentFiles), files);
    announce(QLatin1String(key::recentFiles));
}

void Settings::removeRecentFile(const QString &path)
{
    QStringList files = recentFiles();
    if (files.removeAll(path) > 0) {
        m_store->setValue(QLatin1String(key::recentFiles), files);
        announce(QLatin1String(key::recentFiles));
    }
}

void Settings::clearRecentFiles()
{
    m_store->remove(QLatin1String(key::recentFiles));
    announce(QLatin1String(key::recentFiles));
}

void Settings::resetAllShortcuts()
{
    m_store->beginGroup(QStringLiteral("shortcuts"));
    const QStringList ids = m_store->childKeys();
    m_store->endGroup();
    for (const QString &id : ids)
        resetShortcut(id);
}

} // namespace vn
