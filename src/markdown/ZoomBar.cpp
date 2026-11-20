#include "ZoomBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QtMath>

namespace vn {

namespace {

constexpr int sliderSteps = 1000;

} // namespace

ZoomBar::ZoomBar(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    setAttribute(Qt::WA_NoMousePropagation);
    setCursor(Qt::ArrowCursor);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    auto button = [&](const QString &text, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(b);
        return b;
    };
    connect(button(QStringLiteral("Fit"), tr("Fit to window (0)")), &QToolButton::clicked, this,
            &ZoomBar::fitRequested);
    connect(button(QStringLiteral("−"), tr("Zoom out (-)")), &QToolButton::clicked, this,
            &ZoomBar::zoomOutRequested);
    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(0, sliderSteps);
    m_slider->setFixedWidth(200);
    m_slider->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_slider);
    connect(m_slider, &QSlider::valueChanged, this,
            [this](int value) { Q_EMIT sliderMoved(qreal(value) / sliderSteps); });
    connect(button(QStringLiteral("+"), tr("Zoom in (+)")), &QToolButton::clicked, this, &ZoomBar::zoomInRequested);
    connect(button(QStringLiteral("1:1"), tr("Actual size (1)")), &QToolButton::clicked, this,
            &ZoomBar::actualSizeRequested);
    m_percent = new QLabel(this);
    m_percent->setMinimumWidth(m_percent->fontMetrics().horizontalAdvance(QStringLiteral("3200%")));
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_percent);
    adjustSize();
}

void ZoomBar::setZoom(qreal fraction, int percent)
{
    // Follow the zoom without echoing it back.
    const QSignalBlocker block(m_slider);
    m_slider->setValue(qRound(qBound<qreal>(0, fraction, 1) * sliderSteps));
    m_percent->setText(QStringLiteral("%1%").arg(percent));
}

QSize viewerWindowSize(const QSizeF &natural, const QSize &padding, const QWidget *window)
{
    QSize cap(900, 650);
    if (const QWidget *parent = window->parentWidget())
        cap = parent->window()->size() * 4 / 5;
    // (Less than the whole screen: the title bar and frame come on top.)
    if (const QScreen *screen = window->screen())
        cap = cap.boundedTo(screen->availableGeometry().size() * 9 / 10);
    cap = cap.expandedTo(window->minimumSize());
    QSizeF content = natural;
    // Only ever smaller: a small picture opens at its real size.
    if (!content.isEmpty() && (content.width() > cap.width() - padding.width()
                               || content.height() > cap.height() - padding.height()))
        content.scale(QSizeF((cap - padding).expandedTo(QSize(1, 1))), Qt::KeepAspectRatio);
    return (QSize(qCeil(content.width()), qCeil(content.height())) + padding)
        .expandedTo(window->minimumSize())
        .boundedTo(cap);
}

} // namespace vn
