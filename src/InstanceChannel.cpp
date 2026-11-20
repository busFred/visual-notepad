#include "InstanceChannel.h"

#include "Settings.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QStringDecoder>
#include <QThread>
#include <QTimer>
#include <QtEndian>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vn {

namespace {

constexpr QByteArrayView magic("VNOP\x01");
constexpr qsizetype headerSize = magic.size() + sizeof(quint32);
constexpr qsizetype maxPayload = 256 * 1024;
constexpr int timeoutMs = 5000;
constexpr int maxReaders = 16;

QString serverName()
{
    QByteArray identity = QFileInfo(Settings::instance().fileName()).absolutePath().toUtf8();
    identity += '\0';
#ifdef Q_OS_UNIX
    identity += QByteArray::number(::geteuid());
#else
    identity += qgetenv("USERNAME");
#endif
    identity += '\0';
    identity += qgetenv("VN_INSTANCE_NAME");
    const QByteArray hash = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
    return QStringLiteral("visnotepad-") + QString::fromLatin1(hash.first(32));
}

bool isAbsoluteFilePath(const QString &path)
{
    if (path.contains(QChar::Null))
        return false;
#ifdef Q_OS_WIN
    QString normalized = QDir::fromNativeSeparators(path);
    // Drive roots and UNC shares are absolute. A leading slash on its own
    // is relative to the current drive.
    if (normalized.startsWith(QLatin1String("//?/UNC/"), Qt::CaseInsensitive))
        normalized.replace(0, 8, QLatin1String("//"));
    if (normalized.startsWith(QLatin1String("//?/"))) {
        // Extended drive paths are allowed, other device namespaces are not.
        normalized.remove(0, 4);
    } else if (normalized.startsWith(QLatin1String("//"))) {
        const qsizetype serverEnd = normalized.indexOf(u'/', 2);
        if (serverEnd <= 2 || serverEnd + 1 == normalized.size() || normalized[serverEnd + 1] == u'/')
            return false;
        const QStringView server = QStringView(normalized).sliced(2, serverEnd - 2);
        return server != QLatin1String(".");
    }
    if (normalized.size() < 3 || normalized[1] != u':' || normalized[2] != u'/')
        return false;
    const QChar drive = normalized[0];
    return (drive >= u'A' && drive <= u'Z') || (drive >= u'a' && drive <= u'z');
#else
    return path.startsWith(u'/');
#endif
}

// Each request starts with magic and version, followed by a big-endian
// payload size, then one NUL-terminated UTF-8 string per file.
qsizetype messageSize(QByteArrayView header)
{
    if (header.size() < headerSize || !header.startsWith(magic))
        return 0;
    const quint32 payloadSize = qFromBigEndian<quint32>(header.data() + magic.size());
    return payloadSize <= maxPayload ? headerSize + payloadSize : 0;
}

bool sendRequest(QLocalSocket &socket, const QStringList &files)
{
    const QByteArray message = instanceRequest::encode(files);
    if (message.isEmpty()) {
        qWarning("Cannot forward these file paths to Visual Notepad.");
        return false;
    }
#ifdef Q_OS_WIN
    // The shell may have granted this launch permission to take focus.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    if (socket.write(message) != message.size())
        return false;
    QDeadlineTimer deadline(timeoutMs);
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(int(deadline.remainingTime())) && socket.bytesToWrite() > 0)
            return false;
    }
    socket.disconnectFromServer();
    return true;
}

} // namespace

QByteArray instanceRequest::encode(const QStringList &files)
{
    QByteArray message = magic.toByteArray();
    message.resize(headerSize);
    for (const QString &path : files) {
        if (path.size() >= maxPayload || !isAbsoluteFilePath(path) || !path.isValidUtf16())
            return {};
        const QByteArray bytes = path.toUtf8();
        if (message.size() - headerSize + bytes.size() + 1 > maxPayload)
            return {};
        message.append(bytes);
        message.append('\0');
    }
    qToBigEndian<quint32>(quint32(message.size() - headerSize), message.data() + magic.size());
    return message;
}

std::optional<QStringList> instanceRequest::decode(QByteArrayView message)
{
    if (const qsizetype size = messageSize(message); size == 0 || size != message.size())
        return std::nullopt;
    QStringList files;
    QStringDecoder utf8(QStringDecoder::Utf8, QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
    QByteArrayView payload = message.sliced(headerSize);
    while (!payload.empty()) {
        const qsizetype end = payload.indexOf('\0');
        if (end <= 0)
            return std::nullopt;
        const QString path = utf8(payload.first(end));
        if (utf8.hasError() || !isAbsoluteFilePath(path))
            return std::nullopt;
        files.append(path);
        payload = payload.sliced(end + 1);
    }
    return files;
}

InstanceChannel::InstanceChannel()
    : m_name(serverName())
    , m_lock(QFileInfo(Settings::instance().fileName()).dir().filePath(m_name + QStringLiteral(".lock")))
{
    m_lock.setStaleLockTime(0);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &InstanceChannel::acceptConnections);
}

InstanceChannel::StartResult InstanceChannel::start(const QStringList &files)
{
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        QLocalSocket socket;
        socket.connectToServer(m_name);
        if (socket.state() == QLocalSocket::ConnectedState || socket.waitForConnected(100)) {
            if (sendRequest(socket, files))
                return StartResult::Forwarded;
            qWarning("Could not send the launch request to Visual Notepad.");
            return StartResult::Failed;
        }

        if (m_lock.isLocked()) {
            // Hold the election lock before probing again and removing a
            // stale socket. A slow or concurrent primary keeps its lock.
            if (socket.error() == QLocalSocket::ServerNotFoundError
                || socket.error() == QLocalSocket::ConnectionRefusedError) {
                QLocalServer::removeServer(m_name);
                if (m_server.listen(m_name))
                    return StartResult::Listening;
                qWarning() << m_server.errorString();
            } else {
                qWarning() << socket.errorString();
            }
            return StartResult::Failed;
        }
        if (m_lock.tryLock())
            continue;
        if (m_lock.error() != QLockFile::LockFailedError || elapsed.hasExpired(timeoutMs)) {
            qWarning("Could not reach or start the Visual Notepad instance.");
            return StartResult::Failed;
        }
        QThread::msleep(20);
    }
}

void InstanceChannel::acceptConnections()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        if (m_readers >= maxReaders) {
            socket->abort();
            socket->deleteLater();
            continue;
        }

        ++m_readers;
        connect(socket, &QObject::destroyed, this, [this] { --m_readers; });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        socket->setReadBufferSize(headerSize + maxPayload + 1);
        QTimer::singleShot(timeoutMs, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });

        const auto read = [this, socket] {
            if (socket->bytesAvailable() < headerSize)
                return;
            const qsizetype size = messageSize(socket->peek(headerSize));
            if (size == 0 || socket->bytesAvailable() > size) {
                socket->abort();
                socket->deleteLater();
                return;
            }
            if (socket->bytesAvailable() < size)
                return;
            const auto files = instanceRequest::decode(socket->read(size));
            socket->disconnectFromServer();
            socket->deleteLater();
            if (files)
                Q_EMIT requested(*files);
        };
        connect(socket, &QLocalSocket::readyRead, this, read);
        read();
    }
}

} // namespace vn
