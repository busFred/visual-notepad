// Stands in for "visnotepad --math-worker" (see MathRenderer.cpp) in
// test_math_answers: answers each formula as its text names, well or in one
// of the ways a worker a formula has taken over could.
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
        if (mode == QLatin1String("oversize")) {
            write(header(1, 9 * 1024 * 1024)); // And nothing after.
        } else if (mode == QLatin1String("wrong-status")) {
            write(header(7, 0));
        } else if (mode == QLatin1String("not-tex")) {
            write(header(0, 0));
        } else {
            const QByteArray body = shapes(mode);
            write(header(1, qint32(body.size())) + body);
        }
    }
}
