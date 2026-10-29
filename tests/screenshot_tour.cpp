// Renders the app in a few representative states to PNGs (not a test):
// screenshot_tour <docs dir> <output dir>
#include "DocumentTab.h"
#include "EditorGroup.h"
#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

static void settle()
{
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Visual Notepad"));
    QTemporaryDir settingsDir;
    vn::Settings::instance().useFile(settingsDir.filePath(QStringLiteral("settings.ini")));
    vn::theme::apply(app);
    const QString docs = QString::fromLocal8Bit(argv[1]);
    const QString out = QString::fromLocal8Bit(argv[2]);

    auto *window = new vn::MainWindow;
    window->resize(1180, 860);
    window->openFile(docs + QStringLiteral("/notes.md"));
    window->show();
    (void)QTest::qWaitForWindowExposed(window);
    settle();

    // 1. Visual Editor, editing the "Install" heading's paragraph.
    vn::DocumentTab *notes = window->currentTab();
    vn::TextView *visual = notes->editor();
    visual->setFocus();
    window->activateWindow();
    QTextCursor c(notes->document()->findBlockByNumber(19)); // "Run the build..."
    c.movePosition(QTextCursor::EndOfBlock);
    visual->setTextCursor(c);
    settle();
    window->grab().save(out + QStringLiteral("/1-visual-editor.png"));

    // 1b. Scrolled to the math and diagram, everything rendered.
    window->mainGroup()->setFocus();
    visual->scrollToLine(33);
    settle();
    window->grab().save(out + QStringLiteral("/5-math-mermaid.png"));
    visual->scrollToLine(0);

    // 2. Source / Visual split.
    notes->setViewMode(vn::DocumentTab::ViewMode::SourceVisual);
    settle();
    notes->sourceView()->setFocus();
    notes->sourceView()->setTextCursor(QTextCursor(notes->document()->findBlockByNumber(5)));
    settle();
    window->grab().save(out + QStringLiteral("/2-source-visual.png"));

    // 3. Python with multiple cursors and the find/replace bar.
    window->openFile(docs + QStringLiteral("/build.py"));
    vn::DocumentTab *py = window->currentTab();
    vn::TextView *code = py->editor();
    code->setFocus();
    QTextCursor target(py->document()->findBlockByNumber(13));
    target.movePosition(QTextCursor::NextWord);
    target.movePosition(QTextCursor::NextWord);
    target.movePosition(QTextCursor::NextWord);
    code->setTextCursor(target);
    code->selectAllOccurrences(); // every "target"
    settle();
    py->showFind(true);
    py->findBar()->setQuery(QStringLiteral("build"));
    py->findBar()->replaceField()->setText(QStringLiteral("out"));
    settle();
    window->grab().save(out + QStringLiteral("/3-multicursor-find.png"));

    // 4. Preferences > Markdown.
    vn::PreferencesDialog::showPage(window, QStringLiteral("markdown"));
    settle();
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *dialog = qobject_cast<QDialog *>(w); dialog && dialog->isVisible())
            dialog->grab().save(out + QStringLiteral("/4-preferences.png"));
    }
    return 0;
}
