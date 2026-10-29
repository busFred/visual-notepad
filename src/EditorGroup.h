// A tab strip with its documents, like a VS Code editor group. The strip
// holds the tabs on the left and optional action buttons on the right.
#pragma once

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QTabBar;

namespace vn {

class DocumentTab;

class EditorGroup : public QWidget
{
    Q_OBJECT

public:
    explicit EditorGroup(QWidget *parent = nullptr);

    int count() const;
    int currentIndex() const;
    DocumentTab *tab(int index) const;
    DocumentTab *currentTab() const;
    int indexOf(DocumentTab *tab) const;

    int addTab(DocumentTab *tab, const QString &text);
    // Removes the tab without deleting it.
    void removeTab(int index);
    void setCurrentIndex(int index);
    void setTabText(int index, const QString &text);
    void setTabToolTip(int index, const QString &toolTip);

    // Widgets shown at the right end of the tab strip.
    void addAction(QWidget *widget);

Q_SIGNALS:
    void currentChanged(int index);
    void tabCloseRequested(int index);
    void tabContextMenuRequested(int index, const QPoint &globalPos);
    // Double-click on the empty part of the strip.
    void newTabRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateEmpty();

    QWidget *m_strip;
    QTabBar *m_tabBar;
    QHBoxLayout *m_actions;
    QStackedWidget *m_body;
    QStackedWidget *m_tabs;
    QLabel *m_empty;
};

} // namespace vn
