#include "DocumentTab.h"

#include "CodeEditor.h"
#include "Theme.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QFile>
#include <QFileInfo>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>

namespace vn {

namespace {

const QByteArray utf8Bom("\xEF\xBB\xBF");
constexpr int pillMarginRight = 12;
constexpr int pillMarginTop = 8;

QIcon pillIcon(const QString &name)
{
    QIcon icon;
    icon.addFile(QStringLiteral(":/vn/icons/%1-off.svg").arg(name), {}, QIcon::Normal, QIcon::Off);
    icon.addFile(QStringLiteral(":/vn/icons/%1-on.svg").arg(name), {}, QIcon::Normal, QIcon::On);
    return icon;
}

} // namespace

// Floating read-only (lock) / editing (pencil) switch, top-right of the
// editor, shown for Markdown in the visual modes.
class EditorPill : public QWidget
{
public:
    explicit EditorPill(QWidget *parent)
        : QWidget(parent)
        , m_lock(new QToolButton(this))
        , m_pen(new QToolButton(this))
    {
        setObjectName(QStringLiteral("Pill"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(3, 3, 3, 3);
        layout->setSpacing(2);
        auto *group = new QButtonGroup(this);
        for (auto [button, icon, tip] : {std::tuple{m_lock, "lock", QT_TR_NOOP("Read-only")},
                                         std::tuple{m_pen, "pen", QT_TR_NOOP("Editing")}}) {
            button->setIcon(pillIcon(QString::fromLatin1(icon)));
            button->setIconSize({14, 14});
            button->setFixedSize(24, 24);
            button->setCheckable(true);
            button->setToolTip(DocumentTab::tr(tip));
            group->addButton(button);
            layout->addWidget(button);
        }
        m_pen->setChecked(true);
        adjustSize();
    }

    QToolButton *lockButton() const { return m_lock; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        path.addRoundedRect(r, r.height() / 2, r.height() / 2);
        painter.fillPath(path, theme::sideBar);
        painter.setPen(theme::divider);
        painter.drawPath(path);
    }

private:
    QToolButton *m_lock;
    QToolButton *m_pen;
};

DocumentTab::DocumentTab(QWidget *parent)
    : QWidget(parent)
    , m_editor(new CodeEditor(this))
    , m_pill(new EditorPill(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_editor);

    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_editor->document());
    m_highlighter->setTheme(theme::syntaxTheme());
    updateDefinition();

    connect(m_pill->lockButton(), &QToolButton::toggled, m_editor, &QPlainTextEdit::setReadOnly);
    connect(m_editor, &QPlainTextEdit::modificationChanged, this, &DocumentTab::modificationChanged);
    connect(m_editor, &QPlainTextEdit::cursorPositionChanged, this, &DocumentTab::cursorPositionChanged);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &DocumentTab::textChanged);
    updatePill();
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

QPlainTextEdit *DocumentTab::editor() const
{
    return m_editor;
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
    auto &repository = theme::syntaxRepository();
    const auto def = m_path.isEmpty() ? repository.definitionForName(QStringLiteral("Markdown"))
                                      : repository.definitionForFileName(m_path);
    m_highlighter->setDefinition(def);
    updatePill();
}

void DocumentTab::setViewMode(ViewMode mode)
{
    // The visual modes arrive with the Markdown editor port.
    if (mode != ViewMode::Source)
        return;
    m_viewMode = mode;
    updatePill();
}

void DocumentTab::updatePill()
{
    m_pill->setVisible(isMarkdown() && m_viewMode != ViewMode::Source);
    m_pill->raise();
}

void DocumentTab::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_pill->move(width() - m_pill->width() - pillMarginRight, pillMarginTop);
}

} // namespace vn
