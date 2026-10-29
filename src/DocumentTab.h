// One open file: the editor plus its floating lock/pencil pill.
#pragma once

#include <QString>
#include <QWidget>

class QTextDocument;

namespace KSyntaxHighlighting {
class SyntaxHighlighter;
} // namespace KSyntaxHighlighting

namespace vn {

class EditorPill;
class FindBar;
class TextView;

class DocumentTab : public QWidget
{
    Q_OBJECT

public:
    enum class LineEnding { LF, CRLF };
    // How a Markdown document is shown. Only Source exists so far.
    enum class ViewMode { Source, SourceVisual, Visual };

    explicit DocumentTab(QWidget *parent = nullptr);

    // Returns false and sets errorString() on failure.
    bool load(const QString &path);
    bool save(const QString &path);

    QString path() const { return m_path; }
    QString displayName() const;
    QString errorString() const { return m_error; }
    bool isModified() const;
    bool isMarkdown() const;
    QString languageName() const;
    LineEnding lineEnding() const { return m_lineEnding; }
    QString text() const;

    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);

    QTextDocument *document() const { return m_doc; }
    TextView *editor() const { return m_editor; }
    void goToLine(int line);
    FindBar *findBar() const { return m_findBar; }
    void showFind(bool replace);

Q_SIGNALS:
    void modificationChanged(bool modified);
    void cursorPositionChanged();
    void textChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateDefinition();
    void updatePill();
    void placeOverlays();

    QTextDocument *m_doc;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter;
    TextView *m_editor;
    EditorPill *m_pill;
    FindBar *m_findBar;
    QString m_path;
    QString m_error;
    LineEnding m_lineEnding = LineEnding::LF;
    ViewMode m_viewMode = ViewMode::Source;
    bool m_hasBom = false;
};

} // namespace vn
