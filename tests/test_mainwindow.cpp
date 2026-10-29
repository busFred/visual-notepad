// Offscreen checks of the main window. Set VN_SCREENSHOT=<file.png> to also
// save a rendering of the window.
#include "MainWindow.h"
#include "Theme.h"

#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include "DocumentTab.h"
#include "Settings.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"

#include <QLineEdit>

#include <QTextDocument>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

class TestMainWindow : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void markdownShowsOutline();
    void textFileHidesOutline();
    void crlfIsPreserved();

private:
    QString write(const QString &name, const QByteArray &content);

    QTemporaryDir m_dir;
};

void TestMainWindow::initTestCase()
{
    QVERIFY(m_dir.isValid());
    vn::theme::apply(*qApp);
}

QString TestMainWindow::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

void TestMainWindow::markdownShowsOutline()
{
    const QString path = write(QStringLiteral("notes.md"), "# Title\n\nSome *text*.\n\n## Section\n\n```python\n"
                                                           "from __future__ import annotations\n```\n\n## Other\n");
    vn::MainWindow window;
    QVERIFY(window.openFile(path));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *outline = window.findChild<QTreeWidget *>();
    QVERIFY(outline);
    QVERIFY(window.findChild<QDockWidget *>()->isVisible());
    QCOMPARE(outline->topLevelItemCount(), 1);
    QCOMPARE(outline->topLevelItem(0)->text(0), QStringLiteral("Title"));
    QCOMPARE(outline->topLevelItem(0)->childCount(), 2);
    QCOMPARE(window.windowTitle(), QStringLiteral("notes.md — Visual Notepad"));

    const QString shot = qEnvironmentVariable("VN_SCREENSHOT");
    if (!shot.isEmpty()) {
        QVERIFY(window.grab().save(shot));
        // Also save each menu-bar menu, opened under its title.
        auto *bar = window.menuBar();
        const auto actions = bar->actions();
        for (QAction *action : actions) {
            QMenu *menu = action->menu();
            menu->popup(window.mapToGlobal(bar->actionGeometry(action).bottomLeft()));
            QVERIFY(QTest::qWaitForWindowExposed(menu));
            QString name = action->text();
            name.remove(u'&');
            QVERIFY(menu->grab().save(QString(shot).replace(QStringLiteral(".png"), u'-' + name + u".png")));
            menu->hide();
        }
        // And the find/replace bar.
        auto *tab = window.findChild<vn::DocumentTab *>();
        tab->showFind(true);
        tab->findBar()->setQuery(QStringLiteral("Section"));
        tab->findBar()->replaceField()->setText(QStringLiteral("Part"));
        QVERIFY(window.grab().save(QString(shot).replace(QStringLiteral(".png"), QStringLiteral("-find.png"))));
    }
}

void TestMainWindow::textFileHidesOutline()
{
    const QString path = write(QStringLiteral("script.py"), "print('hi')\n");
    vn::MainWindow window;
    QVERIFY(window.openFile(path));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(!window.findChild<QDockWidget *>()->isVisible());
}

void TestMainWindow::crlfIsPreserved()
{
    const QString path = write(QStringLiteral("dos.txt"), "a\r\nb\r\n");
    vn::MainWindow window;
    QVERIFY(window.openFile(path));
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *editor = window.findChild<vn::TextView *>();
    QVERIFY(editor);
    QCOMPARE(editor->document()->toPlainText(), QStringLiteral("a\nb\n"));
    QTextCursor end(editor->document());
    end.movePosition(QTextCursor::End);
    end.insertText(QStringLiteral("\nc"));

    QTest::keySequence(editor, QKeySequence::Save);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("a\r\nb\r\n\r\nc"));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
