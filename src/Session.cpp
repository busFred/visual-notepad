#include "Session.h"

#include "DocumentTab.h"
#include "MainWindow.h"
#include "Settings.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QSessionManager>
#include <QSet>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace vn {

namespace {

constexpr int backupIntervalMs = 60 * 1000;
constexpr char backupProperty[] = "vnBackup";

QString &dataDirStorage()
{
    static QString dir;
    return dir;
}

QString sessionFile()
{
    return SessionManager::dataDir() + QStringLiteral("/session.json");
}

QString backupsDir()
{
    return SessionManager::dataDir() + QStringLiteral("/backups");
}

using ViewMode = DocumentTab::ViewMode;

QString modeToString(ViewMode mode)
{
    return mode == ViewMode::Source ? QStringLiteral("source")
           : mode == ViewMode::SourceVisual ? QStringLiteral("split")
                                            : QStringLiteral("visual");
}

ViewMode modeFromString(const QString &text)
{
    return text == QLatin1String("source")  ? ViewMode::Source
           : text == QLatin1String("split") ? ViewMode::SourceVisual
                                            : ViewMode::Visual;
}

QJsonArray readWindows()
{
    QFile file(sessionFile());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("windows")).toArray();
}

bool hotExit()
{
    return Settings::instance().flag(key::restoreUnsaved);
}

} // namespace

SessionManager::SessionManager() = default;

SessionManager &SessionManager::instance()
{
    static SessionManager manager;
    return manager;
}

void SessionManager::setDataDir(const QString &path)
{
    dataDirStorage() = path;
}

QString SessionManager::dataDir()
{
    const QString &dir = dataDirStorage();
    return dir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : dir;
}

bool SessionManager::enabled() const
{
    return hotExit() || Settings::instance().flag(key::restoreSession);
}

void SessionManager::registerWindow(MainWindow *window)
{
    if (!m_windows.contains(window))
        m_windows.append(window);
}

void SessionManager::unregisterWindow(MainWindow *window)
{
    m_windows.removeAll(window);
}

bool SessionManager::isLastWindow(const MainWindow *window) const
{
    return m_windows.size() == 1 && m_windows.constFirst() == window;
}

void SessionManager::start()
{
    if (m_started)
        return;
    m_started = true;
    m_timer = new QTimer(this);
    m_timer->setInterval(backupIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &SessionManager::backupIfDirty);
    m_timer->start();
#ifndef QT_NO_SESSIONMANAGER
    // Direct: the session manager is only valid during the emission.
    connect(qApp, &QGuiApplication::commitDataRequest, this, &SessionManager::onCommitData, Qt::DirectConnection);
#endif
}

void SessionManager::backupIfDirty()
{
    if (!m_dirty || m_quitting || m_windows.isEmpty() || !enabled())
        return;
    saveSession();
}

void SessionManager::onCommitData(QSessionManager &)
{
    // Never cancel the shutdown. With hot exit the work is safe in backups, so
    // closing the windows must not ask about it.
    if (!enabled() || m_windows.isEmpty())
        return;
    const bool safe = saveSession().isEmpty();
    if (safe && hotExit())
        m_quitting = true;
}

QString SessionManager::writeBackup(DocumentTab *tab)
{
    QString name = tab->property(backupProperty).toString();
    if (name.isEmpty())
        name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(backupsDir()))
        return {};
    QSaveFile file(backupsDir() + u'/' + name);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    const QByteArray bytes = tab->text().toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return {};
    tab->setProperty(backupProperty, name);
    return name;
}

QList<DocumentTab *> SessionManager::saveSession(const QList<DocumentTab *> &skipBackup)
{
    if (!enabled())
        return {};

    QList<DocumentTab *> failed;
    QJsonArray windows;
    for (MainWindow *window : std::as_const(m_windows)) {
        QJsonArray tabs;
        int current = 0;
        for (DocumentTab *tab : window->tabs()) {
            const bool untitled = tab->path().isEmpty();
            const bool unsaved = tab->isModified() || (untitled && !tab->text().isEmpty());
            QString backup;
            if (unsaved && hotExit() && !skipBackup.contains(tab)) {
                backup = writeBackup(tab);
                if (backup.isEmpty())
                    failed.append(tab);
            }
            if (untitled && backup.isEmpty())
                continue;

            QJsonObject entry;
            entry[QStringLiteral("path")] = tab->path();
            if (!backup.isEmpty()) {
                entry[QStringLiteral("backup")] = backup;
                entry[QStringLiteral("modified")] = tab->isModified();
            }
            entry[QStringLiteral("encoding")] = tab->encoding();
            entry[QStringLiteral("crlf")] = tab->lineEnding() == DocumentTab::LineEnding::CRLF;
            entry[QStringLiteral("mode")] = modeToString(tab->viewMode());
            if (tab->languageOverridden())
                entry[QStringLiteral("language")] = tab->definitionName();
            entry[QStringLiteral("caret")] = tab->editor()->textCursor().position();
            entry[QStringLiteral("topLine")] = tab->sourceView()->topLine();
            if (tab == window->tabs().value(window->currentTabIndex()))
                current = int(tabs.size());
            tabs.append(entry);
        }
        if (tabs.isEmpty())
            continue;
        QJsonObject object;
        object[QStringLiteral("geometry")] = QString::fromLatin1(window->saveGeometry().toBase64());
        object[QStringLiteral("maximized")] = window->isMaximized();
        object[QStringLiteral("current")] = current;
        object[QStringLiteral("tabs")] = tabs;
        windows.append(object);
    }
    for (const QJsonValue &carried : std::as_const(m_carried))
        windows.append(carried);

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("windows")] = windows;
    QSaveFile file(sessionFile());
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (!QDir().mkpath(dataDir()) || !file.open(QIODevice::WriteOnly) || file.write(json) != json.size()
        || !file.commit()) {
        // Backups exist but nothing points at them: the content is not safe.
        failed.clear();
        for (MainWindow *window : std::as_const(m_windows)) {
            for (DocumentTab *tab : window->tabs()) {
                if (tab->isModified() && !skipBackup.contains(tab))
                    failed.append(tab);
            }
        }
        return failed;
    }
    removeStaleBackups(windows);
    m_dirty = !failed.isEmpty();
    return failed;
}

void SessionManager::removeStaleBackups(const QJsonArray &windows)
{
    QSet<QString> keep;
    for (const QJsonValue &window : windows) {
        for (const QJsonValue &tab : window.toObject().value(QStringLiteral("tabs")).toArray())
            keep.insert(tab.toObject().value(QStringLiteral("backup")).toString());
    }
    const QDir dir(backupsDir());
    for (const QString &name : dir.entryList(QDir::Files)) {
        if (!keep.contains(name))
            QFile::remove(dir.filePath(name));
    }
}

bool SessionManager::prepareQuit()
{
    auto windowOf = [this](DocumentTab *tab) -> MainWindow * {
        for (MainWindow *window : std::as_const(m_windows)) {
            if (window->tabs().contains(tab))
                return window;
        }
        return nullptr;
    };

    if (!hotExit()) {
        for (MainWindow *window : QList<MainWindow *>(m_windows)) {
            for (DocumentTab *tab : window->tabs()) {
                if (!window->confirmDiscard(tab))
                    return false;
            }
        }
        saveSession();
        return true;
    }

    const QList<DocumentTab *> failed = saveSession();
    if (failed.isEmpty())
        return true;
    for (DocumentTab *tab : failed) {
        MainWindow *window = windowOf(tab);
        if (window && !window->confirmDiscard(tab))
            return false;
    }
    saveSession(failed);
    return true;
}

bool SessionManager::quit()
{
    if (!prepareQuit())
        return false;
    m_quitting = true;
    QApplication::closeAllWindows();
    m_quitting = false;
    return true;
}

void SessionManager::carryOverUnsaved()
{
    m_carried = {};
    if (!enabled())
        return;
    for (const QJsonValue &value : readWindows()) {
        QJsonObject window = value.toObject();
        QJsonArray tabs;
        for (const QJsonValue &tab : window.value(QStringLiteral("tabs")).toArray()) {
            const QString backup = tab.toObject().value(QStringLiteral("backup")).toString();
            if (!backup.isEmpty() && QFile::exists(backupsDir() + u'/' + backup))
                tabs.append(tab);
        }
        if (tabs.isEmpty())
            continue;
        window[QStringLiteral("tabs")] = tabs;
        window[QStringLiteral("current")] = 0;
        m_carried.append(window);
    }
}

bool SessionManager::restore()
{
    const bool wholeSession = Settings::instance().flag(key::restoreSession);
    const QJsonArray windows = readWindows();
    m_carried = {};

    int created = 0;
    for (const QJsonValue &windowValue : windows) {
        const QJsonObject windowObject = windowValue.toObject();
        auto *window = new MainWindow;
        window->setAttribute(Qt::WA_DeleteOnClose);

        for (const QJsonValue &tabValue : windowObject.value(QStringLiteral("tabs")).toArray()) {
            const QJsonObject entry = tabValue.toObject();
            const QString path = entry.value(QStringLiteral("path")).toString();
            const QString backupName = entry.value(QStringLiteral("backup")).toString();
            // Older sessions stored "utf8", "utf8bom", "legacy", ...
            QString encoding = encoding::canonical(entry.value(QStringLiteral("encoding")).toString());
            if (encoding.isEmpty())
                encoding = encoding::utf8;
            const auto lineEnding = entry.value(QStringLiteral("crlf")).toBool() ? DocumentTab::LineEnding::CRLF
                                                                                 : DocumentTab::LineEnding::LF;

            auto *tab = new DocumentTab;
            bool ok = false;
            if (!backupName.isEmpty()) {
                QFile backup(backupsDir() + u'/' + backupName);
                if (backup.open(QIODevice::ReadOnly)) {
                    tab->loadBackup(path, QString::fromUtf8(backup.readAll()), encoding, lineEnding);
                    if (!entry.value(QStringLiteral("modified")).toBool(true))
                        tab->document()->setModified(false);
                    ok = true;
                }
            }
            // Only unsaved tabs come back when the session itself is not restored.
            if (!ok && wholeSession && !path.isEmpty())
                ok = tab->load(path);
            if (!ok) {
                delete tab;
                continue;
            }
            if (entry.contains(QStringLiteral("language")))
                tab->setLanguage(entry.value(QStringLiteral("language")).toString());
            tab->setViewMode(modeFromString(entry.value(QStringLiteral("mode")).toString()));
            window->adoptTab(tab);

            QTextCursor cursor(tab->document());
            cursor.setPosition(qBound(0, entry.value(QStringLiteral("caret")).toInt(), qMax(0, tab->document()->characterCount() - 1)));
            tab->sourceView()->setTextCursor(cursor);
            if (tab->visualView())
                tab->visualView()->setTextCursor(cursor);
            const int topLine = entry.value(QStringLiteral("topLine")).toInt();
            tab->sourceView()->scrollToLine(topLine);
            // The view has no size yet; scroll again once it is laid out.
            QTimer::singleShot(0, tab, [view = QPointer<TextView>(tab->sourceView()), topLine] {
                if (view)
                    view->scrollToLine(topLine);
            });
        }

        if (window->tabCount() == 0) {
            delete window;
            continue;
        }
        window->setCurrentTabIndex(windowObject.value(QStringLiteral("current")).toInt());
        const QByteArray geometry = QByteArray::fromBase64(windowObject.value(QStringLiteral("geometry")).toString().toLatin1());
        if (!geometry.isEmpty())
            window->restoreGeometry(geometry);
        if (windowObject.value(QStringLiteral("maximized")).toBool())
            window->showMaximized();
        else
            window->show();
        ++created;
    }

    // Rewrite the session from the restored state; this also drops the old
    // backups, which have been loaded into tabs by now.
    if (created > 0)
        saveSession();
    return created > 0;
}

} // namespace vn
