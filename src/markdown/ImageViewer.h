// A window of its own to look at one picture up close: wheel to zoom around
// the pointer, drag to pan, double-click to switch between fitting the window
// and actual size. It does not block the window it came from: it can be moved
// aside, and several can be open.
#pragma once

#include <QDialog>
#include <QImage>
#include <QPointF>

#include <vector>

namespace vn {

class ZoomBar;
namespace columns {
class PageButton;
}

class ImageViewer : public QDialog
{
    Q_OBJECT

public:
    ImageViewer(const QImage &image, const QString &title, QWidget *parent = nullptr);
    // Several pictures (pictures that go together): ‹ › at the sides, and
    // Left / Right, step through them. Starts at `index`.
    struct Entry
    {
        QImage image;
        QString title;
    };
    ImageViewer(std::vector<Entry> pictures, int index, QWidget *parent = nullptr);

    int index() const { return m_index; }
    int count() const { return int(m_pictures.size()); }
    void step(int by);

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
    void showIndex(int index);
    void placeSteps();

    ZoomBar *m_bar;
    QImage m_image;
    qreal m_scale = 1;
    QPointF m_offset; // Where the image's top-left is, in widget coordinates.
    bool m_fitted = true; // Keep fitting when the window is resized.
    bool m_panning = false;
    QPointF m_lastPos;
    std::vector<Entry> m_pictures; // Empty: one picture.
    int m_index = 0;
    columns::PageButton *m_back = nullptr;
    columns::PageButton *m_forward = nullptr;
};

} // namespace vn
