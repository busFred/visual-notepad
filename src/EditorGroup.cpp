#include "EditorGroup.h"

#include "DocumentTab.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

namespace vn {

namespace {

constexpr int stripHeight = 32;

// Paints the strip background and the divider under the tabs; the active tab
// covers the divider so it joins the editor below.
class TabStrip : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), theme::statusBar);
        painter.fillRect(QRect(0, height() - 1, width(), 1), theme::divider);
    }
};

} // namespace

EditorGroup::EditorGroup(QWidget *parent)
    : QWidget(parent)
    , m_strip(new TabStrip(this))
    , m_tabBar(new QTabBar(m_strip))
    , m_actions(new QHBoxLayout)
    , m_body(new QStackedWidget(this))
    , m_tabs(new QStackedWidget(m_body))
    , m_empty(new QLabel(tr("Empty. Drag a tab here or open a file"), m_body))
{
    m_strip->setObjectName(QStringLiteral("TabStrip"));
    m_strip->setFixedHeight(stripHeight);

    m_tabBar->setDocumentMode(true);
    m_tabBar->setDrawBase(false);
    m_tabBar->setExpanding(false);
    m_tabBar->setMovable(true);
    m_tabBar->setTabsClosable(true);
    m_tabBar->setUsesScrollButtons(true);
    m_tabBar->setElideMode(Qt::ElideNone);

    m_actions->setContentsMargins(0, 0, 8, 1);
    m_actions->setSpacing(6);

    auto *stripLayout = new QHBoxLayout(m_strip);
    stripLayout->setContentsMargins(0, 0, 0, 0);
    stripLayout->setSpacing(0);
    stripLayout->addWidget(m_tabBar, 0, Qt::AlignBottom);
    stripLayout->addStretch(1);
    stripLayout->addLayout(m_actions);

    m_empty->setObjectName(QStringLiteral("EmptyGroup"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_body->addWidget(m_tabs);
    m_body->addWidget(m_empty);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_strip);
    layout->addWidget(m_body, 1);

    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0)
            m_tabs->setCurrentIndex(index);
        updateEmpty();
        Q_EMIT currentChanged(index);
    });
    connect(m_tabBar, &QTabBar::tabCloseRequested, this, &EditorGroup::tabCloseRequested);
    m_tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabBar, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int index = m_tabBar->tabAt(pos);
        if (index >= 0)
            Q_EMIT tabContextMenuRequested(index, m_tabBar->mapToGlobal(pos));
    });
    m_tabBar->installEventFilter(this);
    m_strip->installEventFilter(this);
    connect(m_tabBar, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *widget = m_tabs->widget(from);
        m_tabs->removeWidget(widget);
        m_tabs->insertWidget(to, widget);
        m_tabs->setCurrentIndex(m_tabBar->currentIndex());
    });

    updateEmpty();
}

int EditorGroup::count() const
{
    return m_tabBar->count();
}

int EditorGroup::currentIndex() const
{
    return m_tabBar->currentIndex();
}

DocumentTab *EditorGroup::tab(int index) const
{
    return qobject_cast<DocumentTab *>(m_tabs->widget(index));
}

DocumentTab *EditorGroup::currentTab() const
{
    return tab(currentIndex());
}

int EditorGroup::indexOf(DocumentTab *tab) const
{
    return m_tabs->indexOf(tab);
}

int EditorGroup::addTab(DocumentTab *tab, const QString &text)
{
    // Insert the page first so currentChanged sees it.
    const int index = m_tabs->addWidget(tab);
    m_tabBar->insertTab(index, text);
    return index;
}

void EditorGroup::removeTab(int index)
{
    QWidget *widget = m_tabs->widget(index);
    if (!widget)
        return;
    // Drop the page first so that when the bar announces the new current
    // index, the page list already matches the bar.
    m_tabs->removeWidget(widget);
    widget->setParent(nullptr);
    m_tabBar->removeTab(index);
    updateEmpty();
}

void EditorGroup::setCurrentIndex(int index)
{
    m_tabBar->setCurrentIndex(index);
}

void EditorGroup::setTabText(int index, const QString &text)
{
    m_tabBar->setTabText(index, text);
}

void EditorGroup::setTabToolTip(int index, const QString &toolTip)
{
    m_tabBar->setTabToolTip(index, toolTip);
}

void EditorGroup::addAction(QWidget *widget)
{
    m_actions->addWidget(widget);
}

bool EditorGroup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabBar || watched == m_strip) {
        if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            // The strip itself only receives clicks outside the tab bar.
            const int index = watched == m_tabBar ? m_tabBar->tabAt(mouse->position().toPoint()) : -1;
            if (event->type() == QEvent::MouseButtonRelease && watched == m_tabBar && mouse->button() == Qt::MiddleButton
                && index >= 0) {
                Q_EMIT tabCloseRequested(index);
                return true;
            }
            if (event->type() == QEvent::MouseButtonDblClick && mouse->button() == Qt::LeftButton && index < 0) {
                Q_EMIT newTabRequested();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void EditorGroup::updateEmpty()
{
    m_body->setCurrentWidget(m_tabBar->count() > 0 ? static_cast<QWidget *>(m_tabs) : m_empty);
}

} // namespace vn
