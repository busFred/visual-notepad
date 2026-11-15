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
#include <QLibraryInfo>
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

#include <algorithm>
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
#include <QStandardPaths>

#include <cstddef>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

#include <aclapi.h>
#include <objbase.h>
#include <psapi.h>
#include <sddl.h>
#include <userenv.h>
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

// MicroTeX's resources for the program in the folder `app`.
QString findResources(const QString &app = QCoreApplication::applicationDirPath())
{
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
        if (recorder.full())
            return false;
        // The box reaches the ink at both ends: a slanted letter there ("T",
        // "V", "f") reaches past its advance, and TeX adds that, the italic
        // correction (TeXbook, appendix G, rule 17), so the next word does
        // not start under it. MicroTeX leaves it out.
        QRectF ink;
        for (const Formula::Shape &shape : std::as_const(result.shapes))
            ink |= shape.path.boundingRect();
        if (ink.isValid()) {
            if (ink.left() < 0) {
                for (Formula::Shape &shape : result.shapes)
                    shape.path.translate(-ink.left(), 0);
                result.width -= ink.left();
                ink.translate(-ink.left(), 0);
            }
            result.width = std::max(result.width, ink.right());
        }
        if (!plausible(result))
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

// The limits of the worker's job: its memory, and with `locked`, no
// process but itself.
JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits(bool locked)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = SIZE_T(workerMemoryBytes);
    if (locked) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
    }
    return limits;
}

// The worker's job, made and joined on first use. Open until the lock-down
// closes it: the job and its limits stay as long as the worker is in it,
// and with no handle left nothing can change them.
HANDLE &workerJob()
{
    static HANDLE job = [] {
        HANDLE made = CreateJobObjectW(nullptr, nullptr);
        if (made) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = jobLimits(false);
            if (!SetInformationJobObject(made, JobObjectExtendedLimitInformation, &limits, sizeof limits)
                || !AssignProcessToJobObject(made, GetCurrentProcess())) {
                CloseHandle(made);
                made = nullptr;
            }
        }
        return made;
    }();
    return job;
}

void confineWorker()
{
    workerIn = GetStdHandle(STD_INPUT_HANDLE);
    workerOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(endOnFault);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // A job of its own holds the memory limit (and, once locked down, the
    // ban on new processes).
    workerJob();
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
// The last process confineWorkerProcess() was for started in the
// AppContainer, and so did the last worker.
bool confinedStart = false;
bool contained = false;

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

    // Starts the worker if none is running, without waiting for it to come up:
    // the first formula then finds it ready, or nearly, instead of waiting
    // the whole 125 to 150 ms for it.
    void prepare()
    {
        if (m_unavailable || m_process || workerPath().isEmpty() || !QCoreApplication::instance())
            return;
        if (m_failuresInARow >= maxFailuresInARow && m_paused.isValid() && m_paused.elapsed() < pauseMs)
            return;
        launch();
    }

    void stop()
    {
        m_awaitingHello = false;
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
    static QString workerPath()
    {
        const QString program = workerProgram();
        return program.isEmpty() ? qEnvironmentVariable("VN_MATH_WORKER") : program;
    }

    // Starts the process; its greeting is read by start().
    bool launch()
    {
        stop();
        const QString program = workerPath();
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
        confinedStart = false;
        if (m_contain)
            confineWorkerProcess(*m_process);
        if (starts++ == 0) {
            // Ended with the application, before it deletes the process.
            qAddPostRoutine([] { Worker::instance().stop(); });
        }
        startWithFewDescriptorsToClose();
        contained = confinedStart; // (Started by now: CreateProcess() is done.)
        m_awaitingHello = true;
        return true;
    }

    // QProcess::start() with the descriptors of the worker closed one by one
    // up to the limit, as CloseFileDescriptors does (above), costs 0.2 s at
    // the usual limit of a million, with this thread waiting for it. The
    // limit is lowered, for the start, to a little above the highest
    // descriptor open: all there are get closed as before. The worker has the
    // limit back for itself.
    void startWithFewDescriptorsToClose()
    {
#ifdef Q_OS_LINUX
        rlimit original{};
        constexpr rlim_t spare = 1024; // Others opening files at the same time.
        if (getrlimit(RLIMIT_NOFILE, &original) == 0 && original.rlim_cur > 4 * spare) {
            int highest = 2;
            const QStringList open = QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::NoDotAndDotDot);
            for (const QString &name : open)
                highest = qMax(highest, name.toInt());
            rlimit lowered = original;
            lowered.rlim_cur = qMin(original.rlim_cur, rlim_t(highest) + spare);
            if (!open.isEmpty() && lowered.rlim_cur < original.rlim_cur && setrlimit(RLIMIT_NOFILE, &lowered) == 0) {
                m_process->setChildProcessModifier([original] { setrlimit(RLIMIT_NOFILE, &original); });
                m_process->start();
                setrlimit(RLIMIT_NOFILE, &original);
                return;
            }
        }
#endif
        m_process->start();
    }

    bool start()
    {
        if (m_process && !m_awaitingHello && m_process->state() == QProcess::Running)
            return true;
        for (;;) {
            if (!(m_process && m_awaitingHello) && !launch())
                return false;
            qint32 hello[2] = {};
            const QDeadlineTimer deadline(workerStartMs);
            const bool greeted = m_process->waitForStarted(workerStartMs)
                && read(reinterpret_cast<char *>(hello), sizeof hello, deadline) && quint32(hello[0]) == answerMagic;
            if (greeted && hello[1] == 1) {
                m_awaitingHello = false;
                return true;
            }
            // An AppContainer can keep the worker from starting, or from
            // finding MicroTeX's resources: where the app is on a network
            // drive, say. Then it runs without one (locked down as far as
            // it can lock itself). Not after a timeout: a busy machine is
            // no reason to.
            if (contained && (m_process->state() == QProcess::NotRunning || greeted)) {
                qWarning("The math worker could not run in an AppContainer; it runs without one");
                stop();
                m_contain = false;
                continue;
            }
            stop();
            if (greeted) { // It runs, but MicroTeX has no resources.
                m_unavailable = true;
                return false;
            }
            // Not there, not a worker, or the machine is too busy: not
            // tried for every formula, but again after a while.
            m_failuresInARow = maxFailuresInARow;
            m_paused.start();
            return false;
        }
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
    bool m_awaitingHello = false; // Started (by launch()), its greeting not yet read.
    bool m_unavailable = false; // No worker program, or it cannot draw at all.
    bool m_contain = true; // Started in an AppContainer (Windows), unless that failed.
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

void prepareWorker()
{
    Worker::instance().prepare();
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

bool workerContained()
{
    return contained;
}

#ifdef Q_OS_LINUX
namespace {

// Landlock (Linux 5.13 and later; see Documentation/userspace-api/landlock.rst),
// called directly: the C library has no wrappers, and the header of the
// build machine may know fewer rights than the kernel the app runs on.
constexpr long landlockCreateRuleset = 444;
constexpr long landlockAddRule = 445;
constexpr long landlockRestrictSelf = 446;
constexpr quint32 landlockCreateRulesetVersion = 1U << 0;
constexpr int landlockRulePathBeneath = 1;
constexpr quint64 fsExecute = 1ULL << 0;
constexpr quint64 fsReadFile = 1ULL << 2;
constexpr quint64 fsReadDir = 1ULL << 3;
constexpr quint64 netBindTcp = 1ULL << 0;
constexpr quint64 netConnectTcp = 1ULL << 1;
constexpr quint64 scopeAbstractUnixSocket = 1ULL << 0;
constexpr quint64 scopeSignal = 1ULL << 1;

struct RulesetAttr
{
    quint64 handledAccessFs = 0;
    quint64 handledAccessNet = 0; // ABI 4
    quint64 scoped = 0;           // ABI 6
};

struct [[gnu::packed]] PathBeneathAttr
{
    quint64 allowedAccess = 0;
    qint32 parentFd = -1;
};

// Every file system right the kernel knows of, by Landlock ABI version.
quint64 fsRights(int abi)
{
    quint64 rights = (1ULL << 13) - 1; // ABI 1: execute .. make_sym
    if (abi >= 2)
        rights |= 1ULL << 13; // refer
    if (abi >= 3)
        rights |= 1ULL << 14; // truncate
    if (abi >= 5)
        rights |= 1ULL << 15; // ioctl_dev
    return rights;
}

// Where the worker may still read (and load code from): fonts and their
// configuration, the system's and the app's own files. Not the home
// folder, documents or anything else, and nothing may be written anywhere.
QStringList readablePlaces(const QString &resources)
{
    QStringList places = {resources,
                          QStringLiteral("/usr"),
                          QStringLiteral("/lib"),
                          QStringLiteral("/lib64"),
                          QStringLiteral("/etc/fonts"),
                          QStringLiteral("/var/cache/fontconfig"),
                          QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/..")),
                          QLibraryInfo::path(QLibraryInfo::PrefixPath)};
    places += QStandardPaths::standardLocations(QStandardPaths::FontsLocation);
    places += QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/fontconfig");
    places += QDir::homePath() + QStringLiteral("/.fonts");
    return places;
}

// Takes the file system (but for reading the places above) and TCP from
// the worker, for good. False if the kernel has no Landlock.
bool landlock(const QString &resources)
{
    const long abi = syscall(landlockCreateRuleset, nullptr, 0, landlockCreateRulesetVersion);
    if (abi < 1)
        return false;
    RulesetAttr attr;
    attr.handledAccessFs = fsRights(int(abi));
    size_t size = offsetof(RulesetAttr, handledAccessNet);
    if (abi >= 4) {
        attr.handledAccessNet = netBindTcp | netConnectTcp;
        size = offsetof(RulesetAttr, scoped);
    }
    if (abi >= 6) {
        attr.scoped = scopeAbstractUnixSocket | scopeSignal;
        size = sizeof(RulesetAttr);
    }
    const int ruleset = int(syscall(landlockCreateRuleset, &attr, size, 0));
    if (ruleset < 0)
        return false;
    for (const QString &place : readablePlaces(resources)) {
        if (place.isEmpty())
            continue;
        const int fd = open(QFile::encodeName(place).constData(), O_PATH | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct stat info = {};
        PathBeneathAttr rule;
        // A file (not a folder) takes only the rights a file can have.
        rule.allowedAccess = fsExecute | fsReadFile;
        if (fstat(fd, &info) == 0 && S_ISDIR(info.st_mode))
            rule.allowedAccess |= fsReadDir;
        rule.parentFd = fd;
        syscall(landlockAddRule, ruleset, landlockRulePathBeneath, &rule, 0);
        close(fd);
    }
    const bool done = syscall(landlockRestrictSelf, ruleset, 0) == 0;
    close(ruleset);
    return done;
}

// No new sockets of any kind (UDP and local ones included, which Landlock
// does not cover) and no io_uring, which could make them without a
// system call: socket() and io_uring_setup() fail with EPERM.
bool noSockets()
{
#if defined(__x86_64__)
    constexpr quint32 arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr quint32 arch = AUDIT_ARCH_AARCH64;
#else
    return false;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    constexpr quint32 deny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        // x32 system calls (numbers with bit 30 set) would bypass the rest.
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, deny),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_socket, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, deny),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_io_uring_setup, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, deny),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program = {static_cast<unsigned short>(std::size(filter)), filter};
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#endif
}

} // namespace
#endif

#ifdef Q_OS_WIN
namespace {

// Windows has no way for a running process to give up reading the user's
// files or the network, so the app starts the worker in an AppContainer
// (see "AppContainer isolation" in Microsoft's documentation): it can
// read only what all AppContainers may (Windows' folders, the fonts,
// Program Files) and what it was let read, write nowhere, reach no
// network (it has no capabilities) and start no process. The worker then
// locks what it can itself (lockDownWorker): low integrity, which writes
// nowhere but in the few places made for it, and a job of one process.
// Started some other way (scripts/check_math_worker.py, or where the
// AppContainer failed it), the worker has these and can still read the
// user's files and reach the network.

// The worker's AppContainer. Named, not made afresh: what it was let read
// stays readable to the next worker. Windows starts no process in one that
// has no profile (a folder, Packagesisualnotepad.mathworker in the local
// app data, and registry keys), so the first worker makes it; the folder
// is then taken from it (takeContainerFolder).
PSID containerSid()
{
    static const PSID sid = [] {
        const wchar_t *const name = L"VisualNotepad.MathWorker";
        PSID made = nullptr;
        if (SUCCEEDED(CreateAppContainerProfile(name, L"Visual Notepad math", L"Draws formulas for Visual Notepad",
                                                nullptr, 0, &made)))
            return made;
        // Made by an earlier worker.
        return SUCCEEDED(DeriveAppContainerSidFromAppContainerName(name, &made)) ? made : PSID(nullptr);
    }();
    return sid;
}

// Whether `dacl` lets the worker's AppContainer, or every AppContainer,
// read and run (with `contents`, also all that is in a folder).
bool containerMayRead(PACL dacl, PSID container, bool contents)
{
    if (!dacl)
        return true; // No access control (FAT): anyone may.
    BYTE any[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof any;
    if (!CreateWellKnownSid(WinBuiltinAnyPackageSid, nullptr, any, &size))
        return false;
    constexpr ACCESS_MASK needed = FILE_READ_DATA | FILE_EXECUTE;
    constexpr BYTE handedDown = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        ACCESS_ALLOWED_ACE *ace = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void **>(&ace)) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE
            || (ace->Header.AceFlags & INHERIT_ONLY_ACE) || (ace->Mask & needed) != needed
            || (contents && (ace->Header.AceFlags & handedDown) != handedDown))
            continue;
        const PSID sid = &ace->SidStart;
        if (EqualSid(sid, any) || EqualSid(sid, container))
            return true;
    }
    return false;
}

// Lets the worker's AppContainer read (and run) the file or folder `path`:
// a folder's own entry and names, or with `contents`, all that is in it.
// The grant stays: each is changed once.
void letContainerRead(const QString &path, bool contents)
{
    const PSID container = containerSid();
    std::wstring name = QDir::toNativeSeparators(path).toStdWString();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(name.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                              nullptr, &descriptor)
        != ERROR_SUCCESS)
        return;
    EXPLICIT_ACCESSW access = {};
    access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = contents ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(container);
    PACL granted = nullptr;
    if (!containerMayRead(dacl, container, contents)
        && SetEntriesInAclW(1, &access, dacl, &granted) == ERROR_SUCCESS) {
        if (contents) {
            // Handed down to all that is in the folder.
            SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, granted,
                                  nullptr);
        } else {
            // The folder's own entry, and nothing in it (a build's folder
            // has thousands of files).
            SECURITY_DESCRIPTOR_CONTROL control = 0;
            DWORD revision = 0;
            GetSecurityDescriptorControl(descriptor, &control, &revision);
            SECURITY_DESCRIPTOR updated;
            InitializeSecurityDescriptor(&updated, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(&updated, TRUE, granted, FALSE);
            constexpr SECURITY_DESCRIPTOR_CONTROL kept = SE_DACL_AUTO_INHERITED | SE_DACL_PROTECTED;
            SetSecurityDescriptorControl(&updated, kept, control & kept);
            SetFileSecurityW(name.c_str(), DACL_SECURITY_INFORMATION, &updated);
        }
        LocalFree(granted);
    }
    LocalFree(descriptor);
}

// An AppContainer may write to the folder of its profile (and Temp in it):
// the worker's is taken from it, with all in it.
void takeContainerFolder()
{
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(containerSid(), &sid))
        return;
    PWSTR folder = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (SUCCEEDED(GetAppContainerFolderPath(sid, &folder))
        && GetNamedSecurityInfoW(folder, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                                 &descriptor)
            == ERROR_SUCCESS) {
        bool granted = false;
        for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
            ACCESS_ALLOWED_ACE *ace = nullptr;
            if (GetAce(dacl, i, reinterpret_cast<void **>(&ace)) && ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE
                && !(ace->Header.AceFlags & INHERITED_ACE) && EqualSid(&ace->SidStart, containerSid()))
                granted = true;
        }
        EXPLICIT_ACCESSW revoke = {};
        revoke.grfAccessMode = REVOKE_ACCESS;
        revoke.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        revoke.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        revoke.Trustee.ptstrName = static_cast<LPWSTR>(containerSid());
        PACL revoked = nullptr;
        if (granted && SetEntriesInAclW(1, &revoke, dacl, &revoked) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW(folder, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, revoked, nullptr);
            LocalFree(revoked);
        }
    }
    LocalFree(descriptor);
    CoTaskMemFree(folder);
    LocalFree(sid);
}

// Lets the worker's AppContainer read what it needs to start and draw: the
// program, the libraries this process has loaded (the worker is the same
// program, and loads no others), the folders they are in (Qt lists its
// plugin folders, and looks for the program's) and MicroTeX's resources.
// Windows' own files and the fonts every AppContainer may read already.
void letWorkerRead(const QString &program)
{
    takeContainerFolder();
    wchar_t windows[MAX_PATH] = {};
    const UINT length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    const QString system = QDir::fromNativeSeparators(QString::fromWCharArray(windows, int(length))) + u'/';
    QSet<QString> files = {QFileInfo(program).absoluteFilePath()};
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &bytes)) {
        std::wstring name(32768, L'\0');
        for (DWORD i = 0; i < qMin<DWORD>(bytes / sizeof(HMODULE), DWORD(std::size(modules))); ++i) {
            const DWORD n = GetModuleFileNameW(modules[i], name.data(), DWORD(name.size()));
            const QString path = QDir::fromNativeSeparators(QString::fromWCharArray(name.data(), int(n)));
            if (n > 0 && n < name.size() && !path.startsWith(system, Qt::CaseInsensitive))
                files.insert(path);
        }
    }
    QSet<QString> folders;
    for (const QString &file : std::as_const(files)) {
        letContainerRead(file, false);
        folders.insert(QFileInfo(file).absolutePath());
    }
    for (const QString &folder : std::as_const(folders))
        letContainerRead(folder, false);
    const QString resources = findResources(QFileInfo(program).absolutePath());
    if (!resources.isEmpty())
        letContainerRead(resources, true);
    // Qt's plugins, where they are not in the program's folder (as in a
    // build): Qt checks the folder before it looks in it.
    const QString plugins = QLibraryInfo::path(QLibraryInfo::PluginsPath);
    if (QFileInfo(plugins).isDir())
        letContainerRead(plugins, true);
}

// 1 and 2 of lockDownWorker() for a process in an AppContainer: it reads
// only what it was let read, and it has no capabilities (the network is
// one of them).
int containerLocks()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return 0;
    int locks = 0;
    DWORD isContainer = 0;
    DWORD got = 0;
    if (GetTokenInformation(token, TokenIsAppContainer, &isContainer, sizeof isContainer, &got) && isContainer) {
        locks |= 1;
        // Too small for even one capability.
        TOKEN_GROUPS capabilities = {};
        if (GetTokenInformation(token, TokenCapabilities, &capabilities, sizeof capabilities, &got)
            && capabilities.GroupCount == 0)
            locks |= 2;
    }
    CloseHandle(token);
    return locks;
}

// Low integrity (an AppContainer has it from the start): nothing can be
// written but the few places made for it (in an AppContainer not even
// those), whatever their access lists say.
bool lowIntegrity()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token)
        && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    BYTE low[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof low;
    if (CreateWellKnownSid(WinLowLabelSid, nullptr, low, &size)) {
        TOKEN_MANDATORY_LABEL label = {{low, SE_GROUP_INTEGRITY}};
        SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof label + GetLengthSid(low));
    }
    DWORD level = SECURITY_MANDATORY_SYSTEM_RID;
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD got = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof buffer, &got)) {
        const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buffer)->Label.Sid;
        level = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    }
    CloseHandle(token);
    return level <= SECURITY_MANDATORY_LOW_RID;
}

// No process in the worker's job but the worker, for good: the job's one
// handle is closed, and no other can be had (it has no name). Nor the
// clipboard, other programs' windows or the system's settings.
bool noNewProcesses()
{
    HANDLE &job = workerJob();
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = jobLimits(true);
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {
            JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD | JOB_OBJECT_UILIMIT_HANDLES
            | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_DESKTOP
            | JOB_OBJECT_UILIMIT_EXITWINDOWS};
        SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui);
        CloseHandle(job);
        job = nullptr;
    }
    // The job the process is in, if any.
    JOBOBJECT_BASIC_LIMIT_INFORMATION now = {};
    return QueryInformationJobObject(nullptr, JobObjectBasicLimitInformation, &now, sizeof now, nullptr)
        && (now.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) && now.ActiveProcessLimit == 1;
}

} // namespace
#endif

// MicroTeX is not to be trusted with what it reads (see the top of the
// header): once it has started, the worker can no longer open the user's
// files, write anywhere or reach the network, so a formula that takes it
// over gets nothing. Best effort: an older kernel without Landlock still
// gets the socket ban; on Windows, see above.
int lockDownWorker()
{
    int locks = 0;
#ifdef Q_OS_LINUX
    // Both need no new privileges (set in confineWorker) to work unprivileged.
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (landlock(findResources()))
        locks |= 1;
    if (noSockets())
        locks |= 2;
#elif defined(Q_OS_WIN)
    locks |= containerLocks();
    if (lowIntegrity())
        locks |= 4;
    if (noNewProcesses())
        locks |= 8;
#endif
    return locks;
}

void confineWorkerProcess(QProcess &process)
{
#ifdef Q_OS_WIN
    const PSID container = containerSid();
    if (!container)
        return;
    letWorkerRead(process.program());
    // What CreateProcess() is handed: kept with the process until it is
    // started again or deleted.
    struct Start
    {
        SECURITY_CAPABILITIES capabilities = {}; // None.
        DWORD childProcesses = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
        HANDLE inherited[3] = {};
        std::unique_ptr<char[]> attributes;
        STARTUPINFOEXW startup = {};
        wchar_t folder[MAX_PATH] = {};

        ~Start()
        {
            if (startup.lpAttributeList)
                DeleteProcThreadAttributeList(startup.lpAttributeList);
        }
    };
    auto start = std::make_shared<Start>();
    start->capabilities.AppContainerSid = container;
    process.setCreateProcessArgumentsModifier([start](QProcess::CreateProcessArguments *args) {
        Start &s = *start;
        if (s.startup.lpAttributeList) {
            DeleteProcThreadAttributeList(s.startup.lpAttributeList);
            s.startup.lpAttributeList = nullptr;
        }
        s.startup.StartupInfo = *args->startupInfo;
        s.startup.StartupInfo.cb = sizeof s.startup;
        // It gets the pipes and nothing else the app has open, as on Unix.
        DWORD handles = 0;
        for (HANDLE handle : {s.startup.StartupInfo.hStdInput, s.startup.StartupInfo.hStdOutput,
                              s.startup.StartupInfo.hStdError}) {
            if (handle && handle != INVALID_HANDLE_VALUE
                && std::find(s.inherited, s.inherited + handles, handle) == s.inherited + handles)
                s.inherited[handles++] = handle;
        }
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 3, 0, &size);
        s.attributes.reset(new char[size]);
        const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(s.attributes.get());
        if (!InitializeProcThreadAttributeList(list, 3, 0, &size))
            return;
        s.startup.lpAttributeList = list;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &s.capabilities,
                                       sizeof s.capabilities, nullptr, nullptr)
            || !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY, &s.childProcesses,
                                          sizeof s.childProcesses, nullptr, nullptr)
            || (handles > 0
                && !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, s.inherited,
                                              handles * sizeof(HANDLE), nullptr, nullptr)))
            return; // Started as before, and not reported contained.
        // A folder it may open: the app's working folder may be out of its
        // reach.
        GetSystemDirectoryW(s.folder, MAX_PATH);
        args->currentDirectory = s.folder;
        args->startupInfo = &s.startup.StartupInfo;
        // No console (it needs none): a console program fails to start in
        // an AppContainer with one, its libraries unable to start.
        args->flags = (args->flags & ~DWORD(CREATE_NO_WINDOW)) | DETACHED_PROCESS | EXTENDED_STARTUPINFO_PRESENT;
        confinedStart = true;
    });
#else
    Q_UNUSED(process);
#endif
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
    // Draw once before any copy is made (and before the lock-down, which
    // leaves no way to open font files): the copies inherit the font setup
    // and the fonts read, where each would otherwise open the font
    // configuration and some 260 files again for its one formula.
    if (hello[1] == 1) {
        Formula warm;
        microTeXFormula(QStringLiteral("x_{1}^2+\\frac{a}{b}\\sum_{i=0}^{n}\\int\\sqrt{y}\\,\\mathbf{A}\\mathcal{B}\\mathrm{C}\\alpha\\left(\\frac{1}{2}\\right)"), true, warm);
        microTeXFormula(QStringLiteral("x_{1}^2+\\frac{a}{b}"), false, warm);
    }
    if (!writeFully(reinterpret_cast<const char *>(hello), sizeof hello))
        endWorker(1);
    lockDownWorker();
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
