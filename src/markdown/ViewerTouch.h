#pragma once

#include "../TouchInput.h"

#include <functional>

namespace vn {

// Tracks the same canvas gestures in the picture and rendered block viewers.
class ViewerTouch : public QObject
{
public:
    using Pan = std::function<void(const QPointF &delta)>;
    using Zoom = std::function<void(qreal factor, const QPointF &from, const QPointF &to)>;
    using Toggle = std::function<void(const QPointF &at)>;

    ViewerTouch(QWidget *canvas, Pan pan, Zoom zoom, Toggle toggle);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *m_canvas;
    Pan m_pan;
    Zoom m_zoom;
    Toggle m_toggle;
    touch::DoubleTap m_taps;
    QElapsedTimer m_contact;
    QPointF m_last;
    qreal m_distance = 0;
    int m_first = -1;
    int m_second = -1;
    bool m_active = false;
    bool m_moved = false;
};

} // namespace vn
