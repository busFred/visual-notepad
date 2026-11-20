#include "InstanceChannel.h"

#include "Session.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
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

constexpr QByteArrayView magic("VNOP\x02");
constexpr qsizetype sizeAt = magic.size();
constexpr qsizetype sentAt = sizeAt + sizeof(quint32);
constexpr qsizetype headerSize = sentAt + sizeof(qint64);
constexpr qsizetype maxPayload = 256 * 1024;
constexpr int timeoutMs = 5000;
constexpr int maxReaders = 16;
// What the running copy answers once a request is in its queue.
constexpr char received = 'K';

// A random key kept in the user's own settings folder, which other users
// cannot read: it makes the channel's name one they cannot guess, so they
// cannot take the name first and receive the paths this user opens. Made
// once, atomically, so that two first launches agree on it.
QByteArray newKey()
{
    QByteArray key(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(key.begin(), key.end());
    return key.toHex();
}

QByteArray instanceKey()
{
    const QString path = QFileInfo(Settings::instance().fileName()).dir().filePath(QStringLiteral("instance.key"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    constexpr QFileDevice::Permissions ownerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly, ownerOnly)) {
        const QByteArray key = newKey();
        if (file.write(key) == key.size() && file.flush())
            return key;
        file.remove();
        return {};
    }
    // Made by another launch, which writes it right after making it: a
    // moment is enough to see it whole.
    QDeadlineTimer deadline(500);
    do {
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray key = file.read(65);
            file.close();
            if (key.size() == 64)
                return key;
        }
        QThread::msleep(10);
    } while (!deadline.hasExpired());
    // Still not a key (cut short by a crash or a power cut): a new one, put
    // in place whole. (A launch that read the old one meanwhile runs on its
    // own; the session lock keeps the two apart.)
    QSaveFile replacement(path);
    const QByteArray key = newKey();
    if (replacement.open(QIODevice::WriteOnly) && replacement.write(key) == key.size() && replacement.commit()) {
        QFile::setPermissions(path, ownerOnly);
        return key;
    }
    return {};
}

QString serverName()
{
    QByteArray identity = QFileInfo(Settings::instance().fileName()).absolutePath().toUtf8();
    identity += '\0';
    identity += instanceKey();
    identity += '\0';
#ifdef Q_OS_UNIX
    identity += QByteArray::number(::geteuid());
#endif
    identity += '\0';
    identity += qgetenv("VN_INSTANCE_NAME");
    const QByteArray hash = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
    // (Short, for the socket path's limit below; the key makes it unguessable.)
    const QString name = QStringLiteral("visnotepad-") + QString::fromLatin1(hash.first(16));
#ifdef Q_OS_UNIX
    // A path in the user's runtime folder, not a bare name: Qt would put that
    // in the shared temporary folder, where the socket's name shows and
    // another user could take it once this user's copy has quit. On Linux Qt
    // gives the folder only if it is the user's and closed to others; on
    // macOS it is in the user's Library, which only they can enter.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        return {};
    const QString path = QDir(dir).filePath(name);
    // (A socket's path must fit in sockaddr_un: 104 bytes on macOS.)
    return QFile::encodeName(path).size() < 100 ? path : QString();
#else
    return name;
#endif
}

#ifdef Q_OS_WIN
// The SID of the user a token belongs to, as bytes; empty if unknown.
QByteArray tokenUser(HANDLE token)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0)
        return {};
    QByteArray buffer(int(size), Qt::Uninitialized);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
        return {};
    const PSID sid = reinterpret_cast<const TOKEN_USER *>(buffer.constData())->User.Sid;
    if (!IsValidSid(sid))
        return {};
    return QByteArray(static_cast<const char *>(sid), int(GetLengthSid(sid)));
}

QByteArray processUser(HANDLE process)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return {};
    const QByteArray user = tokenUser(token);
    CloseHandle(token);
    return user;
}

// Pipe names are one namespace for all users, and anyone can list them: the
// copy that made this pipe must be this user's, or it gets no paths.
bool serverIsOurs(const QLocalSocket &socket)
{
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket.socketDescriptor()), &pid))
        return false;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return false;
    const QByteArray theirs = processUser(process);
    CloseHandle(process);
    const QByteArray ours = processUser(GetCurrentProcess());
    return !ours.isEmpty() && theirs == ours;
}
#endif

// The lock goes with the session it guards: one per session folder,
// whatever the channel's name came to (a runtime folder that differs or is
// missing, a new key), so that two copies never both own one session.
QString lockPath()
{
    const QString dir = SessionManager::dataDir();
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("instance.lock"));
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

// Each request starts with magic and version, a big-endian payload size and
// the time it was sent (ms since the epoch), then one NUL-terminated UTF-8
// string per file.
qsizetype messageSize(QByteArrayView header)
{
    if (header.size() < headerSize || !header.startsWith(magic))
        return 0;
    const quint32 payloadSize = qFromBigEndian<quint32>(header.data() + sizeAt);
    return payloadSize <= maxPayload ? headerSize + payloadSize : 0;
}

enum class Probe { Connected, Absent, Unreachable };

// Connects to the running copy, if there is one.
Probe connectTo(QLocalSocket &socket, const QString &name)
{
#ifdef Q_OS_WIN
    // As QLocalSocket connects, but at the identification level: the pipe's
    // server may learn who connects, not act as them. (Qt opens pipes at the
    // default level, which lets a server with the right to impersonate, a
    // service for one, act as this user: one that took the name first.)
    const QString path = QStringLiteral("\\\\.\\pipe\\") + name;
    const HANDLE pipe = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                    nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? Probe::Absent : Probe::Unreachable;
    if (!socket.setSocketDescriptor(reinterpret_cast<qintptr>(pipe))) {
        CloseHandle(pipe);
        return Probe::Unreachable;
    }
    return Probe::Connected;
#else
    socket.connectToServer(name);
    if (socket.state() == QLocalSocket::ConnectedState || socket.waitForConnected(100))
        return Probe::Connected;
    return socket.error() == QLocalSocket::ServerNotFoundError || socket.error() == QLocalSocket::ConnectionRefusedError
               ? Probe::Absent
               : Probe::Unreachable;
#endif
}

enum class Sent { Taken, NotTaken, NotOurs };

Sent sendRequest(QLocalSocket &socket, const QStringList &files)
{
    const QByteArray message = instanceRequest::encode(files);
    if (message.isEmpty()) {
        qWarning("Cannot forward these file paths to Visual Notepad.");
        return Sent::NotTaken;
    }
#ifdef Q_OS_WIN
    if (!serverIsOurs(socket)) {
        qWarning("The Visual Notepad channel's name is taken by another user.");
        return Sent::NotOurs;
    }
    // The shell may have granted this launch permission to take focus.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    if (socket.write(message) != message.size())
        return Sent::NotTaken;
    QDeadlineTimer deadline(timeoutMs);
    while (socket.bytesToWrite() > 0) {
        if (!socket.waitForBytesWritten(int(deadline.remainingTime())) && socket.bytesToWrite() > 0)
            return Sent::NotTaken;
    }
    // Only an answer says the files are on their way: a copy that is stuck,
    // quitting or too busy to read takes the bytes and drops them.
    while (socket.bytesAvailable() < 1) {
        if (!socket.waitForReadyRead(int(deadline.remainingTime())))
            return Sent::NotTaken;
    }
    char answer = 0;
    const bool ok = socket.read(&answer, 1) == 1 && answer == received;
    socket.disconnectFromServer();
    return ok ? Sent::Taken : Sent::NotTaken;
}

} // namespace

bool instanceRequest::isFresh(qint64 sent, qint64 now)
{
    // Well inside the time the launch waits for its answer (timeoutMs from
    // before it sent): one older has been given up on, and its files opened
    // by the launch itself.
    return qAbs(now - sent) < timeoutMs - 1000;
}

QByteArray instanceRequest::encode(const QStringList &files, qint64 sent)
{
    QByteArray message = magic.toByteArray();
    message.resize(headerSize);
    qToBigEndian<qint64>(sent < 0 ? QDateTime::currentMSecsSinceEpoch() : sent, message.data() + sentAt);
    for (const QString &path : files) {
        if (path.size() >= maxPayload || !isAbsoluteFilePath(path) || !path.isValidUtf16())
            return {};
        const QByteArray bytes = path.toUtf8();
        if (message.size() - headerSize + bytes.size() + 1 > maxPayload)
            return {};
        message.append(bytes);
        message.append('\0');
    }
    qToBigEndian<quint32>(quint32(message.size() - headerSize), message.data() + sizeAt);
    return message;
}

std::optional<QStringList> instanceRequest::decode(QByteArrayView message, qint64 *sent)
{
    if (const qsizetype size = messageSize(message); size == 0 || size != message.size())
        return std::nullopt;
    if (sent)
        *sent = qFromBigEndian<qint64>(message.data() + sentAt);
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
    , m_lock(lockPath())
{
    m_lock.setStaleLockTime(0);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &InstanceChannel::acceptConnections);
    // A copy on its way out takes no more files: launches from now on get no
    // answer and start on their own.
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { m_server.close(); });
}

InstanceChannel::StartResult InstanceChannel::start(const QStringList &files)
{
    QElapsedTimer elapsed;
    elapsed.start();
    // Another copy has the lock but didn't take the request: run on our own,
    // without the lock, so that it holds back no launch after this one.
    const auto failed = [this] {
        if (m_lock.isLocked())
            m_lock.unlock();
        return StartResult::Failed;
    };
    for (;;) {
        if (!m_name.isEmpty()) {
            QLocalSocket socket;
            const Probe probe = connectTo(socket, m_name);
            if (probe == Probe::Connected) {
                switch (sendRequest(socket, files)) {
                case Sent::Taken:
                    return StartResult::Forwarded;
                case Sent::NotTaken:
                    qWarning("Could not send the launch request to Visual Notepad.");
                    return failed();
                case Sent::NotOurs:
                    // As if no copy ran (none of ours does on that name): the
                    // lock decides who has the session, without a channel.
                    break;
                }
            } else if (probe == Probe::Absent && m_lock.isLocked()) {
                // Hold the election lock before probing again and removing a
                // stale socket. A slow or concurrent primary keeps its lock.
                QLocalServer::removeServer(m_name);
                if (m_server.listen(m_name))
                    return StartResult::Listening;
                qWarning() << m_server.errorString();
            }
        }
        // The lock is ours but there is no channel to offer: the session is
        // still ours alone, and later launches run on their own.
        if (m_lock.isLocked())
            return StartResult::Alone;
        if (m_lock.tryLock())
            continue;
        // (Without a channel name there is no copy to wait for.)
        if (m_name.isEmpty() || m_lock.error() != QLockFile::LockFailedError || elapsed.hasExpired(timeoutMs)) {
            qWarning("Could not reach or start the Visual Notepad instance.");
            return failed();
        }
        QThread::msleep(20);
    }
}

void InstanceChannel::release()
{
    m_server.close();
    if (m_lock.isLocked())
        m_lock.unlock();
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
            qint64 sent = 0;
            auto files = instanceRequest::decode(socket->read(size), &sent);
            // Read too late (this copy was busy): the launch has given up
            // and opens the files itself; opening them here too would make two.
            if (files && !instanceRequest::isFresh(sent, QDateTime::currentMSecsSinceEpoch()))
                files.reset();
            // Answered before the files open: opening can show a dialog,
            // which would keep the launch waiting past its timeout.
            // (Nor opened if the answer cannot even be written.)
            if (files && socket->write(&received, 1) != 1)
                files.reset();
            if (files)
                socket->flush();
            socket->disconnectFromServer(); // (After the answer is written; then deleted.)
            if (files)
                Q_EMIT requested(*files);
        };
        connect(socket, &QLocalSocket::readyRead, this, read);
        read();
    }
}

} // namespace vn
