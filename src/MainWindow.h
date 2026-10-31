#pragma once

#include "DocumentTab.h"
#include "DragGeometry.h"

#include <QHash>
#include <QMainWindow>
#include <QMap>
#include <QPoint>
#include <QPointer>
#include <QSet>

#include <functional>
#include <optional>

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QMenu;
class QJsonObject;
class QSplitter;
class QTimer;
class QToolButton;
class QTreeWidget;
class QUrl;
template<typename T> class QFutureWatcher;

namespace vn {

class EditorGroup;
class EvenSplitter;

// One heading of the outline: what core::outline returns, as Qt types.
struct OutlineHeading
{
    int level;
    int line;
    QString text;
};

// The outline of a file's text as it was at TextFile::changeSerial() `serial`.
struct OutlineSnapshot
{
    quint64 serial = 0;
    QList<OutlineHeading> headings;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void newFile();
    // Shows `path` in a new tab, or the tab that already shows it. A file
    // from TextFile::largeLoadBytes up loads on a worker thread without
    // blocking the window: this returns true at once and the tab follows
    // (tabs of such files appear in the order they were asked for; a failure
    // is reported then). `opened` is called with the tab when it shows the
    // file, at once for a small file; never if the load fails or the window
    // closes first.
    bool openFile(const QString &path, std::function<void(DocumentTab *)> opened = {});
    // Whether files are still loading in the background.
    bool isOpening() const { return !m_pendingOpens.isEmpty(); }
    // Takes ownership of a tab from another window.
    void adoptTab(DocumentTab *tab);

    // Editor groups, in a grid of nested splitters like VS Code's editor
    // part. groups() lists them in reading order (GroupsOrder.GRID_APPEARANCE);
    // mainGroup() is the first. The active group is the one last clicked or
    // focused; commands, status bar and outline follow its current tab.
    QList<EditorGroup *> groups() const;
    EditorGroup *mainGroup() const;
    EditorGroup *activeGroup() const { return m_activeGroup; }
    EditorGroup *groupOf(DocumentTab *tab) const;
    void activateGroup(EditorGroup *group);
    using SplitDirection = dragGeometry::SplitDirection;
    // A new empty group beside `reference` (editorPart.ts addGroup()); None
    // returns `reference`.
    EditorGroup *addGroup(EditorGroup *reference, SplitDirection direction);
    // Split Editor: a new group beside the active one showing its current
    // tab, or its selected tabs (editorCommands.ts splitEditor()).
    void splitEditor(SplitDirection direction);
    // Focus Next / Previous Editor Group, in grid order, wrapping around.
    void focusNextGroup(int step);
    DocumentTab *currentTab() const;
    // Moves a tab of this or another window into `group` of this window at
    // `index` (negative: end) and makes it current. A window left without tabs
    // by the move closes without prompting.
    void receiveTab(DocumentTab *tab, EditorGroup *group, int index);
    // Opens another view of `tab`'s file in `group` at `index`, or, if the
    // group already shows that file, makes that tab current. Returns the tab.
    DocumentTab *copyTab(DocumentTab *tab, EditorGroup *group, int index);
    // A drop of `tabs` on `target`: at `index` of its strip (negative: end),
    // or into a new group beside it for a split, moved or copied
    // (editorDropTarget.ts handleDrop()).
    void dropTabs(const QList<DocumentTab *> &tabs, EditorGroup *target, int index, SplitDirection split, bool copy);
    // With closeIfEmpty (a tab dragged out) this window closes if it has no
    // tabs left.
    void moveTabToNewWindow(DocumentTab *tab, std::optional<QPoint> globalPos = std::nullopt,
                            bool closeIfEmpty = false);
    // One new window with all `tabs`, moved or (copy) as other views of them.
    MainWindow *moveTabsToNewWindow(const QList<DocumentTab *> &tabs, std::optional<QPoint> globalPos = std::nullopt,
                                    bool closeIfEmpty = false, bool copy = false);

    // The grid for the session: splitters with their orientation and sizes,
    // groups with their tabs (as ids from `ids`; others are left out) and
    // current tab, and which group is active.
    QJsonObject saveGrid(const QHash<DocumentTab *, int> &ids) const;
    // Rebuilds the grid saveGrid() wrote and places `tabs` (by id) in it; tabs
    // it does not mention, and all of them for an empty `grid` (sessions from
    // before the grid), go to the active group in id order.
    void restoreGrid(const QJsonObject &grid, const QMap<int, DocumentTab *> &tabs);

    // Which tabs a close request affects, relative to the tab at `index`.
    enum class CloseScope { This, Others, Right, Saved, All };
    // Stops at the first tab whose unsaved-changes prompt is cancelled.
    bool closeTabs(int index, CloseScope scope);
    int tabCount() const;
    // Tabs of every group in grid order, each in strip order, for the session.
    QList<DocumentTab *> tabs() const;
    // Index into tabs() of the active group's current tab, or -1.
    int currentTabIndex() const;
    void setCurrentTabIndex(int index);
    // Asks to save a modified tab; false if the user cancelled.
    bool confirmDiscard(DocumentTab *tab);

    // A save found the file changed on disk since it was loaded or saved.
    enum class SaveConflictChoice { Overwrite, Revert, Cancel };
    // Tests answer the conflict prompt through this instead of a dialog;
    // an empty function restores the dialog.
    static void setSaveConflictHandler(std::function<SaveConflictChoice(DocumentTab *)> handler);
    // Links that would run a program (md::isLaunchable files, schemes other
    // than http, https, mailto and file) ask first. Tests answer through the
    // confirmer (given the path or URL shown; true opens) instead of a
    // dialog, and catch what links hand to the OS through the opener instead
    // of QDesktopServices::openUrl. Empty functions restore the defaults.
    static void setLaunchConfirmer(std::function<bool(const QString &)> confirmer);
    static void setUrlOpener(std::function<void(const QUrl &)> opener);

    // Outline bookkeeping, for tests. outlineParses() counts the documents
    // parsed for the outline (not the ones it served from its cache);
    // outlineBusy() is true while a large document is parsed off the GUI thread.
    int outlineParses() const { return m_outlineParses; }
    bool outlineBusy() const { return m_outlineJobRunning; }

protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QAction *makeAction(const QString &id);
    QAction *makeAction(const QString &id, std::function<void()> slot);
    QAction *action(const QString &id) const;
    void createActions();
    void createMenus();
    void createOutline();
    void createStatusBar();

    EditorGroup *createGroup();
    EvenSplitter *createSplitter(Qt::Orientation orientation);
    // Takes an empty group out of the grid, unless it is the only one
    // (workbench.editor.closeEmptyGroups, on by default).
    void removeGroupIfEmpty(EditorGroup *group);
    // A splitter left with one pane gives way to it.
    void collapse(EvenSplitter *splitter);
    // A new group beside `group` with other views of those of `tabs` it shows.
    EditorGroup *splitTabs(EditorGroup *group, const QList<DocumentTab *> &tabs, SplitDirection direction);
    QJsonObject saveNode(QWidget *node, const QHash<DocumentTab *, int> &ids) const;
    // Builds `node` as the last pane of `parent` and fills its groups.
    void restoreNode(const QJsonObject &node, EvenSplitter *parent, const QMap<int, DocumentTab *> &tabs,
                     QSet<DocumentTab *> &placed);
    void wireGroup(EditorGroup *group);
    void addTab(DocumentTab *tab, EditorGroup *group = nullptr, int index = -1);
    // Stops this window tracking the tab and removes it from its group; the
    // tab is neither closed nor deleted.
    void detachTab(DocumentTab *tab);
    QToolButton *createModeButton(EditorGroup *group);
    void createSplitButton(EditorGroup *group);
    QAction *modeAction(DocumentTab::ViewMode mode) const;
    void openWithDialog();
    bool save(DocumentTab *tab);
    bool saveAs(DocumentTab *tab);
    SaveConflictChoice askSaveConflict(DocumentTab *tab);
    // Asks and acts; true when nothing is left unsaved.
    bool resolveSaveConflict(DocumentTab *tab);
    bool closeTab(EditorGroup *group, int index);
    bool closeTabs(EditorGroup *group, int index, CloseScope scope);
    void showTabContextMenu(EditorGroup *group, int index, const QPoint &globalPos);
    void revealInFileManager(const QString &path);
    // Pops the menu up just above a status bar cell.
    void popupAboveCell(QMenu *menu, QWidget *cell);
    void showIndentationMenu();
    void showLineEndingMenu();
    void showEncodingMenu();
    void showLanguageMenu();
    bool reopenWithEncoding(DocumentTab *tab, const DocumentTab::Encoding &encoding);
    void setViewMode(DocumentTab::ViewMode mode);
    void zoom(int steps);
    void applyZoom();
    void goToLine();
    void showPreferences(const QString &page);
    void onSettingChanged(const QString &key);
    void fillRecentMenu();
    void openLink(DocumentTab *tab, const QString &url);
    // Asks before a link runs `target` (a native path or a URL).
    bool confirmLaunch(const QString &target);
    static void openUrl(const QUrl &url);

    // Auto save: writes a modified tab that has a path; failures only reach
    // the status bar.
    QTimer *autoSaveTimer(DocumentTab *tab) const;
    void autoSave(DocumentTab *tab);
    void autoSaveAll();
    void applyAutoSave();

    void onCurrentTabChanged();
    void updateTabTitle(DocumentTab *tab);
    void updateStatus();
    void updateOutline();
    void updateOutlineVisibility();
    void showOutline(TextFile *file, const OutlineSnapshot &snapshot);
    void startOutlineJob(DocumentTab *tab);
    void finishOutlineJob();
    // The headings of `tab`'s text: from the cache while the text is
    // unchanged, otherwise parsed (and cached).
    const OutlineSnapshot &outlineOf(DocumentTab *tab);
    const OutlineSnapshot *cachedOutline(const TextFile *file) const;
    const OutlineSnapshot &cacheOutline(TextFile *file, const OutlineSnapshot &snapshot);
    void updateModeControls();

    QPointer<DocumentTab> m_previousTab;
    // The grid's root; a group or a splitter of the other orientation in
    // each pane, never two nested splitters of one orientation.
    EvenSplitter *m_root;
    EditorGroup *m_activeGroup = nullptr;
    // Most recently active first (GroupsOrder.MOST_RECENTLY_ACTIVE).
    QList<EditorGroup *> m_groupHistory;
    bool m_removingTab = false;

    QDockWidget *m_outlineDock;
    QTreeWidget *m_outline;
    QTimer *m_outlineTimer;
    // What the outline shows: the file and its TextFile::changeSerial().
    QPointer<TextFile> m_outlineFile;
    quint64 m_outlineSerial = 0;
    // The last outline of every file, so that switching tabs, or rebuilding
    // for text that did not change, parses nothing.
    // Entries of closed files are dropped when the next one is added.
    struct CachedOutline
    {
        QPointer<TextFile> file;
        OutlineSnapshot snapshot;
    };
    QHash<const TextFile *, CachedOutline> m_outlineCache;
    int m_outlineParses = 0;
    // Large documents are parsed on a worker thread, one at a time.
    QFutureWatcher<OutlineSnapshot> *m_outlineWatcher;
    QPointer<TextFile> m_outlineJobFile;
    bool m_outlineJobRunning = false;
    // A rebuild was asked for while the job ran.
    bool m_outlineJobSkipped = false;
    // Cached key::autoSave, read on every keystroke otherwise.
    QString m_autoSaveMode;

    // Files loading on worker threads (openFile()), in the order asked.
    struct PendingOpen
    {
        std::shared_ptr<TextFile> file;
        QString path;
        std::function<void(DocumentTab *)> opened;
        bool finished = false;
        bool ok = false;
    };
    void finishPendingOpens();
    QList<std::shared_ptr<PendingOpen>> m_pendingOpens;

    QHash<EditorGroup *, QToolButton *> m_modeButtons;
    QActionGroup *m_modeGroup;
    QAction *m_modeSource;
    QAction *m_modeSourceVisual;
    QAction *m_modeVisual;
    QAction *m_outlineAction;
    QAction *m_splitAction;
    QAction *m_wrapAction;
    QAction *m_statusBarAction;
    QHash<QString, QAction *> m_actions;
    QMenu *m_recentMenu = nullptr;

    QLabel *m_position;
    QLabel *m_indentation;
    QLabel *m_zoom;
    QLabel *m_lineEnding;
    QLabel *m_encoding;
    QLabel *m_language;
    int m_zoomPercent = 100;
};

} // namespace vn
