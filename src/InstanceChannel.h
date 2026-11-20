#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QLockFile>
#include <QStringList>

#include <optional>

namespace vn {

namespace instanceRequest {

// An empty file list requests a new untitled window. `sent`: when the
// request was sent, in ms since the epoch (negative: now).
QByteArray encode(const QStringList &files, qint64 sent = -1);
std::optional<QStringList> decode(QByteArrayView message, qint64 *sent = nullptr);
// Whether a request sent at `sent` is still awaited by its launch at `now`.
bool isFresh(qint64 sent, qint64 now);

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
    // Closes the channel and lets go of the session's lock, for a launch
    // made on the way out to take them.
    void release();

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
