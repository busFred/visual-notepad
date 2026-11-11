// Renders TeX math to an image. Uses MicroTeX when available; otherwise
// draws the TeX source in an italic serif face so the text stays readable.
//
// MicroTeX never runs in the app itself. It was not written for formulas
// from strangers: one in eight random formulas crashes it, some by writing
// to memory that is not its own, others run or grow without end. So the app
// starts itself a second time as a worker ("visnotepad --math-worker"),
// which does nothing but turn formulas into pictures, and asks it over a
// pipe. A worker that crashes, takes too long or answers nonsense is killed
// and started again, and the formula is shown as its source text.
#pragma once

#include <QColor>
#include <QImage>
#include <QString>

namespace vn::math {

struct Style
{
    qreal pixelSize = 16;
    QColor color = Qt::black;
    qreal devicePixelRatio = 1;
    bool display = false;
};

// Never returns a null image for non-empty input.
QImage render(const QString &tex, const Style &style);

// Prepends \newcommand definitions for the user's macros from Settings.
QString withMacros(const QString &tex);

// The program started as the worker (with "--math-worker"): the app's own
// executable, which main() names. Tests name it through VN_MATH_WORKER or
// this call. With none, formulas are shown as source text.
void setWorkerProgram(const QString &program);

// What "visnotepad --math-worker" runs: answers the formulas on standard
// input until it is closed. Never returns.
[[noreturn]] void runWorker(int &argc, char **argv);

// Whether the last render() had to show source text, and how many times a
// worker was started; for tests.
bool lastRenderFellBack();
int workerStarts();

} // namespace vn::math
