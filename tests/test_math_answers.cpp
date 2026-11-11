// What the app does with the math worker's answers (see MathRenderer.cpp):
// a formula may take the worker over, so whatever it answers is checked, and
// any answer that is not one a worker makes is refused (the formula shown as
// its source text). fake_math_worker answers each formula as its text says.
#include "markdown/MathRenderer.h"

#include <QTest>

using namespace vn;

class TestMathAnswers : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void aWellFormedAnswerIsDrawn();
    void malformedAnswersAreRefused_data();
    void malformedAnswersAreRefused();
};

void TestMathAnswers::initTestCase()
{
    math::setWorkerProgram(QString::fromUtf8(FAKE_MATH_WORKER));
}

void TestMathAnswers::aWellFormedAnswerIsDrawn()
{
    const auto formula = math::layout(QStringLiteral("valid"), false);
    QVERIFY(!math::lastRenderFellBack());
    QVERIFY(!formula->fellBack);
    QCOMPARE(formula->width, 50.0);
    QCOMPARE(formula->ascent, 40.0);
    QCOMPARE(formula->depth, 10.0);
    QCOMPARE(formula->shapes.size(), 1);
    QVERIFY(formula->shapes.first().textColor);
    // Drawn: a box from the top of the picture down to the baseline and past.
    math::Style style;
    style.pixelSize = math::layoutPixelSize;
    const QImage image = math::toImage(*formula, style);
    QCOMPARE(math::baselineRow(image), 41); // A pixel of margin, then the ascent.
    QCOMPARE(qAlpha(image.pixel(25, 30)), 255);
}

void TestMathAnswers::malformedAnswersAreRefused_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("a size that is not a number") << QStringLiteral("nan");
    QTest::newRow("a size past the limit") << QStringLiteral("huge");
    QTest::newRow("a shape not starting with a move") << QStringLiteral("line-first");
    QTest::newRow("a curve without its end") << QStringLiteral("half-curve");
    QTest::newRow("an element of no type") << QStringLiteral("bad-type");
    QTest::newRow("a point far outside") << QStringLiteral("far-point");
    QTest::newRow("more elements than bytes") << QStringLiteral("claims-more");
    QTest::newRow("bytes after the shapes") << QStringLiteral("trailing");
    QTest::newRow("more bytes than allowed") << QStringLiteral("oversize");
    QTest::newRow("an answer of no kind") << QStringLiteral("wrong-status");
}

void TestMathAnswers::malformedAnswersAreRefused()
{
    QFETCH(QString, mode);
    const auto formula = math::layout(mode, false);
    QVERIFY(math::lastRenderFellBack());
    QVERIFY(formula->fellBack);
    QVERIFY(formula->width > 0 && formula->width <= 500 * math::layoutPixelSize);
    // The next worker answers as before.
    math::layout(QStringLiteral("valid"), false);
    QVERIFY(!math::lastRenderFellBack());
}

QTEST_MAIN(TestMathAnswers)
#include "test_math_answers.moc"
