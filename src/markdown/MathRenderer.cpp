#include "MathRenderer.h"

#include "../Settings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtMath>
#include <QtGlobal>

#include <atomic>
#include <climits>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#include <sys/wait.h>
#endif

#include "core/formula.h"
#include "core/macro.h"
#include "latex.h"
#include "platform/qt/graphic_qt.h"
#include "render.h"

#ifdef Q_OS_WIN
// After MicroTeX's headers: the names Windows' headers define as macros
// ("small", and "min" and "max" in <windows.h> itself) are names in them.
#include <qt_windows.h>
#endif

// A process of its own for every formula, where a running process can be
// copied (see formulasAreApart). VN_MATH_SHARED_WORKER builds the other way
// on Linux too, to test it there.
#if defined(Q_OS_LINUX) && !defined(VN_MATH_SHARED_WORKER)
#define VN_MATH_FORK
#endif

namespace vn::math {

namespace {

// The most characters of a formula that are shown as source text.
constexpr qsizetype maxSourceChars = 300;

// A formula longer than this is shown as source text: none that a person
// writes comes near, and MicroTeX's time grows faster than the length.
constexpr qsizetype maxTexChars = 20'000;

// The largest formula, a side in pixels at layoutPixelSize (500 em: some
// 8,000 pixels at the usual size); "\rule{30000pt}{30000pt}" is shown as its
// source. And the most shapes, path elements (a glyph has 20 to 80) and
// bytes an answer may have: drawing it is done on every paint.
constexpr qreal maxExtent = 500 * layoutPixelSize;
constexpr qsizetype maxShapes = 20'000;
constexpr qsizetype maxElements = 400'000;
constexpr quint32 maxAnswerBytes = 8 * 1024 * 1024;

// The largest picture toImage() makes, in device pixels: a side, and both
// together (64 MB of pixels).
constexpr int maxImageSide = 16'384;
constexpr qint64 maxImagePixels = 16 * 1024 * 1024;

// Shapes in this colour are in the colour of the text around the formula;
// MicroTeX draws in it unless the formula names a colour.
constexpr QRgb textColorMark = 0xFF010203;

// How long the worker may take to start (the first formula loads Qt, the
// fonts and MicroTeX's tables), and to draw one formula; an ordinary formula
// takes 20 ms. Every formula that ran out of time halves the time the next
// may take, down to the least: a page full of them costs seconds, not
// minutes.
constexpr int workerStartMs = 20'000;
constexpr int formulaMs = 3000;
constexpr int leastFormulaMs = 250;

// After this many formulas in a row that the worker could not draw (it
// crashed or ran out of time), none is tried for a while.
constexpr int maxFailuresInARow = 8;
constexpr int pauseMs = 30'000;

// The worker ends itself if one formula keeps it busy this long: the app
// that would have killed it may be gone.
constexpr int workerWatchdogMs = 30'000;

// The most memory the worker may have.
constexpr quint64 workerMemoryBytes = quint64(2) * 1024 * 1024 * 1024;

// First bytes of every answer of the worker ("VNM3").
constexpr quint32 answerMagic = 0x564E4D33;

// Where a picture keeps its baseline row (see baselineRow).
constexpr QStringView baselineKey = u"vn-baseline";

void setBaselineRow(QImage &image, qreal row)
{
    image.setText(baselineKey.toString(), QString::number(qBound(0, qRound(row), image.height())));
}

// --- Shapes -----------------------------------------------------------------

// A paint engine that keeps what is painted as filled shapes: text as the
// outlines of its glyphs (QPaintEngine's own drawTextItem() fills them as
// paths), lines and outlines as the area their pen covers. Pictures are
// dropped (formulas have none).
class ShapeEngine : public QPaintEngine
{
public:
    explicit ShapeEngine(Formula &out)
        : QPaintEngine(QPaintEngine::AllFeatures)
        , m_out(out)
    {
    }

    bool begin(QPaintDevice *) override { return true; }
    bool end() override { return true; }
    Type type() const override { return QPaintEngine::User; }

    void updateState(const QPaintEngineState &state) override
    {
        const DirtyFlags dirty = state.state();
        if (dirty & DirtyTransform)
            m_transform = state.transform();
        if (dirty & DirtyPen)
            m_pen = state.pen();
        if (dirty & DirtyBrush)
            m_brush = state.brush();
    }

    void drawPath(const QPainterPath &path) override
    {
        if (m_brush.style() != Qt::NoBrush)
            add(m_transform.map(path), m_brush.color());
        stroke(path);
    }

    void drawPolygon(const QPointF *points, int count, PolygonDrawMode mode) override
    {
        if (count < 1)
            return;
        QPainterPath path(points[0]);
        for (int i = 1; i < count; ++i)
            path.lineTo(points[i]);
        if (mode != PolylineMode) {
            path.closeSubpath();
            path.setFillRule(mode == OddEvenMode ? Qt::OddEvenFill : Qt::WindingFill);
            if (m_brush.style() != Qt::NoBrush)
                add(m_transform.map(path), m_brush.color());
        }
        stroke(path);
    }

    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {}

    bool full() const { return m_full; }

private:
    void stroke(const QPainterPath &path)
    {
        if (m_pen.style() == Qt::NoPen)
            return;
        QPainterPathStroker stroker(m_pen);
        QPainterPath area;
        if (m_pen.isCosmetic()) { // Its width is in pixels, after the transform.
            stroker.setWidth(qMax<qreal>(1, m_pen.widthF()));
            area = stroker.createStroke(m_transform.map(path));
        } else {
            area = m_transform.map(stroker.createStroke(path));
        }
        area.setFillRule(Qt::WindingFill);
        add(area, m_pen.color());
    }

    void add(const QPainterPath &path, const QColor &color)
    {
        if (path.isEmpty() || m_full)
            return;
        m_elements += path.elementCount();
        if (m_out.shapes.size() >= maxShapes || m_elements > maxElements) {
            m_full = true;
            return;
        }
        const QRgb rgba = color.rgba();
        m_out.shapes.append({path, rgba, rgba == textColorMark});
    }

    Formula &m_out;
    QTransform m_transform;
    QPen m_pen;
    QBrush m_brush;
    qsizetype m_elements = 0;
    bool m_full = false;
};

// What a painter paints on, at 72 DPI: a point is a pixel, as MicroTeX's
// sizes are meant.
class ShapeRecorder : public QPaintDevice
{
public:
    explicit ShapeRecorder(Formula &out)
        : m_engine(out)
    {
    }

    QPaintEngine *paintEngine() const override { return &m_engine; }
    bool full() const { return m_engine.full(); }

protected:
    int metric(PaintDeviceMetric metric) const override
    {
        constexpr int side = 1 << 20;
        switch (metric) {
        case PdmWidth:
        case PdmHeight:
            return side;
        case PdmWidthMM:
        case PdmHeightMM:
            return int(side * 25.4 / 72);
        case PdmNumColors:
            return INT_MAX;
        case PdmDepth:
            return 32;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 72;
        case PdmDevicePixelRatio:
            return 1;
        default:
            return QPaintDevice::metric(metric);
        }
    }

private:
    mutable ShapeEngine m_engine;
};

// Whether a formula's numbers are ones the app can lay out and draw. One
// without shapes is shown as its source: MicroTeX makes one of a formula it
// cannot read ("\frac{", while it is being typed) as of "\phantom{x}".
bool plausible(const Formula &formula)
{
    const auto within = [](qreal v) { return std::isfinite(v) && v >= 0 && v <= maxExtent; };
    return within(formula.width) && within(formula.ascent) && within(formula.depth) && formula.width > 0
           && formula.height() > 0 && !formula.shapes.isEmpty() && formula.shapes.size() <= maxShapes
           && formula.elements() <= maxElements;
}

// A formula as the worker sends it: its size, then each shape's colour and
// path elements (type, x, y). QDataStream, single-precision numbers.
QByteArray encode(const Formula &formula)
{
    QByteArray out;
    QDataStream stream(&out, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << float(formula.width) << float(formula.ascent) << float(formula.depth) << quint32(formula.shapes.size());
    for (const Formula::Shape &shape : formula.shapes) {
        const QPainterPath &path = shape.path;
        stream << quint32(shape.color) << quint8(shape.textColor) << quint8(path.fillRule() == Qt::OddEvenFill)
               << quint32(path.elementCount());
        for (int i = 0; i < path.elementCount(); ++i) {
            const QPainterPath::Element e = path.elementAt(i);
            stream << quint8(e.type) << float(e.x) << float(e.y);
        }
    }
    return out;
}

// The formula in an answer, or false: the answer is not one encode() makes,
// within the limits. Answers come from a process a formula may have taken
// over, so every number is checked.
bool decode(const QByteArray &answer, Formula &formula)
{
    QDataStream stream(answer);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    float width = 0, ascent = 0, depth = 0;
    quint32 shapes = 0;
    stream >> width >> ascent >> depth >> shapes;
    if (stream.status() != QDataStream::Ok || shapes == 0 || shapes > quint32(maxShapes))
        return false;
    formula = Formula();
    formula.width = width;
    formula.ascent = ascent;
    formula.depth = depth;
    // Shapes reach past the formula's box (a slanted glyph), not far.
    const qreal reach = 2 * maxExtent;
    qsizetype total = 0;
    for (quint32 s = 0; s < shapes; ++s) {
        quint32 rgba = 0, count = 0;
        quint8 textColor = 0, oddEven = 0;
        stream >> rgba >> textColor >> oddEven >> count;
        // Each element takes nine bytes: no more than there are.
        if (stream.status() != QDataStream::Ok || textColor > 1 || oddEven > 1 || count == 0
            || count > quint32(maxElements - total) || qint64(count) * 9 > stream.device()->bytesAvailable())
            return false;
        total += count;
        QPainterPath path;
        path.setFillRule(oddEven ? Qt::OddEvenFill : Qt::WindingFill);
        QPointF curve[3];
        int curveAt = -1; // Points of a curve read so far, or -1 outside one.
        for (quint32 i = 0; i < count; ++i) {
            quint8 type = 0;
            float x = 0, y = 0;
            stream >> type >> x >> y;
            if (stream.status() != QDataStream::Ok || !std::isfinite(x) || !std::isfinite(y) || qAbs(x) > reach
                || qAbs(y) > reach)
                return false;
            const QPointF p(x, y);
            // A path starts with a move; a curve is its first control point
            // and two data elements (the second control point, the end).
            if (i == 0 && type != QPainterPath::MoveToElement)
                return false;
            if ((curveAt >= 0) != (type == QPainterPath::CurveToDataElement))
                return false;
            switch (type) {
            case QPainterPath::MoveToElement:
                path.moveTo(p);
                break;
            case QPainterPath::LineToElement:
                path.lineTo(p);
                break;
            case QPainterPath::CurveToElement:
                curve[0] = p;
                curveAt = 1;
                break;
            case QPainterPath::CurveToDataElement:
                curve[curveAt++] = p;
                if (curveAt == 3) {
                    path.cubicTo(curve[0], curve[1], curve[2]);
                    curveAt = -1;
                }
                break;
            default:
                return false;
            }
        }
        if (curveAt >= 0)
            return false;
        formula.shapes.append({path, rgba, textColor != 0});
    }
    return stream.atEnd() && plausible(formula);
}

// The formula's source, in an italic serif face: what is shown for one
// MicroTeX cannot draw.
Formula sourceFormula(const QString &tex)
{
    QFont font(QStringLiteral("serif"));
    font.setStyleHint(QFont::Serif);
    font.setItalic(true);
    font.setPixelSize(qRound(layoutPixelSize));
    font.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(font);
    // The start of a long one: its picture would be as wide as it is long.
    QString text = tex.left(4 * maxSourceChars).simplified();
    if (text.size() > maxSourceChars)
        text = text.left(maxSourceChars) + QChar(0x2026);
    Formula formula;
    formula.fellBack = true;
    const qreal pad = layoutPixelSize / 8;
    formula.width = qMin(fm.horizontalAdvance(text) + 2 * pad, maxExtent);
    formula.ascent = fm.ascent();
    formula.depth = fm.descent();
    ShapeRecorder recorder(formula);
    QPainter painter(&recorder);
    painter.setFont(font);
    painter.setPen(QColor::fromRgba(textColorMark));
    painter.drawText(QPointF(pad, 0), text);
    painter.end();
    if (formula.shapes.isEmpty()) // Only spaces, or no font: a box keeps its place.
        formula.shapes.append({QPainterPath(), textColorMark, true});
    return formula;
}

// MicroTeX lays glyphs out in 1pt units and draws them through a painter that
// is scaled by the text size. Rasterizing 1pt fonts under such a transform
// makes FreeType overflow and drop glyphs, so draw text at its final size
// under an unscaled painter instead.
class Graphics2D : public tex::Graphics2D_qt
{
public:
    using tex::Graphics2D_qt::Graphics2D_qt;

    void drawText(const std::wstring &text, float x, float y) override
    {
        const auto *font = static_cast<const tex::Font_qt *>(getFont());
        const qreal scale = sx();
        if (!font || scale <= 0)
            return;
        QFont qfont = font->getQFont();
        qfont.setPointSizeF(qfont.pointSizeF() * scale);
        // The outlines as drawn, to be scaled: not fitted to one size's pixels.
        qfont.setHintingPreference(QFont::PreferNoHinting);
        QPainter *painter = getQPainter();
        painter->save();
        painter->scale(1 / scale, 1 / scale);
        painter->setFont(qfont);
        painter->drawText(QPointF(x * scale, y * scale), tex::wstring_to_QString(text));
        painter->restore();
    }

    void drawChar(wchar_t c, float x, float y) override { drawText(std::wstring(1, c), x, y); }
};

QString findResources()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        app + QStringLiteral("/microtex/res"),
        app + QStringLiteral("/../microtex/res"),
        app + QStringLiteral("/../share/visnotepad/microtex/res"),
        // A macOS bundle: the executable is in Contents/MacOS.
        app + QStringLiteral("/../Resources/microtex/res"),
    };
#ifdef VN_MICROTEX_RES
    candidates << QStringLiteral(VN_MICROTEX_RES);
#endif
    for (const QString &dir : std::as_const(candidates)) {
        if (QFile::exists(dir + QStringLiteral("/.clatexmath-res_root")))
            return QDir::cleanPath(dir);
    }
    return {};
}

// Initializes MicroTeX once; a failure sticks and selects the text fallback.
bool ensureInitialized()
{
    static int state = 0; // 0 = not tried, 1 = ready, -1 = unavailable
    if (state != 0)
        return state > 0;
    state = -1;
    const QString res = findResources();
    if (res.isEmpty()) {
        qWarning("MicroTeX resources not found; math is shown as source text");
        return false;
    }
    try {
        tex::LaTeX::init(QFile::encodeName(res).toStdString());
        // Settings macros are prepended to every formula; let them redefine.
        tex::NewCommandMacro::_errIfConflict = false;
        state = 1;
    } catch (const std::exception &e) {
        qWarning("MicroTeX initialization failed: %s", e.what());
    }
    return state > 0;
}

// The formula's shapes as MicroTeX draws it, or false: it is not TeX
// MicroTeX reads, or too large.
bool microTeXFormula(const QString &tex, bool display, Formula &out)
{
    constexpr int layoutWidth = 20000; // formulas never wrap (wider: not drawn)
    // MicroTeX takes a point (pt, and cm and the rest through it) to be a
    // pixel at the size it lays a formula out at: laid out at the size most
    // formulas are shown at, a "\rule{10pt}{1pt}" is as long beside the text
    // as it was when each was laid out at its own size, at every zoom.
    constexpr float unitSize = 18;
    // The layout scales to any size; drawn at layoutPixelSize, and measured
    // this many times larger (MicroTeX gives sizes in whole pixels).
    constexpr float fine = 16;
    if (!ensureInitialized())
        return false;
    try {
        tex::Formula formula(tex.toStdWString());
        tex::TeXRenderBuilder builder;
        const auto size = static_cast<float>(layoutPixelSize);
        std::unique_ptr<tex::TeXRender> render(
            builder.setStyle(display ? tex::TexStyle::display : tex::TexStyle::text)
                .setTextSize(unitSize)
                .setWidth(tex::UnitType::pixel, layoutWidth, tex::Alignment::left)
                .setIsMaxWidth(true)
                .setLineSpace(tex::UnitType::pixel, 0)
                .setForeground(static_cast<tex::color>(textColorMark))
                .build(formula));
        render->setInsets(tex::Insets(), true); // No margin: the app adds its own.
        // (Too large for an int at the finer size: not drawn.)
        const qreal largest = maxExtent * unitSize / size;
        if (!(render->getWidth() <= largest) || !(render->getHeight() <= largest))
            return false;
        render->setTextSize(size * fine);
        Formula result;
        result.width = render->getWidth() / fine;
        // The depth below the baseline is part of the height.
        result.depth = render->getDepth() / fine;
        result.ascent = render->getHeight() / fine - result.depth;
        // A symbol above the baseline only ("\to", "\cdot") has a depth
        // below none; one under it, an ascent below none. The shapes keep
        // their places; the box reaches the baseline.
        result.ascent = std::max<qreal>(0, result.ascent);
        result.depth = std::max<qreal>(0, result.depth);
        render->setTextSize(size);
        if (!(result.width > 0) || !(result.height() > 0)) // Not a number either.
            return false;

        // draw() puts the baseline `ascent` below the top it is given;
        // the shapes have it at 0.
        ShapeRecorder recorder(result);
        QPainter painter(&recorder);
        painter.translate(0, -result.ascent);
        Graphics2D g2(&painter);
        render->draw(g2, 0, 0);
        painter.end();
        if (recorder.full() || !plausible(result))
            return false;
        out = std::move(result);
        return true;
    } catch (const std::exception &e) {
        qWarning("MicroTeX failed on \"%s\": %s", qPrintable(tex.left(60)), e.what());
    }
    return false;
}

// --- The worker's side -----------------------------------------------------

// Standard input and output of the worker, without the C library's or Qt's
// buffers and text modes in between.
#ifdef Q_OS_WIN
HANDLE workerIn = INVALID_HANDLE_VALUE;
HANDLE workerOut = INVALID_HANDLE_VALUE;

bool readFully(char *to, qint64 size)
{
    while (size > 0) {
        DWORD got = 0;
        if (!ReadFile(workerIn, to, DWORD(qMin<qint64>(size, 1 << 20)), &got, nullptr) || got == 0)
            return false;
        to += got;
        size -= got;
    }
    return true;
}

bool writeFully(const char *from, qint64 size)
{
    while (size > 0) {
        DWORD put = 0;
        if (!WriteFile(workerOut, from, DWORD(qMin<qint64>(size, 1 << 20)), &put, nullptr) || put == 0)
            return false;
        from += put;
        size -= put;
    }
    return true;
}

// A fault ends the worker at once and without a word: no "has stopped
// working" window for a formula.
LONG WINAPI endOnFault(EXCEPTION_POINTERS *)
{
    TerminateProcess(GetCurrentProcess(), 4);
    return EXCEPTION_EXECUTE_HANDLER;
}

void confineWorker()
{
    workerIn = GetStdHandle(STD_INPUT_HANDLE);
    workerOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(endOnFault);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // A job of its own holds the memory limit.
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = SIZE_T(workerMemoryBytes);
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits))
            AssignProcessToJobObject(job, GetCurrentProcess());
        // The handle stays open for as long as the worker runs.
    }
}

[[noreturn]] void endWorker(int code)
{
    TerminateProcess(GetCurrentProcess(), UINT(code));
    std::_Exit(code);
}
#else
int workerIn = 0;
int workerOut = 1;

bool readFully(char *to, qint64 size)
{
    while (size > 0) {
        const ssize_t got = ::read(workerIn, to, size_t(size));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return false;
        to += got;
        size -= got;
    }
    return true;
}

bool writeFully(const char *from, qint64 size)
{
    while (size > 0) {
        const ssize_t put = ::write(workerOut, from, size_t(size));
        if (put < 0 && errno == EINTR)
            continue;
        if (put <= 0)
            return false;
        from += put;
        size -= put;
    }
    return true;
}

// A fault ends the worker at once and without a word: no crash report and
// no core file for a formula.
void endOnFault(int)
{
    _exit(4);
}

void confineWorker()
{
    // The answers go out through a descriptor of their own; whatever else
    // prints to standard output joins standard error.
    workerOut = ::dup(1);
    ::dup2(2, 1);

    // On a stack of its own: the fault may be that the stack ran out.
    static char faultStack[64 * 1024];
    stack_t stack = {};
    stack.ss_sp = faultStack;
    stack.ss_size = sizeof faultStack;
    sigaltstack(&stack, nullptr);
    struct sigaction action = {};
    action.sa_handler = endOnFault;
    action.sa_flags = SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        sigaction(signal, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN); // The app went away: write() says so.

    const rlimit noCore = {0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
#if !defined(__SANITIZE_ADDRESS__) && !defined(Q_OS_MACOS)
    // (An address sanitizer maps terabytes; macOS does not hold a process
    // to this limit.)
    const rlimit memory = {rlim_t(workerMemoryBytes), rlim_t(workerMemoryBytes)};
    setrlimit(RLIMIT_AS, &memory);
#endif
#ifdef Q_OS_LINUX
    prctl(PR_SET_PDEATHSIG, SIGKILL); // Ends with the app, even if it is busy.
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#endif
}

[[noreturn]] void endWorker(int code)
{
    _exit(code);
}
#endif

// An answer: the magic, what became of the formula (answerShapes and the
// size of encode()'s bytes, which follow; answerNotTeX; answerCrashed) and
// the size. Written in one piece, once the formula is done.
constexpr qint32 answerShapes = 1;
constexpr qint32 answerNotTeX = 0;
constexpr qint32 answerCrashed = -1;

bool writeAnswer(qint32 status, const QByteArray &shapes = QByteArray())
{
    QByteArray answer(3 * sizeof(qint32), Qt::Uninitialized);
    const qint32 header[3] = {qint32(answerMagic), status, qint32(shapes.size())};
    std::memcpy(answer.data(), header, sizeof header);
    answer += shapes;
    return writeFully(answer.constData(), answer.size());
}

bool answer(const QString &tex, bool display)
{
    Formula formula;
    if (!microTeXFormula(tex, display, formula))
        return writeAnswer(answerNotTeX);
    const QByteArray shapes = encode(formula);
    return shapes.size() <= qsizetype(maxAnswerBytes) ? writeAnswer(answerShapes, shapes) : writeAnswer(answerNotTeX);
}

// --- The app's side --------------------------------------------------------

QString &workerProgram()
{
    static QString program;
    return program;
}

bool fellBack = false;
int starts = 0;

// MicroTeX keeps what a formula defines or sets (commands, colours, sizes,
// column types, fonts) for every formula after it: a document could change
// how the formulas of another are drawn, or stop them being drawn. On Linux
// every formula is drawn by a copy of the worker made for it, so nothing is
// kept. Elsewhere (no fork() on Windows; on macOS the system's text drawing
// does not work in a copy) one worker draws formula after formula, and is
// replaced after a formula that names one of the commands known to leave
// something behind. (Not after every formula MicroTeX refuses: one being
// typed is refused at most keystrokes.)
#ifdef VN_MATH_FORK
constexpr bool formulasAreApart = true;
#else
constexpr bool formulasAreApart = false;
#endif

bool changesState(const QString &tex)
{
    static const QRegularExpression commands(QStringLiteral(
        "\\\\(newcommand|renewcommand|newenvironment|renewenvironment|Declare[A-Za-z]*|magnification|definecolor"
        "|newcolumntype|cornersize|fatalIfCmdConflict|breakEverywhere|debug|undebug|jlm[A-Za-z]*|makeat[a-z]*)"));
    // The user's own macros, which start every formula, are defined again
    // each time.
    const QString own = withMacros(QString());
    return commands.matchView(tex.startsWith(own) ? QStringView(tex).mid(own.size()) : QStringView(tex)).hasMatch();
}

class Worker
{
public:
    static Worker &instance()
    {
        static Worker worker;
        return worker;
    }

    // The shapes of a formula, or false: it is not TeX MicroTeX reads, or
    // the worker failed on it.
    bool layout(const QString &tex, bool display, Formula &formula)
    {
        if (m_unavailable || tex.size() > maxTexChars)
            return false;
        QByteArray request;
        {
            QDataStream stream(&request, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_0);
            stream << tex.toUtf8() << display;
        }
        // A request the worker failed on is not sent again: it would fail
        // again, at the same price.
        const QByteArray key = QCryptographicHash::hash(request, QCryptographicHash::Sha1);
        if (m_refused.contains(key))
            return false;
        if (m_failuresInARow >= maxFailuresInARow) {
            if (m_paused.isValid() && m_paused.elapsed() < pauseMs)
                return false;
            m_failuresInARow = 0;
        }
        if (!start())
            return false;

        const int allowed = qMax(leastFormulaMs, formulaMs >> qMin(m_timeouts, 16));
        const QDeadlineTimer deadline(allowed);
        QElapsedTimer spent;
        spent.start();
        const quint32 size = quint32(request.size());
        bool drawn = false;
        bool answered = false;
        bool crashed = false;
        // Where formulas share one worker, one that may leave something
        // behind in it is its last.
        const bool usedUp = !formulasAreApart && changesState(tex);
        m_process->write(reinterpret_cast<const char *>(&size), sizeof size);
        m_process->write(request);
        if (flush(deadline)) {
            qint32 header[3] = {};
            if (read(reinterpret_cast<char *>(header), sizeof header, deadline) && quint32(header[0]) == answerMagic) {
                const qint32 status = header[1];
                const qint64 bytes = header[2];
                if (status == answerNotTeX && bytes == 0) {
                    answered = true;
                } else if (status == answerCrashed && bytes == 0) {
                    crashed = true; // And the worker lives on.
                } else if (status == answerShapes && bytes > 0 && bytes <= maxAnswerBytes) {
                    QByteArray shapes(bytes, Qt::Uninitialized);
                    answered = read(shapes.data(), bytes, deadline) && decode(shapes, formula);
                    drawn = answered;
                }
            }
        }
        if (!answered) {
            // Crashed, too slow, or not answering as agreed: whatever state
            // it is in, the next formula gets a new one.
            if (!crashed) {
                // Not deadline.hasExpired(): a wait can end a little before
                // its deadline (Windows wakes waiting threads every 15.6 ms).
                if (spent.elapsed() >= allowed - 50)
                    ++m_timeouts;
                stop();
            }
            if (m_refused.size() < 10'000)
                m_refused.insert(key);
            if (++m_failuresInARow >= maxFailuresInARow)
                m_paused.start();
            return false;
        }
        m_failuresInARow = 0;
        if (usedUp)
            stop();
        return drawn;
    }

    void stop()
    {
        if (!m_process)
            return;
        QProcess *process = m_process;
        m_process = nullptr;
        process->disconnect();
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
        delete process;
    }

private:
    bool start()
    {
        if (m_process && m_process->state() == QProcess::Running)
            return true;
        stop();
        QString program = workerProgram();
        if (program.isEmpty())
            program = qEnvironmentVariable("VN_MATH_WORKER");
        if (program.isEmpty() || !QCoreApplication::instance()) {
            m_unavailable = true;
            return false;
        }
        // Owned by the application: it ends before the application does.
        m_process = new QProcess(QCoreApplication::instance());
        m_process->setProgram(program);
        m_process->setArguments({QStringLiteral("--math-worker")});
        m_process->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_UNIX
        // The worker shows nothing, so it gets no connection to the display
        // (which could watch the keyboard and the screen) and, on macOS, no
        // Dock icon. Not on Windows: Qt's offscreen platform has no fonts
        // there, and a program without windows shows nowhere.
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        m_process->setProcessEnvironment(environment);
#endif
#ifdef Q_OS_UNIX
        // The worker gets the pipes and nothing else the app has open
        // (documents, connections).
        m_process->setUnixProcessParameters(QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
        if (starts++ == 0) {
            // Ended with the application, before it deletes the process.
            qAddPostRoutine([] { Worker::instance().stop(); });
        }
        m_process->start();
        qint32 hello[2] = {};
        const QDeadlineTimer deadline(workerStartMs);
        if (!m_process->waitForStarted(workerStartMs) || !read(reinterpret_cast<char *>(hello), sizeof hello, deadline)
            || quint32(hello[0]) != answerMagic) {
            // Not there, not a worker, or the machine is too busy: not
            // tried for every formula, but again after a while.
            stop();
            m_failuresInARow = maxFailuresInARow;
            m_paused.start();
            return false;
        }
        if (hello[1] != 1) { // It runs, but MicroTeX has no resources.
            stop();
            m_unavailable = true;
            return false;
        }
        return true;
    }

    bool flush(const QDeadlineTimer &deadline)
    {
        while (m_process->bytesToWrite() > 0) {
            if (m_process->state() != QProcess::Running || deadline.hasExpired()
                || !m_process->waitForBytesWritten(qMax(1, int(deadline.remainingTime()))))
                return false;
        }
        return true;
    }

    bool read(char *to, qint64 size, const QDeadlineTimer &deadline)
    {
        while (size > 0) {
            if (m_process->bytesAvailable() == 0) {
                if (deadline.hasExpired() || !m_process->waitForReadyRead(qMax(1, int(deadline.remainingTime()))))
                    return false;
                continue;
            }
            const qint64 got = m_process->read(to, size);
            if (got <= 0)
                return false;
            to += got;
            size -= got;
        }
        return true;
    }

    QPointer<QProcess> m_process;
    bool m_unavailable = false; // No worker program, or it cannot draw at all.
    int m_timeouts = 0;
    int m_failuresInARow = 0;
    QElapsedTimer m_paused;
    QSet<QByteArray> m_refused;
};

} // namespace

QString withMacros(const QString &tex)
{
    const QVariantMap macros = Settings::instance().value(key::mathMacros).toMap();
    QString prefix;
    for (auto it = macros.cbegin(); it != macros.cend(); ++it) {
        const QString command = it.key().trimmed();
        if (command.isEmpty())
            continue;
        prefix += QStringLiteral("\\newcommand{%1}{%2}").arg(command, it.value().toString());
    }
    return prefix + tex;
}

qsizetype Formula::elements() const
{
    qsizetype count = 0;
    for (const Shape &shape : shapes)
        count += shape.path.elementCount();
    return count;
}

void Formula::draw(QPainter &painter, const QPointF &origin, qreal scale, const QColor &text) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.translate(origin);
    painter.scale(scale, scale);
    for (const Shape &shape : shapes) {
        if (!shape.path.isEmpty())
            painter.fillPath(shape.path, shape.textColor ? text : QColor::fromRgba(shape.color));
    }
    painter.restore();
}

std::shared_ptr<const Formula> layout(const QString &tex, bool display)
{
    auto formula = std::make_shared<Formula>();
    fellBack = !Worker::instance().layout(tex, display, *formula);
    if (fellBack)
        *formula = sourceFormula(tex);
    return formula;
}

QImage toImage(const Formula &formula, const Style &style)
{
    constexpr qreal padding = 1;
    qreal scale = style.pixelSize / layoutPixelSize;
    const qreal dpr = style.devicePixelRatio;
    // Smaller than asked rather than none at all.
    const qreal wide = (formula.width * scale + 2 * padding) * dpr;
    const qreal high = (formula.height() * scale + 2 * padding) * dpr;
    const qreal fit = std::min({1.0, maxImageSide / wide, maxImageSide / high, std::sqrt(maxImagePixels / (wide * high))});
    if (!(fit > 0))
        return {};
    scale *= fit;
    const QSize size(qMax(1, qCeil((formula.width * scale + 2 * padding) * dpr)),
                     qMax(1, qCeil((formula.height() * scale + 2 * padding) * dpr)));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    formula.draw(painter, QPointF(padding, padding + formula.ascent * scale), scale, style.color);
    painter.end();
    setBaselineRow(image, (padding + formula.ascent * scale) * dpr);
    return image;
}

QImage render(const QString &tex, const Style &style)
{
    const auto formula = layout(tex, style.display);
    const bool fell = fellBack;
    QImage image = toImage(*formula, style);
    if (image.isNull() && !fell)
        image = toImage(sourceFormula(tex), style);
    fellBack = fell || image.isNull();
    return image;
}

int baselineRow(const QImage &picture)
{
    bool ok = false;
    const int row = picture.text(baselineKey.toString()).toInt(&ok);
    return ok && row >= 0 && row <= picture.height() ? row : picture.height();
}

void setWorkerProgram(const QString &program)
{
    workerProgram() = program;
}

bool lastRenderFellBack()
{
    return fellBack;
}

int workerStarts()
{
    return starts;
}

void runWorker(int &argc, char **argv)
{
    // First: Qt's start is the worker's too.
    confineWorker();
    QGuiApplication application(argc, argv);

#ifndef VN_MATH_FORK
    // Milliseconds since `clock` started at which the formula being drawn
    // was begun, or 0 between formulas. (On Linux each formula has a
    // process and an alarm of its own, and no second thread is in the way
    // of fork().)
    static std::atomic<qint64> busySince{0};
    static QElapsedTimer clock;
    clock.start();
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const qint64 since = busySince.load();
            if (since != 0 && clock.elapsed() - since > workerWatchdogMs)
                endWorker(3);
        }
    }).detach();
#endif

    const qint32 hello[2] = {qint32(answerMagic), ensureInitialized() ? 1 : 0};
    if (!writeFully(reinterpret_cast<const char *>(hello), sizeof hello))
        endWorker(1);
    for (;;) {
        quint32 size = 0;
        if (!readFully(reinterpret_cast<char *>(&size), sizeof size))
            endWorker(0); // The app closed the pipe: done.
        if (size > 16 * 1024 * 1024)
            endWorker(2);
        QByteArray request(qsizetype(size), Qt::Uninitialized);
        if (!readFully(request.data(), size))
            endWorker(0);
        QByteArray utf8;
        bool display = false;
        QDataStream stream(request);
        stream.setVersion(QDataStream::Qt_6_0);
        stream >> utf8 >> display;
        if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
            if (!writeAnswer(answerNotTeX))
                endWorker(1);
            continue;
        }
        const QString tex = QString::fromUtf8(utf8);
#ifdef VN_MATH_FORK
        // A copy of this process draws the formula and ends: whatever the
        // formula does to MicroTeX, or to the process, ends with it.
        const pid_t copy = fork();
        if (copy == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            alarm(workerWatchdogMs / 1000);
            _exit(answer(tex, display) ? 0 : 1);
        }
        int status = 0;
        bool drawn = copy > 0;
        while (drawn && waitpid(copy, &status, 0) < 0)
            drawn = errno == EINTR;
        if (drawn && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            continue; // It has answered.
        if (!writeAnswer(answerCrashed))
            endWorker(1);
#else
        busySince.store(qMax<qint64>(1, clock.elapsed()));
        const bool answered = answer(tex, display);
        busySince.store(0);
        if (!answered)
            endWorker(1);
#endif
    }
}

} // namespace vn::math
