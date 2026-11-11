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
    void fractionIsTallerThanText();
    void displayIsLargerThanInline();
    void baselineIsReported_data();
    void baselineIsReported();
    void shapesScaleToAnySize();
    void colourIsApplied();
    void devicePixelRatioScalesPixels();
    void invalidTexFallsBack();
    void macroFromSettings();
    void formulasThatBreakMicroTeXAreShownAsSource_data();
    void formulasThatBreakMicroTeXAreShownAsSource();
    void aFormulaThatNeverEndsIsGivenUp();
    void longSourceIsNotDrawnInFull();
    void aFormulaLeavesNothingBehind_data();
    void aFormulaLeavesNothingBehind();
    void saveScreenshots();
};

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
    QElapsedTimer timer;
    timer.start();
    math::render(QStringLiteral("\\newcommand{\\zz}{\\zz}\\zz"), inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QVERIFY2(timer.elapsed() < 6000, qPrintable(QString::number(timer.elapsed())));
    // Asked again, it is not tried again.
    timer.restart();
    const int starts = math::workerStarts();
    math::render(QStringLiteral("\\newcommand{\\zz}{\\zz}\\zz"), inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QCOMPARE(math::workerStarts(), starts);
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    // The next one that never ends gets less time. (A new worker is
    // started first: that takes over a second on a slow machine.)
    math::render(QStringLiteral("y"), inlineStyle());
    QVERIFY(!math::lastRenderFellBack());
    timer.restart();
    math::render(QStringLiteral("\\newcommand{\\zw}{\\zw\\zw}\\zw"), inlineStyle());
    QVERIFY(math::lastRenderFellBack());
    QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    math::render(QStringLiteral("x^2"), inlineStyle());
    QVERIFY(!math::lastRenderFellBack());
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

void TestMath::aFormulaLeavesNothingBehind()
{
    QFETCH(QString, tex);
    const QString plain = QStringLiteral("\\frac{\\alpha}{b}+x^2");
    // Where one worker draws formula after formula (Windows, macOS), it is
    // replaced after tex, and on Windows a few pixels of a glyph depend on
    // which formulas a worker has drawn before. So both pictures are a new
    // worker's first: tex comes before the first one too.
    math::render(tex, inlineStyle());
    const QImage before = math::render(plain, inlineStyle());
    QVERIFY(!math::lastRenderFellBack());
    math::render(tex, inlineStyle());
    const QImage after = math::render(plain, inlineStyle());
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

QTEST_MAIN(TestMath)
#include "test_math.moc"
