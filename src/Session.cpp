#include "Session.h"

#include "DocumentTab.h"
#include "MainWindow.h"
#include "SafeWrite.h"
#include "Settings.h"
#include "TextFile.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QSessionManager>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

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
// A document of this many characters (UTF-16 units: at least as many bytes in
// any encoding the app writes) is copied for its backup only after a pause in
// typing, because the copy runs on the window thread.
constexpr qint64 bigDocumentChars = 20'000'000;

qint64 &bigDocumentThreshold()
{
    static qint64 chars = bigDocumentChars;
    return chars;
}

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

// Unsaved work and the list of open files are the user's alone: the data
// and backups folders are 0700 (also when found wider) and the files in
// them 0600. Elsewhere the per-user folder's ACL already keeps others out.
bool makePrivateDir(const QString &path)
{
    if (!QDir().mkpath(path))
        return false;
#ifdef Q_OS_UNIX
    constexpr QFile::Permissions others = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther
                                          | QFile::WriteOther | QFile::ExeOther;
    if (QFile::permissions(path) & others)
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
#endif
    return true;
}

// `file` (a QSaveFile, open) is written 0600 whatever the umask.
void makePrivate(QFileDevice &file)
{
#ifdef Q_OS_UNIX
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
#else
    Q_UNUSED(file);
#endif
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

// The per-file settings a session entry overrides: language, indentation.
void applyFileSettings(DocumentTab *tab, const QJsonObject &entry)
{
    if (entry.contains(QStringLiteral("language")))
        tab->setLanguage(entry.value(QStringLiteral("language")).toString());
    if (entry.contains(QStringLiteral("tabSize")) && entry.contains(QStringLiteral("insertSpaces")))
        tab->setIndentation({qBound(1, entry.value(QStringLiteral("tabSize")).toInt(), 16),
                             entry.value(QStringLiteral("insertSpaces")).toBool()});
}

// Caret and scroll position, once the tab has its text.
void restoreView(DocumentTab *tab, const QJsonObject &entry)
{
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

} // namespace

// The backup files one round writes, on a helper thread. The text of each
// document was copied on the window thread; the helper encodes and writes it
// and the window thread takes the result over once the helper is done.
struct BackupItem
{
    QPointer<TextFile> file;
    QString name;
    // TextFile::changeSerial() of the copied text.
    quint64 serial = 0;
    QString text;
    bool written = false;
};

struct BackupJob
{
    ~BackupJob()
    {
        if (thread) {
            thread->wait();
            delete thread;
        }
    }

    quint64 id = 0;
    QString dataDir;
    QString backupsDir;
    std::function<void()> hook;
    QList<BackupItem> items;
    QThread *thread = nullptr;
};

namespace {

std::function<void()> &backupHook()
{
    static std::function<void()> hook;
    return hook;
}

// Only values and files: runs on the helper thread. Each file is written as
// writeBackup() always did: in the private folders, to a temporary file
// (0600) that replaces the backup in one step once complete.
void runBackupJob(BackupJob &job)
{
    if (!makePrivateDir(job.dataDir) || !makePrivateDir(job.backupsDir))
        return;
    for (BackupItem &item : job.items) {
        if (job.hook)
            job.hook();
        // As TextFile::text() would give it.
        item.text.replace(QChar::ParagraphSeparator, u'\n');
        const QByteArray bytes = std::exchange(item.text, QString()).toUtf8();
        item.written = writeFileSafely(job.backupsDir + u'/' + item.name, bytes, nullptr, makePrivate);
    }
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

// Waits for a write still running (a static is destroyed after main).
SessionManager::~SessionManager() = default;

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
    return m_persistent && (hotExit() || Settings::instance().flag(key::restoreSession));
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

void SessionManager::setBackupHook(std::function<void()> hook)
{
    backupHook() = std::move(hook);
}

void SessionManager::setBigDocumentChars(qint64 chars)
{
    bigDocumentThreshold() = chars > 0 ? chars : bigDocumentChars;
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
    // Closing many tabs at once writes the session once, at the end.
    if (m_syncDeferred > 0) {
        m_syncPending = true;
        return;
    }
    if (!m_quitting && enabled())
        saveSession();
}

SessionManager::SyncBatch::SyncBatch()
{
    ++SessionManager::instance().m_syncDeferred;
}

SessionManager::SyncBatch::~SyncBatch()
{
    SessionManager &session = SessionManager::instance();
    if (--session.m_syncDeferred > 0 || !session.m_syncPending)
        return;
    session.m_syncPending = false;
    session.sync();
}

void SessionManager::terminate()
{
    m_endedBySystem = true;
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
    // A write is running. When it is done the session file is written (see
    // completeBackup) and what changed meanwhile is left to the next round.
    if (m_backup)
        return;
    if (startBackup())
        return;
    if (m_backupDeferred) {
        // Only big documents still being typed in are left: the session file
        // is written with the older backups (a complete one each), and the
        // next round looks again.
        writeSession({}, false);
        if (m_timer && !m_timer->isActive())
            m_timer->start();
        return;
    }
    saveSession();
}

void SessionManager::onCommitData(QSessionManager &)
{
    m_endedBySystem = true;
    // Never cancel the shutdown. The work is safe in backups and comes back
    // on the next start, so closing the windows must not ask about it.
    if (!enabled() || m_windows.isEmpty())
        return;
    const bool safe = saveSession().isEmpty();
    if (safe && hotExit())
        m_quitting = true;
}

bool SessionManager::backupCurrent(const TextFile *file) const
{
    const QString name = file->backupName();
    return !name.isEmpty() && file->backupSerial() == file->changeSerial() && QFileInfo::exists(backupsDir() + u'/' + name);
}

bool SessionManager::startBackup()
{
    if (!hotExit())
        return false;
    auto job = std::make_unique<BackupJob>();
    QSet<const TextFile *> seen;
    m_backupDeferred = false;
    QHash<const TextFile *, quint64> bigSeen;
    for (MainWindow *window : std::as_const(m_windows)) {
        for (DocumentTab *tab : window->tabs()) {
            const bool untitled = tab->path().isEmpty();
            const bool unsaved = tab->isModified() || (untitled && !tab->document()->isEmpty());
            TextFile *file = tab->file();
            // One backup per document, however many tabs show it.
            if (!unsaved || seen.contains(file))
                continue;
            seen.insert(file);
            if (backupCurrent(file))
                continue;
            if (tab->document()->characterCount() >= bigDocumentThreshold()) {
                // Copy only once it has not changed for a whole round: no
                // edit for at least backupDelayMs. (A quit or a session
                // save does not wait: saveSession() writes it.)
                const quint64 now = file->changeSerial();
                bigSeen.insert(file, now);
                if (m_bigSerials.value(file, quint64(-1)) != now) {
                    m_backupDeferred = true;
                    continue;
                }
            }
            BackupItem item;
            item.file = file;
            item.name = file->backupName();
            if (item.name.isEmpty())
                item.name = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.serial = file->changeSerial();
            // The copy is all the window thread does: the newlines, encoding
            // and writing are the helper's.
            item.text = file->rawText();
            job->items.append(std::move(item));
        }
    }
    m_bigSerials = std::move(bigSeen);
    if (job->items.isEmpty())
        return false;

    static quint64 lastId = 0;
    job->id = ++lastId;
    job->dataDir = dataDir();
    job->backupsDir = backupsDir();
    job->hook = backupHook();
    BackupJob *raw = job.get();
    job->thread = QThread::create([raw] { runBackupJob(*raw); });
    connect(
        job->thread, &QThread::finished, this,
        [this, id = job->id] {
            if (m_backup && m_backup->id == id)
                completeBackup();
        },
        Qt::QueuedConnection);
    m_backup = std::move(job);
    m_backup->thread->start();
    return true;
}

void SessionManager::finishBackup()
{
    if (!m_backup)
        return;
    const std::unique_ptr<BackupJob> job = std::move(m_backup);
    job->thread->wait();
    for (const BackupItem &item : std::as_const(job->items)) {
        // A document closed meanwhile: its file is not listed in the session
        // and removeStaleBackups() takes it away.
        if (!item.written || !item.file)
            continue;
        item.file->setBackupName(item.name);
        item.file->setBackupSerial(item.serial);
    }
}

void SessionManager::completeBackup()
{
    finishBackup();
    if (m_quitting || m_windows.isEmpty() || !enabled())
        return;
    // The backups the helper wrote are complete; the session file may now
    // point at them. Text changed meanwhile is written next round, and until
    // then the session keeps the older backup of it.
    writeSession({}, false);
    if ((m_backupStale || m_backupDeferred) && m_timer && !m_timer->isActive())
        m_timer->start();
}

QString SessionManager::writeBackup(DocumentTab *tab, bool write)
{
    // One backup per document, however many tabs show it.
    TextFile *document = tab->file();
    QString name = document->backupName();
    // Unchanged since the last backup (the timer fires for caret moves,
    // format changes and other tabs too): keep it rather than copy, encode
    // and fsync the whole text again. It may have been removed meanwhile as
    // stale, while the document was unmodified.
    const quint64 serial = document->changeSerial();
    if (backupCurrent(document))
        return name;
    if (!write) {
        // Not written here: the older backup, a complete one, still stands.
        m_backupStale = true;
        return !name.isEmpty() && QFileInfo::exists(backupsDir() + u'/' + name) ? name : QString();
    }
    if (name.isEmpty())
        name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!makePrivateDir(dataDir()) || !makePrivateDir(backupsDir()))
        return {};
    if (!writeFileSafely(backupsDir() + u'/' + name, tab->text().toUtf8(), nullptr, makePrivate))
        return {};
    document->setBackupName(name);
    document->setBackupSerial(serial);
    return name;
}

QList<DocumentTab *> SessionManager::saveSession(const QList<DocumentTab *> &skipBackup)
{
    // A write still running would put its file back after removeStaleBackups()
    // took it away, and an older text over a newer one: let it finish first.
    finishBackup();
    return writeSession(skipBackup, true);
}

QList<DocumentTab *> SessionManager::writeSession(const QList<DocumentTab *> &skipBackup, bool writeBackups)
{
    if (!enabled())
        return {};

    m_backupStale = false;
    QList<DocumentTab *> failed;
    QJsonArray windows;
    // Tabs sharing a document share its backup, written once.
    QHash<const TextFile *, QString> written;
    for (MainWindow *window : std::as_const(m_windows)) {
        QJsonArray tabs;
        int current = 0;
        // Each stored tab's index in `tabs`, for the grid.
        QHash<DocumentTab *, int> ids;
        const QList<DocumentTab *> windowTabs = window->tabs();
        for (DocumentTab *tab : windowTabs) {
            // (Unless another tab on its file has changed it: then it is
            // backed up with that one.)
            if (tab->isDeferred() && !tab->isModified()) {
                // Not read since the restore: it has no caret or scroll
                // position of its own, and nothing to back up. What the
                // session stored of it stays, as it was.
                if (tab == windowTabs.value(window->currentTabIndex()))
                    current = int(tabs.size());
                ids.insert(tab, int(tabs.size()));
                tabs.append(tab->sessionEntry());
                continue;
            }
            const bool untitled = tab->path().isEmpty();
            const bool unsaved = tab->isModified() || (untitled && !tab->document()->isEmpty());
            QString backup;
            if (unsaved && hotExit() && !skipBackup.contains(tab)) {
                const auto it = written.constFind(tab->file());
                backup = it != written.constEnd() ? *it : writeBackup(tab, writeBackups);
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
            // (Not a QTextCursor: placing one in a very long line costs a pass
            // over the line after every edit, and this runs after each backup.)
            entry[QStringLiteral("caret")] = tab->editor()->primaryCaret().position;
            entry[QStringLiteral("topLine")] = tab->sourceView()->topLine();
            if (tab == windowTabs.value(window->currentTabIndex()))
                current = int(tabs.size());
            ids.insert(tab, int(tabs.size()));
            tabs.append(entry);
        }
        // Tabs a restore still loads keep what was restored, in their places:
        // they have no text yet, and nothing of theirs can have changed.
        QHash<const void *, int> pendingIds;
        for (const MainWindow::PendingSessionTab &pending : window->pendingSessionTabs()) {
            pendingIds.insert(pending.key, int(tabs.size()));
            tabs.append(pending.entry);
        }
        if (tabs.isEmpty())
            continue;
        QJsonObject object;
        object[QStringLiteral("geometry")] = QString::fromLatin1(window->saveGeometry().toBase64());
        object[QStringLiteral("maximized")] = window->isMaximized();
        object[QStringLiteral("current")] = current;
        object[QStringLiteral("tabs")] = tabs;
        object[QStringLiteral("grid")] = window->saveGrid(ids, pendingIds);
        windows.append(object);
    }
    for (const QJsonValue &carried : std::as_const(m_carried))
        windows.append(carried);

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("windows")] = windows;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (!makePrivateDir(dataDir()) || !writeFileSafely(sessionFile(), json, nullptr, makePrivate)) {
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
    // (A big document left for a pause in typing is still to be backed up.)
    m_dirty = !failed.isEmpty() || m_backupStale || m_backupDeferred;
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
    // The user quits: whatever a logout that was then cancelled left.
    m_endedBySystem = false;
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
    // Files loading off the GUI thread, by path, likewise.
    QHash<QString, std::shared_ptr<TextFile>> loading;
    for (const QJsonValue &windowValue : windows) {
        const QJsonObject windowObject = windowValue.toObject();
        auto *window = new MainWindow;
        window->setAttribute(Qt::WA_DeleteOnClose);
        // Tabs without their text yet are read when they show, not as they
        // are placed (the first of each group is current for a moment).
        window->setHoldLoads(true);

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
            // Which file this tab is the first of, if its reading is put off.
            bool createdHere = false;
            if (!shared && backupName.isEmpty() && wholeSession && !path.isEmpty()) {
                // A big file would hold up the window (and every other file
                // of the session) while it is read and set up: it loads off
                // the GUI thread, as opening it does, and its tab follows.
                const bool second = loading.contains(path);
                if (second || QFileInfo(path).size() >= TextFile::largeLoadBytes) {
                    std::shared_ptr<TextFile> &file = loading[path];
                    if (!file) {
                        file = TextFile::create();
                        file->loadAsync(path);
                    }
                    window->restoreLater(id, file, path, entry, [entry, second](DocumentTab *tab) {
                        if (!second)
                            applyFileSettings(tab, entry);
                        tab->setViewMode(modeFromString(entry.value(QStringLiteral("mode")).toString()));
                        restoreView(tab, entry);
                    });
                    continue;
                }
            }
            // A file of the session that is there: not read until its tab shows
            // (the current tab of each group does, before the window). The
            // rest of 60 tabs would cost seconds of reading and colouring.
            if (!shared && backupName.isEmpty() && wholeSession && !path.isEmpty()) {
                const QFileInfo info(path);
                if (info.isFile() && info.isReadable()) {
                    shared = TextFile::createDeferred(path);
                    createdHere = true;
                }
            }
            DocumentTab *tab = nullptr;
            if (shared && shared->isDeferred()) {
                tab = new DocumentTab(shared);
                tab->setDeferred(entry, [entry, createdHere](DocumentTab *loaded) {
                    if (createdHere)
                        applyFileSettings(loaded, entry);
                    loaded->setViewMode(modeFromString(entry.value(QStringLiteral("mode")).toString()));
                    restoreView(loaded, entry);
                });
                restored.insert(id, tab);
                continue;
            }
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
                applyFileSettings(tab, entry);
            }
            tab->setViewMode(modeFromString(entry.value(QStringLiteral("mode")).toString()));
            restored.insert(id, tab);
            entries.insert(tab, entry);
        }
        // Sessions from before the grid have none: one group, as then.
        const QJsonObject grid = windowObject.value(QStringLiteral("grid")).toObject();
        window->restoreGrid(grid, restored);

        for (DocumentTab *tab : std::as_const(restored)) {
            if (!tab->isDeferred())
                restoreView(tab, entries.value(tab));
        }

        if (window->tabCount() == 0 && !window->isOpening()) {
            delete window;
            continue;
        }
        if (grid.isEmpty()) {
            // "current" is an id; tabs before it may be missing or loading.
            if (DocumentTab *current = restored.value(windowObject.value(QStringLiteral("current")).toInt()))
                window->setCurrentTabIndex(int(window->tabs().indexOf(current)));
        }
        // The current tab of each group is read now, before the window shows.
        window->setHoldLoads(false);
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
