// TeX math rendering through MicroTeX, including the text fallback.
#include "Settings.h"
#include "markdown/MathRenderer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>
#include <QtMath>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#endif

using namespace vn;

namespace {

math::Style inlineStyle()
{
    math::Style s;
    s.pixelSize = 15;
    return s;
}

math::Style displayStyle()
{
    math::Style s;
    s.pixelSize = 15;
    s.display = true;
    return s;
}

// Logical size of the image in device-independent pixels.
QSizeF logicalSize(const QImage &image)
{
    return QSizeF(image.size()) / image.devicePixelRatio();
}

bool hasInk(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > 0)
                return true;
    return false;
}

} // namespace

class TestMath : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void aPreparedWorkerIsTheOneThatDraws();
    void lockedDownWorkerReadsNoUserFiles();
    void lockedDownWorkerStillDrawsEveryFont();
    void fractionIsTallerThanText();
    void displayIsLargerThanInline();
    void baselineIsReported_data();
    void baselineIsReported();
    void shapesScaleToAnySize();
    void lonelySymbolsAreDrawn();
    void slantedLettersStayInTheirBox();
    void colourIsApplied();
    void devicePixelRatioScalesPixels();
    void invalidTexFallsBack();
    void macroFromSettings();
    void formulasThatBreakMicroTeXAreShownAsSource_data();
    void formulasThatBreakMicroTeXAreShownAsSource();
    void aFormulaThatNeverEndsIsGivenUp();
    void theTimeComesBackOnceFormulasAreDrawnAgain();
    void longSourceIsNotDrawnInFull();
    void aFormulaLeavesNothingBehind_data();
    void aFormulaLeavesNothingBehind();
    void saveScreenshots();
};

void TestMath::init()
{
    // Tests here run formulas that run out of time on purpose, and each such
    // formula halves the time the next may take, down to 250 ms, and is not
    // tried again. A test after them would have a worker that may take 250 ms
    // for its first formula, which a busy machine does not give it.
    math::forgetWorkerFailures();
}

void TestMath::fractionIsTallerThanText()
{
    const QImage frac = math::render(QStringLiteral("\\frac{a}{b}"), inlineStyle());
    const QImage text = math::render(QStringLiteral("ab"), inlineStyle());
    QVERIFY(!frac.isNull());
    QVERIFY(!text.isNull());
    QVERIFY(hasInk(frac));
    QVERIFY(logicalSize(frac).height() > logicalSize(text).height());
}

void TestMath::displayIsLargerThanInline()
{
    const QString tex = QStringLiteral("\\sum_{i=1}^n i");
    const QImage inl = math::render(tex, inlineStyle());
    const QImage disp = math::render(tex, displayStyle());
    QVERIFY(!inl.isNull());
    QVERIFY(!disp.isNull());
    const QSizeF a = logicalSize(inl);
    const QSizeF b = logicalSize(disp);
    QVERIFY(b.height() > a.height());
}

// Rows from the top of the first, and from the bottom of the last, with ink.
static int clearRowsAtTop(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > 0)
                return y;
    return image.height();
}

static int clearRowsAtBottom(const QImage &image)
{
    return clearRowsAtTop(image.flipped(Qt::Vertical));
}

void TestMath::baselineIsReported_data()
{
    QTest::addColumn<QString>("tex");
    QTest::addColumn<qreal>("ratio");
    // No ink below the baseline but the edge of a curve.
    QTest::newRow("letter") << QStringLiteral("x") << 1.0;
    QTest::newRow("letter at 2x") << QStringLiteral("x") << 2.0;
    QTest::newRow("equation") << QStringLiteral("k = 32") << 1.0;
    // Longer than any formula: shown as source text.
    QTest::newRow("source text") << QStringLiteral("x").repeated(30'000) << 1.0;
}

void TestMath::baselineIsReported()
{
    QFETCH(QString, tex);
    QFETCH(qreal, ratio);
    math::Style style = inlineStyle();
    style.devicePixelRatio = ratio;
    const QImage image = math::render(tex, style);
    QVERIFY(!image.isNull());
    const int row = math::baselineRow(image);
    QVERIFY(row > 0 && row < image.height());
    // The ink ends at the baseline, within a pixel; the picture is no taller
    // than the ink and its padding (once, the depth was added twice).
    const int inkEnd = image.height() - clearRowsAtBottom(image);
    QVERIFY2(qAbs(inkEnd - row) <= qCeil(ratio), qPrintable(QStringLiteral("%1 %2").arg(inkEnd).arg(row)));
    if (!math::lastRenderFellBack())
        QVERIFY2(image.height() - inkEnd <= qCeil(2 * ratio), qPrintable(QString::number(image.height() - inkEnd)));
}

void TestMath::shapesScaleToAnySize()
{
    // Laid out once and drawn at any size: ten times the size is ten times
    // the picture (with its pixel of margin), and as sharp.
    const auto formula = math::layout(QStringLiteral("\\frac{a}{b}+\\sqrt{x}"), false);
    QVERIFY(!math::lastRenderFellBack());
    math::Style small = inlineStyle();
    small.pixelSize = 20;
    math::Style large = inlineStyle();
    large.pixelSize = 200;
    const QImage a = math::toImage(*formula, small);
    const QImage b = math::toImage(*formula, large).convertToFormat(QImage::Format_ARGB32);
    QVERIFY2(qAbs((b.width() - 2) - 10 * (a.width() - 2)) <= 10, qPrintable(QStringLiteral("%1 %2").arg(a.width()).arg(b.width())));
    QVERIFY2(qAbs((b.height() - 2) - 10 * (a.height() - 2)) <= 10, qPrintable(QStringLiteral("%1 %2").arg(a.height()).arg(b.height())));
    // Edges one pixel wide: few partly covered pixels beside the covered
    // ones (a picture magnified ten times is mostly blurred edge).
    int partial = 0, full = 0;
    for (int y = 0; y < b.height(); ++y) {
        for (int x = 0; x < b.width(); ++x) {
            const int alpha = qAlpha(b.pixel(x, y));
            full += alpha == 255;
            partial += alpha > 0 && alpha < 255;
        }
    }
    QVERIFY2(full > 0 && partial < full / 2, qPrintable(QStringLiteral("%1 partly, %2 fully").arg(partial).arg(full)));
}

void TestMath::lonelySymbolsAreDrawn()
{
    // A symbol that is all above the baseline has a depth below none in
    // MicroTeX's measure: drawn, not taken for a formula it cannot read.
    for (const QString tex : {QStringLiteral("\\to"), QStringLiteral("\\rightarrow"), QStringLiteral("\\cdot")}) {
        const auto formula = math::layout(tex, false);
        QVERIFY2(!formula->fellBack, qPrintable(tex));
        QVERIFY(formula->ascent > 0 && formula->depth >= 0 && formula->width > 0);
    }
}

void TestMath::slantedLettersStayInTheirBox()
{
    // An italic "T" or "V" reaches past its advance; the box takes it in (TeX's
    // italic correction), so "$T$ paid" does not run into the word after.
    for (const QString tex : {QStringLiteral("T"), QStringLiteral("V"), QStringLiteral("f"), QStringLiteral("x_i + y")}) {
        const auto formula = math::layout(tex, false);
        QVERIFY(!formula->fellBack);
        QRectF ink;
        for (const auto &shape : formula->shapes)
            ink |= shape.path.boundingRect();
        QVERIFY2(ink.left() >= -0.01 && ink.right() <= formula->width + 0.01,
                 qPrintable(QStringLiteral("%1: ink %2..%3, box 0..%4").arg(tex).arg(ink.left()).arg(ink.right()).arg(formula->width)));
    }
}

void TestMath::colourIsApplied()
{
    math::Style style = inlineStyle();
    style.color = QColor(200, 30, 40);
    style.devicePixelRatio = 2;
    const QImage image = math::render(QStringLiteral("x^2+y^2"), style).convertToFormat(QImage::Format_ARGB32);
    int close = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = QColor::fromRgba(image.pixel(x, y));
            if (c.alpha() > 240 && qAbs(c.red() - 200) < 30 && qAbs(c.green() - 30) < 30 && qAbs(c.blue() - 40) < 30)
                ++close;
        }
    }
    QVERIFY2(close > 5, qPrintable(QString::number(close)));
}

void TestMath::devicePixelRatioScalesPixels()
{
    const QString tex = QStringLiteral("\\frac{a}{b}+c");
    math::Style one = inlineStyle();
    math::Style two = inlineStyle();
    two.devicePixelRatio = 2;
    const QImage a = math::render(tex, one);
    const QImage b = math::render(tex, two);
    QCOMPARE(a.devicePixelRatio(), 1.0);
    QCOMPARE(b.devicePixelRatio(), 2.0);
    QVERIFY(qAbs(b.width() - 2 * a.width()) <= 2);
    QVERIFY(qAbs(b.height() - 2 * a.height()) <= 2);
}

void TestMath::invalidTexFallsBack()
{
    const QImage image = math::render(QStringLiteral("\\frac{"), inlineStyle());
    QVERIFY(!image.isNull());
    QVERIFY(hasInk(image));
}

// MicroTeX runs in a worker process (see MathRenderer.h): what would crash
// the app, or fill its memory, ends a worker instead, and the next formula
// is drawn by a new one.
void TestMath::formulasThatBreakMicroTeXAreShownAsSource_data()
{
    QTest::addColumn<QString>("tex");
    // Each of these crashed the app before, by the way named.
    QTest::newRow("a table command outside a table (memory that is not its own)")
        << QStringLiteral("\\multicolumn{2}{c}{x}");
    QTest::newRow("a negative number of arguments (past the end of a list)")
        << QStringLiteral("\\newcommand{\\zy}[-3]{#1}\\zy{a}{b}");
    QTest::newRow("a command without its argument (no object)") << QStringLiteral("{\\mathtt\\begin{alignat}{2}\\end{alignat}}~0");
    QTest::newRow("dollars in a formula (endless recursion)") << QStringLiteral("$$x$$ $ \\( \\[ \\] \\)");
    QTest::newRow("a picture of 3.5 GB") << QStringLiteral("\\rule{30000pt}{30000pt}");
    QTest::newRow("longer than any formula") << QStringLiteral("x+").repeated(20'000);
}

void TestMath::formulasThatBreakMicroTeXAreShownAsSource()
{
    QFETCH(QString, tex);
    QVERIFY(!math::render(QStringLiteral("a+b"), inlineStyle()).isNull());
    QVERIFY2(!math::lastRenderFellBack(), "the worker does not draw at all");

    QElapsedTimer timer;
    timer.start();
    const QImage image = math::render(tex, displayStyle());
    QVERIFY(math::lastRenderFellBack());
    QVERIFY(!image.isNull());
    QVERIFY(hasInk(image));
    QVERIFY(image.width() <= 16'384);
    // Most of these crash MicroTeX at once; with another standard library
    // some run on instead (what they do is undefined), until the worker's
    // time limit of 3 s ends them.
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));

    math::render(tex, displayStyle());
    QVERIFY(math::lastRenderFellBack());

    // Other formulas are drawn as before.
    const QImage next = math::render(QStringLiteral("\\frac{a}{b}"), inlineStyle());
    QVERIFY(!math::lastRenderFellBack());
    QVERIFY(hasInk(next));
}

void TestMath::aFormulaThatNeverEndsIsGivenUp()
{
    // A command that stands for itself: MicroTeX replaces it for ever.
    // (The time the next formula may take is read from the renderer and
    // that it is not tried again from the workers started: what a busy
    // machine slows down is not timed.)
    const int fullTime = math::formulaTimeLimitMs();
    QElapsedTimer timer;
    timer.start();
    math::render(QStringLiteral("\\newcommand{\\zz}{\\zz}\\zz"), inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QVERIFY2(timer.elapsed() < 2 * fullTime, qPrintable(QString::number(timer.elapsed())));
    // It ran out of its time, and the next formula gets less.
    const int lessTime = math::formulaTimeLimitMs();
    QVERIFY2(lessTime < fullTime, qPrintable(QString::number(lessTime)));
    // Asked again, it is not tried again: that would start a new worker, the
    // one that failed being gone, and wait for it.
    const int starts = math::workerStarts();
    math::render(QStringLiteral("\\newcommand{\\zz}{\\zz}\\zz"), inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QCOMPARE(math::workerStarts(), starts);
    QCOMPARE(math::formulaTimeLimitMs(), lessTime);
    // The next one that never ends costs less time again.
    math::render(QStringLiteral("\\newcommand{\\zw}{\\zw\\zw}\\zw"), inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QVERIFY2(math::formulaTimeLimitMs() < lessTime, qPrintable(QString::number(math::formulaTimeLimitMs())));
    // And the worker is fine afterwards. (With the time of a new one: a busy
    // machine takes longer than the least a formula may take for the first
    // formula of a worker that has to load its fonts.)
    math::forgetWorkerFailures();
    QCOMPARE(math::formulaTimeLimitMs(), fullTime);
    math::render(QStringLiteral("y"), inlineStyle());
    QVERIFY(!math::lastRenderFellBack());
    math::render(QStringLiteral("x^2"), inlineStyle());
    QVERIFY(!math::lastRenderFellBack());
}

// A formula that never ends, named after `n` so that each is a new one (one
// that ran out of time is not tried again).
static QString endlessFormula(int n)
{
    const QString name = QStringLiteral("\\endless") + QChar(u'a' + n % 26) + QChar(u'a' + n / 26);
    return QStringLiteral("\\newcommand{%1}{%1}%1").arg(name);
}

void TestMath::theTimeComesBackOnceFormulasAreDrawnAgain()
{
    // A machine busy for a while: formula after formula ran out of time, and
    // the time the next may take is down to the least. (Formulas that never
    // end stand in for the busy machine; what is checked is the time limit
    // the renderer reports, not how long anything took.)
    const int fullTime = math::formulaTimeLimitMs();
    int least = fullTime;
    for (int n = 0; n < 6; ++n) {
        math::render(endlessFormula(n), inlineStyle());
        QVERIFY(math::lastRenderFellBack());
        const int now = math::formulaTimeLimitMs();
        QVERIFY2(now <= least, qPrintable(QString::number(now)));
        if (now == least && n > 0)
            break; // Down to the least.
        least = now;
    }
    QVERIFY2(least < fullTime / 4, qPrintable(QString::number(least)));

    // Ordinary formulas are drawn again, and each drawn in its time gives the
    // next more, until it has the whole time again. (One that a busy machine
    // still keeps from being drawn in the least time gives none back: more
    // formulas are drawn than are needed.)
    int before = math::formulaTimeLimitMs();
    int drawn = 0;
    for (int n = 0; n < 40 && math::formulaTimeLimitMs() < fullTime; ++n) {
        math::render(QStringLiteral("x_{%1}+\\frac{a}{b}").arg(n), inlineStyle());
        const int now = math::formulaTimeLimitMs();
        if (!math::lastRenderFellBack()) {
            ++drawn;
            QVERIFY2(now > before, qPrintable(QStringLiteral("%1 after %2").arg(now).arg(before)));
        }
        before = now;
    }
    QVERIFY(drawn > 0);
    QCOMPARE(math::formulaTimeLimitMs(), fullTime);

    // A formula that never ends is still cut off, with the whole time again,
    // and is not tried again.
    const QString endless = endlessFormula(100);
    QElapsedTimer timer;
    timer.start();
    math::render(endless, inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QVERIFY2(timer.elapsed() < 2 * fullTime, qPrintable(QString::number(timer.elapsed())));
    const int lessTime = math::formulaTimeLimitMs();
    QVERIFY2(lessTime < fullTime, qPrintable(QString::number(lessTime)));
    const int starts = math::workerStarts();
    math::render(endless, inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QCOMPARE(math::workerStarts(), starts);
    QCOMPARE(math::formulaTimeLimitMs(), lessTime);
}

void TestMath::longSourceIsNotDrawnInFull()
{
    // Source text is the fallback: of a formula of 400,000 characters it
    // was a picture 2.7 million pixels wide.
    const QImage image = math::render(QStringLiteral("\\frac{").repeated(100'000), inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QVERIFY(image.width() < 4000);
}

// What a formula defines or sets is not there for the next: a document
// cannot change how the formulas of another are drawn.
void TestMath::aFormulaLeavesNothingBehind_data()
{
    QTest::addColumn<QString>("tex");
    QTest::newRow("a command redefined") << QStringLiteral("\\renewcommand{\\frac}[2]{x} \\frac{a}{b}");
    QTest::newRow("a new command") << QStringLiteral("\\newcommand{\\alpha}{\\beta\\beta\\beta} \\alpha");
    QTest::newRow("sizes") << QStringLiteral("\\DeclareMathSizes{30}{30}{30}{30} x");
    QTest::newRow("magnification") << QStringLiteral("\\magnification{5} x");
    QTest::newRow("refused, after setting") << QStringLiteral("\\grkaccent.\\magnification");
}

// The first formula of a new worker loads its fonts, which on a machine busy
// enough takes more than the time a formula may take: the formula is then
// shown as source, and not tried again. A formula is drawn again with the
// time forgiven, a few times; one that something an earlier formula left
// behind keeps from being drawn would not be drawn then either.
static QImage renderWhenBusy(const QString &tex)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        const QImage image = math::render(tex, inlineStyle());
        if (!math::lastRenderFellBack())
            return image;
        math::forgetWorkerFailures();
    }
    return math::render(tex, inlineStyle());
}

void TestMath::aFormulaLeavesNothingBehind()
{
    QFETCH(QString, tex);
    const QString plain = QStringLiteral("\\frac{\\alpha}{b}+x^2");
    // Where one worker draws formula after formula (Windows, macOS), it is
    // replaced after tex, and on Windows a few pixels of a glyph depend on
    // which formulas a worker has drawn before. So both pictures are a new
    // worker's first: tex comes before the first one too.
    math::render(tex, inlineStyle());
    const QImage before = renderWhenBusy(plain);
    QVERIFY(!math::lastRenderFellBack());
    math::render(tex, inlineStyle());
    const QImage after = renderWhenBusy(plain);
    QVERIFY(!math::lastRenderFellBack());
    QVERIFY(after == before);
}

void TestMath::macroFromSettings()
{
    Settings::instance().setValue(key::mathMacros, QVariantMap());
    const QString tex = QStringLiteral("\\RR^n");
    const QImage unknown = math::render(math::withMacros(tex), inlineStyle());
    QVERIFY(!unknown.isNull());

    Settings::instance().setValue(key::mathMacros, QVariantMap{{QStringLiteral("\\RR"), QStringLiteral("\\mathbb{R}")}});
    const QString expanded = math::withMacros(tex);
    QVERIFY(expanded.startsWith(QStringLiteral("\\newcommand{\\RR}{\\mathbb{R}}")));
    const QImage known = math::render(expanded, inlineStyle());
    QVERIFY(!known.isNull());
    QVERIFY(known != unknown);
    // The fallback draws the source text, which is much wider than the glyphs.
    QVERIFY(logicalSize(known).width() < logicalSize(unknown).width());
    // Rendering twice must not trip over the repeated definition.
    QVERIFY(math::render(expanded, inlineStyle()) == known);
    Settings::instance().setValue(key::mathMacros, QVariantMap());
}

void TestMath::saveScreenshots()
{
    const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR");
    if (dir.isEmpty())
        QSKIP("VN_SCREENSHOT_DIR not set");
    QDir().mkpath(dir);
    math::Style inl = inlineStyle();
    inl.devicePixelRatio = 2;
    math::Style disp = displayStyle();
    disp.pixelSize = 18;
    disp.devicePixelRatio = 2;
    auto flat = [](const QImage &img) {
        QImage out(img.size(), QImage::Format_ARGB32_Premultiplied);
        out.setDevicePixelRatio(img.devicePixelRatio());
        out.fill(Qt::white);
        QPainter p(&out);
        p.drawImage(0, 0, img);
        return out;
    };
    QVERIFY(flat(math::render(QStringLiteral("e^{i\\pi}+1=0"), inl)).save(dir + QStringLiteral("/math-inline.png")));
    QVERIFY(flat(math::render(
                     QStringLiteral("\\int_0^\\infty e^{-x^2}\\,dx=\\frac{\\sqrt{\\pi}}{2},\\quad \\sum_{k=1}^{n} k=\\frac{n(n+1)}{2}"),
                     disp))
                .save(dir + QStringLiteral("/math-display.png")));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

// Once locked down (as the worker is after MicroTeX starts), a process can
// still read fonts but not the user's files, cannot write and cannot make
// sockets. Checked in a child process: the lock is for good.
void TestMath::aPreparedWorkerIsTheOneThatDraws()
{
    // The worker can be started ahead of the first formula (for a document
    // with math); that formula is then drawn by it, not by another.
    if (math::workerStarts() != 0)
        QSKIP("a worker was started before this test");
    math::prepareWorker();
    QCOMPARE(math::workerStarts(), 1);
    math::prepareWorker(); // Not again.
    QCOMPARE(math::workerStarts(), 1);
    // (A machine busy enough can keep it from drawing in time: the formula
    // is then drawn by a new worker, and that is no other worker having been
    // started in its place.)
    QImage image;
    int tooSlow = 0;
    for (; tooSlow < 3; ++tooSlow) {
        image = math::render(QStringLiteral("x^2+\\frac{a}{b}"), inlineStyle());
        if (!math::lastRenderFellBack())
            break;
        math::forgetWorkerFailures();
    }
    QVERIFY(!math::lastRenderFellBack());
    QVERIFY(hasInk(image));
    QCOMPARE(math::workerStarts(), 1 + tooSlow);
}

void TestMath::lockedDownWorkerReadsNoUserFiles()
{
#ifdef Q_OS_LINUX
    QTemporaryDir home(QDir::homePath() + QStringLiteral("/.vn-lock-test-XXXXXX"));
    QVERIFY(home.isValid());
    const QByteArray secret = QFile::encodeName(home.filePath(QStringLiteral("secret.txt")));
    {
        QFile file(QFile::decodeName(secret));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("secret");
    }
    const QByteArray written = QFile::encodeName(home.filePath(QStringLiteral("new.txt")));
    const pid_t child = fork();
    if (child == 0) {
        const int locks = vn::math::lockDownWorker();
        int code = 0;
        if (!(locks & 2))
            code |= 1; // sockets were not banned
        if ((locks & 1)) {
            if (open(secret.constData(), O_RDONLY) >= 0 || errno != EACCES)
                code |= 2; // a user file could be read
            if (open(written.constData(), O_WRONLY | O_CREAT, 0600) >= 0)
                code |= 4; // a file could be written
            if (access("/etc/fonts", F_OK) == 0 && open("/etc/fonts/fonts.conf", O_RDONLY) < 0 && errno == EACCES)
                code |= 8; // fonts could not be read
        } else {
            code |= 16; // no Landlock here (reported, not failed)
        }
        if (socket(AF_INET, SOCK_STREAM, 0) >= 0 || socket(AF_UNIX, SOCK_STREAM, 0) >= 0)
            code |= 32;
        _exit(code);
    }
    QVERIFY(child > 0);
    int status = 0;
    QVERIFY(waitpid(child, &status, 0) == child);
    QVERIFY(WIFEXITED(status));
    const int code = WEXITSTATUS(status);
    if (code & 16)
        qWarning("Landlock is not available on this kernel: only the socket ban was checked");
    QCOMPARE(code & ~16, 0);
    QVERIFY(!QFile::exists(QFile::decodeName(written)));
#elif defined(Q_OS_WIN)
    // This program again, started as the worker is (lockDownCheck below
    // does the checking): no fork() here.
    QTemporaryDir home(QDir::homePath() + QStringLiteral("/.vn-lock-test-XXXXXX"));
    QVERIFY(home.isValid());
    const QString secret = home.filePath(QStringLiteral("secret.txt"));
    {
        QFile file(secret);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("secret");
    }
    const QString written = home.filePath(QStringLiteral("new.txt"));
    const QString fonts = qEnvironmentVariable("SystemRoot") + QStringLiteral("/Fonts/");
    const QStringList font = QDir(fonts).entryList({QStringLiteral("*.ttf")}, QDir::Files);
    QVERIFY(!font.isEmpty());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("VN_LOCK_CHECK_SECRET"), QDir::toNativeSeparators(secret));
    environment.insert(QStringLiteral("VN_LOCK_CHECK_WRITTEN"), QDir::toNativeSeparators(written));
    environment.insert(QStringLiteral("VN_LOCK_CHECK_FONT"), QDir::toNativeSeparators(fonts + font.first()));
    environment.insert(QStringLiteral("VN_LOCK_CHECK_PORT"), QString::number(server.serverPort()));
    QProcess child;
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setProcessEnvironment(environment);
    math::confineWorkerProcess(child);
    child.start();
    QVERIFY2(child.waitForFinished(60'000), qPrintable(child.errorString()));
    const uint code = uint(child.exitCode());
    QVERIFY2(child.exitStatus() == QProcess::NormalExit, qPrintable(QString::number(code, 16)));
    QCOMPARE(code, 0u);
    QVERIFY(!QFile::exists(written));
#else
    QSKIP("No lock down on this system");
#endif
}

#ifdef Q_OS_WIN
// Here, not at the top: Windows' headers make "small", a name above, a macro.
#include <qt_windows.h>

#include <sddl.h>
#include <userenv.h>

// The child of lockedDownWorkerReadsNoUserFiles: locks itself down as the
// worker does and ends with what it could still do, a bit for each (0:
// nothing it should not).
static void lockDownCheck()
{
    const QString secret = qEnvironmentVariable("VN_LOCK_CHECK_SECRET");
    if (secret.isEmpty())
        return;
    const int locks = math::lockDownWorker();
    int code = 0;
    if ((locks & 15) != 15)
        code |= 1 | (locks << 8); // a lock did not take (and which did)
    const HANDLE read = CreateFileW(reinterpret_cast<const wchar_t *>(secret.utf16()), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (read != INVALID_HANDLE_VALUE || GetLastError() != ERROR_ACCESS_DENIED)
        code |= 2; // a user file could be read
    // Nothing written: not in the user's folders, the temporary folder, or
    // the folder (with Temp in it) an AppContainer has of its own.
    QStringList written = {qEnvironmentVariable("VN_LOCK_CHECK_WRITTEN"),
                           QDir::tempPath() + QStringLiteral("/vn-lock-test.txt")};
    HANDLE token = nullptr;
    alignas(TOKEN_APPCONTAINER_INFORMATION)
        BYTE container[sizeof(TOKEN_APPCONTAINER_INFORMATION) + SECURITY_MAX_SID_SIZE];
    DWORD got = 0;
    LPWSTR sid = nullptr;
    PWSTR own = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)
        && GetTokenInformation(token, TokenAppContainerSid, container, sizeof container, &got)
        && reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(container)->TokenAppContainer
        && ConvertSidToStringSidW(reinterpret_cast<TOKEN_APPCONTAINER_INFORMATION *>(container)->TokenAppContainer, &sid)
        && SUCCEEDED(GetAppContainerFolderPath(sid, &own))) {
        written << QString::fromWCharArray(own) + QStringLiteral("/vn-lock-test.txt")
                << QString::fromWCharArray(own) + QStringLiteral("/Temp/vn-lock-test.txt");
    }
    for (const QString &file : std::as_const(written)) {
        const QString name = QDir::toNativeSeparators(file);
        if (CreateFileW(reinterpret_cast<const wchar_t *>(name.utf16()), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_FLAG_DELETE_ON_CLOSE, nullptr)
            != INVALID_HANDLE_VALUE)
            code |= 4; // a file could be written
    }
    const QString font = qEnvironmentVariable("VN_LOCK_CHECK_FONT");
    if (CreateFileW(reinterpret_cast<const wchar_t *>(font.utf16()), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, 0, nullptr)
        == INVALID_HANDLE_VALUE)
        code |= 8; // fonts could not be read
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, quint16(qEnvironmentVariableIntValue("VN_LOCK_CHECK_PORT")));
    if (socket.waitForConnected(3000))
        code |= 32; // a connection was made
    wchar_t command[] = L"cmd.exe /c exit 0";
    STARTUPINFOW startup = {sizeof startup};
    PROCESS_INFORMATION started = {};
    if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &started)) {
        TerminateProcess(started.hProcess, 0);
        code |= 64; // a process could be started
    }
    TerminateProcess(GetCurrentProcess(), UINT(code));
}
Q_COREAPP_STARTUP_FUNCTION(lockDownCheck)
#endif

// The worker locks itself down after MicroTeX starts; fonts MicroTeX or Qt
// load only when a formula first needs them must still be readable.
void TestMath::lockedDownWorkerStillDrawsEveryFont()
{
    for (const QString &tex : {QStringLiteral("\\mathfrak{A}\\mathcal{B}\\mathbb{C}\\mathscr{D}"),
                               QStringLiteral("\\text{caf\u00e9 \u0394 \u4e2d\u6587}\\operatorname{sinc}"),
                               QStringLiteral("\\mathtt{x}\\mathsf{y}\\boldsymbol{\\alpha}\\oint_C"),
                               QStringLiteral("\\begin{pmatrix}1&2\\\\3&4\\end{pmatrix}")}) {
        QVERIFY(!math::render(tex, displayStyle()).isNull());
        QVERIFY2(!math::lastRenderFellBack(), qPrintable(tex));
    }
#ifdef Q_OS_WIN
    QVERIFY(math::workerContained()); // Not a worker that could do more.
#endif
}

QTEST_MAIN(TestMath)
#include "test_math.moc"
