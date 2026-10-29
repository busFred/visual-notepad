#include "MainWindow.h"

#include "EditorGroup.h"
#include "Theme.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"
#include "vn_core_bridge/lib.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QSizeGrip>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

namespace vn {

namespace {

constexpr int outlineLineRole = Qt::UserRole;
constexpr int outlineWidth = 200;
constexpr int zoomStepPercent = 10;

QString productName()
{
    return QStringLiteral("Visual Notepad");
}

QKeySequence keys(const char *sequence)
{
    return QKeySequence(QString::fromLatin1(sequence));
}

// Platform-specific default, following VS Code's Windows and Linux keymaps.
QKeySequence platformKeys(const char *windows, const char *other)
{
#ifdef Q_OS_WIN
    Q_UNUSED(other);
    return keys(windows);
#else
    Q_UNUSED(windows);
    return keys(other);
#endif
}

// Size grip drawn as the dotted triangle from the mockup.
class DotGrip : public QSizeGrip
{
public:
    using QSizeGrip::QSizeGrip;

    QSize sizeHint() const override { return {16, 16}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QPoint origin(width() - 14, height() - 14);
        for (const QPoint &dot : {QPoint(9, 9), QPoint(5, 9), QPoint(1, 9), QPoint(9, 5), QPoint(5, 5), QPoint(9, 1)})
            painter.fillRect(QRect(origin + dot, QSize(2, 2)), theme::gripDots);
    }
};

// Status bar column: text with a divider on its left, as in Notepad.
class StatusCell : public QLabel
{
public:
    explicit StatusCell(bool divider, QWidget *parent)
        : QLabel(parent)
        , m_divider(divider)
    {
        setContentsMargins(10, 0, 10, 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QLabel::paintEvent(event);
        if (m_divider) {
            QPainter painter(this);
            painter.fillRect(QRect(0, 0, 1, height()), theme::divider);
        }
    }

private:
    bool m_divider;
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_groups(new QSplitter(Qt::Horizontal, this))
    , m_group(new EditorGroup(m_groups))
    , m_outlineTimer(new QTimer(this))
{
    setWindowTitle(productName());
    resize(1100, 720);

    m_groups->setHandleWidth(1);
    m_groups->setChildrenCollapsible(false);
    m_groups->addWidget(m_group);
    setCentralWidget(m_groups);
    connect(m_group, &EditorGroup::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_group, &EditorGroup::tabCloseRequested, this, &MainWindow::closeTab);

    m_outlineTimer->setSingleShot(true);
    m_outlineTimer->setInterval(250);
    connect(m_outlineTimer, &QTimer::timeout, this, &MainWindow::updateOutline);

    createActions();
    createOutline();
    createMenus();
    createTabActions();
    createStatusBar();
    onCurrentTabChanged();
}

MainWindow::~MainWindow()
{
    // Child widgets are destroyed after this destructor; stop them signalling
    // back into a half-destroyed window (e.g. a tab bar's currentChanged).
    const auto children = findChildren<QObject *>();
    for (QObject *child : children)
        child->disconnect(this);
}

void MainWindow::createActions()
{
    m_modeGroup = new QActionGroup(this);
    const auto addMode = [this](const QString &text, const char *icon, DocumentTab::ViewMode mode) {
        auto *action = new QAction(QIcon(QStringLiteral(":/vn/icons/%1.svg").arg(QLatin1String(icon))), text, this);
        action->setCheckable(true);
        action->setIconVisibleInMenu(false);
        m_modeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setViewMode(mode); });
        return action;
    };
    m_modeSource = addMode(tr("Source"), "source", DocumentTab::ViewMode::Source);
    m_modeSourceVisual = addMode(tr("Source / Visual"), "source-visual", DocumentTab::ViewMode::SourceVisual);
    m_modeSourceVisual->setShortcut(keys("Ctrl+K, V"));
    m_modeVisual = addMode(tr("Visual Editor"), "visual", DocumentTab::ViewMode::Visual);
    // Milestone 3: the visual modes need the Markdown editor port.
    m_modeSourceVisual->setEnabled(false);
    m_modeVisual->setEnabled(false);

    m_outlineAction = new QAction(tr("Outline"), this);
    m_outlineAction->setCheckable(true);
    m_outlineAction->setChecked(true);
    m_outlineAction->setShortcut(keys("Ctrl+B"));
    connect(m_outlineAction, &QAction::toggled, this, &MainWindow::updateOutlineVisibility);

    m_splitAction = new QAction(QIcon(QStringLiteral(":/vn/icons/split-editor.svg")), tr("Split Editor"), this);
    m_splitAction->setCheckable(true);
    m_splitAction->setIconVisibleInMenu(false);
    m_splitAction->setShortcut(keys("Ctrl+\\"));
    connect(m_splitAction, &QAction::toggled, this, &MainWindow::toggleSplitEditor);

    m_wrapAction = new QAction(tr("Word Wrap"), this);
    m_wrapAction->setCheckable(true);
    m_wrapAction->setChecked(true);
    m_wrapAction->setShortcut(keys("Alt+Z"));
    connect(m_wrapAction, &QAction::toggled, this, [this](bool on) {
        for (auto *editor : findChildren<TextView *>())
            editor->setWordWrap(on);
    });

    m_statusBarAction = new QAction(tr("Status Bar"), this);
    m_statusBarAction->setCheckable(true);
    m_statusBarAction->setChecked(true);
    connect(m_statusBarAction, &QAction::toggled, this, [this](bool on) { statusBar()->setVisible(on); });

    m_formatAction = new QAction(tr("Format Document"), this);
    m_formatAction->setShortcut(platformKeys("Shift+Alt+F", "Ctrl+Shift+I"));
    m_formatAction->setEnabled(false); // Planned formatter.
}

void MainWindow::createMenus()
{
    // Keep the menu bar inside the window on every desktop rather than a
    // global/dbus menu.
    menuBar()->setNativeMenuBar(false);

    // Items whose feature is not built yet are listed but disabled.
    const auto todo = [](QAction *action) { action->setEnabled(false); };
    const auto forward = [this](void (TextView::*slot)()) {
        return [this, slot] {
            if (auto *tab = currentTab())
                (tab->editor()->*slot)();
        };
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("New File"), QKeySequence::New, this, &MainWindow::newFile);
    file->addAction(tr("New Window"), keys("Ctrl+Shift+N"), this, [] {
        auto *window = new MainWindow;
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->newFile();
        window->show();
    });
    file->addAction(tr("Open…"), QKeySequence::Open, this, &MainWindow::openWithDialog);
    file->addMenu(tr("Open Recent"))->setEnabled(false);
    file->addSeparator();
    file->addAction(tr("Save"), QKeySequence::Save, this, [this] {
        if (auto *tab = currentTab())
            save(tab);
    });
    file->addAction(tr("Save As…"), keys("Ctrl+Shift+S"), this, [this] {
        if (auto *tab = currentTab())
            saveAs(tab);
    });
    file->addSeparator();
    file->addAction(tr("Close Tab"), keys("Ctrl+W"), this, [this] { closeTab(m_group->currentIndex()); });
    file->addAction(tr("Close Window"), keys("Ctrl+Shift+W"), this, &QWidget::close);
    file->addSeparator();
    file->addAction(tr("Quit"), keys("Ctrl+Q"), qApp, &QApplication::closeAllWindows);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("Undo"), QKeySequence::Undo, this, forward(&TextView::undo));
    QAction *redo = edit->addAction(tr("Redo"), this, forward(&TextView::redo));
    redo->setShortcuts({keys("Ctrl+Y"), keys("Ctrl+Shift+Z")});
    edit->addSeparator();
    edit->addAction(tr("Cut"), QKeySequence::Cut, this, forward(&TextView::cut));
    edit->addAction(tr("Copy"), QKeySequence::Copy, this, forward(&TextView::copy));
    edit->addAction(tr("Paste"), QKeySequence::Paste, this, forward(&TextView::paste));
    edit->addSeparator();
    edit->addAction(tr("Find"), keys("Ctrl+F"), this, [this] {
        if (auto *tab = currentTab())
            tab->showFind(false);
    });
    edit->addAction(tr("Replace"), keys("Ctrl+H"), this, [this] {
        if (auto *tab = currentTab())
            tab->showFind(true);
    });
    edit->addAction(tr("Find Next"), keys("F3"), this, [this] {
        if (auto *tab = currentTab())
            tab->findBar()->findNext();
    });
    edit->addAction(tr("Find Previous"), keys("Shift+F3"), this, [this] {
        if (auto *tab = currentTab())
            tab->findBar()->findPrevious();
    });
    todo(edit->addAction(tr("Go to Line…"), keys("Ctrl+G"), this, [] {}));
    edit->addSeparator();
    edit->addAction(tr("Toggle Line Comment"), keys("Ctrl+/"), this, forward(&TextView::toggleLineComment));
    edit->addAction(m_formatAction);
    edit->addSeparator();
    todo(edit->addAction(tr("Preferences…"), keys("Ctrl+,"), this, [] {}));

    QMenu *selection = menuBar()->addMenu(tr("&Selection"));
    selection->addAction(tr("Select All"), QKeySequence::SelectAll, this, forward(&TextView::selectAll));
    selection->addAction(tr("Expand Selection"), keys("Shift+Alt+Right"), this, forward(&TextView::expandSelection));
    selection->addAction(tr("Shrink Selection"), keys("Shift+Alt+Left"), this, forward(&TextView::shrinkSelection));
    selection->addSeparator();
    selection->addAction(tr("Add Cursor Above"), platformKeys("Ctrl+Alt+Up", "Shift+Alt+Up"), this,
                         forward(&TextView::addCursorAbove));
    selection->addAction(tr("Add Cursor Below"), platformKeys("Ctrl+Alt+Down", "Shift+Alt+Down"), this,
                         forward(&TextView::addCursorBelow));
    selection->addAction(tr("Add Next Occurrence"), keys("Ctrl+D"), this, forward(&TextView::addNextOccurrence));
    selection->addAction(tr("Select All Occurrences"), keys("Ctrl+Shift+L"), this,
                         forward(&TextView::selectAllOccurrences));
    selection->addSeparator();
    selection->addAction(tr("Copy Line Up"), platformKeys("Shift+Alt+Up", "Ctrl+Shift+Alt+Up"), this,
                         forward(&TextView::copyLinesUp));
    selection->addAction(tr("Copy Line Down"), platformKeys("Shift+Alt+Down", "Ctrl+Shift+Alt+Down"), this,
                         forward(&TextView::copyLinesDown));
    selection->addAction(tr("Move Line Up"), keys("Alt+Up"), this, forward(&TextView::moveLinesUp));
    selection->addAction(tr("Move Line Down"), keys("Alt+Down"), this, forward(&TextView::moveLinesDown));
    selection->addAction(tr("Delete Line"), keys("Ctrl+Shift+K"), this, forward(&TextView::deleteLines));

    QMenu *view = menuBar()->addMenu(tr("&View"));
    view->addAction(m_outlineAction);
    view->addSeparator();
    view->addAction(m_modeSource);
    view->addAction(m_modeSourceVisual);
    view->addAction(m_modeVisual);
    view->addSeparator();
    view->addAction(m_splitAction);
    view->addAction(tr("Move Tab to New Window"), keys("Ctrl+K, O"), this, &MainWindow::moveTabToNewWindow);
    view->addSeparator();
    view->addAction(m_wrapAction);
    view->addAction(m_statusBarAction);
    view->addSeparator();
    view->addAction(tr("Zoom In"), keys("Ctrl+="), this, [this] { zoom(1); });
    view->addAction(tr("Zoom Out"), keys("Ctrl+-"), this, [this] { zoom(-1); });
    view->addAction(tr("Reset Zoom"), keys("Ctrl+0"), this, [this] { zoom(0); });
    view->addAction(tr("Full Screen"), keys("F11"), this,
                    [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    todo(help->addAction(tr("Keyboard Shortcuts"), keys("Ctrl+K, Ctrl+S"), this, [] {}));
    help->addSeparator();
    help->addAction(tr("About Visual Notepad"), this, [this] {
        QMessageBox::about(this, tr("About Visual Notepad"),
                           tr("Visual Notepad %1").arg(QApplication::applicationVersion()));
    });
}

void MainWindow::createTabActions()
{
    // Mode dropdown: icon plus the current mode's name; items have no icons.
    m_modeButton = new QToolButton(this);
    m_modeButton->setObjectName(QStringLiteral("ModeButton"));
    m_modeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_modeButton->setPopupMode(QToolButton::InstantPopup);
    m_modeButton->setIconSize({14, 14});
    auto *modeMenu = new QMenu(m_modeButton);
    modeMenu->addActions(m_modeGroup->actions());
    m_modeButton->setMenu(modeMenu);
    m_group->addAction(m_modeButton);

    auto *split = new QToolButton(this);
    split->setObjectName(QStringLiteral("IconButton"));
    split->setDefaultAction(m_splitAction);
    split->setFixedSize(24, 24);
    split->setIconSize({16, 16});
    split->setToolTip(tr("Split Editor (%1)").arg(m_splitAction->shortcut().toString(QKeySequence::NativeText)));
    m_group->addAction(split);
}

void MainWindow::createOutline()
{
    m_outline = new QTreeWidget(this);
    m_outline->setHeaderHidden(true);
    m_outline->setIndentation(14);
    connect(m_outline, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (auto *tab = currentTab())
            tab->goToLine(item->data(0, outlineLineRole).toInt());
    });
    connect(m_outline, &QTreeWidget::itemClicked, m_outline, &QTreeWidget::itemActivated);

    auto *title = new QWidget(this);
    title->setObjectName(QStringLiteral("OutlineTitle"));
    title->setAttribute(Qt::WA_StyledBackground);
    auto *titleLayout = new QHBoxLayout(title);
    titleLayout->setContentsMargins(10, 4, 4, 4);
    auto *label = new QLabel(tr("OUTLINE"), title);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.85);
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
    label->setFont(font);
    auto *close = new QToolButton(title);
    close->setIcon(QIcon(QStringLiteral(":/vn/icons/close-hover.svg")));
    close->setIconSize({14, 14});
    close->setFixedSize(18, 18);
    close->setToolTip(tr("Hide Outline (%1)").arg(keys("Ctrl+B").toString(QKeySequence::NativeText)));
    connect(close, &QToolButton::clicked, this, [this] { m_outlineAction->setChecked(false); });
    titleLayout->addWidget(label);
    titleLayout->addStretch(1);
    titleLayout->addWidget(close);

    m_outlineDock = new QDockWidget(tr("Outline"), this);
    m_outlineDock->setObjectName(QStringLiteral("outline"));
    m_outlineDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    m_outlineDock->setAllowedAreas(Qt::LeftDockWidgetArea);
    m_outlineDock->setTitleBarWidget(title);
    m_outlineDock->setWidget(m_outline);
    addDockWidget(Qt::LeftDockWidgetArea, m_outlineDock);
    resizeDocks({m_outlineDock}, {outlineWidth}, Qt::Horizontal);
}

void MainWindow::createStatusBar()
{
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setContentsMargins(0, 0, 0, 0);
    bool first = true;
    for (QLabel **label : {&m_position, &m_zoom, &m_lineEnding, &m_encoding, &m_language}) {
        *label = new StatusCell(!first, this);
        statusBar()->addPermanentWidget(*label);
        first = false;
    }
    statusBar()->addPermanentWidget(new DotGrip(this));
}

DocumentTab *MainWindow::currentTab() const
{
    return m_group->currentTab();
}

void MainWindow::newFile()
{
    addTab(new DocumentTab);
}

bool MainWindow::openFile(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (int i = 0; i < m_group->count(); ++i) {
        if (!canonical.isEmpty() && QFileInfo(m_group->tab(i)->path()).canonicalFilePath() == canonical) {
            m_group->setCurrentIndex(i);
            return true;
        }
    }

    auto *tab = new DocumentTab;
    if (!tab->load(path)) {
        QMessageBox::warning(this, tr("Open File"),
                             tr("Could not open %1:\n%2").arg(QDir::toNativeSeparators(path), tab->errorString()));
        delete tab;
        return false;
    }
    addTab(tab);
    return true;
}

void MainWindow::adoptTab(DocumentTab *tab)
{
    addTab(tab);
}

void MainWindow::addTab(DocumentTab *tab)
{
    tab->editor()->setWordWrap(m_wrapAction->isChecked());
    connect(tab, &DocumentTab::modificationChanged, this, [this, tab] { updateTabTitle(tab); });
    connect(tab, &DocumentTab::cursorPositionChanged, this, [this, tab] {
        if (tab == currentTab())
            updateStatus();
    });
    connect(tab, &DocumentTab::textChanged, this, [this, tab] {
        if (tab == currentTab() && tab->isMarkdown())
            m_outlineTimer->start();
    });

    const int index = m_group->addTab(tab, tab->displayName());
    updateTabTitle(tab);
    m_group->setCurrentIndex(index);
    applyZoom();
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
    m_group->setCurrentIndex(m_group->indexOf(tab));
    const auto answer = QMessageBox::warning(
        this, productName(), tr("Do you want to save the changes you made to %1?").arg(tab->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return save(tab);
    return answer == QMessageBox::Discard;
}

bool MainWindow::closeTab(int index)
{
    DocumentTab *tab = m_group->tab(index);
    if (!tab || !confirmDiscard(tab))
        return false;
    m_group->removeTab(m_group->indexOf(tab));
    tab->deleteLater();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < m_group->count(); ++i) {
        if (!confirmDiscard(m_group->tab(i))) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

void MainWindow::moveTabToNewWindow()
{
    DocumentTab *tab = currentTab();
    if (!tab)
        return;
    tab->disconnect(this);
    m_group->removeTab(m_group->currentIndex());

    auto *window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->adoptTab(tab);
    window->show();
}

void MainWindow::toggleSplitEditor(bool on)
{
    // The new group starts empty; moving tabs between groups comes with tab
    // drag and drop.
    if (on && !m_sideGroup) {
        m_sideGroup = new EditorGroup(m_groups);
        m_groups->addWidget(m_sideGroup);
        const int half = m_groups->width() / 2;
        m_groups->setSizes({half, m_groups->width() - half});
    } else if (!on && m_sideGroup) {
        m_sideGroup->deleteLater();
        m_sideGroup = nullptr;
    }
}

void MainWindow::setViewMode(DocumentTab::ViewMode mode)
{
    if (auto *tab = currentTab())
        tab->setViewMode(mode);
    updateModeControls();
}

void MainWindow::zoom(int steps)
{
    m_zoomPercent = steps == 0 ? 100 : qBound(30, m_zoomPercent + steps * zoomStepPercent, 500);
    applyZoom();
}

void MainWindow::applyZoom()
{
    QFont font = theme::editorFont();
    font.setPointSizeF(font.pointSizeF() * m_zoomPercent / 100.0);
    for (int i = 0; i < m_group->count(); ++i)
        m_group->tab(i)->editor()->setFont(font);
    updateStatus();
}

void MainWindow::onCurrentTabChanged()
{
    DocumentTab *tab = currentTab();
    setWindowTitle(tab ? tr("%1 — %2").arg(tab->displayName(), productName()) : productName());
    updateStatus();
    updateOutline();
    updateOutlineVisibility();
    updateModeControls();
}

void MainWindow::updateTabTitle(DocumentTab *tab)
{
    const int index = m_group->indexOf(tab);
    if (index < 0)
        return;
    m_group->setTabText(index, tab->isModified() ? QStringLiteral("● ") + tab->displayName() : tab->displayName());
    m_group->setTabToolTip(index, QDir::toNativeSeparators(tab->path()));
    if (tab == currentTab())
        setWindowTitle(tr("%1 — %2").arg(tab->displayName(), productName()));
}

void MainWindow::updateStatus()
{
    DocumentTab *tab = currentTab();
    if (!tab) {
        for (QLabel *label : {m_position, m_zoom, m_lineEnding, m_encoding, m_language})
            label->clear();
        return;
    }
    const TextView *editor = tab->editor();
    const QTextCursor cursor = editor->textCursor();
    QString position = tr("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1);
    if (editor->cursorCount() > 1)
        position += u' ' + tr("(%1 selections)").arg(editor->cursorCount());
    else if (cursor.hasSelection())
        position += u' ' + tr("(%1 selected)").arg(cursor.selectionEnd() - cursor.selectionStart());
    m_position->setText(position);
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

void MainWindow::updateModeControls()
{
    DocumentTab *tab = currentTab();
    const bool markdown = tab && tab->isMarkdown();
    m_modeButton->setVisible(markdown);
    m_modeSource->setEnabled(markdown);
    if (!markdown) {
        // An exclusive group cannot be fully unchecked; drop exclusivity briefly.
        m_modeGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::None);
        for (QAction *action : m_modeGroup->actions())
            action->setChecked(false);
        m_modeGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::Exclusive);
        return;
    }
    QAction *current = m_modeSource;
    if (tab->viewMode() == DocumentTab::ViewMode::SourceVisual)
        current = m_modeSourceVisual;
    else if (tab->viewMode() == DocumentTab::ViewMode::Visual)
        current = m_modeVisual;
    current->setChecked(true);
    m_modeButton->setIcon(current->icon());
    m_modeButton->setText(current->text());
}

} // namespace vn
