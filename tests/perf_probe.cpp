// Manual timing probe for large files (not a test): perf_probe <file>...
#include "DocumentTab.h"
#include "editor/TextView.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    for (int i = 1; i < argc; ++i) {
        QElapsedTimer t;
        t.start();
        vn::DocumentTab tab;
        tab.resize(900, 700);
        tab.load(QString::fromLocal8Bit(argv[i]));
        tab.show();
        QTest::qWaitForWindowExposed(&tab);
        tab.editor()->grab();
        qInfo() << argv[i] << "open+paint ms" << t.restart();
        vn::TextView *view = tab.editor();
        view->setFocus();
        for (int k = 0; k < 20; ++k) {
            QTest::keyClick(view, Qt::Key_A);
            view->grab();
        }
        qInfo() << argv[i] << "per keystroke+paint ms" << t.restart() / 20.0;
        view->scrollToLine(90000);
        view->grab();
        qInfo() << argv[i] << "jump+paint ms" << t.restart();
    }
}
