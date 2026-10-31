// A floating window to look at one picture up close: wheel to zoom around
// the pointer, drag to pan, double-click to switch between fitting the window
// and actual size.
#pragma once

#include <QDialog>
#include <QImage>
#include <QPointF>

class QLabel;
class QSlider;

namespace vn {

class ImageViewer : public QDialog
{
    Q_OBJECT

public:
    ImageViewer(const QImage &image, const QString &title, QWidget *parent = nullptr);

    // Display scale: logical pixels per image pixel.
    qreal scale() const { return m_scale; }
    qreal fitScale() const;
    void zoomAt(const QPointF &anchor, qreal factor);
    void fit();
    void actualSize(const QPointF &anchor);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setScale(qreal scale, const QPointF &anchor);
    QPointF windowCentre() const;
    qreal lowestScale() const;
    void refresh(); // Repaint, and bring the controls up to date.
    void clampOffset();

    QWidget *m_bar;
    QSlider *m_slider;
    QLabel *m_percent;
    QImage m_image;
    qreal m_scale = 1;
    QPointF m_offset; // Where the image's top-left is, in widget coordinates.
    bool m_fitted = true; // Keep fitting when the window is resized.
    bool m_panning = false;
    QPointF m_lastPos;
};

} // namespace vn
