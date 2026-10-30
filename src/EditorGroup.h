// A tab strip with its documents, like a VS Code editor group. The strip
// holds the tabs on the left and optional action buttons on the right.
#pragma once

#include "DragGeometry.h"

#include <QList>
#include <QPoint>
#include <QPointer>
#include <QWidget>

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QHBoxLayout;
class QLabel;
class QMouseEvent;
class QStackedWidget;
class QMimeData;
class QTabBar;

namespace vn {

class DocumentTab;
class WelcomeView;

class EditorGroup : public QWidget
{
    Q_OBJECT

public:
    // Mime type of a dragged tab; the payload is an id from mimeForTab().
    static QString tabMimeType();
    // Mime data for dragging `tab`. The id stays valid while the tab lives; it
    // is looked up through a registry of guarded pointers, never dereferenced
    // blindly. The caller owns the result (or passes it to QDrag).
    static QMimeData *mimeForTab(DocumentTab *tab);
    // Several tabs, dragged together; tabsForMime() reads either.
    static QMimeData *mimeForTabs(const QList<DocumentTab *> &tabs);
    // Null if the tab was destroyed since the id was issued.
    static DocumentTab *tabForId(quint64 id);
    // The tabs of a drag that still exist, in drag order.
    static QList<DocumentTab *> tabsForMime(const QMimeData *mime);

    explicit EditorGroup(QWidget *parent = nullptr);

    int count() const;
    int currentIndex() const;
    DocumentTab *tab(int index) const;
    DocumentTab *currentTab() const;
    int indexOf(DocumentTab *tab) const;

    // Inserts at `index`, or at the end when negative.
    int addTab(DocumentTab *tab, const QString &text, int index = -1);
    // Reorders without changing the current tab.
    void moveTab(int from, int to);
    // Removes the tab without deleting it.
    void removeTab(int index);
    void setCurrentIndex(int index);
    void setTabText(int index, const QString &text);
    void setTabToolTip(int index, const QString &toolTip);

    // VS Code's selected editors: the current tab and any others picked with
    // Ctrl+click or Shift+click, in strip order. Making another tab current
    // any other way clears it.
    QList<DocumentTab *> selectedTabs() const;
    bool isSelected(DocumentTab *tab) const;
    // Strip indexes of the tabs picked besides the current one (what the tab
    // bar highlights), found in one pass over the selection.
    QList<int> extraSelectedIndexes() const;
    // A left click on the tab at `index` with `modifiers`
    // (multiEditorTabsControl.ts handleClickOrTouch()); plain clicks on a
    // selected tab keep the selection until releaseTab() so it can be dragged.
    void clickTab(int index, Qt::KeyboardModifiers modifiers);
    void releaseTab();
    void clearSelection();

    // Widgets shown at the right end of the tab strip.
    void addAction(QWidget *widget);

    QTabBar *tabBar() const { return m_tabBar; }
    // Index a tab dropped at `pos` (group coordinates) would take: the slot
    // under the cursor on the strip, the end elsewhere.
    int insertIndexAt(const QPoint &pos) const;
    // Where tabs dragged to `pos` (group coordinates) with `modifiers` would
    // go: a split of the body's edge, or None to merge into the group. For
    // the strip always None.
    dragGeometry::SplitDirection dropZoneAt(const QPoint &pos, Qt::KeyboardModifiers modifiers) const;
    // Handles the drag events of a tab drag; `pos` is in group coordinates.
    // Called for the group itself and for editor children that would
    // otherwise swallow the drag.
    void handleTabDrag(QDropEvent *event, const QPoint &pos);
    void hideDropHints();

Q_SIGNALS:
    void currentChanged(int index);
    void tabCloseRequested(int index);
    void tabContextMenuRequested(int index, const QPoint &globalPos);
    // Double-click on the empty part of the strip.
    void newTabRequested();
    // From the welcome page shown when the group has no tabs.
    void commandRequested(const QString &id);
    void recentFileRequested(const QString &path);
    // The user clicked or focused into this group.
    void activated();
    // Tabs from this or another group were dropped: on the strip at
    // insertIndex, else (-1) on the body, into a new group beside this one
    // unless split is None. With copy (Ctrl held) the sources stay.
    void tabsDropped(const QList<DocumentTab *> &tabs, int insertIndex, dragGeometry::SplitDirection split, bool copy);
    // A drag started here ended on no drop target.
    void tabsDroppedOutside(const QList<DocumentTab *> &tabs, const QPoint &globalPos, bool copy);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void updateEmpty();
    void startDrag(DocumentTab *tab);
    // Makes `active` current with `others` selected besides it.
    void setSelection(DocumentTab *active, const QList<DocumentTab *> &others);
    void selectBetween(DocumentTab *target, DocumentTab *anchor);
    void unselect(DocumentTab *tab);

    QWidget *m_strip;
    QTabBar *m_tabBar;
    QHBoxLayout *m_actions;
    QStackedWidget *m_body;
    QStackedWidget *m_tabs;
    WelcomeView *m_empty = nullptr; // created by updateEmpty() when first needed
    QWidget *m_dropOverlay;
    QWidget *m_dropMarker;
    QPointer<DocumentTab> m_pressedTab;
    QPoint m_pressPos; // In tab bar coordinates, where m_pressedTab was pressed.
    // Selected tabs besides the current one.
    QList<QPointer<DocumentTab>> m_selection;
    // The anchor of Shift+click ranges (lastSingleSelectSelectedEditor).
    QPointer<DocumentTab> m_anchor;
    // Tabs in the order they were last current, most recent first.
    QList<QPointer<DocumentTab>> m_recent;
    bool m_selecting = false;
    bool m_clearSelectionOnRelease = false;
};

} // namespace vn
