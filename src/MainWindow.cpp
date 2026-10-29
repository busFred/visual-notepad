#include "MainWindow.h"

#include "DocumentTab.h"
#include "vn_core_bridge/lib.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>

namespace vn {

namespace {

constexpr int outlineLineRole = Qt::UserRole;
constexpr int zoomStepPercent = 10;

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_tabs(new QTabWidget(this))
    , m_outlineTimer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("Visual Notepad"));
    resize(1100, 720);

    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    setCentralWidget(m_tabs);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

    m_outlineTimer->setSingleShot(true);
    m_outlineTimer->setInterval(250);
    connect(m_outlineTimer, &QTimer::timeout, this, &MainWindow::updateOutline);

    createOutline();
    createMenus();
    createStatusBar();
}

MainWindow::~MainWindow()
{
    // Child widgets are destroyed after this destructor; stop them signalling
    // back into a half-destroyed window (e.g. QTabWidget::currentChanged).
    const auto children = findChildren<QObject *>();
    for (QObject *child : children)
        child->disconnect(this);
}

void MainWindow::createMenus()
{
    // Keep the menu bar inside the window on every desktop, matching the
    // agreed look rather than a global/dbus menu.
    menuBar()->setNativeMenuBar(false);

    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New File"), QKeySequence::New, this, &MainWindow::newFile);
    file->addAction(tr("&Open File..."), QKeySequence::Open, this, &MainWindow::openWithDialog);
    file->addSeparator();
    file->addAction(tr("&Save"), QKeySequence::Save, this, [this] {
        if (auto *tab = currentTab())
            save(tab);
    });
    file->addAction(tr("Save &As..."), QKeySequence(QStringLiteral("Ctrl+Shift+S")), this, [this] {
        if (auto *tab = currentTab())
            saveAs(tab);
    });
    file->addSeparator();
    file->addAction(tr("&Close Editor"), QKeySequence(QStringLiteral("Ctrl+W")), this,
                    [this] { closeTab(m_tabs->currentIndex()); });
    file->addAction(tr("E&xit"), QKeySequence(QStringLiteral("Ctrl+Q")), this, &QWidget::close);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    const auto forward = [this](void (QPlainTextEdit::*slot)()) {
        return [this, slot] {
            if (auto *tab = currentTab())
                (tab->editor()->*slot)();
        };
    };
    edit->addAction(tr("&Undo"), QKeySequence::Undo, this, forward(&QPlainTextEdit::undo));
    edit->addAction(tr("&Redo"), QKeySequence(QStringLiteral("Ctrl+Y")), this, forward(&QPlainTextEdit::redo));
    edit->addSeparator();
    edit->addAction(tr("Cu&t"), QKeySequence::Cut, this, forward(&QPlainTextEdit::cut));
    edit->addAction(tr("&Copy"), QKeySequence::Copy, this, forward(&QPlainTextEdit::copy));
    edit->addAction(tr("&Paste"), QKeySequence::Paste, this, forward(&QPlainTextEdit::paste));
    edit->addSeparator();
    QAction *prefs = edit->addAction(tr("P&references..."));
    prefs->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    prefs->setEnabled(false); // Milestone 2.

    QMenu *selection = menuBar()->addMenu(tr("&Selection"));
    selection->addAction(tr("Select &All"), QKeySequence::SelectAll, this, forward(&QPlainTextEdit::selectAll));

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_outlineAction);
    view->addSeparator();
    view->addAction(tr("Zoom &In"), QKeySequence(QStringLiteral("Ctrl+=")), this, [this] { zoom(1); });
    view->addAction(tr("Zoom &Out"), QKeySequence(QStringLiteral("Ctrl+-")), this, [this] { zoom(-1); });
    view->addAction(tr("&Reset Zoom"), QKeySequence(QStringLiteral("Ctrl+0")), this, [this] { zoom(0); });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Visual Notepad"), this, [this] {
        QMessageBox::about(this, tr("About Visual Notepad"),
                           tr("Visual Notepad %1").arg(QApplication::applicationVersion()));
    });
}

void MainWindow::createOutline()
{
    m_outline = new QTreeWidget(this);
    m_outline->setHeaderHidden(true);
    m_outline->setRootIsDecorated(true);
    connect(m_outline, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (auto *tab = currentTab())
            tab->goToLine(item->data(0, outlineLineRole).toInt());
    });
    connect(m_outline, &QTreeWidget::itemClicked, m_outline, &QTreeWidget::itemActivated);

    m_outlineDock = new QDockWidget(tr("OUTLINE"), this);
    m_outlineDock->setObjectName(QStringLiteral("outline"));
    m_outlineDock->setFeatures(QDockWidget::DockWidgetClosable);
    m_outlineDock->setAllowedAreas(Qt::LeftDockWidgetArea);
    m_outlineDock->setWidget(m_outline);
    addDockWidget(Qt::LeftDockWidgetArea, m_outlineDock);
    resizeDocks({m_outlineDock}, {240}, Qt::Horizontal);

    // The action records the user's choice; the dock is only shown for
    // Markdown tabs.
    m_outlineAction = new QAction(tr("&Outline"), this);
    m_outlineAction->setCheckable(true);
    m_outlineAction->setChecked(true);
    m_outlineAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    connect(m_outlineAction, &QAction::toggled, this, &MainWindow::updateOutlineVisibility);
    connect(m_outlineDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        // Closing the dock with its own button turns the setting off.
        if (!visible && !m_outlineDock->isVisible() && currentTab() && currentTab()->isMarkdown())
            m_outlineAction->setChecked(false);
    });
}

void MainWindow::createStatusBar()
{
    const auto addLabel = [this](QLabel *&label) {
        label = new QLabel(this);
        statusBar()->addPermanentWidget(label);
    };
    addLabel(m_position);
    addLabel(m_zoom);
    addLabel(m_lineEnding);
    addLabel(m_encoding);
    addLabel(m_language);
    statusBar()->setSizeGripEnabled(true);
}

DocumentTab *MainWindow::currentTab() const
{
    return qobject_cast<DocumentTab *>(m_tabs->currentWidget());
}

DocumentTab *MainWindow::tabAt(int index) const
{
    return qobject_cast<DocumentTab *>(m_tabs->widget(index));
}

void MainWindow::newFile()
{
    addTab(new DocumentTab(m_repository, m_tabs));
}

bool MainWindow::openFile(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (!canonical.isEmpty() && QFileInfo(tabAt(i)->path()).canonicalFilePath() == canonical) {
            m_tabs->setCurrentIndex(i);
            return true;
        }
    }

    auto *tab = new DocumentTab(m_repository, m_tabs);
    if (!tab->load(path)) {
        QMessageBox::warning(this, tr("Open File"),
                             tr("Could not open %1:\n%2").arg(QDir::toNativeSeparators(path), tab->errorString()));
        delete tab;
        return false;
    }
    addTab(tab);
    return true;
}

void MainWindow::addTab(DocumentTab *tab)
{
    connect(tab, &DocumentTab::modificationChanged, this, [this, tab] { updateTabTitle(tab); });
    connect(tab, &DocumentTab::cursorPositionChanged, this, [this, tab] {
        if (tab == currentTab())
            updateStatus();
    });
    connect(tab, &DocumentTab::textChanged, this, [this, tab] {
        if (tab == currentTab() && tab->isMarkdown())
            m_outlineTimer->start();
    });

    const int index = m_tabs->addTab(tab, tab->displayName());
    updateTabTitle(tab);
    m_tabs->setCurrentIndex(index);
    tab->editor()->setFocus();
}

void MainWindow::openWithDialog()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open File"));
    for (const QString &path : paths)
        openFile(path);
}

bool MainWindow::save(DocumentTab *tab)
{
    if (tab->path().isEmpty())
        return saveAs(tab);
    if (!tab->save(tab->path())) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2")
                                                   .arg(QDir::toNativeSeparators(tab->path()), tab->errorString()));
        return false;
    }
    updateTabTitle(tab);
    return true;
}

bool MainWindow::saveAs(DocumentTab *tab)
{
    const QString suggested = tab->path().isEmpty() ? tr("Untitled.md") : tab->path();
    const QString path = QFileDialog::getSaveFileName(this, tr("Save As"), suggested);
    if (path.isEmpty())
        return false;
    if (!tab->save(path)) {
        QMessageBox::warning(this, tr("Save As"),
                             tr("Could not save %1:\n%2").arg(QDir::toNativeSeparators(path), tab->errorString()));
        return false;
    }
    updateTabTitle(tab);
    onCurrentTabChanged();
    return true;
}

bool MainWindow::confirmDiscard(DocumentTab *tab)
{
    if (!tab->isModified())
        return true;
    m_tabs->setCurrentWidget(tab);
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Visual Notepad"),
        tr("Do you want to save the changes you made to %1?").arg(tab->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return save(tab);
    return answer == QMessageBox::Discard;
}

bool MainWindow::closeTab(int index)
{
    DocumentTab *tab = tabAt(index);
    if (!tab || !confirmDiscard(tab))
        return false;
    m_tabs->removeTab(m_tabs->indexOf(tab));
    tab->deleteLater();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (!confirmDiscard(tabAt(i))) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

void MainWindow::zoom(int steps)
{
    m_zoomPercent = steps == 0 ? 100 : qBound(30, m_zoomPercent + steps * zoomStepPercent, 500);
    for (int i = 0; i < m_tabs->count(); ++i) {
        QPlainTextEdit *editor = tabAt(i)->editor();
        QFont font = editor->font();
        font.setPointSizeF(QFont().pointSizeF() * m_zoomPercent / 100.0);
        editor->setFont(font);
    }
    updateStatus();
}

void MainWindow::onCurrentTabChanged()
{
    DocumentTab *tab = currentTab();
    setWindowTitle(tab ? tr("%1 - Visual Notepad").arg(tab->displayName()) : QStringLiteral("Visual Notepad"));
    updateStatus();
    updateOutline();
    updateOutlineVisibility();
}

void MainWindow::updateTabTitle(DocumentTab *tab)
{
    const int index = m_tabs->indexOf(tab);
    if (index < 0)
        return;
    m_tabs->setTabText(index, tab->isModified() ? QStringLiteral("● ") + tab->displayName() : tab->displayName());
    m_tabs->setTabToolTip(index, QDir::toNativeSeparators(tab->path()));
}

void MainWindow::updateStatus()
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        for (QLabel *label : {m_position, m_zoom, m_lineEnding, m_encoding, m_language})
            label->clear();
        return;
    }
    const QTextCursor cursor = tab->editor()->textCursor();
    m_position->setText(tr("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));
    m_zoom->setText(QStringLiteral("%1%").arg(m_zoomPercent));
    m_lineEnding->setText(tab->lineEnding() == DocumentTab::LineEnding::CRLF ? tr("Windows (CRLF)")
                                                                             : tr("Unix (LF)"));
    m_encoding->setText(QStringLiteral("UTF-8"));
    m_language->setText(tab->languageName());
}

void MainWindow::updateOutline()
{
    m_outline->clear();
    DocumentTab *tab = currentTab();
    if (!tab || !tab->isMarkdown())
        return;

    const QByteArray utf8 = tab->text().toUtf8();
    const auto headings = core::outline(rust::Str(utf8.constData(), utf8.size()));

    // Nest each heading under the nearest previous heading of lower level.
    QList<QPair<int, QTreeWidgetItem *>> stack;
    for (const auto &heading : headings) {
        const int level = heading.level;
        while (!stack.isEmpty() && stack.last().first >= level)
            stack.removeLast();
        auto *item = stack.isEmpty() ? new QTreeWidgetItem(m_outline) : new QTreeWidgetItem(stack.last().second);
        item->setText(0, QString::fromUtf8(heading.text.data(), heading.text.size()));
        item->setData(0, outlineLineRole, static_cast<int>(heading.line));
        stack.append({level, item});
    }
    m_outline->expandAll();
}

void MainWindow::updateOutlineVisibility()
{
    DocumentTab *tab = currentTab();
    m_outlineDock->setVisible(m_outlineAction->isChecked() && tab && tab->isMarkdown());
}

} // namespace vn
