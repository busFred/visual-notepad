#include "ExternalFiles.h"

#include "MainWindow.h"
#include "Session.h"
#include "Settings.h"

#include <QApplication>
#include <QFileOpenEvent>
#include <QScopedValueRollback>
#include <QTimer>

#include <utility>

namespace vn {

ExternalFiles::ExternalFiles(QApplication &app)
    : m_dialogWait(new QTimer(this))
{
    app.installEventFilter(this);
    m_dialogWait->setSingleShot(true);
    m_dialogWait->setInterval(100);
    connect(m_dialogWait, &QTimer::timeout, this, [this] { dispatch(); });
}

bool ExternalFiles::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::WindowActivate) {
        if (auto *window = qobject_cast<MainWindow *>(watched); window && window->isWindow()) {
            m_activationOrder.removeIf([window](const QPointer<MainWindow> &entry) {
                return entry.isNull() || entry == window;
            });
            m_activationOrder.prepend(QPointer<MainWindow>(window));
        }
    }
#ifdef Q_OS_MACOS
    // Finder sends FileOpen events, including for a drop on the Dock icon:
    // one per file, back to back. Gather those of one go into one request, so
    // that "A new window" opens the files together, not a window each.
    if (event->type() == QEvent::FileOpen) {
        const QString file = static_cast<QFileOpenEvent *>(event)->file();
        if (!file.isEmpty()) {
            if (m_finderFiles.isEmpty())
                QTimer::singleShot(0, this, [this] { request(std::exchange(m_finderFiles, {})); });
            m_finderFiles.append(file);
            return true;
        }
    }
#endif
    return QObject::eventFilter(watched, event);
}

void ExternalFiles::request(const QStringList &files)
{
    m_requests.enqueue(files);
    dispatch();
}

void ExternalFiles::finishStartup()
{
    m_ready = true;
    dispatch();
}

QList<QStringList> ExternalFiles::takeWaiting()
{
    QList<QStringList> requests(m_requests.cbegin(), m_requests.cend());
    m_requests.clear();
    return requests;
}

QList<QStringList> ExternalFiles::commandLines(const QList<QStringList> &requests)
{
    constexpr qsizetype room = 16'000;
    QList<QStringList> launches;
    for (const QStringList &files : requests) {
        if (files.isEmpty()) {
            launches.append(QStringList());
            continue;
        }
        QStringList arguments{QStringLiteral("--")};
        qsizetype length = 0;
        for (const QString &file : files) {
            if (arguments.size() > 1 && length + file.size() + 3 > room) {
                launches.append(arguments);
                arguments = {QStringLiteral("--")};
                length = 0;
            }
            arguments.append(file);
            length += file.size() + 3; // (Quotes and a space.)
        }
        launches.append(arguments);
    }
    return launches;
}

MainWindow *ExternalFiles::lastActiveWindow() const
{
    const QList<MainWindow *> windows = SessionManager::instance().windows();
    for (const QPointer<MainWindow> &window : m_activationOrder) {
        if (window && window->isVisible() && windows.contains(window))
            return window;
    }
    // A restored window might have been shown without ever taking focus.
    for (auto it = windows.crbegin(); it != windows.crend(); ++it) {
        if ((*it)->isVisible())
            return *it;
    }
    return nullptr;
}

void ExternalFiles::dispatch()
{
    if (!m_ready || m_dispatching)
        return;
    // Opening a file can run a dialog. Finish this launch before the next.
    const QScopedValueRollback dispatching(m_dispatching, true);
    while (!m_requests.isEmpty()) {
        // A dialog asks (whether to save before a window or the app closes,
        // say): the files wait for it, rather than open in a window about to
        // go. (If the app then quits, main() hands them to a new launch.)
        if (QApplication::activeModalWidget()) {
            m_dialogWait->start();
            return;
        }
        const QStringList files = m_requests.dequeue();
        MainWindow *window = nullptr;
        if (!files.isEmpty() && Settings::instance().text(key::externalFilesWindow) == QLatin1String("active"))
            window = lastActiveWindow();
        if (!window) {
            window = new MainWindow;
            window->setAttribute(Qt::WA_DeleteOnClose);
            if (files.isEmpty())
                window->newFile();
            window->show();
        }
        if (window->isMinimized())
            window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
        window->raise();
        window->activateWindow();
        for (const QString &file : files)
            window->openFile(file);
    }
}

} // namespace vn
