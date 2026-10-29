#include "Settings.h"

#include <QHash>
#include <QSettings>

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
        {key::lineSpacing, 1.4},
        {key::multiCursorModifier, QStringLiteral("alt")},
        {key::defaultMode, QStringLiteral("split")},
        {key::highlightActiveBlock, true},
        {key::breaks, false},
        {key::linkify, true},
        {key::typographer, false},
        {key::frontMatter, QStringLiteral("table")},
        {key::textFontFamily, QString()},
        {key::textFontSize, 0},
        {key::textLineHeight, 1.4},
        {key::renderMath, true},
        {key::mathMacros, QVariantMap()},
        {key::mermaidTheme, QStringLiteral("auto")},
        {key::pasteImageFolder, QString()},
        {key::updateLinksOnPaste, true},
        {key::validateLinks, false},
        {key::restoreUnsaved, true},
        {key::restoreSession, false},
        {key::rememberRecent, true},
        {key::autoSave, QStringLiteral("off")},
        {key::newFileLanguage, QStringLiteral("markdown")},
        {key::encoding, QStringLiteral("utf8")},
        {key::guessEncoding, true},
        {key::legacyEncoding, QString()},
        {key::lineEnding, QStringLiteral("auto")},
        {key::statusBar, true},
        {key::outline, true},
        {key::zoom, 100},
        {key::theme, QStringLiteral("modern")},
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
    migrate();
}

void Settings::useFile(const QString &path)
{
    delete m_store;
    m_store = new QSettings(path, QSettings::IniFormat, this);
    migrate();
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

QVariant Settings::defaultValue(const char *key) const
{
    return defaults().value(QLatin1String(key));
}

QVariant Settings::value(const char *key) const
{
    const QVariant fallback = defaultValue(key);
    QVariant stored = m_store->value(QLatin1String(key), fallback);
    // INI files store everything as strings; convert back to the default's type.
    if (fallback.isValid() && stored.metaType() != fallback.metaType() && stored.canConvert(fallback.metaType()))
        stored.convert(fallback.metaType());
    return stored;
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
    Q_EMIT changed(QLatin1String(key));
}

void Settings::reset(const char *key)
{
    if (!m_store->contains(QLatin1String(key)))
        return;
    m_store->remove(QLatin1String(key));
    Q_EMIT changed(QLatin1String(key));
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
    Q_EMIT changed(shortcutKey(commandId));
}

void Settings::resetShortcut(const QString &commandId)
{
    if (!m_store->contains(shortcutKey(commandId)))
        return;
    m_store->remove(shortcutKey(commandId));
    Q_EMIT changed(shortcutKey(commandId));
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
    Q_EMIT changed(colorKey(themeId, role));
}

void Settings::resetColorOverride(const QString &themeId, const QString &role)
{
    if (!m_store->contains(colorKey(themeId, role)))
        return;
    m_store->remove(colorKey(themeId, role));
    Q_EMIT changed(colorKey(themeId, role));
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
    Q_EMIT changed(QLatin1String(key::recentFiles));
}

void Settings::removeRecentFile(const QString &path)
{
    QStringList files = recentFiles();
    if (files.removeAll(path) > 0) {
        m_store->setValue(QLatin1String(key::recentFiles), files);
        Q_EMIT changed(QLatin1String(key::recentFiles));
    }
}

void Settings::clearRecentFiles()
{
    m_store->remove(QLatin1String(key::recentFiles));
    Q_EMIT changed(QLatin1String(key::recentFiles));
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
