// One open file: the editor plus its floating lock/pencil pill.
#pragma once

#include <QString>
#include <QWidget>

class QSplitter;
class QTextDocument;

namespace KSyntaxHighlighting {
class SyntaxHighlighter;
} // namespace KSyntaxHighlighting

namespace vn {

class EditorPill;
class FindBar;
class MarkdownModel;
class TextView;
class VisualView;

class DocumentTab : public QWidget
{
    Q_OBJECT

public:
    enum class LineEnding { LF, CRLF };
    // How a Markdown document is shown.
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
    // The view that has (or last had) focus; for plain text the source view.
    TextView *editor() const;
    TextView *sourceView() const { return m_editor; }
    VisualView *visualView() const { return m_visual; }
    bool isReadOnly() const;
    void goToLine(int line);
    FindBar *findBar() const { return m_findBar; }
    void showFind(bool replace);
    void showGoToLine();
    void setZoom(int percent);

Q_SIGNALS:
    void linkActivated(const QString &url);
    void viewModeChanged();
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
    void applySettings();
    void applyViewSettings(TextView *view);
    QWidget *createGoToLine();
    void ensureVisualView();
    void updateViews();
    void syncScroll(TextView *from, TextView *to);
    void setActiveView(TextView *view);

    QTextDocument *m_doc;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter;
    TextView *m_editor; // Source view.
    MarkdownModel *m_model = nullptr;
    VisualView *m_visual = nullptr;
    QSplitter *m_splitter;
    TextView *m_active = nullptr;
    bool m_syncingScroll = false;
    EditorPill *m_pill;
    FindBar *m_findBar;
    QWidget *m_goToLine;
    int m_zoom = 100;
    QString m_path;
    QString m_error;
    LineEnding m_lineEnding = LineEnding::LF;
    ViewMode m_viewMode = ViewMode::Source;
    bool m_hasBom = false;
};

} // namespace vn
