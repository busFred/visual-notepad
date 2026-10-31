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
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QUrl>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPromise>
#include <QSizeGrip>
#include "EvenSplitter.h"
#include <QSplitter>
#include <QStatusBar>
#include <QThreadPool>
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
// Parsing the headings costs about 12 ms per MB. Past this many characters
// (a few ms) it runs off the GUI thread, so that a pause in typing does not
// freeze the window.
constexpr qsizetype outlineThreadChars = 256 * 1024;
constexpr int zoomStepPercent = 10;

// The headings of a Markdown text. Plain Rust and Qt value types only, so it
// also runs on a worker thread.
QList<OutlineHeading> parseHeadings(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    const auto headings = core::outline(rust::Str(utf8.constData(), utf8.size()));
    QList<OutlineHeading> result;
    result.reserve(qsizetype(headings.size()));
    for (const auto &heading : headings)
        result.append({heading.level, static_cast<int>(heading.line),
                       QString::fromUtf8(heading.text.data(), heading.text.size())});
    return result;
}

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
            painter.fillRect(QRect(origin + dot, QSize(2, 2)), theme::color(theme::Role::gripDots));
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
            QPainter(this).fillRect(rect(), theme::color(theme::Role::panelTitle));
        QLabel::paintEvent(event);
        if (m_divider) {
            QPainter painter(this);
            painter.fillRect(QRect(0, 0, 1, height()), theme::color(theme::Role::divider));
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
    , m_root(new EvenSplitter(Qt::Horizontal, this))
    , m_outlineTimer(new QTimer(this))
    , m_outlineWatcher(new QFutureWatcher<OutlineSnapshot>(this))
{
    setWindowTitle(productName());
    resize(1100, 720);
    setAcceptDrops(true);

    m_root->setHandleWidth(1);
    m_root->setChildrenCollapsible(false);
    setCentralWidget(m_root);
    // Focus anywhere in a group makes it the active one.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        // Focus handed on by a tab being taken out says nothing.
        if (m_removingTab)
            return;
        for (EditorGroup *group : groups()) {
            if (now && group->isAncestorOf(now)) {
                activateGroup(group);
                return;
            }
        }
    });

    m_zoomPercent = Settings::instance().number(key::zoom);
    m_autoSaveMode = Settings::instance().text(key::autoSave);
    m_outlineTimer->setSingleShot(true);
    m_outlineTimer->setInterval(250);
    connect(m_outlineTimer, &QTimer::timeout, this, &MainWindow::updateOutline);
    connect(m_outlineWatcher, &QFutureWatcher<OutlineSnapshot>::finished, this, &MainWindow::finishOutlineJob);

    createActions();
    createOutline();
    createMenus();
    createStatusBar();
    // The groups' strip buttons use the actions, so groups come after them.
    EditorGroup *first = createGroup();
    m_root->addWidget(first);
    activateGroup(first);
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
    auto *action = new QAction(cmd->menuText, this);
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
    makeAction(QStringLiteral("edit.findNextSelection"), [this] {
        if (auto *tab = currentTab())
            tab->findBar()->findSelection(true);
    });
    makeAction(QStringLiteral("edit.findPreviousSelection"), [this] {
        if (auto *tab = currentTab())
            tab->findBar()->findSelection(false);
    });
    makeAction(QStringLiteral("edit.goToLine"), [this] { goToLine(); });
    makeAction(QStringLiteral("edit.toggleComment"), onEditor(&TextView::toggleLineComment));
    makeAction(QStringLiteral("edit.indentLines"), onEditor(&TextView::indent));
    makeAction(QStringLiteral("edit.outdentLines"), onEditor(&TextView::outdent));
    makeAction(QStringLiteral("edit.trimTrailingWhitespace"), onEditor(&TextView::trimTrailingWhitespace));
    makeAction(QStringLiteral("edit.format"), onTab(&DocumentTab::formatDocument))->setEnabled(false);
    makeAction(QStringLiteral("edit.preferences"), [this] { showPreferences(QString()); });

    makeAction(QStringLiteral("selection.selectAll"), onEditor(&TextView::selectAll));
    makeAction(QStringLiteral("selection.expand"), onEditor(&TextView::expandSelection));
    makeAction(QStringLiteral("selection.shrink"), onEditor(&TextView::shrinkSelection));
    makeAction(QStringLiteral("selection.addCursorAbove"), onEditor(&TextView::addCursorAbove));
    makeAction(QStringLiteral("selection.addCursorBelow"), onEditor(&TextView::addCursorBelow));
    makeAction(QStringLiteral("selection.addNextOccurrence"), onEditor(&TextView::addNextOccurrence));
    makeAction(QStringLiteral("selection.selectAllOccurrences"), onEditor(&TextView::selectAllOccurrences));
    makeAction(QStringLiteral("selection.addCursorsToLineEnds"), onEditor(&TextView::addCursorsToLineEnds));
    makeAction(QStringLiteral("selection.moveLastToNextMatch"), onEditor(&TextView::moveLastSelectionToNextMatch));
    makeAction(QStringLiteral("selection.expandLine"), onEditor(&TextView::expandLineSelection));
    makeAction(QStringLiteral("selection.cursorUndo"), onEditor(&TextView::cursorUndo));
    makeAction(QStringLiteral("selection.cursorRedo"), onEditor(&TextView::cursorRedo));
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

    // workbench.editor.openSideBySideDirection's default, "right".
    m_splitAction = makeAction(QStringLiteral("view.splitEditor"), [this] { splitEditor(SplitDirection::Right); });
    m_splitAction->setIcon(QIcon(QStringLiteral(":/vn/icons/split-editor.svg")));
    makeAction(QStringLiteral("view.splitEditorUp"), [this] { splitEditor(SplitDirection::Up); });
    makeAction(QStringLiteral("view.splitEditorDown"), [this] { splitEditor(SplitDirection::Down); });
    makeAction(QStringLiteral("view.splitEditorLeft"), [this] { splitEditor(SplitDirection::Left); });
    makeAction(QStringLiteral("view.splitEditorRight"), [this] { splitEditor(SplitDirection::Right); });
    makeAction(QStringLiteral("view.focusNextGroup"), [this] { focusNextGroup(1); });
    makeAction(QStringLiteral("view.focusPreviousGroup"), [this] { focusNextGroup(-1); });
    makeAction(QStringLiteral("view.moveToNewWindow"), [this] {
        if (m_activeGroup)
            moveTabsToNewWindow(m_activeGroup->selectedTabs());
    });
    makeAction(QStringLiteral("view.copyToNewWindow"), [this] {
        if (m_activeGroup)
            moveTabsToNewWindow(m_activeGroup->selectedTabs(), std::nullopt, false, true);
    });
    makeAction(QStringLiteral("edit.changeLanguage"), [this] { showLanguageMenu(); });
    makeAction(QStringLiteral("view.scrollPageUp"), [this] {
        if (auto *tab = currentTab())
            tab->editor()->scrollPage(false);
    });
    makeAction(QStringLiteral("view.scrollPageDown"), [this] {
        if (auto *tab = currentTab())
            tab->editor()->scrollPage(true);
    });
    // Markdown formatting; enabled for Markdown documents only.
    using Format = TextView::MarkdownFormat;
    static const QList<std::pair<const char *, Format>> formats{
        {"format.bold", Format::Bold}, {"format.italic", Format::Italic},
        {"format.underline", Format::Underline}, {"format.strikethrough", Format::Strikethrough},
        {"format.inlineCode", Format::InlineCode}, {"format.heading1", Format::Heading1},
        {"format.heading2", Format::Heading2}, {"format.heading3", Format::Heading3},
        {"format.heading4", Format::Heading4}, {"format.heading5", Format::Heading5},
        {"format.heading6", Format::Heading6}, {"format.headingUp", Format::HeadingUp},
        {"format.headingDown", Format::HeadingDown}, {"format.quote", Format::Quote},
        {"format.orderedList", Format::OrderedList}, {"format.bulletList", Format::BulletList},
        {"format.taskList", Format::TaskList}, {"format.link", Format::Link},
        {"format.codeBlock", Format::CodeBlock}};
    for (const auto &[id, format] : formats) {
        makeAction(QLatin1String(id), [this, format = format] {
            if (auto *tab = currentTab(); tab && tab->isMarkdown())
                tab->editor()->formatMarkdown(format);
        })->setEnabled(false);
    }
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

    const QStringList menus = menuTitles();
    QMenu *file = menuBar()->addMenu(menus.at(0));
    add(file, {"file.new", "file.newWindow", "file.open"});
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    add(file, {"-", "file.save", "file.saveAs", "-", "file.closeTab", "file.closeWindow", "-", "file.quit"});
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::fillRecentMenu);
    m_recentMenu->menuAction()->setVisible(Settings::instance().flag(key::rememberRecent));

    QMenu *edit = menuBar()->addMenu(menus.at(1));
    add(edit, {"edit.undo", "edit.redo", "-", "edit.cut", "edit.copy", "edit.paste", "-", "edit.find", "edit.replace",
               "edit.findNext", "edit.findPrevious", "edit.findNextSelection", "edit.findPreviousSelection", "edit.goToLine",
               "-", "edit.toggleComment", "edit.indentLines", "edit.outdentLines", "edit.trimTrailingWhitespace",
               "edit.format", "-",
               "edit.preferences"});

    QMenu *selection = menuBar()->addMenu(menus.at(2));
    add(selection, {"selection.selectAll", "selection.expand", "selection.shrink", "-", "selection.addCursorAbove",
                    "selection.addCursorBelow", "selection.addCursorsToLineEnds", "selection.addNextOccurrence",
                    "selection.moveLastToNextMatch", "selection.selectAllOccurrences", "-", "selection.expandLine",
                    "selection.cursorUndo", "selection.cursorRedo", "-",
                    "selection.copyLineUp", "selection.copyLineDown", "selection.moveLineUp",
                    "selection.moveLineDown", "selection.deleteLine"});

    QMenu *format = menuBar()->addMenu(menus.at(3));
    add(format, {"format.bold", "format.italic", "format.underline", "format.strikethrough", "format.inlineCode", "-",
                 "format.heading1", "format.heading2", "format.heading3", "format.heading4", "format.heading5",
                 "format.heading6", "format.headingUp", "format.headingDown", "-", "format.quote",
                 "format.orderedList", "format.bulletList", "format.taskList", "-", "format.link",
                 "format.codeBlock"});

    QMenu *view = menuBar()->addMenu(menus.at(4));
    add(view, {"view.outline", "-", "view.modeSource", "view.modeSplit", "view.modeVisual", "-", "view.splitEditor",
               "view.splitEditorUp", "view.splitEditorDown", "view.splitEditorLeft", "view.splitEditorRight",
               "view.focusNextGroup", "view.focusPreviousGroup", "view.moveToNewWindow", "view.copyToNewWindow", "-", "view.wordWrap", "view.statusBar", "edit.changeLanguage", "-", "view.zoomIn", "view.zoomOut",
               "view.zoomReset", "view.fullScreen"});

    QMenu *help = menuBar()->addMenu(menus.at(5));
    add(help, {"help.shortcuts", "-", "help.about"});
}

void MainWindow::createSplitButton(EditorGroup *group)
{
    // Every group has its own, like VS Code's editor title actions: it splits
    // that group, not whichever is active.
    auto *split = new QToolButton(group);
    split->setObjectName(QStringLiteral("IconButton"));
    split->setIcon(m_splitAction->icon());
    split->setFixedSize(24, 24);
    split->setIconSize({16, 16});
    split->setToolTip(tr("Split Editor (%1)").arg(m_splitAction->shortcut().toString(QKeySequence::NativeText)));
    connect(split, &QToolButton::clicked, this, [this, group] {
        activateGroup(group);
        splitEditor(SplitDirection::Right);
    });
    group->addAction(split);
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
    connect(group, &EditorGroup::commandRequested, this, [this, group](const QString &id) {
        activateGroup(group);
        if (QAction *a = action(id))
            a->trigger();
    });
    connect(group, &EditorGroup::recentFileRequested, this, [this, group](const QString &path) {
        activateGroup(group);
        openFile(path);
    });
    connect(group, &EditorGroup::tabContextMenuRequested, this,
            [this, group](int index, const QPoint &pos) { showTabContextMenu(group, index, pos); });
    connect(group, &EditorGroup::newTabRequested, this, [this, group] {
        activateGroup(group);
        newFile();
    });
    connect(group, &EditorGroup::activated, this, [this, group] { activateGroup(group); });
    connect(group, &EditorGroup::tabsDropped, this,
            [this, group](const QList<DocumentTab *> &tabs, int index, SplitDirection split, bool copy) {
                dropTabs(tabs, group, index, split, copy);
            });
    connect(group, &EditorGroup::tabsDroppedOutside, this,
            [this](const QList<DocumentTab *> &tabs, const QPoint &pos, bool copy) {
                moveTabsToNewWindow(tabs, pos, !copy, copy);
            });
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
    for (QLabel **label : {&m_position, &m_indentation, &m_zoom, &m_lineEnding, &m_encoding, &m_language}) {
        *label = new StatusCell(!first, this);
        statusBar()->addPermanentWidget(*label);
        first = false;
    }
    const auto clickable = [](QLabel *label, std::function<void()> handler) {
        static_cast<StatusCell *>(label)->setClickHandler(std::move(handler));
    };
    m_indentation->setObjectName(QStringLiteral("IndentationStatus"));
    clickable(m_indentation, [this] { showIndentationMenu(); });
    clickable(m_zoom, [this] { zoom(0); });
    clickable(m_lineEnding, [this] { showLineEndingMenu(); });
    clickable(m_encoding, [this] { showEncodingMenu(); });
    clickable(m_language, [this] { showLanguageMenu(); });
    statusBar()->addPermanentWidget(new DotGrip(this));
}

DocumentTab *MainWindow::currentTab() const
{
    return m_activeGroup ? m_activeGroup->currentTab() : nullptr;
}

namespace {

void collectGroups(QWidget *node, QList<EditorGroup *> &out)
{
    if (auto *group = qobject_cast<EditorGroup *>(node)) {
        out.append(group);
    } else if (auto *splitter = qobject_cast<QSplitter *>(node)) {
        for (int i = 0; i < splitter->count(); ++i)
            collectGroups(splitter->widget(i), out);
    }
}

EvenSplitter *splitterOf(QWidget *node)
{
    return node ? qobject_cast<EvenSplitter *>(node->parentWidget()) : nullptr;
}

} // namespace

QList<EditorGroup *> MainWindow::groups() const
{
    QList<EditorGroup *> result;
    collectGroups(m_root, result);
    return result;
}

EditorGroup *MainWindow::mainGroup() const
{
    return groups().value(0);
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
    m_groupHistory.removeAll(group);
    m_groupHistory.prepend(group);
    onCurrentTabChanged();
}

EditorGroup *MainWindow::createGroup()
{
    auto *group = new EditorGroup;
    wireGroup(group);
    createModeButton(group);
    createSplitButton(group);
    return group;
}

EvenSplitter *MainWindow::createSplitter(Qt::Orientation orientation)
{
    auto *splitter = new EvenSplitter(orientation);
    splitter->setHandleWidth(1);
    splitter->setChildrenCollapsible(false);
    return splitter;
}

EditorGroup *MainWindow::addGroup(EditorGroup *reference, SplitDirection direction)
{
    EvenSplitter *parent = splitterOf(reference);
    if (direction == SplitDirection::None || !parent || !groups().contains(reference))
        return reference;
    const Qt::Orientation orientation = direction == SplitDirection::Left || direction == SplitDirection::Right
                                            ? Qt::Horizontal
                                            : Qt::Vertical;
    const bool after = direction == SplitDirection::Right || direction == SplitDirection::Down;
    EditorGroup *group = createGroup();
    // gridview.ts addView(): a sibling in a branch of the same orientation,
    // else the reference becomes a branch of the two. The new group takes
    // half of the reference's space (workbench.editor.splitSizing "split").
    if (parent->count() == 1)
        parent->setOrientation(orientation);
    const int at = parent->indexOf(reference);
    QList<int> sizes = parent->sizes();
    if (parent->orientation() == orientation) {
        const int half = sizes.value(at) / 2;
        parent->insertWidget(at + (after ? 1 : 0), group);
        if (half > 0) {
            sizes[at] -= half;
            sizes.insert(at + (after ? 1 : 0), half);
            parent->setSizes(sizes);
        }
    } else {
        const int extent = orientation == Qt::Horizontal ? reference->width() : reference->height();
        EvenSplitter *branch = createSplitter(orientation);
        parent->replaceWidget(at, branch);
        branch->addWidget(after ? reference : group);
        branch->addWidget(after ? group : reference);
        if (extent > 1)
            branch->setSizes({extent / 2, extent - extent / 2});
        parent->setSizes(sizes);
        reference->show();
        branch->show();
    }
    group->show();
    SessionManager::instance().markDirty();
    return group;
}

void MainWindow::removeGroupIfEmpty(EditorGroup *group)
{
    const QList<EditorGroup *> all = groups();
    if (!group || group->count() > 0 || all.size() < 2 || !all.contains(group))
        return;
    EvenSplitter *parent = splitterOf(group);
    // editorPart.ts doRemoveEmptyGroup(): the most recently active of the
    // others takes over.
    // Focus left with the group's last tab, if it was in this window.
    QWidget *focus = QApplication::focusWidget();
    const bool hadFocus = !focus || isAncestorOf(focus);
    m_groupHistory.removeAll(group);
    m_modeButtons.remove(group);
    if (m_activeGroup == group) {
        m_activeGroup = nullptr;
        EditorGroup *next = nullptr;
        for (EditorGroup *candidate : std::as_const(m_groupHistory)) {
            if (all.contains(candidate)) {
                next = candidate;
                break;
            }
        }
        group->hide();
        group->setParent(nullptr);
        collapse(parent);
        activateGroup(next ? next : groups().value(0));
    } else {
        group->hide();
        group->setParent(nullptr);
        collapse(parent);
    }
    group->disconnect(this);
    group->deleteLater();
    if (hadFocus && m_activeGroup) {
        if (DocumentTab *tab = m_activeGroup->currentTab())
            tab->editor()->setFocus();
        else
            m_activeGroup->setFocus();
    }
    SessionManager::instance().markDirty();
}

void MainWindow::collapse(EvenSplitter *splitter)
{
    if (!splitter || splitter->count() != 1)
        return;
    QWidget *only = splitter->widget(0);
    auto *branch = qobject_cast<EvenSplitter *>(only);
    // gridview.ts removeView(): a branch with one child is replaced by it,
    // and a child branch of its parent's orientation merges into the parent.
    if (splitter == m_root) {
        if (!branch)
            return;
        const QList<int> sizes = branch->sizes();
        m_root->setOrientation(branch->orientation());
        while (branch->count() > 0) {
            QWidget *child = branch->widget(0);
            m_root->addWidget(child);
            child->show();
        }
        branch->hide();
        branch->setParent(nullptr);
        branch->deleteLater();
        m_root->setSizes(sizes);
        return;
    }
    EvenSplitter *parent = splitterOf(splitter);
    const int at = parent->indexOf(splitter);
    QList<int> sizes = parent->sizes();
    if (branch && branch->orientation() == parent->orientation()) {
        const QList<int> inner = branch->sizes();
        int innerTotal = 0;
        for (int size : inner)
            innerTotal += size;
        const int outer = sizes.value(at);
        sizes.removeAt(at);
        int given = 0;
        for (int i = 0; i < inner.size(); ++i) {
            const int share = i == inner.size() - 1 ? outer - given
                                                    : (innerTotal > 0 ? outer * inner[i] / innerTotal : 0);
            given += share;
            sizes.insert(at + i, share);
        }
        for (int i = 0; branch->count() > 0; ++i) {
            QWidget *child = branch->widget(0);
            parent->insertWidget(at + i, child);
            child->show();
        }
        splitter->hide();
        splitter->setParent(nullptr);
        splitter->deleteLater();
        parent->setSizes(sizes);
        return;
    }
    parent->replaceWidget(at, only);
    only->show();
    parent->setSizes(sizes);
    splitter->deleteLater();
}

void MainWindow::splitEditor(SplitDirection direction)
{
    if (m_activeGroup)
        splitTabs(m_activeGroup, m_activeGroup->selectedTabs(), direction);
}

EditorGroup *MainWindow::splitTabs(EditorGroup *group, const QList<DocumentTab *> &tabs, SplitDirection direction)
{
    DocumentTab *current = group->currentTab();
    EditorGroup *created = addGroup(group, direction);
    if (created == group)
        return group;
    for (DocumentTab *tab : tabs) {
        if (group->indexOf(tab) >= 0)
            copyTab(tab, created, -1);
    }
    // The copy of the current tab is the new group's current one.
    for (int i = 0; current && i < created->count(); ++i) {
        if (created->tab(i)->file() == current->file())
            created->setCurrentIndex(i);
    }
    activateGroup(created);
    if (DocumentTab *tab = created->currentTab())
        tab->editor()->setFocus();
    else
        created->setFocus();
    return created;
}

namespace {

QString orientationName(Qt::Orientation orientation)
{
    return orientation == Qt::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical");
}

} // namespace

QJsonObject MainWindow::saveNode(QWidget *node, const QHash<DocumentTab *, int> &ids) const
{
    // Like VS Code's serialized grid (gridview.ts ISerializedNode): branches
    // with their orientation, children and sizes; leaves are groups.
    QJsonObject object;
    if (auto *group = qobject_cast<EditorGroup *>(node)) {
        QJsonArray tabs;
        for (int i = 0; i < group->count(); ++i) {
            const auto it = ids.constFind(group->tab(i));
            if (it != ids.constEnd())
                tabs.append(*it);
        }
        object[QStringLiteral("tabs")] = tabs;
        object[QStringLiteral("current")] = ids.value(group->currentTab(), -1);
    } else if (auto *splitter = qobject_cast<QSplitter *>(node)) {
        QJsonArray children;
        QJsonArray sizes;
        const QList<int> current = splitter->sizes();
        for (int i = 0; i < splitter->count(); ++i) {
            children.append(saveNode(splitter->widget(i), ids));
            sizes.append(current.value(i));
        }
        object[QStringLiteral("orientation")] = orientationName(splitter->orientation());
        object[QStringLiteral("sizes")] = sizes;
        object[QStringLiteral("children")] = children;
    }
    return object;
}

QJsonObject MainWindow::saveGrid(const QHash<DocumentTab *, int> &ids) const
{
    QJsonObject grid;
    grid[QStringLiteral("root")] = saveNode(m_root, ids);
    grid[QStringLiteral("active")] = int(groups().indexOf(m_activeGroup));
    return grid;
}

void MainWindow::restoreNode(const QJsonObject &node, EvenSplitter *parent, const QMap<int, DocumentTab *> &tabs,
                             QSet<DocumentTab *> &placed)
{
    if (node.contains(QStringLiteral("children"))) {
        const auto orientation = node.value(QStringLiteral("orientation")).toString() == QLatin1String("vertical")
                                     ? Qt::Vertical
                                     : Qt::Horizontal;
        EvenSplitter *splitter = createSplitter(orientation);
        parent->addWidget(splitter);
        const QJsonArray children = node.value(QStringLiteral("children")).toArray();
        for (const QJsonValue &child : children)
            restoreNode(child.toObject(), splitter, tabs, placed);
        QList<int> sizes;
        for (const QJsonValue &size : node.value(QStringLiteral("sizes")).toArray())
            sizes.append(qMax(1, size.toInt()));
        if (sizes.size() == splitter->count())
            splitter->setSizes(sizes);
        return;
    }
    EditorGroup *group = createGroup();
    parent->addWidget(group);
    for (const QJsonValue &id : node.value(QStringLiteral("tabs")).toArray()) {
        DocumentTab *tab = tabs.value(id.toInt());
        if (tab && !placed.contains(tab)) {
            addTab(tab, group);
            placed.insert(tab);
        }
    }
    if (DocumentTab *current = tabs.value(node.value(QStringLiteral("current")).toInt(-1)))
        group->setCurrentIndex(group->indexOf(current));
}

void MainWindow::restoreGrid(const QJsonObject &grid, const QMap<int, DocumentTab *> &tabs)
{
    QSet<DocumentTab *> placed;
    const QJsonObject root = grid.value(QStringLiteral("root")).toObject();
    // A root that is one group is a branch of one.
    const QJsonArray children = root.contains(QStringLiteral("tabs")) ? QJsonArray{root}
                                                                       : root.value(QStringLiteral("children")).toArray();
    EditorGroup *initial = mainGroup();
    if (!children.isEmpty() && initial && initial->count() == 0) {
        // The saved grid replaces the empty group a window starts with.
        m_root->setOrientation(root.value(QStringLiteral("orientation")).toString() == QLatin1String("vertical")
                                   ? Qt::Vertical
                                   : Qt::Horizontal);
        for (const QJsonValue &child : children)
            restoreNode(child.toObject(), m_root, tabs, placed);
        m_groupHistory.removeAll(initial);
        m_modeButtons.remove(initial);
        if (m_activeGroup == initial)
            m_activeGroup = nullptr;
        initial->setParent(nullptr);
        initial->disconnect(this);
        initial->deleteLater();
        QList<int> sizes;
        for (const QJsonValue &size : root.value(QStringLiteral("sizes")).toArray())
            sizes.append(qMax(1, size.toInt()));
        if (sizes.size() == m_root->count())
            m_root->setSizes(sizes);
    }
    const QList<EditorGroup *> all = groups();
    EditorGroup *active = all.value(grid.value(QStringLiteral("active")).toInt(-1));
    if (!active)
        active = m_activeGroup ? m_activeGroup : all.value(0);
    for (auto it = tabs.cbegin(); it != tabs.cend(); ++it) {
        if (it.value() && !placed.contains(it.value()))
            addTab(it.value(), active);
    }
    // Forced: the active group may have been removed above.
    m_activeGroup = nullptr;
    activateGroup(active);
}

void MainWindow::focusNextGroup(int step)
{
    const QList<EditorGroup *> all = groups();
    const int at = int(all.indexOf(m_activeGroup));
    if (all.size() < 2 || at < 0)
        return;
    EditorGroup *next = all[(at + step + all.size()) % all.size()];
    activateGroup(next);
    if (DocumentTab *tab = next->currentTab())
        tab->editor()->setFocus();
    else
        next->setFocus();
}

void MainWindow::newFile()
{
    addTab(new DocumentTab);
}

bool MainWindow::openFile(const QString &path)
{
    // Open files are registered under their canonical path: one lookup
    // instead of resolving every open tab's path.
    std::shared_ptr<TextFile> file = TextFile::find(path);
    if (file && !QFileInfo(path).canonicalFilePath().isEmpty()) {
        for (EditorGroup *group : groups()) {
            for (int i = 0; i < group->count(); ++i) {
                if (group->tab(i)->file() == file.get()) {
                    activateGroup(group);
                    group->setCurrentIndex(i);
                    return true;
                }
            }
        }
    }

    // Open in another window: a new tab on the same document.
    if (!file) {
        file = TextFile::create();
        if (!file->load(path)) {
            Settings::instance().removeRecentFile(path);
            QMessageBox::warning(this, tr("Open File"),
                                 tr("Could not open %1:\n%2").arg(QDir::toNativeSeparators(path), file->errorString()));
            return false;
        }
    }
    addTab(new DocumentTab(file));
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
    connect(tab, &DocumentTab::titleChanged, this, [this, tab] { updateTabTitle(tab); });
    connect(tab, &DocumentTab::cursorPositionChanged, this, [this, tab] {
        if (tab == currentTab())
            updateStatus();
    });
    connect(tab, &DocumentTab::viewModeChanged, this, [this] { updateModeControls(); });
    connect(tab, &DocumentTab::indentationChanged, this, [this, tab] {
        if (tab == currentTab())
            updateStatus();
    });
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
    connect(tab, &DocumentTab::indentationChanged, &session, &SessionManager::markDirty);
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
        if (m_autoSaveMode == QLatin1String("afterDelay") && tab->isModified()
            && !tab->path().isEmpty())
            saveTimer->start();
    });
    connect(tab, &DocumentTab::linkActivated, this, [this, tab](const QString &url) { openLink(tab, url); });
    connect(tab, &DocumentTab::filesDropped, this, [this](const QStringList &files) {
        for (const QString &file : files)
            openFile(file);
    });
    connect(tab, &DocumentTab::textChanged, this, [this, tab] {
        if (tab == currentTab() && !m_outlineDock->isHidden() && tab->isMarkdown())
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

namespace {

std::function<MainWindow::SaveConflictChoice(DocumentTab *)> &saveConflictHandler()
{
    static std::function<MainWindow::SaveConflictChoice(DocumentTab *)> handler;
    return handler;
}

} // namespace

void MainWindow::setSaveConflictHandler(std::function<SaveConflictChoice(DocumentTab *)> handler)
{
    saveConflictHandler() = std::move(handler);
}

MainWindow::SaveConflictChoice MainWindow::askSaveConflict(DocumentTab *tab)
{
    if (const auto &handler = saveConflictHandler())
        return handler(tab);
    // VS Code's save conflict notification, without Compare (not yet).
    QMessageBox box(QMessageBox::Warning, productName(),
                    tr("%1 has been changed on disk since you opened or last saved it.").arg(tab->displayName()),
                    QMessageBox::NoButton, this);
    box.setInformativeText(tr("Overwrite it with your version, or revert to the version on disk and lose your changes?"));
    QPushButton *overwrite = box.addButton(tr("&Overwrite"), QMessageBox::AcceptRole);
    QPushButton *revert = box.addButton(tr("&Revert"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == overwrite)
        return SaveConflictChoice::Overwrite;
    if (box.clickedButton() == revert)
        return SaveConflictChoice::Revert;
    return SaveConflictChoice::Cancel;
}

bool MainWindow::resolveSaveConflict(DocumentTab *tab)
{
    TextFile *file = tab->file();
    bool ok = false;
    switch (askSaveConflict(tab)) {
    case SaveConflictChoice::Overwrite:
        ok = file->save(file->path(), true);
        break;
    case SaveConflictChoice::Revert:
        // Nothing is left unsaved afterwards, which is what callers ask.
        ok = file->revert();
        break;
    case SaveConflictChoice::Cancel:
        return false;
    }
    if (!ok)
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1:\n%2")
                                                   .arg(QDir::toNativeSeparators(file->path()), file->errorString()));
    return ok;
}

bool MainWindow::save(DocumentTab *tab)
{
    if (tab->path().isEmpty())
        return saveAs(tab);
    if (!tab->save(tab->path())) {
        if (tab->file()->hasSaveConflict())
            return resolveSaveConflict(tab);
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
    // Laid out like Windows Notepad's prompt: no icon, Save / Don't Save / Cancel.
    QMessageBox box(QMessageBox::NoIcon, productName(),
                    tr("Do you want to save changes to %1?").arg(tab->displayName()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    // Alt+S and Alt+N, as in VS Code (abstractTaskService.ts "&&Save",
    // "Do&&n't Save"); Esc cancels.
    box.button(QMessageBox::Save)->setText(tr("&Save"));
    box.button(QMessageBox::Discard)->setText(tr("Do&n't Save"));
    if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
        // A larger question on the left with room around it, buttons below.
        QFont font = label->font();
        font.setPointSizeF(font.pointSizeF() * 1.3);
        label->setFont(font);
        label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        label->setMinimumWidth(360);
        label->setContentsMargins(4, 8, 4, 16);
    }
    box.setDefaultButton(QMessageBox::Save);
    const auto answer = box.exec();
    if (answer == QMessageBox::Save)
        return save(tab);
    return answer == QMessageBox::Discard;
}

bool MainWindow::closeTab(EditorGroup *group, int index)
{
    DocumentTab *tab = group->tab(index);
    // Only the last tab of a document asks; the others just go.
    if (!tab || (tab->otherViews().isEmpty() && !confirmDiscard(tab)))
        return false;
    tab->setClosing();
    const bool hadFocus = tab->isAncestorOf(QApplication::focusWidget());
    {
        const QScopedValueRollback<bool> removing(m_removingTab, true);
        group->removeTab(group->indexOf(tab));
    }
    tab->deleteLater();
    // Like VS Code, focus stays in the group, on its next tab.
    if (hadFocus && group == m_activeGroup && group->currentTab())
        group->currentTab()->editor()->setFocus();
    removeGroupIfEmpty(group);
    // Its changes were saved or discarded; drop the backup now.
    SessionManager::instance().sync();
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
    // One session write for all of them, not one per tab.
    const SessionManager::SyncBatch batch;
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
    // VS Code's editor title context menu: the split commands and the new
    // window ones act on the selection when the clicked tab is part of it
    // (editorCommandsContext.ts resolveCommandsContext()).
    const QList<DocumentTab *> selected = group->isSelected(tab) ? group->selectedTabs() : QList<DocumentTab *>{tab};
    const auto split = [this, group, selected](SplitDirection direction) {
        return [this, group, selected, direction] {
            if (groups().contains(group))
                splitTabs(group, selected, direction);
        };
    };
    menu.addSeparator();
    menu.addAction(tr("Split Up"), this, split(SplitDirection::Up));
    menu.addAction(tr("Split Down"), this, split(SplitDirection::Down));
    menu.addAction(tr("Split Left"), this, split(SplitDirection::Left));
    menu.addAction(tr("Split Right"), this, split(SplitDirection::Right));
    menu.addSeparator();
    menu.addAction(tr("Move to New Window"), this, [this, selected] { moveTabsToNewWindow(selected); });
    menu.addAction(tr("Copy to New Window"), this,
                   [this, selected] { moveTabsToNewWindow(selected, std::nullopt, false, true); });
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
    dragMoveEvent(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
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
        const bool copy = dragGeometry::isCopyOperation(event->modifiers());
        dropTabs(EditorGroup::tabsForMime(event->mimeData()), m_activeGroup, -1, SplitDirection::None, copy);
        event->setDropAction(copy ? Qt::CopyAction : Qt::MoveAction);
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

void MainWindow::showIndentationMenu()
{
    DocumentTab *tab = currentTab();
    if (!tab)
        return;
    QMenu menu(this);
    const indentation::Settings current = tab->indentation();
    // VS Code's "Indent Using Spaces / Tabs": sizes 1 to 8, for this file only.
    for (const bool spaces : {true, false}) {
        QMenu *submenu = menu.addMenu(spaces ? tr("Indent Using Spaces") : tr("Indent Using Tabs"));
        for (int size = 1; size <= 8; ++size) {
            QAction *a = submenu->addAction(QString::number(size), this,
                                            [tab, size, spaces] { tab->setIndentation({size, spaces}); });
            a->setCheckable(true);
            a->setChecked(current.insertSpaces == spaces && current.tabSize == size);
        }
    }
    menu.addSeparator();
    menu.addAction(tr("Detect Indentation from Content"), this, [tab] { tab->detectIndentation(); });
    for (const bool spaces : {true, false}) {
        QAction *a = menu.addAction(spaces ? tr("Convert Indentation to Spaces") : tr("Convert Indentation to Tabs"),
                                    this, [tab, spaces] { tab->convertIndentation(spaces); });
        a->setEnabled(!tab->isReadOnly());
    }
    popupAboveCell(&menu, m_indentation);
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

bool MainWindow::reopenWithEncoding(DocumentTab *tab, const DocumentTab::Encoding &encoding)
{
    if (tab->isModified()) {
        QMessageBox box(QMessageBox::Warning, productName(),
                        tr("%1 has unsaved changes. Discard them and reopen with another encoding?")
                            .arg(tab->displayName()),
                        QMessageBox::Discard | QMessageBox::Cancel, this);
        box.button(QMessageBox::Discard)->setText(tr("&Discard"));
        box.setDefaultButton(QMessageBox::Cancel);
        if (box.exec() != QMessageBox::Discard)
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
    QMenu menu(this);
    QMenu *reopen = menu.addMenu(tr("Reopen with Encoding"));
    reopen->setEnabled(!tab->path().isEmpty());
    QMenu *save = menu.addMenu(tr("Save with Encoding"));
    const auto fill = [&](QMenu *parent, const QList<encoding::Entry> &entries, bool checkable) {
        for (const encoding::Entry &entry : entries) {
            const QString name = entry.name;
            QAction *a = parent->addAction(entry.label);
            if (checkable) {
                connect(a, &QAction::triggered, this, [tab, name] { tab->setEncoding(name); });
                a->setCheckable(true);
                a->setChecked(tab->encoding() == name);
            } else {
                connect(a, &QAction::triggered, this, [this, tab, name] { reopenWithEncoding(tab, name); });
            }
        }
    };
    for (QMenu *target : {reopen, save}) {
        const bool checkable = target == save;
        bool first = true;
        for (const auto &group : encoding::curatedGroups()) {
            if (!first)
                target->addSeparator();
            first = false;
            fill(target, group, checkable);
        }
        // Every other codec: a long list that the menu scrolls.
        if (const auto others = encoding::otherEntries(); !others.isEmpty()) {
            target->addSeparator();
            fill(target->addMenu(tr("More…")), others, checkable);
        }
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

    // About 600 definitions, sorted by locale once rather than on every open.
    struct Language
    {
        QString text;
        QString name;
    };
    static const std::map<QString, QList<Language>> sections = [] {
        std::map<QString, QList<Language>> result;
        for (const auto &def : theme::syntaxRepository().definitions()) {
            if (!def.isHidden())
                result[def.translatedSection()].append({def.translatedName(), def.name()});
        }
        for (auto &[section, languages] : result) {
            std::sort(languages.begin(), languages.end(),
                      [](const Language &a, const Language &b) { return a.text.localeAwareCompare(b.text) < 0; });
        }
        return result;
    }();
    for (const auto &[section, languages] : sections) {
        QMenu *submenu = menu.addMenu(section.isEmpty() ? tr("Other") : section);
        for (const Language &language : languages)
            add(submenu, language.text, language.name);
    }
    popupAboveCell(&menu, m_language);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Closing the last window quits; a single window among several only asks
    // about its own tabs.
    auto &session = SessionManager::instance();
    bool sync = false;
    if (!session.isQuitting()) {
        if (session.isLastWindow(this)) {
            if (!session.prepareQuit()) {
                event->ignore();
                return;
            }
        } else {
            // Once per document, and not for one still shown in another window.
            const QList<DocumentTab *> closing = tabs();
            QSet<const TextFile *> asked;
            for (DocumentTab *tab : closing) {
                const QList<DocumentTab *> others = tab->otherViews();
                const bool elsewhere = std::any_of(others.begin(), others.end(),
                                                   [&](DocumentTab *other) { return !closing.contains(other); });
                if (elsewhere || asked.contains(tab->file()))
                    continue;
                asked.insert(tab->file());
                if (!confirmDiscard(tab)) {
                    event->ignore();
                    return;
                }
            }
            for (DocumentTab *tab : closing)
                tab->setClosing();
            sync = true;
        }
    }
    session.unregisterWindow(this);
    // The other windows' session, without this one's discarded backups. Not
    // for the last window: prepareQuit() already wrote the final session.
    if (sync)
        session.sync();
    event->accept();
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate && m_autoSaveMode == QLatin1String("onFocusChange"))
        autoSaveAll();
    // Like VS Code's TextFileEditorTracker: look at the disk again on return,
    // in case a change notification was missed.
    if (event->type() == QEvent::WindowActivate)
        TextFile::checkAllOnDisk();
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
        // Never overwrite a file changed on disk without asking.
        if (tab->file()->hasSaveConflict()) {
            statusBar()->showMessage(tr("Auto save skipped: %1 changed on disk. Save it to choose which version to keep.")
                                         .arg(QDir::toNativeSeparators(tab->path())),
                                     5000);
            return;
        }
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
    m_autoSaveMode = Settings::instance().text(key::autoSave);
    const QString &mode = m_autoSaveMode;
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
    {
        const QScopedValueRollback<bool> removing(m_removingTab, true);
        group->removeTab(group->indexOf(tab));
    }
    removeGroupIfEmpty(group);
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

DocumentTab *MainWindow::copyTab(DocumentTab *tab, EditorGroup *group, int index)
{
    if (!tab || !group || !groups().contains(group))
        return nullptr;
    // A group shows a file once (editorGroupView.ts doOpenEditor() finds the
    // existing editor): copying there only brings it forward.
    for (int i = 0; i < group->count(); ++i) {
        if (group->tab(i)->file() == tab->file()) {
            activateGroup(group);
            group->setCurrentIndex(i);
            return group->tab(i);
        }
    }
    DocumentTab *copy = tab->openAnotherView();
    addTab(copy, group, index);
    return copy;
}

void MainWindow::dropTabs(const QList<DocumentTab *> &tabs, EditorGroup *target, int index, SplitDirection split,
                          bool copy)
{
    if (tabs.isEmpty() || !target || !groups().contains(target))
        return;
    auto *source = qobject_cast<MainWindow *>(tabs.constFirst()->window());
    EditorGroup *from = source ? source->groupOf(tabs.constFirst()) : nullptr;
    if (!from)
        return;
    // editorDropTarget.ts onDragOver(): a group's only tab cannot be moved
    // onto that group's own body.
    if (!copy && split != SplitDirection::None && from == target && from->count() < 2)
        return;
    EditorGroup *group = split == SplitDirection::None ? target : addGroup(target, split);
    // Within their own group tabs only move (editorTabsControl.ts
    // isMoveOperation()).
    if (from == group)
        copy = false;
    // handleDrop(): onto the source group's own body nothing happens.
    if (split == SplitDirection::None && index < 0 && from == group && !copy)
        return;
    QPointer<EditorGroup> guard(group);
    DocumentTab *last = nullptr;
    for (DocumentTab *tab : tabs) {
        if (!guard)
            break;
        if (copy) {
            last = copyTab(tab, group, index);
        } else if (DocumentTab *existing = [&]() -> DocumentTab * {
                       for (int i = 0; i < group->count(); ++i) {
                           if (group->tab(i) != tab && group->tab(i)->file() == tab->file())
                               return group->tab(i);
                       }
                       return nullptr;
                   }()) {
            // Moving a file onto a group that already shows it leaves one
            // tab there (editorGroupView.ts doMoveEditorAcrossGroups() opens
            // the existing editor and closes the moved one).
            if (auto *owner = qobject_cast<MainWindow *>(tab->window())) {
                if (EditorGroup *its = owner->groupOf(tab))
                    owner->closeTab(its, its->indexOf(tab));
                if (owner != this && owner->tabCount() == 0)
                    owner->close();
            }
            last = existing;
        } else {
            receiveTab(tab, group, index);
            last = tab;
        }
        // The next one goes after it (editorGroupView.ts moveEditors()).
        if (index >= 0 && last && guard && group->indexOf(last) >= 0)
            index = group->indexOf(last) + 1;
    }
    if (guard && last && group->indexOf(last) >= 0) {
        activateGroup(group);
        group->setCurrentIndex(group->indexOf(last));
        last->editor()->setFocus();
    }
}

void MainWindow::moveTabToNewWindow(DocumentTab *tab, std::optional<QPoint> globalPos, bool closeIfEmpty)
{
    moveTabsToNewWindow({tab}, globalPos, closeIfEmpty);
}

MainWindow *MainWindow::moveTabsToNewWindow(const QList<DocumentTab *> &tabs, std::optional<QPoint> globalPos,
                                            bool closeIfEmpty, bool copy)
{
    QList<DocumentTab *> mine;
    for (DocumentTab *tab : tabs) {
        if (tab && groupOf(tab))
            mine.append(tab);
    }
    if (mine.isEmpty())
        return nullptr;
    auto *window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    // editorActions.ts Move / CopyEditorToNewWindowAction.
    for (DocumentTab *tab : std::as_const(mine)) {
        if (copy) {
            window->adoptTab(tab->openAnotherView());
        } else {
            detachTab(tab);
            window->adoptTab(tab);
        }
    }
    if (globalPos)
        window->move(*globalPos - QPoint(60, 14));
    window->show();
    if (closeIfEmpty && tabCount() == 0)
        close();
    return window;
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
    } else if (changedKey == QLatin1String(key::rememberRecent)) {
        m_recentMenu->menuAction()->setVisible(settings.flag(key::rememberRecent));
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
        && m_autoSaveMode == QLatin1String("onFocusChange"))
        autoSave(m_previousTab);
    m_previousTab = tab;
    if (tab)
        updateTabTitle(tab);
    else
        setWindowTitle(productName());
    updateStatus();
    // Visibility first: a hidden outline is not built.
    updateOutlineVisibility();
    updateOutline();
    updateModeControls();
}

void MainWindow::updateTabTitle(DocumentTab *tab)
{
    EditorGroup *group = groupOf(tab);
    if (!group)
        return;
    const int index = group->indexOf(tab);
    // VS Code marks an editor whose file was deleted on disk.
    const QString name = tab->file()->isOrphaned() ? tr("%1 (deleted)").arg(tab->displayName()) : tab->displayName();
    group->setTabText(index, tab->isModified() ? QStringLiteral("● ") + name : name);
    group->setTabToolTip(index, QDir::toNativeSeparators(tab->path()));
    if (tab == currentTab())
        setWindowTitle(tr("%1 — %2").arg(name, productName()));
}

void MainWindow::updateStatus()
{
    DocumentTab *tab = currentTab();
    // With no file open the columns (and their dividers) are hidden.
    for (QLabel *label : {m_position, m_indentation, m_zoom, m_lineEnding, m_encoding, m_language})
        label->setVisible(tab != nullptr);
    if (!tab)
        return;
    const TextView *editor = tab->editor();
    const QTextCursor cursor = editor->textCursor();
    QString position = tr("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1);
    if (editor->cursorCount() > 1)
        position += u' ' + tr("(%1 selections)").arg(editor->cursorCount());
    else if (cursor.hasSelection())
        position += u' ' + tr("(%1 selected)").arg(cursor.selectionEnd() - cursor.selectionStart());
    m_position->setText(position);
    m_indentation->setText(tab->indentation().insertSpaces ? tr("Spaces: %1").arg(tab->indentation().tabSize)
                                                           : tr("Tab Size: %1").arg(tab->indentation().tabSize));
    m_zoom->setText(QStringLiteral("%1%").arg(m_zoomPercent));
    m_lineEnding->setText(tab->lineEnding() == DocumentTab::LineEnding::CRLF ? tr("Windows (CRLF)")
                                                                             : tr("Unix (LF)"));
    m_encoding->setText(tab->encodingName());
    m_language->setText(tab->languageName());
}

void MainWindow::updateOutline()
{
    m_outlineTimer->stop();
    DocumentTab *tab = currentTab();
    TextFile *file = tab && tab->isMarkdown() ? tab->file() : nullptr;
    if (!file) {
        m_outline->clear();
        m_outlineFile = nullptr;
        return;
    }
    // Hidden: built when shown (updateOutlineVisibility). Unchanged, as
    // after switching back to the tab it was built for: kept.
    if (m_outlineDock->isHidden() || (m_outlineFile == file && m_outlineSerial == file->changeSerial()))
        return;
    if (const OutlineSnapshot *cached = cachedOutline(file)) {
        showOutline(file, *cached);
        return;
    }
    if (file->document()->characterCount() > outlineThreadChars) {
        // The old outline stays while the same file is parsed again; another
        // file's does not.
        if (m_outlineFile != file) {
            m_outline->clear();
            m_outlineFile = nullptr;
        }
        startOutlineJob(tab);
        return;
    }
    showOutline(file, outlineOf(tab));
}

const OutlineSnapshot &MainWindow::outlineOf(DocumentTab *tab)
{
    TextFile *file = tab->file();
    if (const OutlineSnapshot *cached = cachedOutline(file))
        return *cached;
    ++m_outlineParses;
    return cacheOutline(file, {file->changeSerial(), parseHeadings(tab->text())});
}

const OutlineSnapshot *MainWindow::cachedOutline(const TextFile *file) const
{
    const auto it = m_outlineCache.constFind(file);
    // The pointer is null for a file that closed and whose address was reused.
    if (it == m_outlineCache.constEnd() || it->file != file || it->snapshot.serial != file->changeSerial())
        return nullptr;
    return &it->snapshot;
}

const OutlineSnapshot &MainWindow::cacheOutline(TextFile *file, const OutlineSnapshot &snapshot)
{
    for (auto it = m_outlineCache.begin(); it != m_outlineCache.end();) {
        if (it->file)
            ++it;
        else
            it = m_outlineCache.erase(it);
    }
    CachedOutline &entry = m_outlineCache[file];
    entry.file = file;
    entry.snapshot = snapshot;
    return entry.snapshot;
}

void MainWindow::showOutline(TextFile *file, const OutlineSnapshot &snapshot)
{
    m_outlineFile = file;
    m_outlineSerial = snapshot.serial;
    m_outline->clear();

    // Nest each heading under the nearest previous heading of lower level.
    QList<QPair<int, QTreeWidgetItem *>> stack;
    for (const OutlineHeading &heading : snapshot.headings) {
        while (!stack.isEmpty() && stack.last().first >= heading.level)
            stack.removeLast();
        auto *item = stack.isEmpty() ? new QTreeWidgetItem(m_outline) : new QTreeWidgetItem(stack.last().second);
        item->setText(0, heading.text);
        item->setData(0, outlineLineRole, heading.line);
        stack.append({heading.level, item});
    }
    m_outline->expandAll();
}

void MainWindow::startOutlineJob(DocumentTab *tab)
{
    if (m_outlineJobRunning) {
        m_outlineJobSkipped = true;
        return;
    }
    m_outlineJobRunning = true;
    m_outlineJobSkipped = false;
    m_outlineJobFile = tab->file();
    ++m_outlineParses;
    // The text is read here; QPromise is not copyable, the pool function must be.
    auto promise = std::make_shared<QPromise<OutlineSnapshot>>();
    promise->start();
    m_outlineWatcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start(
        [promise, serial = tab->file()->changeSerial(), text = tab->text()] {
            promise->addResult(OutlineSnapshot{serial, parseHeadings(text)});
            promise->finish();
        });
}

void MainWindow::finishOutlineJob()
{
    m_outlineJobRunning = false;
    TextFile *file = m_outlineJobFile;
    m_outlineJobFile = nullptr;
    if (file) {
        const OutlineSnapshot snapshot = m_outlineWatcher->result();
        cacheOutline(file, snapshot);
        DocumentTab *tab = currentTab();
        // Shown even when edits have made it a little old: newer than what is
        // there. The timer (or the skipped request) builds the next one.
        if (tab && tab->file() == file && tab->isMarkdown() && !m_outlineDock->isHidden())
            showOutline(file, snapshot);
    }
    if (m_outlineJobSkipped) {
        m_outlineJobSkipped = false;
        updateOutline();
    }
}

void MainWindow::updateOutlineVisibility()
{
    DocumentTab *tab = currentTab();
    const bool show = m_outlineAction->isChecked() && tab && tab->isMarkdown();
    const bool wasHidden = m_outlineDock->isHidden();
    m_outlineDock->setVisible(show);
    if (show && wasHidden)
        updateOutline();
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
        int line = -1;
        if (anchor.startsWith(QLatin1String("fn-"))) {
            const QStringList lines = tab->text().split(u'\n');
            for (int i = 0; i < lines.size() && line < 0; ++i) {
                if (lines[i].startsWith(QLatin1String("[^"))
                    && slugOf(lines[i].section(u']', 0, 0).mid(2).toLower()) == anchor.mid(3))
                    line = i + 1;
            }
        } else {
            QHash<QString, int> seen;
            for (const OutlineHeading &heading : outlineOf(tab).headings) {
                QString slug = slugOf(heading.text);
                const int n = seen[slug]++;
                if (n > 0)
                    slug += u'-' + QString::number(n);
                if (slug == anchor) {
                    line = heading.line;
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
    if (md::isNetworkPath(path)) // Not even a look: on Windows that is an SMB login.
        return;
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
            it.value()->setText(shown->iconText()); // Without the menu mnemonic.
        }
    }
    DocumentTab *tab = currentTab();
    const bool markdown = tab && tab->isMarkdown();
    action(QStringLiteral("edit.format"))->setEnabled(markdown);
    for (auto it = m_actions.cbegin(); it != m_actions.cend(); ++it) {
        if (it.key().startsWith(QLatin1String("format.")))
            it.value()->setEnabled(markdown);
    }
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
