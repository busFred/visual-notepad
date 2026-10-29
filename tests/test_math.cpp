// TeX math rendering through MicroTeX, including the text fallback.
#include "Settings.h"
#include "markdown/MathRenderer.h"

#include <QDir>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

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
    void colourIsApplied();
    void devicePixelRatioScalesPixels();
    void invalidTexFallsBack();
    void macroFromSettings();
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
