#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QLockFile>
#include <QStringList>

#include <optional>

namespace vn {

namespace instanceRequest {

// An empty file list requests a new untitled window.
QByteArray encode(const QStringList &files);
std::optional<QStringList> decode(QByteArrayView message);

} // namespace instanceRequest

class InstanceChannel : public QObject
{
    Q_OBJECT

public:
    enum class StartResult { Listening, Forwarded, Failed };

    InstanceChannel();
    StartResult start(const QStringList &files);

Q_SIGNALS:
    void requested(const QStringList &files);

private:
    void acceptConnections();

    const QString m_name;
    // Keep the lock until the server has closed and removed its socket.
    QLockFile m_lock;
    int m_readers = 0;
    QLocalServer m_server;
};

} // namespace vn
