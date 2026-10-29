#pragma once

#include "DocumentTab.h"

#include <QHash>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>

#include <functional>
#include <optional>

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QMenu;
class QSplitter;
class QTimer;
class QToolButton;
class QTreeWidget;

namespace vn {

class EditorGroup;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void newFile();
    bool openFile(const QString &path);
    // Takes ownership of a tab from another window.
    void adoptTab(DocumentTab *tab);

    // Editor groups: the main one and, with Split Editor on, one on the right.
    // The active group is the one last clicked or focused; commands, status
    // bar and outline follow its current tab.
    EditorGroup *mainGroup() const { return m_group; }
    EditorGroup *sideGroup() const { return m_sideGroup; }
    EditorGroup *activeGroup() const { return m_activeGroup; }
    EditorGroup *groupOf(DocumentTab *tab) const;
    void activateGroup(EditorGroup *group);
    bool isSplit() const { return m_sideGroup != nullptr; }
    // Turning it off moves the right group's tabs back to the main group.
    void setSplit(bool on);
    DocumentTab *currentTab() const;
    // Moves a tab of this or another window into `group` of this window at
    // `index` (negative: end) and makes it current. A window left without tabs
    // by the move closes without prompting.
    void receiveTab(DocumentTab *tab, EditorGroup *group, int index);
    void moveToOtherGroup(DocumentTab *tab);
    // With closeIfEmpty (a tab dragged out) this window closes if it has no
    // tabs left.
    void moveTabToNewWindow(DocumentTab *tab, std::optional<QPoint> globalPos = std::nullopt,
                            bool closeIfEmpty = false);

    // Which tabs a close request affects, relative to the tab at `index`.
    enum class CloseScope { This, Others, Right, Saved, All };
    // Stops at the first tab whose unsaved-changes prompt is cancelled.
    bool closeTabs(int index, CloseScope scope);
    int tabCount() const;
    // Tabs of the main group then the right group, in strip order, for the session.
    QList<DocumentTab *> tabs() const;
    // Index into tabs() of the active group's current tab, or -1.
    int currentTabIndex() const;
    void setCurrentTabIndex(int index);
    // Asks to save a modified tab; false if the user cancelled.
    bool confirmDiscard(DocumentTab *tab);

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
    void createTabActions();
    void createOutline();
    void createStatusBar();

    QList<EditorGroup *> groups() const;
    void wireGroup(EditorGroup *group);
    void addTab(DocumentTab *tab, EditorGroup *group = nullptr, int index = -1);
    // Stops this window tracking the tab and removes it from its group; the
    // tab is neither closed nor deleted.
    void detachTab(DocumentTab *tab);
    QToolButton *createModeButton(EditorGroup *group);
    QAction *modeAction(DocumentTab::ViewMode mode) const;
    void openWithDialog();
    bool save(DocumentTab *tab);
    bool saveAs(DocumentTab *tab);
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
    void toggleSplitEditor(bool on);
    void setViewMode(DocumentTab::ViewMode mode);
    void zoom(int steps);
    void applyZoom();
    void goToLine();
    void showPreferences(const QString &page);
    void onSettingChanged(const QString &key);
    void fillRecentMenu();
    void openLink(DocumentTab *tab, const QString &url);

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
    void updateModeControls();

    QPointer<DocumentTab> m_previousTab;
    QSplitter *m_groups;
    EditorGroup *m_group;
    EditorGroup *m_sideGroup = nullptr;
    EditorGroup *m_activeGroup;

    QDockWidget *m_outlineDock;
    QTreeWidget *m_outline;
    QTimer *m_outlineTimer;

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
