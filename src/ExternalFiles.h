#pragma once

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>

class QApplication;

namespace vn {

class MainWindow;

// Routes launches and Finder events using the same window preference.
class ExternalFiles : public QObject
{
public:
    explicit ExternalFiles(QApplication &app);

    // Requests can arrive while the first instance is restoring its session.
    void request(const QStringList &files);
    void finishStartup();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void dispatch();
    MainWindow *lastActiveWindow() const;

    QList<QPointer<MainWindow>> m_activationOrder;
    QQueue<QStringList> m_requests;
    bool m_ready = false;
    bool m_dispatching = false;
};

} // namespace vn
