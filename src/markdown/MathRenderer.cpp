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
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QtMath>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
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

// The largest picture a formula may make, in device pixels: a side, and
// both together (64 MB of pixels). "\rule{30000pt}{30000pt}" would be 3.5 GB.
constexpr int maxImageSide = 16'384;
constexpr qint64 maxImagePixels = 16 * 1024 * 1024;

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

// First bytes of every answer of the worker ("VNM1").
constexpr quint32 answerMagic = 0x564E4D31;

QImage renderSource(const QString &tex, const Style &style)
{
    QFont font(QStringLiteral("serif"));
    font.setStyleHint(QFont::Serif);
    font.setItalic(true);
    font.setPixelSize(qRound(style.pixelSize));
    const QFontMetricsF fm(font);
    // The start of a long one: its picture would be as wide as it is long.
    QString text = tex.left(4 * maxSourceChars).simplified();
    if (text.size() > maxSourceChars)
        text = text.left(maxSourceChars) + QChar(0x2026);
    const QSizeF size(qMin<qreal>(fm.horizontalAdvance(text) + 4, maxImageSide / style.devicePixelRatio), fm.height());
    QImage image((size * style.devicePixelRatio).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(style.devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(style.color);
    painter.drawText(QPointF(2, fm.ascent()), text);
    return image;
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

QImage renderMicroTeX(const QString &tex, const Style &style)
{
    constexpr qreal padding = 1;
    constexpr int layoutWidth = 20000; // formulas never wrap
    if (!ensureInitialized())
        return {};
    try {
        tex::Formula formula(tex.toStdWString());
        tex::TeXRenderBuilder builder;
        // Qt converts the point sizes MicroTeX gives its fonts using the
        // paint device's DPI; the image below is set to 72 DPI so that
        // one point is one logical pixel.
        const auto fg = static_cast<tex::color>(style.color.rgba());
        std::unique_ptr<tex::TeXRender> render(
            builder.setStyle(style.display ? tex::TexStyle::display : tex::TexStyle::text)
                .setTextSize(static_cast<float>(style.pixelSize))
                .setWidth(tex::UnitType::pixel, layoutWidth, tex::Alignment::left)
                .setIsMaxWidth(true)
                .setLineSpace(tex::UnitType::pixel, 0)
                .setForeground(fg)
                .build(formula));
        const qreal width = render->getWidth();
        const qreal height = render->getHeight() + render->getDepth();
        if (!(width > 0) || !(height > 0)) // Not a number either.
            return {};

        const qreal dpr = style.devicePixelRatio;
        const qreal pixelsWide = std::ceil((width + 2 * padding) * dpr);
        const qreal pixelsHigh = std::ceil((height + 2 * padding) * dpr);
        if (!(pixelsWide <= maxImageSide) || !(pixelsHigh <= maxImageSide)
            || pixelsWide * pixelsHigh > qreal(maxImagePixels))
            return {};
        QImage image(QSize(int(pixelsWide), int(pixelsHigh)), QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            return {};
        image.setDevicePixelRatio(dpr);
        image.setDotsPerMeterX(2835); // 72 DPI
        image.setDotsPerMeterY(2835);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
            Graphics2D g2(&painter);
            render->draw(g2, qRound(padding), qRound(padding));
        }
        return image;
    } catch (const std::exception &e) {
        qWarning("MicroTeX failed on \"%s\": %s", qPrintable(tex.left(60)), e.what());
    }
    return {};
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

bool writeAnswer(const QImage &image)
{
    const bool drawn = !image.isNull() && image.format() == QImage::Format_ARGB32_Premultiplied;
    const qint32 header[3] = {qint32(answerMagic), drawn ? image.width() : 0, drawn ? image.height() : 0};
    if (!writeFully(reinterpret_cast<const char *>(header), sizeof header))
        return false;
    if (!drawn)
        return true;
    // Row by row: a row in memory may be longer than its pixels.
    for (int y = 0; y < image.height(); ++y) {
        if (!writeFully(reinterpret_cast<const char *>(image.constScanLine(y)), qint64(image.width()) * 4))
            return false;
    }
    return true;
}

// The formula ended the process that drew it; the worker goes on.
[[maybe_unused]] bool writeFailure()
{
    const qint32 header[3] = {qint32(answerMagic), -1, -1};
    return writeFully(reinterpret_cast<const char *>(header), sizeof header);
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

    // The picture of a formula, or a null image: it is not TeX MicroTeX
    // reads, or the worker failed on it.
    QImage render(const QString &tex, const Style &style)
    {
        if (m_unavailable || tex.size() > maxTexChars)
            return {};
        QByteArray request;
        {
            QDataStream stream(&request, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_0);
            stream << tex.toUtf8() << double(style.pixelSize) << quint32(style.color.rgba())
                   << double(style.devicePixelRatio) << style.display;
        }
        // A request the worker failed on is not sent again: it would fail
        // again, at the same price.
        const QByteArray key = QCryptographicHash::hash(request, QCryptographicHash::Sha1);
        if (m_refused.contains(key))
            return {};
        if (m_failuresInARow >= maxFailuresInARow) {
            if (m_paused.isValid() && m_paused.elapsed() < pauseMs)
                return {};
            m_failuresInARow = 0;
        }
        if (!start())
            return {};

        const int allowed = qMax(leastFormulaMs, formulaMs >> qMin(m_timeouts, 16));
        const QDeadlineTimer deadline(allowed);
        const quint32 size = quint32(request.size());
        QImage image;
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
                const qint64 width = header[1];
                const qint64 height = header[2];
                if (width == 0 && height == 0) {
                    answered = true; // Not TeX it reads.
                } else if (width == -1 && height == -1) {
                    crashed = true; // And the worker lives on.
                } else if (width > 0 && height > 0 && width <= maxImageSide && height <= maxImageSide
                           && width * height <= maxImagePixels) {
                    image = QImage(int(width), int(height), QImage::Format_ARGB32_Premultiplied);
                    answered = !image.isNull();
                    for (int y = 0; answered && y < image.height(); ++y)
                        answered = read(reinterpret_cast<char *>(image.scanLine(y)), width * 4, deadline);
                }
            }
        }
        if (!answered) {
            // Crashed, too slow, or not answering as agreed: whatever state
            // it is in, the next formula gets a new one.
            if (!crashed) {
                if (deadline.hasExpired())
                    ++m_timeouts;
                stop();
            }
            if (m_refused.size() < 10'000)
                m_refused.insert(key);
            if (++m_failuresInARow >= maxFailuresInARow)
                m_paused.start();
            return {};
        }
        m_failuresInARow = 0;
        if (usedUp)
            stop();
        if (image.isNull())
            return {};
        image.setDevicePixelRatio(style.devicePixelRatio);
        image.setDotsPerMeterX(2835); // 72 DPI, as the worker drew it.
        image.setDotsPerMeterY(2835);
        return image;
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

QImage render(const QString &tex, const Style &style)
{
    QImage image = Worker::instance().render(tex, style);
    fellBack = image.isNull();
    return fellBack ? renderSource(tex, style) : image;
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
        double pixelSize = 0;
        quint32 rgba = 0;
        double devicePixelRatio = 0;
        bool display = false;
        QDataStream stream(request);
        stream.setVersion(QDataStream::Qt_6_0);
        stream >> utf8 >> pixelSize >> rgba >> devicePixelRatio >> display;
        QImage image;
        if (stream.status() == QDataStream::Ok && pixelSize > 0 && pixelSize <= 1000 && devicePixelRatio > 0
            && devicePixelRatio <= 16) {
            Style style;
            style.pixelSize = pixelSize;
            style.color = QColor::fromRgba(rgba);
            style.devicePixelRatio = devicePixelRatio;
            style.display = display;
#ifdef VN_MATH_FORK
            // A copy of this process draws the formula and ends: whatever
            // the formula does to MicroTeX, or to the process, ends with it.
            const pid_t copy = fork();
            if (copy == 0) {
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                alarm(workerWatchdogMs / 1000);
                _exit(writeAnswer(renderMicroTeX(QString::fromUtf8(utf8), style)) ? 0 : 1);
            }
            int status = 0;
            bool drawn = copy > 0;
            while (drawn && waitpid(copy, &status, 0) < 0)
                drawn = errno == EINTR;
            if (drawn && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                continue; // It has answered.
            if (!writeFailure())
                endWorker(1);
            continue;
#else
            busySince.store(qMax<qint64>(1, clock.elapsed()));
            image = renderMicroTeX(QString::fromUtf8(utf8), style);
            busySince.store(0);
#endif
        }
        if (!writeAnswer(image))
            endWorker(1);
    }
}

} // namespace vn::math
