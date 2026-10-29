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
        {key::wordWrap, true},
        {key::multiCursorModifier, QStringLiteral("alt")},
        {key::defaultMode, QStringLiteral("visual")},
        {key::highlightActiveBlock, true},
        {key::breaks, false},
        {key::linkify, true},
        {key::typographer, false},
        {key::frontMatter, QStringLiteral("table")},
        {key::renderMath, true},
        {key::mathMacros, QVariantMap()},
        {key::mermaidTheme, QStringLiteral("auto")},
        {key::pasteImageFolder, QString()},
        {key::updateLinksOnPaste, true},
        {key::validateLinks, false},
        {key::restoreUnsaved, true},
        {key::restoreSession, true},
        {key::autoSave, QStringLiteral("off")},
        {key::newFileLanguage, QStringLiteral("markdown")},
        {key::encoding, QStringLiteral("utf8")},
        {key::lineEnding, QStringLiteral("auto")},
        {key::statusBar, true},
        {key::outline, true},
        {key::zoom, 100},
    };
    return values;
}

QString shortcutKey(const QString &commandId)
{
    return QLatin1String(key::shortcutsGroup) + commandId;
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
}

void Settings::useFile(const QString &path)
{
    delete m_store;
    m_store = new QSettings(path, QSettings::IniFormat, this);
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

QStringList Settings::recentFiles() const
{
    return m_store->value(QStringLiteral("state/recentFiles")).toStringList();
}

void Settings::addRecentFile(const QString &path)
{
    QStringList files = recentFiles();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 15)
        files.removeLast();
    m_store->setValue(QStringLiteral("state/recentFiles"), files);
}

void Settings::removeRecentFile(const QString &path)
{
    QStringList files = recentFiles();
    if (files.removeAll(path) > 0)
        m_store->setValue(QStringLiteral("state/recentFiles"), files);
}

void Settings::clearRecentFiles()
{
    m_store->remove(QStringLiteral("state/recentFiles"));
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
