#include "EvenSplitter.h"

#include <QMouseEvent>
#include <QPointer>

namespace vn {

namespace {

class EvenHandle;

// Transparent strip over a handle that widens its hit area; mouse events are
// handed to the handle so dragging and double-clicking work as on the handle.
class GrabStrip : public QWidget
{
public:
    GrabStrip(EvenHandle *handle, QWidget *parent);

protected:
    void mousePressEvent(QMouseEvent *event) override { forward(event); }
    void mouseMoveEvent(QMouseEvent *event) override { forward(event); }
    void mouseReleaseEvent(QMouseEvent *event) override { forward(event); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { forward(event); }

private:
    void forward(QMouseEvent *event);
    EvenHandle *m_handle;
};

class EvenHandle : public QSplitterHandle
{
public:
    EvenHandle(Qt::Orientation orientation, EvenSplitter *splitter)
        : QSplitterHandle(orientation, splitter)
        , m_splitter(splitter)
        , m_strip(new GrabStrip(this, splitter))
    {
        m_strip->setCursor(orientation == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
        m_strip->hide();
    }
    ~EvenHandle() override { delete m_strip.data(); }

    void syncStrip()
    {
        if (!m_strip)
            return;
        const int m = EvenSplitter::grabMargin;
        QRect r = geometry();
        r = orientation() == Qt::Horizontal ? r.adjusted(-m, 0, m, 0) : r.adjusted(0, -m, 0, m);
        m_strip->setGeometry(r);
        m_strip->setVisible(isVisible());
        m_strip->raise();
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_splitter->distributeEvenly();
    }
    void moveEvent(QMoveEvent *event) override
    {
        QSplitterHandle::moveEvent(event);
        syncStrip();
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QSplitterHandle::resizeEvent(event);
        syncStrip();
    }
    void showEvent(QShowEvent *event) override
    {
        QSplitterHandle::showEvent(event);
        syncStrip();
    }
    void hideEvent(QHideEvent *event) override
    {
        QSplitterHandle::hideEvent(event);
        if (m_strip)
            m_strip->hide();
    }

private:
    EvenSplitter *m_splitter;
    QPointer<GrabStrip> m_strip;
};

GrabStrip::GrabStrip(EvenHandle *handle, QWidget *parent)
    : QWidget(parent)
    , m_handle(handle)
{
    setAttribute(Qt::WA_TranslucentBackground);
}

void GrabStrip::forward(QMouseEvent *event)
{
    const QPointF pos = event->position() + QPointF(this->pos() - m_handle->pos());
    QMouseEvent copy(event->type(), pos, event->globalPosition(), event->button(), event->buttons(),
                     event->modifiers());
    QCoreApplication::sendEvent(m_handle, &copy);
    event->accept();
}

} // namespace

EvenSplitter::EvenSplitter(Qt::Orientation orientation, QWidget *parent)
    : QSplitter(orientation, parent)
{
}

QSplitterHandle *EvenSplitter::createHandle()
{
    return new EvenHandle(orientation(), this);
}

void EvenSplitter::distributeEvenly()
{
    int visible = 0;
    for (int i = 0; i < count(); ++i)
        visible += !widget(i)->isHidden();
    if (visible == 0)
        return;
    int total = 0;
    const QList<int> current = sizes();
    for (int size : current)
        total += size;
    QList<int> next;
    int index = 0;
    int given = 0;
    for (int i = 0; i < count(); ++i) {
        if (widget(i)->isHidden()) {
            next << 0;
            continue;
        }
        ++index;
        const int share = index == visible ? total - given : total / visible;
        given += share;
        next << share;
    }
    setSizes(next);
}

} // namespace vn
