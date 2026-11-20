// Session restore and recovery of unsaved work. While the app runs, unsaved
// content is backed up shortly after each change (restoreUnsaved), so a
// crash, logout or kill loses at most a moment of typing. A normal quit asks
// about every unsaved tab instead and leaves no backups behind; what the user
// discarded never comes back. session.json also lists the open files for
// restoreSession. The next start without files on the command line restores.
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QSessionManager;
class QTimer;

namespace vn {

class DocumentTab;
class MainWindow;
class TextFile;
struct BackupJob;

class SessionManager : public QObject
{
    Q_OBJECT

public:
    static SessionManager &instance();

    // Directory holding session.json and backups/. Defaults to the
    // application's local data location; tests point it at a temporary one.
    static void setDataDir(const QString &path);
    static QString dataDir();

    // Windows register themselves so the session knows them in creation order.
    void registerWindow(MainWindow *window);
    void unregisterWindow(MainWindow *window);
    QList<MainWindow *> windows() const { return m_windows; }
    bool isLastWindow(const MainWindow *window) const;

    // True while the final quit is closing windows: closeEvent must not prompt.
    bool isQuitting() const { return m_quitting; }

    // File > Quit: asks about unsaved changes, saves the session, then closes
    // every window. Returns false if the user cancelled.
    bool quit();
    // The part of quit() that also runs when the last window is closed.
    bool prepareQuit();

    // Recreates windows and tabs from session.json and shows them. Returns
    // false if nothing was restored. Only unsaved tabs when restoreSession is off.
    bool restore();
    // For a start with files on the command line: keeps the previous
    // session's unsaved tabs in the session file so the next plain start
    // restores them.
    void carryOverUnsaved();

    // Writes session.json and backups without prompting. Returns the modified
    // tabs whose content could not be saved.
    QList<DocumentTab *> saveSession(const QList<DocumentTab *> &skipBackup = {});

    // Something a session stores changed; backs up again soon.
    void markDirty();
    // True while the timer's backup is being written on its helper thread.
    bool isBackingUp() const { return m_backup != nullptr; }
    // Tests only: called on the helper thread before each backup file is
    // written, so a test can hold a write in flight.
    static void setBackupHook(std::function<void()> hook);
    // Tests only: the character count from which a document is backed up
    // only after a pause in typing (0: the default).
    static void setBigDocumentChars(qint64 chars);
    // Writes the session now, e.g. after a tab was closed so its discarded
    // backup cannot be recovered. Within a SyncBatch, once when it ends.
    void sync();
    // Coalesces the sync() calls made while it lives (closing many tabs).
    class SyncBatch
    {
    public:
        SyncBatch();
        ~SyncBatch();
        Q_DISABLE_COPY_MOVE(SyncBatch)
    };
    // Backups, the logout/shutdown hook and, on Unix, SIGTERM/SIGHUP/SIGINT.
    void start();
    // Off: nothing of the session is read or written, for a process that
    // runs beside the copy that owns it (see main.cpp).
    void setPersistent(bool on) { m_persistent = on; }

private:
    SessionManager();
    ~SessionManager() override;
    void onCommitData(QSessionManager &manager);
    // A forced close that cannot ask: back up and exit.
    void terminate();
    void backupIfDirty();
    // `writeBackups` false (while a write finished and the session file is
    // updated, see completeBackup) keeps the backups as they are, old ones
    // included; saveSession() writes them.
    QList<DocumentTab *> writeSession(const QList<DocumentTab *> &skipBackup, bool writeBackups);
    QString writeBackup(DocumentTab *tab, bool write);
    // The backup `file` already has is of its current text and still there.
    bool backupCurrent(const TextFile *file) const;
    // Copies the text of the documents whose backup is out of date and hands
    // the writing to a helper thread. False when none needs one.
    bool startBackup();
    // Waits for the helper (if any) and takes over what it wrote.
    void finishBackup();
    // The helper is done: updates the session file to point at its backups.
    void completeBackup();
    void removeStaleBackups(const QJsonArray &windows);
    bool enabled() const;
    bool m_persistent = true;

    QList<MainWindow *> m_windows;
    QJsonArray m_carried;
    QTimer *m_timer = nullptr;
    std::unique_ptr<BackupJob> m_backup;
    // A backup out of date or missing was left to the next round.
    bool m_backupStale = false;
    // Big documents are copied once their change serial is the one the
    // previous round saw; true when a round left one for the next.
    bool m_backupDeferred = false;
    QHash<const TextFile *, quint64> m_bigSerials;
    bool m_quitting = false;
    bool m_dirty = false;
    bool m_started = false;
    int m_syncDeferred = 0;
    bool m_syncPending = false;
};

} // namespace vn
