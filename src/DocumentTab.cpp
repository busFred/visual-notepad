#include "DocumentTab.h"

#include "Theme.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTextBlock>
#include <QVBoxLayout>

namespace vn {

namespace {

const QByteArray utf8Bom("\xEF\xBB\xBF");

} // namespace

DocumentTab::DocumentTab(KSyntaxHighlighting::Repository &repository, QWidget *parent)
    : QWidget(parent)
    , m_repository(repository)
    , m_editor(new QPlainTextEdit(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor);

    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);

    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_editor->document());
    m_highlighter->setTheme(m_repository.defaultTheme(KSyntaxHighlighting::Repository::LightTheme));
    updateDefinition();

    connect(m_editor, &QPlainTextEdit::modificationChanged, this, &DocumentTab::modificationChanged);
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, &DocumentTab::cursorPositionChanged);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &DocumentTab::textChanged);
}

bool DocumentTab::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = file.errorString();
        return false;
    }
    QByteArray bytes = file.readAll();

    m_hasBom = bytes.startsWith(utf8Bom);
    if (m_hasBom)
        bytes.remove(0, utf8Bom.size());

    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString content = decoder(bytes);
    if (decoder.hasError()) {
        m_error = tr("The file is not valid UTF-8.");
        return false;
    }

    // The first line ending decides the style used on save.
    const qsizetype lf = content.indexOf(u'\n');
    m_lineEnding = (lf > 0 && content.at(lf - 1) == u'\r') ? LineEnding::CRLF : LineEnding::LF;
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    m_path = path;
    updateDefinition();
    m_editor->setPlainText(content);
    m_editor->document()->setModified(false);
    return true;
}

bool DocumentTab::save(const QString &path)
{
    QString content = text();
    if (m_lineEnding == LineEnding::CRLF)
        content.replace(u'\n', QStringLiteral("\r\n"));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_error = file.errorString();
        return false;
    }
    if (m_hasBom)
        file.write(utf8Bom);
    file.write(content.toUtf8());
    if (!file.commit()) {
        m_error = file.errorString();
        return false;
    }

    const bool renamed = path != m_path;
    m_path = path;
    if (renamed)
        updateDefinition();
    m_editor->document()->setModified(false);
    return true;
}

QString DocumentTab::displayName() const
{
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

bool DocumentTab::isModified() const
{
    return m_editor->document()->isModified();
}

bool DocumentTab::isMarkdown() const
{
    return m_highlighter->definition().name() == QLatin1String("Markdown");
}

QString DocumentTab::languageName() const
{
    const auto def = m_highlighter->definition();
    return def.isValid() ? def.translatedName() : tr("Plain Text");
}

QString DocumentTab::text() const
{
    return m_editor->toPlainText();
}

void DocumentTab::goToLine(int line)
{
    const QTextBlock block = m_editor->document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    QTextCursor cursor(block);
    m_editor->setTextCursor(cursor);
    m_editor->centerCursor();
    m_editor->setFocus();
}

void DocumentTab::updateDefinition()
{
    // New documents default to Markdown, like the old app.
    const auto def = m_path.isEmpty() ? m_repository.definitionForName(QStringLiteral("Markdown"))
                                      : m_repository.definitionForFileName(m_path);
    m_highlighter->setDefinition(def);
}

} // namespace vn
