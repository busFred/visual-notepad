// Manual timing probe for large files (not a test): perf_probe <file>...
//
// Times are wall-clock and, in brackets, the GUI thread's CPU time, which a
// loaded machine disturbs much less. Set VN_PROBE_LINES=a,b,... (0-based
// lines) to also time keystrokes on each of those lines (in a table row, in
// its last cell) and caret moves between them; the first move away from an
// edited table is shown apart, as the visual view realigns the table then.
// VN_PROBE_KEY=backspace types Backspace instead of "a" (VN_PROBE_COUNT
// times, 20 by default): a row that gets narrower, not wider; =enter types
// Enter then "a" (a new list item, timed per pair). In the visual
// editor the keystrokes go into rendered text; VN_PROBE_OPEN=1 opens the
// line's block as source first. VN_PROBE_PAINT=1 paints after each one.
// VN_PROBE_READ=1 times read mode on a Markdown file (Visual mode, locked,
// Columns): turning it on, 50 page turns, 50 pages back, 50 nudges of a line
// and a resize, each painted. VN_PROBE_COLUMNS=1 does the same for the
// source view's columns on any file (Source mode), with Word Wrap on and off,
// from the middle of the file, and times keys there: typing in the last
// column, Enter at the bottom of the last column (the view moves on a column
// each time) and Down from the first line on screen to past the last (into
// each next column, then a column on), with the slowest key of those.
#include "DocumentTab.h"
#include "TextFile.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QToolButton>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace {

// CPU time of the calling thread.
double cpuMs()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    const auto ticks = [](const FILETIME &t) { return (quint64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 1e4; // 100 ns units.
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

// Wall and CPU milliseconds since the last lap.
struct Lap
{
    QElapsedTimer wall;
    double cpu = 0;
    Lap() { restart(); }
    void restart()
    {
        wall.restart();
        cpu = cpuMs();
    }
    QString take(int count = 1)
    {
        const QString s = QStringLiteral("%1 (%2 cpu)").arg(wall.nsecsElapsed() / 1e6 / count, 0, 'f', 2).arg((cpuMs() - cpu) / count, 0, 'f', 2);
        restart();
        return s;
    }
};

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // As the application does (main.cpp): big files are coloured in slices.
    vn::TextFile::setHighlightInSlices(true);
    for (int i = 1; i < argc; ++i) {
        Lap lap;
        vn::DocumentTab tab;
        tab.resize(900, 700);
        tab.load(QString::fromLocal8Bit(argv[i]));
        tab.show();
        (void)QTest::qWaitForWindowExposed(&tab);
        tab.editor()->grab();
        qInfo().noquote() << argv[i] << "open+paint ms" << lap.take();
        vn::TextView *view = tab.editor();
        view->setFocus();
        for (int k = 0; k < 20; ++k) {
            QTest::keyClick(view, Qt::Key_A);
            view->grab();
        }
        qInfo().noquote() << argv[i] << "per keystroke+paint ms" << lap.take(20);

        const QString probeLines = qEnvironmentVariable("VN_PROBE_LINES");
        if (!probeLines.isEmpty()) {
            QList<int> lines;
            for (const QString &s : probeLines.split(u','))
                lines.append(s.toInt());
            const bool backspace = qEnvironmentVariable("VN_PROBE_KEY") == QLatin1String("backspace");
            const bool enter = qEnvironmentVariable("VN_PROBE_KEY") == QLatin1String("enter");
            const int count = qEnvironmentVariableIsSet("VN_PROBE_COUNT") ? qEnvironmentVariableIntValue("VN_PROBE_COUNT") : 20;
            const bool paint = qEnvironmentVariableIntValue("VN_PROBE_PAINT") != 0;
            const auto place = [&](int line) {
                QTextCursor c(view->document()->findBlockByNumber(line));
                c.movePosition(QTextCursor::EndOfBlock);
                // Inside the last cell of a table row, not after its pipe.
                if (c.block().text().endsWith(u'|'))
                    c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
                view->setTextCursor(c);
            };
            for (int line : lines) {
                place(line);
                if (auto *visual = qobject_cast<vn::VisualView *>(view);
                    visual && qEnvironmentVariableIntValue("VN_PROBE_OPEN") != 0)
                    visual->openBlockAtCaret();
                view->grab();
                lap.restart();
                for (int k = 0; k < count; ++k) {
                    if (enter)
                        QTest::keyClick(view, Qt::Key_Return);
                    QTest::keyClick(view, backspace ? Qt::Key_Backspace : Qt::Key_A);
                    if (paint)
                        view->grab();
                }
                const QString took = lap.take(count);
                const auto *visual = qobject_cast<vn::VisualView *>(view);
                qInfo().noquote() << "line" << line << "keystroke ms" << took
                                  << (visual ? (visual->isBlockOpen() ? "(as source)" : "(rendered)") : "");
            }
            if (lines.size() >= 2) {
                // Back to the last line typed on, then away: the first move
                // out of an edited table realigns it (in the visual view).
                place(lines.last());
                lap.restart();
                place(lines.first());
                qInfo().noquote() << "first move away from line" << lines.last() << "ms" << lap.take();
                for (int k = 0; k < 20; ++k)
                    place(lines.at(k % lines.size()));
                qInfo().noquote() << "caret move between lines ms" << lap.take(20);
                // Between the second line and the one after it (both in the
                // same block when that is a table): no block opens or closes.
                for (int k = 0; k < 20; ++k)
                    place(lines.at(1) + k % 2);
                qInfo().noquote() << "caret move within the second line's block ms" << lap.take(20);
            }
        }
        if (qEnvironmentVariableIntValue("VN_PROBE_ZOOM") != 0) {
            // Painting the whole tab (both views in split mode) again,
            // nothing changed; then zooming in and back out a step at a
            // time, each step painted.
            tab.grab();
            lap.restart();
            for (int k = 0; k < 20; ++k)
                tab.grab();
            qInfo().noquote() << argv[i] << "repaint ms" << lap.take(20);
            const QList<int> steps = {110, 125, 150, 175, 200, 175, 150, 125, 110, 100};
            for (int zoom : steps) {
                tab.setZoom(zoom);
                tab.grab();
            }
            qInfo().noquote() << argv[i] << "zoom step+paint ms" << lap.take(int(steps.size()));
        }
        if (qEnvironmentVariableIntValue("VN_PROBE_READ") != 0 && tab.isMarkdown()) {
            tab.setViewMode(vn::DocumentTab::ViewMode::Visual);
            vn::VisualView *visual = tab.visualView();
            visual->grab();
            lap.restart();
            for (QToolButton *button : tab.findChildren<QToolButton *>()) {
                if (button->toolTip() == QLatin1String("Read-only"))
                    button->click();
            }
            tab.setColumns(true);
            visual->grab();
            // (A large document shows its columns once its parse is in.)
            QElapsedTimer waited;
            waited.start();
            while (!visual->showsReadColumns() && waited.elapsed() < 120000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                visual->grab();
            }
            qInfo().noquote() << argv[i] << "read mode on+paint ms" << lap.take() << visual->readColumnCount()
                              << "columns";
            for (int k = 0; k < 50; ++k) {
                visual->readPageForward();
                visual->grab();
            }
            qInfo().noquote() << argv[i] << "page turn+paint ms" << lap.take(50);
            for (int k = 0; k < 50; ++k) {
                visual->readPageBack();
                visual->grab();
            }
            qInfo().noquote() << argv[i] << "page back+paint ms" << lap.take(50);
            for (int k = 0; k < 50; ++k) {
                visual->readNudge(1);
                visual->grab();
            }
            qInfo().noquote() << argv[i] << "nudge+paint ms" << lap.take(50);
            // Pages back with nothing remembered to go back to.
            for (int k = 0; k < 10; ++k) {
                visual->readPageBack();
                visual->grab();
            }
            qInfo().noquote() << argv[i] << "page back (worked out)+paint ms" << lap.take(10);
            tab.resize(1400, 900);
            visual->grab();
            qInfo().noquote() << argv[i] << "resize+paint ms" << lap.take() << visual->readColumnCount() << "columns";
            visual->readToEnd();
            visual->grab();
            qInfo().noquote() << argv[i] << "to the end+paint ms" << lap.take();
            const auto [first, last] = visual->readLineRange();
            qInfo().noquote() << argv[i] << "last screen: lines" << first + 1 << "to" << last + 1;
        }
        if (qEnvironmentVariableIntValue("VN_PROBE_COLUMNS") != 0) {
            tab.setViewMode(vn::DocumentTab::ViewMode::Source);
            vn::TextView *source = tab.sourceView();
            const int middle = tab.document()->blockCount() / 2;
            for (const bool wrap : {true, false}) {
                tab.setColumns(false);
                source->setWordWrap(wrap);
                source->scrollToLine(middle);
                source->grab();
                const QString name = QString::fromLocal8Bit(argv[i]) + (wrap ? QStringLiteral(" (wrap)") : QStringLiteral(" (no wrap)"));
                lap.restart();
                tab.setColumns(true);
                source->grab();
                qInfo().noquote() << name << "columns on+paint ms" << lap.take() << source->shownColumnCount() << "columns";
                for (int k = 0; k < 50; ++k) {
                    source->columnsPageForward();
                    source->grab();
                }
                qInfo().noquote() << name << "page turn+paint ms" << lap.take(50);
                for (int k = 0; k < 50; ++k) {
                    source->columnsPageBack();
                    source->grab();
                }
                qInfo().noquote() << name << "page back+paint ms" << lap.take(50);
                for (int k = 0; k < 50; ++k) {
                    source->columnsNudge(1);
                    source->grab();
                }
                qInfo().noquote() << name << "nudge+paint ms" << lap.take(50);
                for (int k = 0; k < 10; ++k) {
                    source->columnsPageBack();
                    source->grab();
                }
                qInfo().noquote() << name << "page back (worked out)+paint ms" << lap.take(10);
                for (int k = 0; k < 20; ++k) {
                    QTest::keyClick(source, Qt::Key_Down);
                    source->grab();
                }
                qInfo().noquote() << name << "caret down+paint ms" << lap.take(20);
                tab.resize(1400, 900);
                source->grab();
                qInfo().noquote() << name << "resize+paint ms" << lap.take() << source->shownColumnCount() << "columns";
                const auto toLine = [source](int line, bool end) {
                    QTextCursor cursor(source->document()->findBlockByNumber(line));
                    if (end)
                        cursor.movePosition(QTextCursor::EndOfBlock);
                    source->setTextCursor(cursor);
                    source->grab();
                };
                toLine(source->columnLineRange().second, true);
                lap.restart();
                for (int k = 0; k < 50; ++k) {
                    QTest::keyClick(source, Qt::Key_A);
                    source->grab();
                }
                qInfo().noquote() << name << "typing in the last column+paint ms" << lap.take(50);
                // (Placing the caret is not timed.)
                double wall = 0;
                double cpu = 0;
                double slowest = 0;
                for (int k = 0; k < 20; ++k) {
                    toLine(source->columnLineRange().second, true);
                    QElapsedTimer one;
                    one.start();
                    const double cpuBefore = cpuMs();
                    QTest::keyClick(source, Qt::Key_Return);
                    source->grab();
                    const double ms = one.nsecsElapsed() / 1e6;
                    wall += ms;
                    cpu += cpuMs() - cpuBefore;
                    slowest = qMax(slowest, ms);
                }
                qInfo().noquote() << name << "Enter at the bottom (a column on)+paint ms"
                                  << QStringLiteral("%1 (%2 cpu)").arg(wall / 20, 0, 'f', 2).arg(cpu / 20, 0, 'f', 2)
                                  << "slowest" << slowest;
                toLine(source->columnLineRange().first, false);
                const auto [onFirst, onLast] = source->columnLineRange();
                const int downs = onLast - onFirst + 1 + 10;
                slowest = 0;
                lap.restart();
                for (int k = 0; k < downs; ++k) {
                    QElapsedTimer one;
                    one.start();
                    QTest::keyClick(source, Qt::Key_Down);
                    source->grab();
                    slowest = qMax(slowest, one.nsecsElapsed() / 1e6);
                }
                qInfo().noquote() << name << "Down across columns+paint ms" << lap.take(downs) << "slowest" << slowest
                                  << "over" << downs << "keys";
                lap.restart();
                source->columnsToEnd();
                source->grab();
                qInfo().noquote() << name << "to the end+paint ms" << lap.take();
                tab.resize(900, 700);
                tab.setColumns(false);
                source->grab();
                qInfo().noquote() << name << "columns off+paint ms" << lap.take();
            }
            source->setWordWrap(true);
        }
        view->scrollToLine(90000);
        view->grab();
        qInfo().noquote() << argv[i] << "jump+paint ms" << lap.take();
    }
}
