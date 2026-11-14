// The controls at the bottom of a viewer window: Fit, −, a zoom slider, +,
// 1:1 and the zoom in per cent. It only asks; the viewer zooms and then sets
// what the bar shows (see setZoom).
#pragma once

#include <QWidget>

class QLabel;
class QSlider;

namespace vn {

class ZoomBar : public QWidget
{
    Q_OBJECT

public:
    explicit ZoomBar(QWidget *parent = nullptr);

    // The slider at `fraction` (0 to 1: how far the zoom is between the least
    // and the most, evenly in the logarithm) and the zoom's `percent`, without
    // sending sliderMoved.
    void setZoom(qreal fraction, int percent);

Q_SIGNALS:
    void fitRequested();
    void zoomOutRequested();
    void zoomInRequested();
    void actualSizeRequested();
    void sliderMoved(qreal fraction);

private:
    QSlider *m_slider;
    QLabel *m_percent;
};

} // namespace vn
