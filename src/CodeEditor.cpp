#include "CodeEditor.h"

#include "Theme.h"

#include <QPainter>
#include <QTextBlock>

namespace vn {

namespace {

constexpr int gutterLeftPad = 6;
constexpr int gutterRightPad = 14;

class Gutter : public QWidget
{
public:
    explicit Gutter(CodeEditor *editor)
        : QWidget(editor)
        , m_editor(editor)
    {
    }

    QSize sizeHint() const override { return {m_editor->gutterWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->paintGutter(event); }

private:
    CodeEditor *m_editor;
};

} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
    , m_gutter(new Gutter(this))
{
    setFont(theme::editorFont());
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    document()->setDocumentMargin(4);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateGutterWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateGutter);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::highlightCurrentLine);

    updateGutterWidth();
    highlightCurrentLine();
}

int CodeEditor::gutterWidth() const
{
    int digits = 1;
    for (int max = qMax(1, blockCount()); max >= 10; max /= 10)
        ++digits;
    digits = qMax(digits, 2);
    return gutterLeftPad + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits + gutterRightPad;
}

void CodeEditor::updateGutterWidth()
{
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

void CodeEditor::updateGutter(const QRect &rect, int dy)
{
    if (dy)
        m_gutter->scroll(0, dy);
    else
        m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
}

void CodeEditor::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateGutterWidth();
        m_gutter->update();
    }
}

void CodeEditor::highlightCurrentLine()
{
    QTextEdit::ExtraSelection line;
    line.format.setBackground(theme::currentLine);
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    setExtraSelections({line});
    m_gutter->update();
}

void CodeEditor::paintGutter(QPaintEvent *event)
{
    QPainter painter(m_gutter);
    painter.fillRect(event->rect(), theme::editor);
    painter.setFont(font());

    const int current = textCursor().blockNumber();
    const int right = m_gutter->width() - gutterRightPad;
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom()) {
        const int height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top()) {
            painter.setPen(block.blockNumber() == current ? theme::text : theme::gutterText);
            painter.drawText(0, top, right, fontMetrics().height(), Qt::AlignRight,
                             QString::number(block.blockNumber() + 1));
        }
        top += height;
        block = block.next();
    }
}

} // namespace vn
