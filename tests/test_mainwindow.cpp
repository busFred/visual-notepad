// Offscreen checks of the main window. Set VN_SCREENSHOT=<file.png> to also
// save a rendering of the window.
#include "Commands.h"
#include "MainWindow.h"
#include "Theme.h"

#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include "DocumentTab.h"
#include "Settings.h"
#include "WelcomeView.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"

#include <QLineEdit>
#include <QMimeData>

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
    void fileDropOnEmptyWindowOpensIt();
    void fileDropOnTextTabOpensIt();
    void vscodeEditorShortcuts();
    void menuMnemonics();

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

// With no tabs, a file dragged from the file manager lands on the welcome
// page. Qt sends the drag's moves and drop to whichever widget accepted its
// entry; this used to bounce between the editor group and the window until
// the stack overflowed.
void TestMainWindow::fileDropOnEmptyWindowOpensIt()
{
    const QString path = write(QStringLiteral("dropped.md"), "# Dropped\n");
    vn::MainWindow window;
    window.resize(800, 500);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCOMPARE(window.tabCount(), 0);
    auto *welcome = window.findChild<vn::WelcomeView *>();
    QVERIFY(welcome && welcome->isVisible());

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    const QPoint pos = welcome->rect().center();
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
    QDragEnterEvent enter(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(welcome, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(welcome, &move);
    QVERIFY(move.isAccepted());
    QDropEvent drop(QPointF(pos), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(welcome, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(window.tabCount(), 1);
    QCOMPARE(window.windowTitle(), QStringLiteral("dropped.md — Visual Notepad"));
}

// A plain-text editor does not take file drops itself; they open as tabs.
void TestMainWindow::fileDropOnTextTabOpensIt()
{
    const QString text = write(QStringLiteral("plain.txt"), "plain\n");
    const QString path = write(QStringLiteral("dropped2.md"), "# Dropped\n");
    vn::MainWindow window;
    window.resize(800, 500);
    QVERIFY(window.openFile(text));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QWidget *target = window.currentTab()->editor()->viewport();

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    const QPoint pos = target->rect().center();
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
    QDragEnterEvent enter(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent drop(QPointF(pos), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(window.tabCount(), 2);
    QCOMPARE(window.windowTitle(), QStringLiteral("dropped2.md — Visual Notepad"));
}

// The VS Code editor commands are reachable through their default keys.
void TestMainWindow::vscodeEditorShortcuts()
{
    const QString path = write(QStringLiteral("keys.txt"), "alpha  \nbeta\ngamma\n");
    vn::MainWindow window;
    window.resize(800, 500);
    QVERIFY(window.openFile(path));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    vn::TextView *editor = window.currentTab()->editor();
    editor->setFocus();
    QVERIFY(QTest::qWaitFor([&] { return editor->hasFocus(); }));
    const auto actionFor = [&window](const QString &id) -> QAction * {
        const QString text = vn::command(id)->menuText;
        for (QAction *action : window.actions()) {
            if (action->text() == text)
                return action;
        }
        return nullptr;
    };

    for (const char *id : {"selection.addCursorsToLineEnds", "selection.expandLine", "selection.cursorUndo",
                           "selection.cursorRedo", "selection.moveLastToNextMatch", "edit.indentLines",
                           "edit.outdentLines", "edit.trimTrailingWhitespace", "edit.findNextSelection",
                           "edit.findPreviousSelection", "view.scrollPageUp", "view.scrollPageDown"})
        QVERIFY2(actionFor(QLatin1String(id)), id);
    QVERIFY(actionFor(QStringLiteral("selection.cursorUndo"))->shortcuts().isEmpty());

    // Ctrl+L selects the line, again adds one.
    QTest::keyClick(editor, Qt::Key_L, Qt::ControlModifier);
    QCOMPARE(editor->textCursor().selectedText().replace(QChar::ParagraphSeparator, u'|'), QStringLiteral("alpha  |"));
    QTest::keyClick(editor, Qt::Key_L, Qt::ControlModifier);
    QCOMPARE(editor->textCursor().selectedText().replace(QChar::ParagraphSeparator, u'|'),
             QStringLiteral("alpha  |beta|"));

    // Shift+Alt+I: a caret at the end of each selected line.
    QTest::keyClick(editor, Qt::Key_I, Qt::ShiftModifier | Qt::AltModifier);
    QCOMPARE(editor->cursorCount(), 2);

    // Ctrl+] / Ctrl+[ indent and outdent.
    editor->setTextCursor(QTextCursor(editor->document()));
    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::ControlModifier);
    QVERIFY(editor->document()->toPlainText().startsWith(u' '));
    QTest::keyClick(editor, Qt::Key_BracketLeft, Qt::ControlModifier);
    QVERIFY(editor->document()->toPlainText().startsWith(QStringLiteral("alpha")));

    // Ctrl+K Ctrl+X trims trailing whitespace.
    QTest::keyClick(editor, Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    QVERIFY(editor->document()->toPlainText().startsWith(QStringLiteral("alpha\nbeta")));
}

void TestMainWindow::menuMnemonics()
{
    vn::MainWindow window;
    QStringList barKeys;
    for (QAction *menuAction : window.menuBar()->actions()) {
        QMenu *menu = menuAction->menu();
        QVERIFY(menu);
        barKeys << QKeySequence::mnemonic(menuAction->text()).toString();
        // A repeated letter only selects in QMenu; it must be unique to trigger.
        QStringList keys;
        for (QAction *item : menu->actions()) {
            if (item->isSeparator() || item->text() == QLatin1String("Reset Zoom")) // No free letter
                continue;
            const QKeySequence key = QKeySequence::mnemonic(item->text());
            QVERIFY2(!key.isEmpty(), qPrintable(item->text()));
            QVERIFY2(!keys.contains(key.toString()), qPrintable(item->text()));
            keys << key.toString();
        }
    }
    barKeys.removeDuplicates();
    QCOMPARE(barKeys.size(), window.menuBar()->actions().size());
    for (const vn::Command &command : vn::commands())
        QVERIFY2(!command.text.contains(QLatin1Char('&')), qPrintable(command.text));
    QCOMPARE(vn::command(QStringLiteral("file.saveAs"))->text, QStringLiteral("Save As…"));
    QCOMPARE(vn::command(QStringLiteral("file.saveAs"))->menuText, QStringLiteral("Save &As…"));
    QCOMPARE(vn::menuForMnemonic(QKeySequence(QStringLiteral("Alt+O"))), QStringLiteral("Format"));
    QVERIFY(vn::menuForMnemonic(QKeySequence(QStringLiteral("Alt+C"))).isEmpty());
    QVERIFY(vn::menuForMnemonic(QKeySequence(QStringLiteral("Shift+Alt+F"))).isEmpty());
}

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
