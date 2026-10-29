// One open file. The editor widget is a QPlainTextEdit placeholder until the
// custom multi-cursor editor core replaces it.
#pragma once

#include <QString>
#include <QWidget>

class QPlainTextEdit;

namespace KSyntaxHighlighting {
class Repository;
class SyntaxHighlighter;
} // namespace KSyntaxHighlighting

namespace vn {

class DocumentTab : public QWidget
{
    Q_OBJECT

public:
    enum class LineEnding { LF, CRLF };

    DocumentTab(KSyntaxHighlighting::Repository &repository, QWidget *parent = nullptr);

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

    QPlainTextEdit *editor() const { return m_editor; }
    void goToLine(int line);

signals:
    void modificationChanged(bool modified);
    void cursorPositionChanged();
    void textChanged();

private:
    void updateDefinition();

    KSyntaxHighlighting::Repository &m_repository;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter;
    QPlainTextEdit *m_editor;
    QString m_path;
    QString m_error;
    LineEnding m_lineEnding = LineEnding::LF;
    bool m_hasBom = false;
};

} // namespace vn
