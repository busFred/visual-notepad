#include "EditorGroup.h"

#include "DocumentTab.h"
#include "Theme.h"
#include "WelcomeView.h"

#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QLoggingCategory>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>

namespace vn {

// QT_LOGGING_RULES="vn.drag.debug=true" traces tab drags.
Q_LOGGING_CATEGORY(lcDrag, "vn.drag", QtWarningMsg)

// Set when one of our groups takes a dragged tab. Other applications may
// report a drop of our private format as accepted (Chromium-based windows
// do), so QDrag's result alone can't tell "dropped outside our windows".
static bool s_tabDropTaken = false;

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
        painter.fillRect(rect(), theme::color(theme::Role::tabStrip));
        painter.fillRect(QRect(0, height() - 1, width(), 1), theme::color(theme::Role::divider));
    }
};

// Translucent wash over the group body while a tab is dragged over it.
class DropOverlay : public QWidget
{
public:
    explicit DropOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QColor color = theme::color(theme::Role::selection);
        color.setAlphaF(0.4);
        QPainter(this).fillRect(rect(), color);
    }
};

// Insertion marker between tabs.
class DropMarker : public QWidget
{
public:
    explicit DropMarker(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), theme::color(theme::Role::accent)); }
};

EditorGroup *groupAround(QWidget *widget)
{
    for (; widget; widget = widget->parentWidget()) {
        if (auto *group = qobject_cast<EditorGroup *>(widget))
            return group;
    }
    return nullptr;
}

// Editors accept text drops themselves and would hide a tab drag from the
// group, so tab drags over any descendant are routed to the group here.
class DragRouter : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const auto type = event->type();
        if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop
            && type != QEvent::DragLeave)
            return false;
        auto *widget = qobject_cast<QWidget *>(watched);
        EditorGroup *group = groupAround(widget);
        // The group's own handlers deal with events aimed at it.
        if (!group || group == widget)
            return false;
        if (type == QEvent::DragLeave) {
            group->hideDropHints();
            return false;
        }
        auto *drop = static_cast<QDropEvent *>(event);
        if (!drop->mimeData()->hasFormat(EditorGroup::tabMimeType()))
            return false;
        group->handleTabDrag(drop, widget->mapTo(group, drop->position().toPoint()));
        return true;
    }
};

QHash<quint64, QPointer<DocumentTab>> &tabRegistry()
{
    static QHash<quint64, QPointer<DocumentTab>> registry;
    return registry;
}

} // namespace

QString EditorGroup::tabMimeType()
{
    return QStringLiteral("application/x-visnotepad-tab");
}

QMimeData *EditorGroup::mimeForTab(DocumentTab *tab)
{
    static quint64 lastId = 0;
    auto &registry = tabRegistry();
    // Forget tabs that were destroyed since earlier drags.
    registry.removeIf([](auto it) { return it.value().isNull(); });
    const quint64 id = ++lastId;
    registry.insert(id, tab);
    auto *mime = new QMimeData;
    mime->setData(tabMimeType(), QByteArray::number(id));
    return mime;
}

DocumentTab *EditorGroup::tabForId(quint64 id)
{
    return tabRegistry().value(id).data();
}

EditorGroup::EditorGroup(QWidget *parent)
    : QWidget(parent)
    , m_strip(new TabStrip(this))
    , m_tabBar(new QTabBar(m_strip))
    , m_actions(new QHBoxLayout)
    , m_body(new QStackedWidget(this))
    , m_tabs(new QStackedWidget(m_body))
    , m_empty(new WelcomeView(m_body))
    , m_dropOverlay(new DropOverlay(this))
    , m_dropMarker(new DropMarker(m_strip))
{
    setAcceptDrops(true);
    static const auto *router = [] {
        auto *r = new DragRouter(qApp);
        qApp->installEventFilter(r);
        return r;
    }();
    Q_UNUSED(router);

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

    connect(m_empty, &WelcomeView::commandRequested, this, &EditorGroup::commandRequested);
    connect(m_empty, &WelcomeView::recentFileRequested, this, &EditorGroup::recentFileRequested);
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

int EditorGroup::addTab(DocumentTab *tab, const QString &text, int index)
{
    if (index < 0 || index > m_tabs->count())
        index = m_tabs->count();
    // Insert the page first so currentChanged sees it.
    m_tabs->insertWidget(index, tab);
    m_tabBar->insertTab(index, text);
    updateEmpty();
    return index;
}

void EditorGroup::moveTab(int from, int to)
{
    // Goes through tabMoved, which reorders the pages.
    m_tabBar->moveTab(from, to);
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
    m_tabBar->updateGeometry();
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
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress) {
            qCDebug(lcDrag) << "press on" << watched;
            Q_EMIT activated();
            auto *mouse = static_cast<QMouseEvent *>(event);
            m_pressedTab = nullptr;
            if (watched == m_tabBar && mouse->button() == Qt::LeftButton)
                m_pressedTab = tab(m_tabBar->tabAt(mouse->position().toPoint()));
        } else if (type == QEvent::MouseButtonRelease) {
            m_pressedTab = nullptr;
        } else if (type == QEvent::MouseMove && watched == m_tabBar) {
            qCDebug(lcDrag) << "move" << static_cast<QMouseEvent *>(event)->globalPosition() << "pressed tab" << m_pressedTab;
        }
        if (type == QEvent::MouseMove && m_pressedTab && watched == m_tabBar) {
            // Leaving the strip turns the tab move into a drag to another group
            // or window.
            auto *mouse = static_cast<QMouseEvent *>(event);
            const int slack = QApplication::startDragDistance();
            const QPoint inStrip = m_strip->mapFromGlobal(mouse->globalPosition().toPoint());
            if ((mouse->buttons() & Qt::LeftButton) && !m_strip->rect().adjusted(-slack, -slack, slack, slack).contains(inStrip)) {
                DocumentTab *dragged = m_pressedTab;
                m_pressedTab = nullptr;
                startDrag(dragged);
                return true;
            }
        }
        if (type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) {
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

void EditorGroup::startDrag(DocumentTab *tab)
{
    const int index = indexOf(tab);
    if (index < 0)
        return;
    // End QTabBar's own tab move first; the drag takes over from here.
    const QPoint local = m_tabBar->mapFromGlobal(QCursor::pos());
    QMouseEvent release(QEvent::MouseButtonRelease, local, QCursor::pos(), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(m_tabBar, &release);

    const QRect rect = m_tabBar->tabRect(indexOf(tab));
    auto *drag = new QDrag(m_tabBar);
    drag->setMimeData(mimeForTab(tab));
    drag->setPixmap(m_tabBar->grab(rect));
    drag->setHotSpot(local - rect.topLeft());

    // The drop may close this group's window, so keep guarded handles.
    QPointer<EditorGroup> self(this);
    QPointer<DocumentTab> guarded(tab);
    qCDebug(lcDrag) << "start drag of tab" << index;
    s_tabDropTaken = false;
    const Qt::DropAction result = drag->exec(Qt::MoveAction);
    qCDebug(lcDrag) << "drag finished" << result << "taken by us" << s_tabDropTaken;
    // Not taken by any of our windows: like VS Code, the tab opens in a new
    // window wherever it was released, even over another application.
    // Released over one of our own windows (or cancelled there with Escape):
    // nothing happens.
    const bool overOurWindow = QApplication::topLevelAt(QCursor::pos()) != nullptr;
    if (!s_tabDropTaken && !overOurWindow && self && guarded && self->indexOf(guarded) >= 0)
        Q_EMIT self->tabDroppedOutside(guarded, QCursor::pos());
}

int EditorGroup::insertIndexAt(const QPoint &pos) const
{
    if (!m_strip->geometry().contains(pos) || count() == 0)
        return -1;
    // Only the horizontal position matters: the cursor is already on the strip.
    const int x = m_tabBar->mapFrom(this, pos).x();
    for (int i = 0; i < count(); ++i) {
        if (x < m_tabBar->tabRect(i).center().x())
            return i;
    }
    return count();
}

void EditorGroup::handleTabDrag(QDropEvent *event, const QPoint &pos)
{
    const bool onStrip = m_strip->geometry().contains(pos);
    // On the strip -1 from insertIndexAt only means an empty group: append.
    const int index = onStrip ? insertIndexAt(pos) : -1;

    if (event->type() == QEvent::Drop) {
        hideDropHints();
        event->setDropAction(Qt::MoveAction);
        event->accept();
        bool ok = false;
        const quint64 id = event->mimeData()->data(tabMimeType()).toULongLong(&ok);
        if (ok) {
            s_tabDropTaken = true;
            Q_EMIT tabDropped(id, index);
        }
        return;
    }

    event->setDropAction(Qt::MoveAction);
    event->accept();
    m_dropOverlay->setGeometry(m_body->geometry());
    m_dropOverlay->setVisible(!onStrip);
    m_dropOverlay->raise();
    if (onStrip) {
        const int slot = index < 0 ? 0 : index;
        int x = 0;
        if (count() > 0)
            x = slot < count() ? m_tabBar->tabRect(slot).left() : m_tabBar->tabRect(count() - 1).right() + 1;
        const QPoint at = m_strip->mapFromGlobal(m_tabBar->mapToGlobal(QPoint(x, 0)));
        m_dropMarker->setGeometry(at.x() - 1, 3, 2, stripHeight - 6);
        m_dropMarker->show();
        m_dropMarker->raise();
    } else {
        m_dropMarker->hide();
    }
}

void EditorGroup::hideDropHints()
{
    m_dropOverlay->hide();
    m_dropMarker->hide();
}

void EditorGroup::dragEnterEvent(QDragEnterEvent *event)
{
    // Anything but a tab (e.g. files from the file manager) is the window's.
    // Ignoring the entry lets Qt offer the drag to the parents; it then sends
    // the moves and the drop straight to the window. (Forwarding them from
    // here looped: Qt delivers every move to the widget that took the entry.)
    if (!event->mimeData()->hasFormat(tabMimeType()))
        event->ignore();
    else
        handleTabDrag(event, event->position().toPoint());
}

void EditorGroup::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat(tabMimeType()))
        event->ignore();
    else
        handleTabDrag(event, event->position().toPoint());
}

void EditorGroup::dragLeaveEvent(QDragLeaveEvent *)
{
    hideDropHints();
}

void EditorGroup::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasFormat(tabMimeType()))
        event->ignore();
    else
        handleTabDrag(event, event->position().toPoint());
}

void EditorGroup::mousePressEvent(QMouseEvent *event)
{
    Q_EMIT activated();
    QWidget::mousePressEvent(event);
}

void EditorGroup::updateEmpty()
{
    const bool empty = m_tabBar->count() == 0;
    m_body->setCurrentWidget(empty ? static_cast<QWidget *>(m_empty) : m_tabs);
    // Like VS Code, an empty group shows only its welcome page, no tab strip.
    m_strip->setVisible(!empty);
    // QTabBar does not always re-announce its size when tabs change before
    // the window is shown, which left the layout's cached hint at zero height
    // and the tabs invisible.
    m_tabBar->updateGeometry();
}

} // namespace vn
