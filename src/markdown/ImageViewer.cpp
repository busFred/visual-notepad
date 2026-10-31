#include "ImageViewer.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace vn {

namespace {

constexpr qreal maxScale = 32;
constexpr qreal margin = 12; // Around a fitted picture.
constexpr int sliderSteps = 1000;

} // namespace

ImageViewer::ImageViewer(const QImage &image, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_image(image)
{
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setCursor(Qt::OpenHandCursor);
    QSize size(900, 650);
    if (parent) {
        const QSize host = parent->window()->size();
        size = QSize(qMax(480, host.width() * 4 / 5), qMax(360, host.height() * 4 / 5));
    }
    resize(size);
    setMinimumSize(320, 180);

    // The controls float over the bottom of the picture.
    m_bar = new QWidget(this);
    m_bar->setAutoFillBackground(true);
    m_bar->setAttribute(Qt::WA_NoMousePropagation);
    m_bar->setCursor(Qt::ArrowCursor);
    auto *layout = new QHBoxLayout(m_bar);
    layout->setContentsMargins(8, 4, 8, 4);
    auto button = [&](const QString &text, const QString &tip) {
        auto *b = new QToolButton(m_bar);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(b);
        return b;
    };
    connect(button(QStringLiteral("Fit"), tr("Fit to window (0)")), &QToolButton::clicked, this, [this] { fit(); });
    connect(button(QStringLiteral("\u2212"), tr("Zoom out (-)")), &QToolButton::clicked, this,
            [this] { zoomAt(windowCentre(), 1 / 1.25); });
    m_slider = new QSlider(Qt::Horizontal, m_bar);
    m_slider->setRange(0, sliderSteps);
    m_slider->setFixedWidth(200);
    m_slider->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_slider);
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        // Exponential: every step is the same relative change.
        const qreal low = lowestScale();
        setScale(low * std::pow(maxScale / low, qreal(value) / sliderSteps), windowCentre());
    });
    connect(button(QStringLiteral("+"), tr("Zoom in (+)")), &QToolButton::clicked, this,
            [this] { zoomAt(windowCentre(), 1.25); });
    connect(button(QStringLiteral("1:1"), tr("Actual size (1)")), &QToolButton::clicked, this,
            [this] { actualSize(windowCentre()); });
    m_percent = new QLabel(m_bar);
    m_percent->setMinimumWidth(m_percent->fontMetrics().horizontalAdvance(QStringLiteral("3200%")));
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_percent);
    m_bar->adjustSize();
}

QPointF ImageViewer::windowCentre() const
{
    return QPointF(width() / 2.0, height() / 2.0);
}

qreal ImageViewer::lowestScale() const
{
    return qMin(fitScale(), m_scale);
}

void ImageViewer::refresh()
{
    // Follow the zoom without echoing it back through the slider.
    const QSignalBlocker block(m_slider);
    const qreal low = lowestScale();
    m_slider->setValue(qRound(std::log(m_scale / low) / std::log(maxScale / low) * sliderSteps));
    m_percent->setText(QStringLiteral("%1%").arg(qRound(m_scale * devicePixelRatioF() * 100)));
    m_bar->move((width() - m_bar->width()) / 2, height() - m_bar->height() - int(margin));
    update();
}

qreal ImageViewer::fitScale() const
{
    if (m_image.isNull())
        return 1;
    const qreal w = qMax<qreal>(1, width() - 2 * margin);
    const qreal h = qMax<qreal>(1, height() - 2 * margin);
    // Small pictures stay at their real size rather than blow up.
    return std::min({w / m_image.width(), h / m_image.height(), 1 / devicePixelRatioF()});
}

void ImageViewer::fit()
{
    m_fitted = true;
    m_scale = fitScale();
    m_offset = QPointF(width() - m_image.width() * m_scale, height() - m_image.height() * m_scale) / 2;
    refresh();
}

void ImageViewer::actualSize(const QPointF &anchor)
{
    setScale(1 / devicePixelRatioF(), anchor);
}

void ImageViewer::zoomAt(const QPointF &anchor, qreal factor)
{
    setScale(m_scale * factor, anchor);
}

void ImageViewer::setScale(qreal scale, const QPointF &anchor)
{
    // Zooming out past the fitted size has nothing to show.
    scale = qBound(lowestScale(), scale, maxScale);
    // The image point under the anchor stays under it.
    const QPointF imagePoint = (anchor - m_offset) / m_scale;
    m_scale = scale;
    m_offset = anchor - imagePoint * m_scale;
    m_fitted = std::abs(m_scale - fitScale()) < 1e-9;
    clampOffset();
    refresh();
}

void ImageViewer::clampOffset()
{
    // A picture smaller than the window is centred; a larger one cannot
    // be dragged off so that the window shows empty space beside it.
    const QSizeF shown(m_image.width() * m_scale, m_image.height() * m_scale);
    auto clamp = [](qreal offset, qreal image, qreal view) {
        return image <= view ? (view - image) / 2 : qBound(view - image, offset, qreal(0));
    };
    m_offset = QPointF(clamp(m_offset.x(), shown.width(), width()), clamp(m_offset.y(), shown.height(), height()));
}

void ImageViewer::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_fitted)
        fit();
}

void ImageViewer::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_fitted)
        fit();
    else {
        clampOffset();
        refresh();
    }
}

void ImageViewer::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_scale * devicePixelRatioF() < 4);
    painter.drawImage(QRectF(m_offset, QSizeF(m_image.width() * m_scale, m_image.height() * m_scale)), m_image);
}

void ImageViewer::wheelEvent(QWheelEvent *event)
{
    const qreal steps = event->angleDelta().y() / 120.0;
    if (steps == 0)
        return;
    zoomAt(event->position(), std::pow(1.2, steps));
    event->accept();
}

void ImageViewer::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_panning = true;
    m_lastPos = event->position();
    setCursor(Qt::ClosedHandCursor);
}

void ImageViewer::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_panning)
        return;
    m_offset += event->position() - m_lastPos;
    m_lastPos = event->position();
    clampOffset();
    update();
}

void ImageViewer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_panning = false;
    setCursor(Qt::OpenHandCursor);
}

void ImageViewer::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_fitted && std::abs(fitScale() - 1 / devicePixelRatioF()) > 1e-9)
        actualSize(event->position());
    else
        fit();
}

void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    const QPointF center = windowCentre();
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
