#include "InstanceChannel.h"

#include "Session.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QRandomGenerator>
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

constexpr QByteArrayView magic("VNOP\x01");
constexpr qsizetype headerSize = magic.size() + sizeof(quint32);
constexpr qsizetype maxPayload = 256 * 1024;
constexpr int timeoutMs = 5000;
constexpr int maxReaders = 16;
// What the running copy answers once a request is in its queue.
constexpr char received = 'K';

// A random key kept in the user's own settings folder, which other users
// cannot read: it makes the channel's name one they cannot guess, so they
// cannot take the name first and receive the paths this user opens. Made
// once, atomically, so that two first launches agree on it.
QByteArray instanceKey()
{
    const QString path = QFileInfo(Settings::instance().fileName()).dir().filePath(QStringLiteral("instance.key"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        QByteArray key(32, Qt::Uninitialized);
        QRandomGenerator::system()->generate(key.begin(), key.end());
        key = key.toHex();
        if (file.write(key) == key.size() && file.flush())
            return key;
        file.remove();
        return {};
    }
    // Made by another launch, which may still be writing it.
    QDeadlineTimer deadline(timeoutMs);
    do {
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray key = file.read(64);
            file.close();
            if (key.size() == 64)
                return key;
        }
        QThread::msleep(10);
    } while (!deadline.hasExpired());
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
    const QString name = QStringLiteral("visnotepad-") + QString::fromLatin1(hash.first(32));
#ifdef Q_OS_UNIX
    // A path in the user's runtime folder, which only they can enter (Qt
    // makes it so, or gives none), not a bare name: Qt would put that in the
    // shared temporary folder, where the socket's name shows and another user
    // could take it once this user's copy has quit.
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
    if (!serverIsOurs(socket)) {
        qWarning("The Visual Notepad channel belongs to another user.");
        return false;
    }
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
    // Only an answer says the files are on their way: a copy that is stuck,
    // quitting or too busy to read takes the bytes and drops them.
    while (socket.bytesAvailable() < 1) {
        if (!socket.waitForReadyRead(int(deadline.remainingTime())))
            return false;
    }
    char answer = 0;
    const bool ok = socket.read(&answer, 1) == 1 && answer == received;
    socket.disconnectFromServer();
    return ok;
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
            socket.connectToServer(m_name);
            if (socket.state() == QLocalSocket::ConnectedState || socket.waitForConnected(100)) {
                if (sendRequest(socket, files))
                    return StartResult::Forwarded;
                qWarning("Could not send the launch request to Visual Notepad.");
                return failed();
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
            }
        }
        // The lock is ours but there is no channel to offer: the session is
        // still ours alone, and later launches run on their own.
        if (m_lock.isLocked())
            return StartResult::Alone;
        if (m_lock.tryLock())
            continue;
        if (m_lock.error() != QLockFile::LockFailedError || elapsed.hasExpired(timeoutMs)) {
            qWarning("Could not reach or start the Visual Notepad instance.");
            return failed();
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
            auto files = instanceRequest::decode(socket->read(size));
            // Not now (a dialog asks, such as whether to save before a window
            // or the app closes): no answer, and the launch opens the files
            // on its own rather than in a window about to go.
            if (m_accepting && !m_accepting())
                files.reset();
            // Answered before the files open: opening can show a dialog,
            // which would keep the launch waiting past its timeout.
            if (files) {
                socket->write(&received, 1);
                socket->flush();
            }
            socket->disconnectFromServer(); // (After the answer is written; then deleted.)
            if (files)
                Q_EMIT requested(*files);
        };
        connect(socket, &QLocalSocket::readyRead, this, read);
        read();
    }
}

} // namespace vn
