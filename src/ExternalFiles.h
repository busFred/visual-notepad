#pragma once

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>

class QApplication;
class QTimer;

namespace vn {

class MainWindow;

// Routes launches and Finder events using the same window preference.
class ExternalFiles : public QObject
{
public:
    explicit ExternalFiles(QApplication &app);

    // Requests can arrive while the first instance is restoring its session,
    // or while a dialog asks: they wait for it to finish.
    void request(const QStringList &files);
    void finishStartup();
    // The requests still waiting (the app quit while a dialog asked), for
    // other launches to open; the queue is emptied. (An empty one asked for
    // a new window.)
    QList<QStringList> takeWaiting();
    // Command lines that make `requests` again, each well inside the shortest
    // system limit (Windows': 32,767 characters): "--" then files, a
    // request's files split over as many as they need; none for a new window.
    static QList<QStringList> commandLines(const QList<QStringList> &requests);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void dispatch();
    MainWindow *lastActiveWindow() const;

    QList<QPointer<MainWindow>> m_activationOrder;
    QQueue<QStringList> m_requests;
    QStringList m_finderFiles; // FileOpen events not yet made a request
    QTimer *m_dialogWait; // Looks again whether the dialog has gone.
    bool m_ready = false;
    bool m_dispatching = false;
};

} // namespace vn
