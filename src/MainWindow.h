#pragma once

#include "DocumentTab.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>

#include <functional>

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

    // Which tabs a close request affects, relative to the tab at `index`.
    enum class CloseScope { This, Others, Right, Saved, All };
    // Stops at the first tab whose unsaved-changes prompt is cancelled.
    bool closeTabs(int index, CloseScope scope);
    int tabCount() const;
    // Tabs in strip order, for the session.
    QList<DocumentTab *> tabs() const;
    int currentTabIndex() const;
    void setCurrentTabIndex(int index);
    // Asks to save a modified tab; false if the user cancelled.
    bool confirmDiscard(DocumentTab *tab);

protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
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

    DocumentTab *currentTab() const;
    void addTab(DocumentTab *tab);
    void openWithDialog();
    bool save(DocumentTab *tab);
    bool saveAs(DocumentTab *tab);
    bool closeTab(int index);
    void moveTabToNewWindow(DocumentTab *tab);
    void showTabContextMenu(int index, const QPoint &globalPos);
    void revealInFileManager(const QString &path);
    // Pops the menu up just above a status bar cell.
    void popupAboveCell(QMenu *menu, QWidget *cell);
    void showLineEndingMenu();
    void showEncodingMenu();
    void showLanguageMenu();
    bool reopenWithEncoding(DocumentTab *tab, DocumentTab::Encoding encoding);
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

    QDockWidget *m_outlineDock;
    QTreeWidget *m_outline;
    QTimer *m_outlineTimer;

    QToolButton *m_modeButton;
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
    QLabel *m_zoom;
    QLabel *m_lineEnding;
    QLabel *m_encoding;
    QLabel *m_language;
    int m_zoomPercent = 100;
};

} // namespace vn
