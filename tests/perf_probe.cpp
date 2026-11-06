// Manual timing probe for large files (not a test): perf_probe <file>...
//
// Times are wall-clock and, in brackets, the GUI thread's CPU time, which a
// loaded machine disturbs much less. Set VN_PROBE_LINES=a,b,... (0-based
// lines) to also time keystrokes on each of those lines (in a table row, in
// its last cell) and caret moves between them; the first move away from an
// edited table is shown apart, as the visual view realigns the table then.
// VN_PROBE_KEY=backspace types Backspace instead of "a" (VN_PROBE_COUNT
// times, 20 by default): a row that gets narrower, not wider.
#include "DocumentTab.h"
#include "TextFile.h"
#include "editor/TextView.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <ctime>

namespace {

double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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
            const int count = qEnvironmentVariableIsSet("VN_PROBE_COUNT") ? qEnvironmentVariableIntValue("VN_PROBE_COUNT") : 20;
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
                lap.restart();
                for (int k = 0; k < count; ++k)
                    QTest::keyClick(view, backspace ? Qt::Key_Backspace : Qt::Key_A);
                qInfo().noquote() << "line" << line << "keystroke ms" << lap.take(count);
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
        view->scrollToLine(90000);
        view->grab();
        qInfo().noquote() << argv[i] << "jump+paint ms" << lap.take();
    }
}
