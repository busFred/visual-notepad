#include "MainWindow.h"

#include "Commands.h"
#include "EditorGroup.h"
#include "PreferencesDialog.h"
#include "Session.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"
#include "vn_core_bridge/lib.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QProcess>
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

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>

#include <algorithm>
#include <map>

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

// Status bar column: text with a divider on its left, as in Notepad. Cells
// with a click handler highlight on hover.
class StatusCell : public QLabel
{
public:
    explicit StatusCell(bool divider, QWidget *parent)
        : QLabel(parent)
        , m_divider(divider)
    {
        setContentsMargins(10, 0, 10, 0);
    }

    void setClickHandler(std::function<void()> handler)
    {
        m_handler = std::move(handler);
        setCursor(m_handler ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (m_hover && m_handler)
            QPainter(this).fillRect(rect(), theme::panelTitle);
        QLabel::paintEvent(event);
        if (m_divider) {
            QPainter painter(this);
            painter.fillRect(QRect(0, 0, 1, height()), theme::divider);
        }
    }

    void enterEvent(QEnterEvent *event) override
    {
        m_hover = true;
        update();
        QLabel::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hover = false;
        update();
        QLabel::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_handler && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
            m_handler();
        else
            QLabel::mouseReleaseEvent(event);
    }

private:
    bool m_divider;
    bool m_hover = false;
    std::function<void()> m_handler;
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_groups(new QSplitter(Qt::Horizontal, this))
    , m_group(new EditorGroup(m_groups))
    , m_activeGroup(m_group)
    , m_outlineTimer(new QTimer(this))
{
    setWindowTitle(productName());
    resize(1100, 720);
    setAcceptDrops(true);

    m_groups->setHandleWidth(1);
    m_groups->setChildrenCollapsible(false);
    m_groups->addWidget(m_group);
    setCentralWidget(m_groups);
    wireGroup(m_group);
    // Focus anywhere in a group makes it the active one.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (EditorGroup *group : groups()) {
            if (now && group->isAncestorOf(now)) {
                activateGroup(group);
                return;
            }
        }
    });

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
    SessionManager::instance().registerWindow(this);
}

MainWindow::~MainWindow()
{
    SessionManager::instance().unregisterWindow(this);
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
    makeAction(QStringLiteral("file.closeTab"), [this] { closeTab(m_activeGroup, m_activeGroup->currentIndex()); });
    makeAction(QStringLiteral("file.closeWindow"), [this] { close(); });
    makeAction(QStringLiteral("file.quit"), [] { SessionManager::instance().quit(); });

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
    makeAction(QStringLiteral("view.moveToNewWindow"), [this] { moveTabToNewWindow(currentTab()); });
    makeAction(QStringLiteral("edit.changeLanguage"), [this] { showLanguageMenu(); });
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
               "view.moveToNewWindow", "-", "view.wordWrap", "view.statusBar", "edit.changeLanguage", "-", "view.zoomIn", "view.zoomOut",
               "view.zoomReset", "view.fullScreen"});

    QMenu *help = menuBar()->addMenu(tr("&Help"));
    add(help, {"help.shortcuts", "-", "help.about"});
}

void MainWindow::createTabActions()
{
    createModeButton(m_group);
    auto *split = new QToolButton(this);
    split->setObjectName(QStringLiteral("IconButton"));
    split->setDefaultAction(m_splitAction);
    split->setFixedSize(24, 24);
    split->setIconSize({16, 16});
    split->setToolTip(tr("Split Editor (%1)").arg(m_splitAction->shortcut().toString(QKeySequence::NativeText)));
    m_group->addAction(split);
}

QToolButton *MainWindow::createModeButton(EditorGroup *group)
{
    // Mode dropdown: icon plus the current mode's name; items have no icons.
    // Each group has its own button, bound to that group's current tab; the
    // menu shares the actions, so opening it first activates the group.
    auto *button = new QToolButton(group);
    button->setObjectName(QStringLiteral("ModeButton"));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setIconSize({14, 14});
    auto *modeMenu = new QMenu(button);
    modeMenu->addActions(m_modeGroup->actions());
    connect(modeMenu, &QMenu::aboutToShow, this, [this, group] { activateGroup(group); });
    button->setMenu(modeMenu);
    group->addAction(button);
    m_modeButtons.insert(group, button);
    return button;
}

QAction *MainWindow::modeAction(DocumentTab::ViewMode mode) const
{
    switch (mode) {
    case DocumentTab::ViewMode::SourceVisual:
        return m_modeSourceVisual;
    case DocumentTab::ViewMode::Visual:
        return m_modeVisual;
    case DocumentTab::ViewMode::Source:
        break;
    }
    return m_modeSource;
}

void MainWindow::wireGroup(EditorGroup *group)
{
    connect(group, &EditorGroup::currentChanged, this, [this, group] {
        if (group == m_activeGroup)
            onCurrentTabChanged();
        else
            updateModeControls();
    });
    connect(group, &EditorGroup::tabCloseRequested, this, [this, group](int index) { closeTab(group, index); });
    connect(group, &EditorGroup::tabContextMenuRequested, this,
            [this, group](int index, const QPoint &pos) { showTabContextMenu(group, index, pos); });
    connect(group, &EditorGroup::newTabRequested, this, [this, group] {
        activateGroup(group);
        newFile();
    });
    connect(group, &EditorGroup::activated, this, [this, group] { activateGroup(group); });
    connect(group, &EditorGroup::tabDropped, this, [this, group](quint64 id, int index) {
        if (DocumentTab *tab = EditorGroup::tabForId(id))
            receiveTab(tab, group, index);
    });
    connect(group, &EditorGroup::tabDroppedOutside, this,
            [this](DocumentTab *tab, const QPoint &pos) { moveTabToNewWindow(tab, pos, true); });
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
    const auto clickable = [](QLabel *label, std::function<void()> handler) {
        static_cast<StatusCell *>(label)->setClickHandler(std::move(handler));
    };
    clickable(m_zoom, [this] { zoom(0); });
    clickable(m_lineEnding, [this] { showLineEndingMenu(); });
    clickable(m_encoding, [this] { showEncodingMenu(); });
    clickable(m_language, [this] { showLanguageMenu(); });
    statusBar()->addPermanentWidget(new DotGrip(this));
}

DocumentTab *MainWindow::currentTab() const
{
    return m_activeGroup->currentTab();
}

QList<EditorGroup *> MainWindow::groups() const
{
    QList<EditorGroup *> result{m_group};
    if (m_sideGroup)
        result.append(m_sideGroup);
    return result;
}

EditorGroup *MainWindow::groupOf(DocumentTab *tab) const
{
    for (EditorGroup *group : groups()) {
        if (tab && group->indexOf(tab) >= 0)
            return group;
    }
    return nullptr;
}

void MainWindow::activateGroup(EditorGroup *group)
{
    if (!group || group == m_activeGroup || !groups().contains(group))
        return;
    m_activeGroup = group;
    onCurrentTabChanged();
}

void MainWindow::newFile()
{
    addTab(new DocumentTab);
}

bool MainWindow::openFile(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    for (EditorGroup *group : groups()) {
        for (int i = 0; i < group->count(); ++i) {
            if (!canonical.isEmpty() && QFileInfo(group->tab(i)->path()).canonicalFilePath() == canonical) {
                activateGroup(group);
                group->setCurrentIndex(i);
                return true;
            }
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

void MainWindow::addTab(DocumentTab *tab, EditorGroup *group, int index)
{
    if (!group || !groups().contains(group))
        group = m_activeGroup;
    activateGroup(group);

    connect(tab, &DocumentTab::modificationChanged, this, [this, tab] { updateTabTitle(tab); });
    connect(tab, &DocumentTab::cursorPositionChanged, this, [this, tab] {
        if (tab == currentTab())
            updateStatus();
    });
    connect(tab, &DocumentTab::viewModeChanged, this, [this] { updateModeControls(); });
    connect(tab, &DocumentTab::formatChanged, this, [this, tab] {
        if (tab != currentTab())
            return;
        updateStatus();
        updateModeControls();
        updateOutlineVisibility();
        if (tab->isMarkdown())
            m_outlineTimer->start();
    });
    // Anything that changes what a session would store.
    auto &session = SessionManager::instance();
    connect(tab, &DocumentTab::textChanged, &session, &SessionManager::markDirty);
    connect(tab, &DocumentTab::modificationChanged, &session, &SessionManager::markDirty);
    connect(tab, &DocumentTab::formatChanged, &session, &SessionManager::markDirty);
    session.markDirty();

    // One timer per tab, restarted on each edit; it travels with the tab.
    QTimer *saveTimer = autoSaveTimer(tab);
    if (!saveTimer) {
        saveTimer = new QTimer(tab);
        saveTimer->setObjectName(QStringLiteral("autoSave"));
        saveTimer->setSingleShot(true);
        saveTimer->setInterval(1000);
    }
    connect(saveTimer, &QTimer::timeout, this, [this, tab] { autoSave(tab); });
    connect(tab, &DocumentTab::textChanged, this, [this, tab, saveTimer] {
        if (Settings::instance().text(key::autoSave) == QLatin1String("afterDelay") && tab->isModified()
            && !tab->path().isEmpty())
            saveTimer->start();
    });
    connect(tab, &DocumentTab::linkActivated, this, [this, tab](const QString &url) { openLink(tab, url); });
    connect(tab, &DocumentTab::textChanged, this, [this, tab] {
        if (tab == currentTab() && tab->isMarkdown())
            m_outlineTimer->start();
    });

    const int at = group->addTab(tab, tab->displayName(), index);
    updateTabTitle(tab);
    group->setCurrentIndex(at);
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
    if (EditorGroup *group = groupOf(tab)) {
        activateGroup(group);
        group->setCurrentIndex(group->indexOf(tab));
    }
    const auto answer = QMessageBox::warning(
        this, productName(), tr("Do you want to save the changes you made to %1?").arg(tab->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save)
        return save(tab);
    return answer == QMessageBox::Discard;
}

bool MainWindow::closeTab(EditorGroup *group, int index)
{
    DocumentTab *tab = group->tab(index);
    if (!tab || !confirmDiscard(tab))
        return false;
    group->removeTab(group->indexOf(tab));
    tab->deleteLater();
    return true;
}

int MainWindow::tabCount() const
{
    int count = 0;
    for (EditorGroup *group : groups())
        count += group->count();
    return count;
}

QList<DocumentTab *> MainWindow::tabs() const
{
    QList<DocumentTab *> result;
    for (EditorGroup *group : groups()) {
        for (int i = 0; i < group->count(); ++i)
            result.append(group->tab(i));
    }
    return result;
}

int MainWindow::currentTabIndex() const
{
    DocumentTab *tab = currentTab();
    return tab ? int(tabs().indexOf(tab)) : -1;
}

void MainWindow::setCurrentTabIndex(int index)
{
    DocumentTab *tab = tabs().value(index);
    if (EditorGroup *group = groupOf(tab)) {
        activateGroup(group);
        group->setCurrentIndex(group->indexOf(tab));
    }
}

bool MainWindow::closeTabs(int index, CloseScope scope)
{
    return closeTabs(m_activeGroup, index, scope);
}

bool MainWindow::closeTabs(EditorGroup *group, int index, CloseScope scope)
{
    DocumentTab *anchor = group->tab(index);
    if (!anchor)
        return false;
    QList<DocumentTab *> victims;
    for (int i = 0; i < group->count(); ++i) {
        DocumentTab *tab = group->tab(i);
        const bool hit = scope == CloseScope::All || (scope == CloseScope::This && i == index)
                         || (scope == CloseScope::Others && i != index) || (scope == CloseScope::Right && i > index)
                         || (scope == CloseScope::Saved && !tab->isModified());
        if (hit)
            victims.append(tab);
    }
    for (DocumentTab *tab : victims) {
        if (!closeTab(group, group->indexOf(tab)))
            return false;
    }
    return true;
}

void MainWindow::showTabContextMenu(EditorGroup *group, int index, const QPoint &globalPos)
{
    DocumentTab *tab = group->tab(index);
    if (!tab)
        return;
    const bool hasPath = !tab->path().isEmpty();
    QMenu menu(this);
    const auto close = [this, group, tab](CloseScope scope) {
        return [this, group, tab, scope] { closeTabs(group, group->indexOf(tab), scope); };
    };
    menu.addAction(tr("Close"), this, close(CloseScope::This));
    menu.addAction(tr("Close Others"), this, close(CloseScope::Others))->setEnabled(group->count() > 1);
    menu.addAction(tr("Close to the Right"), this, close(CloseScope::Right))
        ->setEnabled(index < group->count() - 1);
    menu.addAction(tr("Close Saved"), this, close(CloseScope::Saved));
    menu.addAction(tr("Close All"), this, close(CloseScope::All));
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [tab] {
        QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(tab->path()));
    })->setEnabled(hasPath);
    menu.addAction(tr("Reveal in File Manager"), this, [this, tab] { revealInFileManager(tab->path()); })
        ->setEnabled(hasPath);
    menu.addSeparator();
    menu.addAction(tr("Move to Other Group"), this, [this, tab] { moveToOtherGroup(tab); })
        ->setEnabled(m_sideGroup != nullptr);
    menu.addAction(tr("Move to New Window"), this, [this, tab] { moveTabToNewWindow(tab); });
    menu.exec(globalPos);
}

void MainWindow::revealInFileManager(const QString &path)
{
#ifdef Q_OS_WIN
    if (QProcess::startDetached(QStringLiteral("explorer"),
                                {QStringLiteral("/select,") + QDir::toNativeSeparators(path)}))
        return;
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    // A tab dropped beside the groups (menu bar, outline) joins the active one.
    if (event->mimeData()->hasFormat(EditorGroup::tabMimeType())) {
        event->acceptProposedAction();
        return;
    }
    const auto urls = event->mimeData()->urls();
    if (std::any_of(urls.begin(), urls.end(), [](const QUrl &url) { return url.isLocalFile(); }))
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (event->mimeData()->hasFormat(EditorGroup::tabMimeType())) {
        if (DocumentTab *tab = EditorGroup::tabForId(event->mimeData()->data(EditorGroup::tabMimeType()).toULongLong()))
            receiveTab(tab, m_activeGroup, -1);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    const auto urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile())
            openFile(url.toLocalFile());
    }
    event->acceptProposedAction();
}

void MainWindow::popupAboveCell(QMenu *menu, QWidget *cell)
{
    QPoint pos;
    if (statusBar()->isVisible()) {
        pos = cell->mapToGlobal(QPoint(0, 0)) - QPoint(0, menu->sizeHint().height());
    } else {
        // No status bar: centre the menu under the tab strip.
        pos = m_activeGroup->mapToGlobal(QPoint((m_activeGroup->width() - menu->sizeHint().width()) / 2, 32));
    }
    menu->exec(pos);
}

void MainWindow::showLineEndingMenu()
{
    DocumentTab *tab = currentTab();
    if (!tab)
        return;
    QMenu menu(this);
    const auto add = [&](const QString &text, DocumentTab::LineEnding ending) {
        QAction *a = menu.addAction(text, this, [tab, ending] { tab->setLineEnding(ending); });
        a->setCheckable(true);
        a->setChecked(tab->lineEnding() == ending);
    };
    add(tr("Unix (LF)"), DocumentTab::LineEnding::LF);
    add(tr("Windows (CRLF)"), DocumentTab::LineEnding::CRLF);
    popupAboveCell(&menu, m_lineEnding);
}

bool MainWindow::reopenWithEncoding(DocumentTab *tab, DocumentTab::Encoding encoding)
{
    if (tab->isModified()) {
        const auto answer = QMessageBox::warning(
            this, productName(), tr("%1 has unsaved changes. Discard them and reopen with another encoding?")
                                     .arg(tab->displayName()),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard)
            return false;
        tab->document()->setModified(false);
    }
    if (!tab->reopenWithEncoding(encoding)) {
        QMessageBox::warning(this, tr("Reopen with Encoding"), tab->errorString());
        return false;
    }
    return true;
}

void MainWindow::showEncodingMenu()
{
    DocumentTab *tab = currentTab();
    if (!tab)
        return;
    using E = DocumentTab::Encoding;
    const E all[] = {E::Utf8, E::Utf8Bom, E::Utf16LE, E::Utf16BE, E::Legacy};
    QMenu menu(this);
    QMenu *reopen = menu.addMenu(tr("Reopen with Encoding"));
    reopen->setEnabled(!tab->path().isEmpty());
    QMenu *save = menu.addMenu(tr("Save with Encoding"));
    for (E encoding : all) {
        const QString name = DocumentTab::encodingDisplayName(encoding);
        reopen->addAction(name, this, [this, tab, encoding] { reopenWithEncoding(tab, encoding); });
        QAction *a = save->addAction(name, this, [tab, encoding] { tab->setEncoding(encoding); });
        a->setCheckable(true);
        a->setChecked(tab->encoding() == encoding);
    }
    popupAboveCell(&menu, m_encoding);
}

void MainWindow::showLanguageMenu()
{
    DocumentTab *tab = currentTab();
    if (!tab)
        return;
    QMenu menu(this);
    const QString current = tab->definitionName();
    const auto add = [&](QMenu *parent, const QString &text, const QString &name) {
        QAction *a = parent->addAction(text, this, [tab, name] { tab->setLanguage(name); });
        a->setCheckable(true);
        a->setChecked(current == name);
    };
    add(&menu, tr("Plain Text"), QString());
    menu.addSeparator();

    std::map<QString, QList<KSyntaxHighlighting::Definition>> sections;
    for (const auto &def : theme::syntaxRepository().definitions()) {
        if (!def.isHidden())
            sections[def.translatedSection()].append(def);
    }
    for (auto &[section, defs] : sections) {
        std::sort(defs.begin(), defs.end(), [](const auto &a, const auto &b) {
            return a.translatedName().localeAwareCompare(b.translatedName()) < 0;
        });
        QMenu *submenu = menu.addMenu(section.isEmpty() ? tr("Other") : section);
        for (const auto &def : defs)
            add(submenu, def.translatedName(), def.name());
    }
    popupAboveCell(&menu, m_language);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // The final quit uses hot exit, which saves instead of asking; a single
    // window among several closes normally.
    auto &session = SessionManager::instance();
    if (!session.isQuitting()) {
        if (session.isLastWindow(this)) {
            if (!session.prepareQuit()) {
                event->ignore();
                return;
            }
        } else {
            for (DocumentTab *tab : tabs()) {
                if (!confirmDiscard(tab)) {
                    event->ignore();
                    return;
                }
            }
        }
    }
    session.unregisterWindow(this);
    event->accept();
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate && Settings::instance().text(key::autoSave) == QLatin1String("onFocusChange"))
        autoSaveAll();
    return QMainWindow::event(event);
}

QTimer *MainWindow::autoSaveTimer(DocumentTab *tab) const
{
    return tab->findChild<QTimer *>(QStringLiteral("autoSave"), Qt::FindDirectChildrenOnly);
}

void MainWindow::autoSave(DocumentTab *tab)
{
    if (!tab->isModified() || tab->path().isEmpty())
        return;
    if (!tab->save(tab->path())) {
        statusBar()->showMessage(
            tr("Auto save failed for %1: %2").arg(QDir::toNativeSeparators(tab->path()), tab->errorString()), 5000);
        return;
    }
    updateTabTitle(tab);
}

void MainWindow::autoSaveAll()
{
    for (DocumentTab *tab : tabs())
        autoSave(tab);
}

void MainWindow::applyAutoSave()
{
    const QString mode = Settings::instance().text(key::autoSave);
    for (DocumentTab *tab : tabs()) {
        QTimer *timer = autoSaveTimer(tab);
        if (!timer)
            continue;
        if (mode != QLatin1String("afterDelay"))
            timer->stop();
        else if (tab->isModified() && !tab->path().isEmpty())
            timer->start();
    }
}

void MainWindow::detachTab(DocumentTab *tab)
{
    EditorGroup *group = groupOf(tab);
    if (!group)
        return;
    tab->disconnect(this);
    tab->disconnect(&SessionManager::instance());
    if (QTimer *timer = autoSaveTimer(tab))
        timer->disconnect(this);
    group->removeTab(group->indexOf(tab));
}

void MainWindow::receiveTab(DocumentTab *tab, EditorGroup *group, int index)
{
    if (!tab || !group || !groups().contains(group))
        return;
    // The tab's window is the top-level ancestor: the window it lives in.
    auto *source = qobject_cast<MainWindow *>(tab->window());
    EditorGroup *from = source ? source->groupOf(tab) : nullptr;
    if (!from)
        return;

    if (from == group) {
        const int old = group->indexOf(tab);
        const int to = index < 0 ? group->count() - 1 : (index > old ? index - 1 : index);
        if (to != old)
            group->moveTab(old, to);
        activateGroup(group);
        group->setCurrentIndex(group->indexOf(tab));
        return;
    }
    source->detachTab(tab);
    addTab(tab, group, index);
    // Only a drag-out empties a window this way, so no unsaved-changes prompt.
    if (source != this && source->tabCount() == 0)
        source->close();
}

void MainWindow::moveToOtherGroup(DocumentTab *tab)
{
    EditorGroup *from = groupOf(tab);
    if (!from || !m_sideGroup)
        return;
    receiveTab(tab, from == m_group ? m_sideGroup : m_group, -1);
}

void MainWindow::moveTabToNewWindow(DocumentTab *tab, std::optional<QPoint> globalPos, bool closeIfEmpty)
{
    if (!tab || !groupOf(tab))
        return;
    detachTab(tab);

    auto *window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->adoptTab(tab);
    if (globalPos)
        window->move(*globalPos - QPoint(60, 14));
    window->show();
    if (closeIfEmpty && tabCount() == 0)
        close();
}

void MainWindow::setSplit(bool on)
{
    m_splitAction->setChecked(on);
}

void MainWindow::toggleSplitEditor(bool on)
{
    if (on && !m_sideGroup) {
        m_sideGroup = new EditorGroup(m_groups);
        wireGroup(m_sideGroup);
        createModeButton(m_sideGroup);
        m_groups->addWidget(m_sideGroup);
        const int half = m_groups->width() / 2;
        m_groups->setSizes({half, m_groups->width() - half});
        m_sideGroup->show();
    } else if (!on && m_sideGroup) {
        EditorGroup *side = m_sideGroup;
        DocumentTab *keep = m_activeGroup == side ? side->currentTab() : m_group->currentTab();
        // Closing the group keeps its tabs: they join the main group.
        while (side->count() > 0) {
            DocumentTab *tab = side->tab(0);
            detachTab(tab);
            addTab(tab, m_group);
        }
        if (keep)
            m_group->setCurrentIndex(m_group->indexOf(keep));
        activateGroup(m_group);
        m_modeButtons.remove(side);
        m_sideGroup = nullptr;
        side->hide();
        side->deleteLater();
    }
    if (m_splitAction->isChecked() != (m_sideGroup != nullptr))
        m_splitAction->setChecked(m_sideGroup != nullptr);
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
    for (DocumentTab *tab : tabs())
        tab->setZoom(m_zoomPercent);
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
    } else if (changedKey == QLatin1String(key::autoSave)) {
        applyAutoSave();
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
    // Not for a tab that was just closed or moved away: its changes were discarded.
    if (m_previousTab && m_previousTab != tab && groupOf(m_previousTab)
        && Settings::instance().text(key::autoSave) == QLatin1String("onFocusChange"))
        autoSave(m_previousTab);
    m_previousTab = tab;
    setWindowTitle(tab ? tr("%1 — %2").arg(tab->displayName(), productName()) : productName());
    updateStatus();
    updateOutline();
    updateOutlineVisibility();
    updateModeControls();
}

void MainWindow::updateTabTitle(DocumentTab *tab)
{
    EditorGroup *group = groupOf(tab);
    if (!group)
        return;
    const int index = group->indexOf(tab);
    group->setTabText(index, tab->isModified() ? QStringLiteral("● ") + tab->displayName() : tab->displayName());
    group->setTabToolTip(index, QDir::toNativeSeparators(tab->path()));
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
    m_encoding->setText(tab->encodingName());
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
    // Every group's button shows its own tab; the menu actions follow the
    // active tab.
    for (auto it = m_modeButtons.cbegin(); it != m_modeButtons.cend(); ++it) {
        const DocumentTab *tab = it.key()->currentTab();
        it.value()->setVisible(tab && tab->isMarkdown());
        if (tab && tab->isMarkdown()) {
            QAction *shown = modeAction(tab->viewMode());
            it.value()->setIcon(shown->icon());
            it.value()->setText(shown->text());
        }
    }
    DocumentTab *tab = currentTab();
    const bool markdown = tab && tab->isMarkdown();
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
    modeAction(tab->viewMode())->setChecked(true);
}

} // namespace vn
