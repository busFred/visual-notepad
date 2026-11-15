// Lays TeX math out as shapes (outlines of its glyphs, its rules), which the
// app draws at any size without losing sharpness. Uses MicroTeX when
// available; otherwise the shapes are the TeX source in an italic serif face,
// so the text stays readable.
//
// MicroTeX never runs in the app itself. It was not written for formulas
// from strangers: one in eight random formulas crashes it, some by writing
// to memory that is not its own, others run or grow without end. So the app
// starts itself a second time as a worker ("visnotepad --math-worker"),
// which does nothing but turn formulas into shapes, and asks it over a pipe.
// A worker that crashes, takes too long or answers nonsense is killed and
// started again, and the formula is shown as its source text. Its answers
// are plain lists of numbers, each checked before the app uses it.
#pragma once

#include <QColor>
#include <QImage>
#include <QList>
#include <QPainterPath>
#include <QString>

#include <memory>

class QPainter;
class QProcess;

namespace vn::math {

// The size, in pixels, a formula is laid out at; it is drawn scaled from it.
inline constexpr qreal layoutPixelSize = 100;

// A formula as filled shapes, in pixels at layoutPixelSize, with the left end
// of its baseline at (0, 0).
struct Formula
{
    struct Shape
    {
        QPainterPath path;
        QRgb color = 0;
        // In the colour of the text around it (the formula's own colour),
        // not one the formula names.
        bool textColor = true;
    };
    QList<Shape> shapes;
    qreal width = 0;
    qreal ascent = 0; // Above the baseline.
    qreal depth = 0; // Below it.
    // Shown as its source text: MicroTeX could not lay it out.
    bool fellBack = false;

    qreal height() const { return ascent + depth; }
    // Path elements in all shapes: what drawing it costs.
    qsizetype elements() const;
    // Draws it `scale` times the size it was laid out at, the left end of its
    // baseline at `origin`.
    void draw(QPainter &painter, const QPointF &origin, qreal scale, const QColor &text) const;
};

// Starts the worker now, if it is not running, without waiting for it, so
// the first formula of a document with math does not wait for it to come up.
// Does nothing without a worker program.
void prepareWorker();

// The formula's shapes; never empty for non-empty input (source text when
// MicroTeX cannot draw it). Asks the worker every time: cache the result.
std::shared_ptr<const Formula> layout(const QString &tex, bool display);

struct Style
{
    qreal pixelSize = 16;
    QColor color = Qt::black;
    qreal devicePixelRatio = 1;
    bool display = false;
};

// A picture of the formula at `style`, with a pixel of clear space around
// it; never null for non-empty input.
QImage toImage(const Formula &formula, const Style &style);

// toImage(*layout(tex, style.display), style).
QImage render(const QString &tex, const Style &style);

// The row of a picture from toImage() that the formula's baseline is on, in
// device pixels from the top (the bottom edge for any other picture).
int baselineRow(const QImage &picture);

// Prepends \newcommand definitions for the user's macros from Settings.
QString withMacros(const QString &tex);

// The program started as the worker (with "--math-worker"): the app's own
// executable, which main() names. Tests name it through VN_MATH_WORKER or
// this call. With none, formulas are shown as source text.
void setWorkerProgram(const QString &program);

// What "visnotepad --math-worker" runs: answers the formulas on standard
// input until it is closed. Never returns.
[[noreturn]] void runWorker(int &argc, char **argv);

// Takes the user's files, writing and the network from the calling
// process, for good (the worker does this once MicroTeX has started).
// Returns which locks hold: 1 the file system (Linux Landlock; on Windows
// the AppContainer of confineWorkerProcess(): only fonts, Windows' and the
// app's own files can be read), 2 sockets (seccomp) or, on Windows, the
// network (that AppContainer, which is given none), 4 writing (Windows:
// low integrity, also outside the AppContainer), 8 new processes (Windows:
// a job of one process); 0 on systems without them. For tests and the
// worker.
int lockDownWorker();

// Has `process` (the worker, or a test standing in for it) start the way
// the worker does: on Windows in an AppContainer that can read nothing of
// the user's, reach no network and start no process, which is let read
// the program, the libraries this process has loaded and MicroTeX's
// resources. Nothing elsewhere. For tests and the worker.
void confineWorkerProcess(QProcess &process);

// Whether the last layout() had to show source text, how many times a
// worker was started, and whether the last one started in its
// AppContainer (Windows); for tests.
bool lastRenderFellBack();
int workerStarts();
bool workerContained();

} // namespace vn::math
