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
    // The files of requests still waiting (the app quit while a dialog
    // asked), for another launch to open; the queue is emptied.
    QStringList takeWaiting();

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
