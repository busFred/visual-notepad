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
    // Listening: this copy owns the session and takes other launches' files.
    // Alone: it owns the session but has no channel to offer.
    // Forwarded: a running copy took the files; this one should exit.
    // Failed: a running copy has the session but didn't take the files.
    enum class StartResult { Listening, Alone, Forwarded, Failed };

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
