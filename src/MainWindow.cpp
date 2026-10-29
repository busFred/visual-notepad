#include "MainWindow.h"

#include "Commands.h"
#include "EditorGroup.h"
#include "PreferencesDialog.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"
#include "vn_core_bridge/lib.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QClipboard>
#include <QDesktopServices>
#include <QDockWidget>
#include <QGuiApplication>
#include <QMimeDatabase>
#include <QPushButton>
#include <QUrl>
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

    m_zoomPercent = Settings::instance().number(key::zoom);
    m_outlineTimer->setSingleShot(true);
    m_outlineTimer->setInterval(250);
    connect(m_outlineTimer, &QTimer::timeout, this, &MainWindow::updateOutline);

    createActions();
    createOutline();
    createMenus();
    createTabActions();
    createStatusBar();
    statusBar()->setVisible(m_statusBarAction->isChecked());
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

QAction *MainWindow::makeAction(const QString &id)
{
    const Command *cmd = command(id);
    Q_ASSERT(cmd);
    auto *action = new QAction(cmd->text, this);
    action->setShortcuts(shortcutsFor(id));
    action->setShortcutContext(Qt::WindowShortcut);
    action->setIconVisibleInMenu(false);
    addAction(action); // Keeps shortcuts working while the menu bar is hidden.
    m_actions.insert(id, action);
    return action;
}

QAction *MainWindow::makeAction(const QString &id, std::function<void()> slot)
{
    QAction *action = makeAction(id);
    connect(action, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    return action;
}

QAction *MainWindow::action(const QString &id) const
{
    return m_actions.value(id);
}

void MainWindow::createActions()
{
    // Calls a TextView slot on the current tab's editor.
    const auto onEditor = [this](void (TextView::*slot)()) {
        return [this, slot] {
            if (auto *tab = currentTab())
                (tab->editor()->*slot)();
        };
    };
    const auto onTab = [this](void (DocumentTab::*slot)()) {
        return [this, slot] {
            if (auto *tab = currentTab())
                (tab->*slot)();
        };
    };

    makeAction(QStringLiteral("file.new"), [this] { newFile(); });
    makeAction(QStringLiteral("file.newWindow"), [] {
        auto *window = new MainWindow;
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->newFile();
        window->show();
    });
    makeAction(QStringLiteral("file.open"), [this] { openWithDialog(); });
    makeAction(QStringLiteral("file.save"), [this] {
        if (auto *tab = currentTab())
            save(tab);
    });
    makeAction(QStringLiteral("file.saveAs"), [this] {
        if (auto *tab = currentTab())
            saveAs(tab);
    });
    makeAction(QStringLiteral("file.closeTab"), [this] { closeTab(m_group->currentIndex()); });
    makeAction(QStringLiteral("file.closeWindow"), [this] { close(); });
    makeAction(QStringLiteral("file.quit"), [] { QApplication::closeAllWindows(); });

    makeAction(QStringLiteral("edit.undo"), onEditor(&TextView::undo));
    makeAction(QStringLiteral("edit.redo"), onEditor(&TextView::redo));
    makeAction(QStringLiteral("edit.cut"), onEditor(&TextView::cut));
    makeAction(QStringLiteral("edit.copy"), onEditor(&TextView::copy));
    makeAction(QStringLiteral("edit.paste"), onEditor(&TextView::paste));
    makeAction(QStringLiteral("edit.find"), [this] {
        if (auto *tab = currentTab())
            tab->showFind(false);
    });
    makeAction(QStringLiteral("edit.replace"), [this] {
        if (auto *tab = currentTab())
            tab->showFind(true);
    });
    makeAction(QStringLiteral("edit.findNext"), [this] {
        if (auto *tab = currentTab())
            tab->findBar()->findNext();
    });
    makeAction(QStringLiteral("edit.findPrevious"), [this] {
        if (auto *tab = currentTab())
            tab->findBar()->findPrevious();
    });
    makeAction(QStringLiteral("edit.goToLine"), [this] { goToLine(); });
    makeAction(QStringLiteral("edit.toggleComment"), onEditor(&TextView::toggleLineComment));
    makeAction(QStringLiteral("edit.format"))->setEnabled(false); // Planned formatter.
    makeAction(QStringLiteral("edit.preferences"), [this] { showPreferences(QString()); });

    makeAction(QStringLiteral("selection.selectAll"), onEditor(&TextView::selectAll));
    makeAction(QStringLiteral("selection.expand"), onEditor(&TextView::expandSelection));
    makeAction(QStringLiteral("selection.shrink"), onEditor(&TextView::shrinkSelection));
    makeAction(QStringLiteral("selection.addCursorAbove"), onEditor(&TextView::addCursorAbove));
    makeAction(QStringLiteral("selection.addCursorBelow"), onEditor(&TextView::addCursorBelow));
    makeAction(QStringLiteral("selection.addNextOccurrence"), onEditor(&TextView::addNextOccurrence));
    makeAction(QStringLiteral("selection.selectAllOccurrences"), onEditor(&TextView::selectAllOccurrences));
    makeAction(QStringLiteral("selection.copyLineUp"), onEditor(&TextView::copyLinesUp));
    makeAction(QStringLiteral("selection.copyLineDown"), onEditor(&TextView::copyLinesDown));
    makeAction(QStringLiteral("selection.moveLineUp"), onEditor(&TextView::moveLinesUp));
    makeAction(QStringLiteral("selection.moveLineDown"), onEditor(&TextView::moveLinesDown));
    makeAction(QStringLiteral("selection.deleteLine"), onEditor(&TextView::deleteLines));
    Q_UNUSED(onTab);

    // View toggles mirror settings, so every window stays in step.
    const auto bindSetting = [this](const QString &id, const char *settingKey) {
        QAction *a = makeAction(id);
        a->setCheckable(true);
        a->setChecked(Settings::instance().flag(settingKey));
        connect(a, &QAction::triggered, this,
                [settingKey](bool on) { Settings::instance().setValue(settingKey, on); });
        return a;
    };
    m_outlineAction = bindSetting(QStringLiteral("view.outline"), key::outline);
    m_wrapAction = bindSetting(QStringLiteral("view.wordWrap"), key::wordWrap);
    m_statusBarAction = bindSetting(QStringLiteral("view.statusBar"), key::statusBar);

    m_modeGroup = new QActionGroup(this);
    const auto addMode = [this](const QString &id, const char *icon, DocumentTab::ViewMode mode) {
        QAction *a = makeAction(id, [this, mode] { setViewMode(mode); });
        a->setIcon(QIcon(QStringLiteral(":/vn/icons/%1.svg").arg(QLatin1String(icon))));
        a->setCheckable(true);
        m_modeGroup->addAction(a);
        return a;
    };
    m_modeSource = addMode(QStringLiteral("view.modeSource"), "source", DocumentTab::ViewMode::Source);
    m_modeSourceVisual = addMode(QStringLiteral("view.modeSplit"), "source-visual", DocumentTab::ViewMode::SourceVisual);
    m_modeVisual = addMode(QStringLiteral("view.modeVisual"), "visual", DocumentTab::ViewMode::Visual);

    m_splitAction = makeAction(QStringLiteral("view.splitEditor"));
    m_splitAction->setIcon(QIcon(QStringLiteral(":/vn/icons/split-editor.svg")));
    m_splitAction->setCheckable(true);
    connect(m_splitAction, &QAction::toggled, this, &MainWindow::toggleSplitEditor);
    makeAction(QStringLiteral("view.moveToNewWindow"), [this] { moveTabToNewWindow(); });
    makeAction(QStringLiteral("view.zoomIn"), [this] { zoom(1); });
    makeAction(QStringLiteral("view.zoomOut"), [this] { zoom(-1); });
    makeAction(QStringLiteral("view.zoomReset"), [this] { zoom(0); });
    makeAction(QStringLiteral("view.fullScreen"), [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });

    makeAction(QStringLiteral("help.shortcuts"), [this] { showPreferences(QStringLiteral("shortcuts")); });
    makeAction(QStringLiteral("help.about"), [this] {
        QMessageBox::about(this, tr("About Visual Notepad"),
                           tr("Visual Notepad %1").arg(QApplication::applicationVersion()));
    });

    connect(&Settings::instance(), &Settings::changed, this, &MainWindow::onSettingChanged);
}

void MainWindow::createMenus()
{
    // Keep the menu bar inside the window on every desktop rather than a
    // global/dbus menu.
    menuBar()->setNativeMenuBar(false);
    const auto add = [this](QMenu *menu, std::initializer_list<const char *> ids) {
        for (const char *id : ids) {
            if (qstrcmp(id, "-") == 0)
                menu->addSeparator();
            else
                menu->addAction(action(QLatin1String(id)));
        }
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    add(file, {"file.new", "file.newWindow", "file.open"});
    m_recentMenu = file->addMenu(tr("Open Recent"));
    add(file, {"-", "file.save", "file.saveAs", "-", "file.closeTab", "file.closeWindow", "-", "file.quit"});
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::fillRecentMenu);

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    add(edit, {"edit.undo", "edit.redo", "-", "edit.cut", "edit.copy", "edit.paste", "-", "edit.find", "edit.replace",
               "edit.findNext", "edit.findPrevious", "edit.goToLine", "-", "edit.toggleComment", "edit.format", "-",
               "edit.preferences"});

    QMenu *selection = menuBar()->addMenu(tr("&Selection"));
    add(selection, {"selection.selectAll", "selection.expand", "selection.shrink", "-", "selection.addCursorAbove",
                    "selection.addCursorBelow", "selection.addNextOccurrence", "selection.selectAllOccurrences", "-",
                    "selection.copyLineUp", "selection.copyLineDown", "selection.moveLineUp",
                    "selection.moveLineDown", "selection.deleteLine"});

    QMenu *view = menuBar()->addMenu(tr("&View"));
    add(view, {"view.outline", "-", "view.modeSource", "view.modeSplit", "view.modeVisual", "-", "view.splitEditor",
               "view.moveToNewWindow", "-", "view.wordWrap", "view.statusBar", "-", "view.zoomIn", "view.zoomOut",
               "view.zoomReset", "view.fullScreen"});

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    add(help, {"help.shortcuts", "-", "help.about"});
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
    close->setToolTip(tr("Hide Outline (%1)").arg(m_outlineAction->shortcut().toString(QKeySequence::NativeText)));
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
        Settings::instance().removeRecentFile(path);
        QMessageBox::warning(this, tr("Open File"),
                             tr("Could not open %1:\n%2").arg(QDir::toNativeSeparators(path), tab->errorString()));
        delete tab;
        return false;
    }
    addTab(tab);
    Settings::instance().addRecentFile(QFileInfo(path).absoluteFilePath());
    return true;
}

void MainWindow::adoptTab(DocumentTab *tab)
{
    addTab(tab);
}

void MainWindow::addTab(DocumentTab *tab)
{
    connect(tab, &DocumentTab::modificationChanged, this, [this, tab] { updateTabTitle(tab); });
    connect(tab, &DocumentTab::cursorPositionChanged, this, [this, tab] {
        if (tab == currentTab())
            updateStatus();
    });
    connect(tab, &DocumentTab::viewModeChanged, this, [this, tab] {
        if (tab == currentTab())
            updateModeControls();
    });
    connect(tab, &DocumentTab::linkActivated, this, [this, tab](const QString &url) { openLink(tab, url); });
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
    for (int i = 0; i < m_group->count(); ++i)
        m_group->tab(i)->setZoom(m_zoomPercent);
    updateStatus();
}

void MainWindow::goToLine()
{
    if (auto *tab = currentTab())
        tab->showGoToLine();
}

void MainWindow::showPreferences(const QString &page)
{
    PreferencesDialog::showPage(this, page);
}

void MainWindow::onSettingChanged(const QString &changedKey)
{
    if (changedKey.startsWith(QLatin1String(key::shortcutsGroup))) {
        const QString id = changedKey.mid(int(qstrlen(key::shortcutsGroup)));
        if (QAction *a = action(id))
            a->setShortcuts(shortcutsFor(id));
        return;
    }
    Settings &settings = Settings::instance();
    if (changedKey == QLatin1String(key::outline)) {
        m_outlineAction->setChecked(settings.flag(key::outline));
        updateOutlineVisibility();
    } else if (changedKey == QLatin1String(key::wordWrap)) {
        m_wrapAction->setChecked(settings.flag(key::wordWrap));
    } else if (changedKey == QLatin1String(key::statusBar)) {
        m_statusBarAction->setChecked(settings.flag(key::statusBar));
        statusBar()->setVisible(settings.flag(key::statusBar));
    }
}

void MainWindow::fillRecentMenu()
{
    m_recentMenu->clear();
    const QStringList files = Settings::instance().recentFiles();
    for (const QString &path : files) {
        m_recentMenu->addAction(QDir::toNativeSeparators(path), this, [this, path] { openFile(path); });
    }
    if (files.isEmpty()) {
        m_recentMenu->addAction(tr("No Recent Files"))->setEnabled(false);
        return;
    }
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(tr("Clear Recently Opened"), this, [] { Settings::instance().clearRecentFiles(); });
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

void MainWindow::openLink(DocumentTab *tab, const QString &url)
{
    if (url.startsWith(u'#')) {
        // In-document anchors: headings by GitHub slug, footnotes by label.
        const QString anchor = QUrl::fromPercentEncoding(url.mid(1).toUtf8());
        const auto slugOf = [](const QString &text) {
            const QByteArray utf8 = text.toUtf8();
            const rust::String slug = core::heading_slug(rust::Str(utf8.constData(), utf8.size()));
            return QString::fromUtf8(slug.data(), slug.size());
        };
        const QString text = tab->text();
        int line = -1;
        if (anchor.startsWith(QLatin1String("fn-"))) {
            const QStringList lines = text.split(u'\n');
            for (int i = 0; i < lines.size() && line < 0; ++i) {
                if (lines[i].startsWith(QLatin1String("[^"))
                    && slugOf(lines[i].section(u']', 0, 0).mid(2).toLower()) == anchor.mid(3))
                    line = i + 1;
            }
        } else {
            const QByteArray utf8 = text.toUtf8();
            QHash<QString, int> seen;
            for (const auto &heading : core::outline(rust::Str(utf8.constData(), utf8.size()))) {
                QString slug = slugOf(QString::fromUtf8(heading.text.data(), heading.text.size()));
                const int n = seen[slug]++;
                if (n > 0)
                    slug += u'-' + QString::number(n);
                if (slug == anchor) {
                    line = int(heading.line);
                    break;
                }
            }
        }
        if (line > 0)
            tab->editor()->scrollToLine(line - 1);
        return;
    }

    const QUrl target(url);
    const QString scheme = target.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto")
        || scheme == QLatin1String("ftp")) {
        // Always ask before leaving the app, like VS Code's trusted-domain prompt.
        QMessageBox box(QMessageBox::Information, productName(),
                        tr("Do you want %1 to open the external website?").arg(productName()), QMessageBox::NoButton,
                        this);
        box.setInformativeText(url);
        QPushButton *open = box.addButton(tr("Open"), QMessageBox::AcceptRole);
        QPushButton *copy = box.addButton(tr("Copy"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(open);
        box.exec();
        if (box.clickedButton() == open)
            QDesktopServices::openUrl(target);
        else if (box.clickedButton() == copy)
            QGuiApplication::clipboard()->setText(url);
        return;
    }

    // Local files, relative to the document.
    QString path = scheme == QLatin1String("file") ? target.toLocalFile()
                                                   : QUrl::fromPercentEncoding(url.section(u'#', 0, 0).toUtf8());
    const QString fragment = url.contains(u'#') ? url.section(u'#', 1) : QString();
    if (QFileInfo(path).isRelative() && !tab->path().isEmpty())
        path = QFileInfo(tab->path()).absoluteDir().filePath(path);
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, productName(), tr("The file %1 does not exist.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(path);
    if (!mime.inherits(QStringLiteral("text/plain"))) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    if (openFile(path) && !fragment.isEmpty())
        openLink(currentTab(), u'#' + fragment);
}

void MainWindow::updateModeControls()
{
    DocumentTab *tab = currentTab();
    const bool markdown = tab && tab->isMarkdown();
    m_modeButton->setVisible(markdown);
    for (QAction *action : m_modeGroup->actions())
        action->setEnabled(markdown);
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
