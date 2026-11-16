// Stands in for "visnotepad --math-worker" (see MathRenderer.cpp) in
// test_math_answers: answers each formula as its text names, well or in one
// of the ways a worker a formula has taken over could, or (Linux) is slow to
// end once killed, as a worker is on a busy machine.
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <unistd.h>
#endif

namespace {

constexpr qint32 magic = 0x564E4D33; // "VNM3"

bool readFully(char *to, size_t size)
{
    return std::fread(to, 1, size, stdin) == size;
}

void write(const QByteArray &bytes)
{
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    std::fflush(stdout);
}

QByteArray header(qint32 status, qint32 size)
{
    const qint32 words[3] = {magic, status, size};
    return QByteArray(reinterpret_cast<const char *>(words), sizeof words);
}

struct Element
{
    quint8 type;
    float x, y;
};

// One shape, a box 50 wide, 40 above the baseline and 10 below, unless the
// mode changes something.
QByteArray shapes(const QString &mode)
{
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_0);
    s.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float width = mode == QLatin1String("nan") ? nan : mode == QLatin1String("huge") ? 1e9f : 50.f;
    s << width << 40.f << 10.f << quint32(1);
    QList<Element> elements = {{0, 0, 10}, {1, 50, 10}, {1, 50, -40}, {1, 0, -40}, {1, 0, 10}};
    if (mode == QLatin1String("line-first"))
        elements[0].type = 1;
    else if (mode == QLatin1String("half-curve"))
        elements = {{0, 0, 0}, {2, 10, 10}, {3, 20, 10}, {1, 30, 0}};
    else if (mode == QLatin1String("bad-type"))
        elements[2].type = 9;
    else if (mode == QLatin1String("far-point"))
        elements[2].x = 1e9f;
    const quint32 count = mode == QLatin1String("claims-more") ? 1'000'000 : quint32(elements.size());
    s << quint32(0xFF010203) << quint8(1) << quint8(0) << count;
    for (const Element &e : elements)
        s << e.type << e.x << e.y;
    if (mode == QLatin1String("trailing"))
        s << quint8(0);
    return out;
}

// "slow-to-end:<file>": a helper process traces this one, and whoever
// started it cannot see it end, or clear it away, until the helper lets go:
// when <file> exists, or after a minute. (A killed process that the kernel
// takes seconds to end, on a busy machine, looks the same to the app.) Then
// an answer the app refuses, so that it kills this process. False if it
// could not be traced.
bool slowToEnd(const QString &mode)
{
#ifdef __linux__
    const QByteArray release = mode.section(QLatin1Char(':'), 1).toLocal8Bit();
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    const pid_t self = getpid();
    int ready[2];
    if (pipe(ready) != 0)
        return false;
    const pid_t helper = fork();
    if (helper < 0)
        return false;
    if (helper == 0) {
        // Holds no end of the pipes to the app.
        close(0);
        close(1);
        close(ready[0]);
        const char held = ptrace(PTRACE_SEIZE, self, nullptr, nullptr) == 0 ? 1 : 0;
        (void)!::write(ready[1], &held, 1);
        close(ready[1]);
        for (int i = 0; held && i < 1200 && access(release.constData(), F_OK) != 0; ++i)
            usleep(50'000);
        _exit(0); // Lets go of it.
    }
    close(ready[1]);
    char held = 0;
    const bool told = ::read(ready[0], &held, 1) == 1;
    close(ready[0]);
    if (!told || !held)
        return false;
    write(header(7, 0));
    for (;;)
        pause(); // Until killed.
#else
    Q_UNUSED(mode);
    return false;
#endif
}

} // namespace

int main()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const qint32 hello[2] = {magic, 1};
    write(QByteArray(reinterpret_cast<const char *>(hello), sizeof hello));
    for (;;) {
        quint32 size = 0;
        if (!readFully(reinterpret_cast<char *>(&size), sizeof size) || size > 1 << 20)
            return 0;
        QByteArray request(qsizetype(size), Qt::Uninitialized);
        if (!readFully(request.data(), size))
            return 0;
        QDataStream stream(request);
        stream.setVersion(QDataStream::Qt_6_0);
        QByteArray utf8;
        bool display = false;
        stream >> utf8 >> display;
        const QString mode = QString::fromUtf8(utf8);
        if (mode.startsWith(QLatin1String("slow-to-end:")) && slowToEnd(mode))
            continue; // (Never: it is killed.)
        if (mode == QLatin1String("oversize")) {
            write(header(1, 9 * 1024 * 1024)); // And nothing after.
        } else if (mode == QLatin1String("wrong-status")) {
            write(header(7, 0));
        } else if (mode == QLatin1String("not-tex")) {
            write(header(0, 0));
        } else {
            // Well formed (also where slow-to-end could not be had).
            const QByteArray body = shapes(mode);
            write(header(1, qint32(body.size())) + body);
        }
    }
}
