// Session restore and recovery of unsaved work. While the app runs, unsaved
// content is backed up shortly after each change (restoreUnsaved), so a
// crash, logout or kill loses at most a moment of typing. A normal quit asks
// about every unsaved tab instead and leaves no backups behind; what the user
// discarded never comes back. session.json also lists the open files for
// restoreSession. The next start without files on the command line restores.
#pragma once

#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QString>

class QSessionManager;
class QTimer;

namespace vn {

class DocumentTab;
class MainWindow;

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
    // Writes the session now, e.g. after a tab was closed so its discarded
    // backup cannot be recovered.
    void sync();
    // Backups, the logout/shutdown hook and, on Unix, SIGTERM/SIGHUP/SIGINT.
    void start();

private:
    SessionManager();
    void onCommitData(QSessionManager &manager);
    // A forced close that cannot ask: back up and exit.
    void terminate();
    void backupIfDirty();
    QString writeBackup(DocumentTab *tab);
    void removeStaleBackups(const QJsonArray &windows);
    bool enabled() const;

    QList<MainWindow *> m_windows;
    QJsonArray m_carried;
    QTimer *m_timer = nullptr;
    bool m_quitting = false;
    bool m_dirty = false;
    bool m_started = false;
};

} // namespace vn
