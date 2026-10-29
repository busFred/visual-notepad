#include "Session.h"

#include "DocumentTab.h"
#include "MainWindow.h"
#include "Settings.h"
#include "TextFile.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
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

#ifdef Q_OS_UNIX
#include <QSocketNotifier>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vn {

namespace {

// At most this long between a change and its backup.
constexpr int backupDelayMs = 2000;

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

#ifdef Q_OS_UNIX
int signalPipe[2] = {-1, -1};

void onSignal(int)
{
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(signalPipe[1], &byte, 1);
}
#endif

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
    m_timer->setSingleShot(true);
    m_timer->setInterval(backupDelayMs);
    connect(m_timer, &QTimer::timeout, this, &SessionManager::backupIfDirty);
    if (m_dirty)
        m_timer->start();
#ifndef QT_NO_SESSIONMANAGER
    // Direct: the session manager is only valid during the emission.
    connect(qApp, &QGuiApplication::commitDataRequest, this, &SessionManager::onCommitData, Qt::DirectConnection);
#endif
#ifdef Q_OS_UNIX
    // `reboot`, `kill` or a closed terminal: the handler only wakes the event
    // loop, which does the work.
    if (::pipe(signalPipe) == 0) {
        for (int fd : signalPipe)
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(signalPipe[1], F_SETFL, O_NONBLOCK);
        auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] {
            char byte;
            [[maybe_unused]] const auto bytes = ::read(signalPipe[0], &byte, 1);
            terminate();
        });
        struct sigaction action = {};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        for (int signal : {SIGTERM, SIGHUP, SIGINT})
            ::sigaction(signal, &action, nullptr);
    }
#endif
}

void SessionManager::markDirty()
{
    m_dirty = true;
    // Not restarted on every keystroke, so steady typing is still backed up.
    if (m_timer && !m_timer->isActive())
        m_timer->start();
}

void SessionManager::sync()
{
    if (!m_quitting && enabled())
        saveSession();
}

void SessionManager::terminate()
{
    if (m_quitting)
        return;
    if (enabled() && !m_windows.isEmpty())
        saveSession();
    m_quitting = true;
    QApplication::closeAllWindows();
    QCoreApplication::quit();
}

void SessionManager::backupIfDirty()
{
    if (!m_dirty || m_quitting || m_windows.isEmpty() || !enabled())
        return;
    saveSession();
}

void SessionManager::onCommitData(QSessionManager &)
{
    // Never cancel the shutdown. The work is safe in backups and comes back
    // on the next start, so closing the windows must not ask about it.
    if (!enabled() || m_windows.isEmpty())
        return;
    const bool safe = saveSession().isEmpty();
    if (safe && hotExit())
        m_quitting = true;
}

QString SessionManager::writeBackup(DocumentTab *tab)
{
    // One backup per document, however many tabs show it.
    TextFile *document = tab->file();
    QString name = document->backupName();
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
    document->setBackupName(name);
    return name;
}

QList<DocumentTab *> SessionManager::saveSession(const QList<DocumentTab *> &skipBackup)
{
    if (!enabled())
        return {};

    QList<DocumentTab *> failed;
    QJsonArray windows;
    // Tabs sharing a document share its backup, written once.
    QHash<const TextFile *, QString> written;
    for (MainWindow *window : std::as_const(m_windows)) {
        QJsonArray tabs;
        int current = 0;
        // Each stored tab's index in `tabs`, for the grid.
        QHash<DocumentTab *, int> ids;
        for (DocumentTab *tab : window->tabs()) {
            const bool untitled = tab->path().isEmpty();
            const bool unsaved = tab->isModified() || (untitled && !tab->text().isEmpty());
            QString backup;
            if (unsaved && hotExit() && !skipBackup.contains(tab)) {
                const auto it = written.constFind(tab->file());
                backup = it != written.constEnd() ? *it : writeBackup(tab);
                if (backup.isEmpty())
                    failed.append(tab);
                else
                    written.insert(tab->file(), backup);
            }
            if (untitled && backup.isEmpty())
                continue;

            QJsonObject entry;
            entry[QStringLiteral("path")] = tab->path();
            if (!backup.isEmpty()) {
                entry[QStringLiteral("backup")] = backup;
                entry[QStringLiteral("modified")] = tab->isModified();
                // What the edits were based on, to notice disk changes made
                // while the app was not running.
                const TextFile::DiskState disk = tab->file()->diskBaseline();
                if (!untitled && disk.known) {
                    entry[QStringLiteral("diskModified")] = disk.modified.toMSecsSinceEpoch();
                    entry[QStringLiteral("diskSize")] = disk.size;
                    entry[QStringLiteral("diskHash")] = QString::fromLatin1(disk.hash.toHex());
                }
            }
            entry[QStringLiteral("encoding")] = tab->encoding();
            entry[QStringLiteral("crlf")] = tab->lineEnding() == DocumentTab::LineEnding::CRLF;
            entry[QStringLiteral("mode")] = modeToString(tab->viewMode());
            if (tab->languageOverridden())
                entry[QStringLiteral("language")] = tab->definitionName();
            if (tab->indentationOverridden()) {
                entry[QStringLiteral("tabSize")] = tab->indentation().tabSize;
                entry[QStringLiteral("insertSpaces")] = tab->indentation().insertSpaces;
            }
            entry[QStringLiteral("caret")] = tab->editor()->textCursor().position();
            entry[QStringLiteral("topLine")] = tab->sourceView()->topLine();
            if (tab == window->tabs().value(window->currentTabIndex()))
                current = int(tabs.size());
            ids.insert(tab, int(tabs.size()));
            tabs.append(entry);
        }
        if (tabs.isEmpty())
            continue;
        QJsonObject object;
        object[QStringLiteral("geometry")] = QString::fromLatin1(window->saveGeometry().toBase64());
        object[QStringLiteral("maximized")] = window->isMaximized();
        object[QStringLiteral("current")] = current;
        object[QStringLiteral("tabs")] = tabs;
        object[QStringLiteral("grid")] = window->saveGrid(ids);
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
    // A normal quit asks; recovery is only for closes that could not.
    QList<DocumentTab *> tabs;
    QSet<const TextFile *> asked;
    for (MainWindow *window : QList<MainWindow *>(m_windows)) {
        for (DocumentTab *tab : window->tabs()) {
            // Once per document, whichever tabs show it.
            if (!asked.contains(tab->file()) && !window->confirmDiscard(tab))
                return false;
            asked.insert(tab->file());
            tabs.append(tab);
        }
    }
    // Everything was saved or deliberately discarded: no backups.
    saveSession(tabs);
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
        // Its tab ids no longer match; the tabs go into one group.
        window.remove(QStringLiteral("grid"));
        m_carried.append(window);
    }
}

bool SessionManager::restore()
{
    const bool wholeSession = Settings::instance().flag(key::restoreSession);
    const QJsonArray windows = readWindows();
    m_carried = {};

    int created = 0;
    // Entries for one document (several tabs on it) restore into one.
    QHash<QString, std::shared_ptr<TextFile>> restoredBackups;
    for (const QJsonValue &windowValue : windows) {
        const QJsonObject windowObject = windowValue.toObject();
        auto *window = new MainWindow;
        window->setAttribute(Qt::WA_DeleteOnClose);

        // Tabs by their index in "tabs", placed in the grid once all exist.
        QMap<int, DocumentTab *> restored;
        QHash<DocumentTab *, QJsonObject> entries;
        const QJsonArray tabEntries = windowObject.value(QStringLiteral("tabs")).toArray();
        for (int id = 0; id < tabEntries.size(); ++id) {
            const QJsonObject entry = tabEntries[id].toObject();
            const QString path = entry.value(QStringLiteral("path")).toString();
            const QString backupName = entry.value(QStringLiteral("backup")).toString();
            // Older sessions stored "utf8", "utf8bom", "legacy", ...
            QString encoding = encoding::canonical(entry.value(QStringLiteral("encoding")).toString());
            if (encoding.isEmpty())
                encoding = encoding::utf8;
            const auto lineEnding = entry.value(QStringLiteral("crlf")).toBool() ? DocumentTab::LineEnding::CRLF
                                                                                 : DocumentTab::LineEnding::LF;

            std::shared_ptr<TextFile> shared = !backupName.isEmpty() ? restoredBackups.value(backupName) : nullptr;
            if (!shared && !path.isEmpty() && (!backupName.isEmpty() || wholeSession))
                shared = TextFile::find(path);
            DocumentTab *tab = nullptr;
            if (shared) {
                tab = new DocumentTab(shared);
            } else {
                tab = new DocumentTab;
                bool ok = false;
                if (!backupName.isEmpty()) {
                    QFile backup(backupsDir() + u'/' + backupName);
                    if (backup.open(QIODevice::ReadOnly)) {
                        tab->loadBackup(path, QString::fromUtf8(backup.readAll()), encoding, lineEnding);
                        if (!entry.value(QStringLiteral("modified")).toBool(true))
                            tab->document()->setModified(false);
                        if (entry.contains(QStringLiteral("diskHash"))) {
                            TextFile::DiskState disk;
                            disk.known = true;
                            disk.modified = QDateTime::fromMSecsSinceEpoch(
                                qint64(entry.value(QStringLiteral("diskModified")).toDouble()));
                            disk.size = qint64(entry.value(QStringLiteral("diskSize")).toDouble());
                            disk.hash = QByteArray::fromHex(entry.value(QStringLiteral("diskHash")).toString().toLatin1());
                            tab->file()->setDiskBaseline(disk);
                            tab->file()->scheduleDiskCheck();
                        }
                        restoredBackups.insert(backupName, tab->sharedFile());
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
                if (entry.contains(QStringLiteral("tabSize")) && entry.contains(QStringLiteral("insertSpaces")))
                    tab->setIndentation({qBound(1, entry.value(QStringLiteral("tabSize")).toInt(), 16),
                                         entry.value(QStringLiteral("insertSpaces")).toBool()});
            }
            tab->setViewMode(modeFromString(entry.value(QStringLiteral("mode")).toString()));
            restored.insert(id, tab);
            entries.insert(tab, entry);
        }
        // Sessions from before the grid have none: one group, as then.
        const QJsonObject grid = windowObject.value(QStringLiteral("grid")).toObject();
        window->restoreGrid(grid, restored);

        for (DocumentTab *tab : std::as_const(restored)) {
            const QJsonObject entry = entries.value(tab);
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
        if (grid.isEmpty())
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
