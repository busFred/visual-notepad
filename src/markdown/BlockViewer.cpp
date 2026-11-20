#include "BlockViewer.h"

#include "../Theme.h"
#include "ZoomBar.h"

#include <QAbstractScrollArea>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace vn {

namespace {

constexpr qreal maxScale = 32;
constexpr qreal margin = 12; // Around the block, and under the bar.

} // namespace

// The block painted at the scale, in a scroll area: a block smaller than
// the window is centred, a larger one scrolls.
class BlockViewer::Canvas : public QAbstractScrollArea
{
public:
    explicit Canvas(BlockViewer *owner)
        : QAbstractScrollArea(owner)
        , m_owner(owner)
    {
        setFrameShape(QFrame::NoFrame);
        viewport()->setAutoFillBackground(false);
        setFocusPolicy(Qt::StrongFocus);
    }

    // Where the block's top-left is in the viewport.
    QPointF origin() const
    {
        const QSizeF size = m_owner->scaledSize();
        return QPointF(size.width() < viewport()->width() ? (viewport()->width() - size.width()) / 2
                                                         : -horizontalScrollBar()->value(),
                       size.height() < viewport()->height() ? (viewport()->height() - size.height()) / 2
                                                           : -verticalScrollBar()->value());
    }

    // The block's own coordinates of a point in the viewport.
    QPointF toBlock(const QPointF &point) const { return (point - origin()) / m_owner->scale(); }

    void updateRanges()
    {
        const QSizeF size = m_owner->scaledSize();
        horizontalScrollBar()->setRange(0, qMax(0, int(std::ceil(size.width())) - viewport()->width()));
        horizontalScrollBar()->setPageStep(viewport()->width());
        verticalScrollBar()->setRange(0, qMax(0, int(std::ceil(size.height())) - viewport()->height()));
        verticalScrollBar()->setPageStep(viewport()->height());
        horizontalScrollBar()->setSingleStep(24);
        verticalScrollBar()->setSingleStep(24);
        viewport()->update();
    }

    QList<QPair<int, int>> selection() const
    {
        if (m_anchor < 0 || m_head < 0 || m_anchor == m_head)
            return {};
        return {{std::min(m_anchor, m_head), std::max(m_anchor, m_head)}};
    }
    void select(int anchor, int head)
    {
        m_anchor = anchor;
        m_head = head;
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(viewport());
        painter.fillRect(event->rect(), theme::color(theme::Role::editor));
        painter.setClipRect(event->rect());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(origin());
        painter.scale(m_owner->scale(), m_owner->scale());
        RenderedSelection selected;
        selected.ranges = selection();
        m_owner->block().paintContent(painter, QPointF(0, 0), selected);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QAbstractScrollArea::resizeEvent(event);
        m_owner->refresh();
    }

    void scrollContentsBy(int, int) override { viewport()->update(); }

    void wheelEvent(QWheelEvent *event) override
    {
        if (event->modifiers() & Qt::ControlModifier) {
            const qreal steps = event->angleDelta().y() / 120.0;
            if (steps != 0)
                m_owner->zoomAt(event->position(), std::pow(1.2, steps));
            event->accept();
            return;
        }
        QAbstractScrollArea::wheelEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        // A press on text puts the selection's start there; elsewhere it clears it.
        const QPointF at = toBlock(event->position()) + QPointF(0, m_owner->block().stripHeight());
        m_selecting = m_owner->block().textAt(at);
        select(m_selecting ? m_owner->block().hitTest(at) : -1, -1);
        setFocus();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_selecting)
            return;
        const QPointF at = toBlock(event->position()) + QPointF(0, m_owner->block().stripHeight());
        select(m_anchor, m_owner->block().hitTest(at));
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            m_selecting = false;
    }

private:
    BlockViewer *m_owner;
    int m_anchor = -1;
    int m_head = -1;
    bool m_selecting = false;
};

BlockViewer::BlockViewer(std::unique_ptr<RenderedBlock> block, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_block(std::move(block))
{
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    // A window to move about and size: it has its maximise button.
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    setMinimumSize(320, 180);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_canvas = new Canvas(this);
    layout->addWidget(m_canvas);

    // The controls float over the bottom of the block.
    m_bar = new ZoomBar(m_canvas->viewport());
    // The content is centred, so leave room on both sides of the floating bar.
    const QSize padding(int(2 * margin), int(2 * (margin + m_bar->height())));
    QSize cap(900, 650);
    if (parent)
        cap = parent->window()->size() * 4 / 5;
    if (const QScreen *screen = this->screen())
        cap = cap.boundedTo(screen->availableGeometry().size());
    cap = cap.expandedTo(minimumSize());
    QSizeF content = m_block->contentSize();
    if (!content.isEmpty()) {
        content.scale(QSizeF((cap - padding).expandedTo(QSize(1, 1))), Qt::KeepAspectRatio);
        const QSizeF natural = m_block->contentSize();
        if (content.width() > natural.width())
            content = natural;
    }
    resize((QSize(int(std::ceil(content.width())), int(std::ceil(content.height()))) + padding)
               .expandedTo(minimumSize()).boundedTo(cap));

    connect(m_bar, &ZoomBar::fitRequested, this, [this] { fit(); });
    connect(m_bar, &ZoomBar::zoomOutRequested, this, [this] { zoomAt(viewCentre(), 1 / 1.25); });
    connect(m_bar, &ZoomBar::zoomInRequested, this, [this] { zoomAt(viewCentre(), 1.25); });
    connect(m_bar, &ZoomBar::actualSizeRequested, this, [this] { actualSize(viewCentre()); });
    connect(m_bar, &ZoomBar::sliderMoved, this, [this](qreal fraction) {
        // Exponential: every step is the same relative change.
        const qreal low = lowestScale();
        setScale(low * std::pow(maxScale / low, fraction), viewCentre());
    });
    m_canvas->setFocus();
}

BlockViewer::~BlockViewer() = default;

QWidget *BlockViewer::viewport() const
{
    return m_canvas->viewport();
}

QPointF BlockViewer::toViewport(const QPointF &blockPoint) const
{
    return m_canvas->origin() + (blockPoint - QPointF(0, m_block->stripHeight())) * m_scale;
}

QPointF BlockViewer::viewCentre() const
{
    return QPointF(m_canvas->viewport()->width() / 2.0, m_canvas->viewport()->height() / 2.0);
}

QSizeF BlockViewer::scaledSize() const
{
    return m_block->contentSize() * m_scale;
}

qreal BlockViewer::fitScale() const
{
    const QSizeF size = m_block->contentSize();
    if (size.isEmpty())
        return 1;
    const qreal w = qMax<qreal>(1, m_canvas->viewport()->width() - 2 * margin);
    const qreal h = qMax<qreal>(1, m_canvas->viewport()->height() - 2 * (margin + (m_bar ? m_bar->height() : 0)));
    // Smaller than the window stays at its real size rather than blow up.
    return std::min({w / size.width(), h / size.height(), 1.0});
}

qreal BlockViewer::lowestScale() const
{
    return qMin(fitScale(), m_scale);
}

void BlockViewer::refresh()
{
    if (!m_bar)
        return; // (Still being made.)
    if (m_fitted)
        m_scale = fitScale();
    m_canvas->updateRanges();
    const qreal low = lowestScale();
    m_bar->setZoom(maxScale > low ? std::log(m_scale / low) / std::log(maxScale / low) : 0, qRound(m_scale * 100));
    const QWidget *view = m_canvas->viewport();
    m_bar->move((view->width() - m_bar->width()) / 2, view->height() - m_bar->height() - int(margin));
    m_bar->raise();
}

void BlockViewer::fit()
{
    m_fitted = true;
    m_scale = fitScale();
    m_canvas->horizontalScrollBar()->setValue(0);
    m_canvas->verticalScrollBar()->setValue(0);
    refresh();
}

void BlockViewer::actualSize(const QPointF &anchor)
{
    setScale(1, anchor);
}

void BlockViewer::zoomAt(const QPointF &anchor, qreal factor)
{
    setScale(m_scale * factor, anchor);
}

void BlockViewer::setScale(qreal scale, const QPointF &anchor)
{
    // Zooming out past the fitted size has nothing to show.
    scale = qBound(lowestScale(), scale, maxScale);
    // The block's point under the anchor stays under it.
    const QPointF point = m_canvas->toBlock(anchor);
    m_scale = scale;
    m_fitted = std::abs(m_scale - fitScale()) < 1e-9;
    m_canvas->updateRanges();
    // A block smaller than the window is centred (no scrolling); otherwise
    // the scroll puts the point back under the anchor.
    const QSizeF size = scaledSize();
    if (size.width() >= m_canvas->viewport()->width())
        m_canvas->horizontalScrollBar()->setValue(qRound(point.x() * m_scale - anchor.x()));
    if (size.height() >= m_canvas->viewport()->height())
        m_canvas->verticalScrollBar()->setValue(qRound(point.y() * m_scale - anchor.y()));
    refresh();
}

void BlockViewer::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    refresh();
}

QString BlockViewer::selectedText() const
{
    const auto ranges = m_canvas->selection();
    if (ranges.isEmpty())
        return {};
    const QString text = m_block->renderedText();
    // (A formula is a placeholder character in the text.)
    return text.mid(ranges.first().first, ranges.first().second - ranges.first().first)
        .remove(QChar::ObjectReplacementCharacter);
}

void BlockViewer::selectAll()
{
    m_canvas->select(0, m_block->renderedText().size());
}

void BlockViewer::copy() const
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void BlockViewer::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        copy();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        return;
    }
    const QPointF center = viewCentre();
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomAt(center, 1.25);
        break;
    case Qt::Key_Minus:
        zoomAt(center, 1 / 1.25);
        break;
    case Qt::Key_0:
        fit();
        break;
    case Qt::Key_1:
        actualSize(center);
        break;
    default:
        QDialog::keyPressEvent(event); // Escape closes.
    }
}

} // namespace vn
