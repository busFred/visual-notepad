// Plain text view with a line-number gutter and current-line highlight.
// Stands in for the custom multi-cursor editor core.
#pragma once

#include <QPlainTextEdit>

namespace vn {

class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateGutterWidth();
    void updateGutter(const QRect &rect, int dy);
    void highlightCurrentLine();

    QWidget *m_gutter;
};

} // namespace vn
