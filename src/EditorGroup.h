// A tab strip with its documents, like a VS Code editor group. The strip
// holds the tabs on the left and optional action buttons on the right.
#pragma once

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
    // Null if the tab was destroyed since the id was issued.
    static DocumentTab *tabForId(quint64 id);

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

    // Widgets shown at the right end of the tab strip.
    void addAction(QWidget *widget);

    QTabBar *tabBar() const { return m_tabBar; }
    // Index a tab dropped at `pos` (group coordinates) would take: the slot
    // under the cursor on the strip, the end elsewhere.
    int insertIndexAt(const QPoint &pos) const;
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
    // A tab from this or another group was dropped; insertIndex is -1 for the
    // body (append).
    void tabDropped(quint64 tabId, int insertIndex);
    // A drag started here ended on no drop target.
    void tabDroppedOutside(DocumentTab *tab, const QPoint &globalPos);

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

    QWidget *m_strip;
    QTabBar *m_tabBar;
    QHBoxLayout *m_actions;
    QStackedWidget *m_body;
    QStackedWidget *m_tabs;
    WelcomeView *m_empty;
    QWidget *m_dropOverlay;
    QWidget *m_dropMarker;
    QPointer<DocumentTab> m_pressedTab;
    QPoint m_pressPos; // In tab bar coordinates, where m_pressedTab was pressed.
};

} // namespace vn
