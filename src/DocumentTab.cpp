#include "DocumentTab.h"

#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"
#include "markdown/MarkdownModel.h"
#include "markdown/VisualView.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QEvent>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QFileInfo>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSplitter>
#include <QStringDecoder>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace vn {

namespace {

const QByteArray utf8Bom("\xEF\xBB\xBF");
constexpr int pillMarginRight = 12;
constexpr int pillMarginTop = 8;
constexpr int findBarMarginRight = 24;

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
    , m_doc(new QTextDocument(this))
{
    m_doc->setDocumentLayout(new DocumentLayout(m_doc));
    m_doc->setUndoRedoEnabled(true);
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(1);
    m_splitter->setChildrenCollapsible(false);
    m_editor = new TextView(m_doc, m_splitter);
    m_splitter->addWidget(m_editor);
    m_active = m_editor;
    m_pill = new EditorPill(this);
    m_findBar = new FindBar(m_editor, this);
    auto *shadow = new QGraphicsDropShadowEffect(m_findBar);
    shadow->setBlurRadius(8);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 40));
    m_findBar->setGraphicsEffect(shadow);
    m_findBar->installEventFilter(this);
    m_goToLine = createGoToLine();

    // New files take their line ending from the settings.
    const QString eol = Settings::instance().text(key::lineEnding);
#ifdef Q_OS_WIN
    m_lineEnding = eol == QLatin1String("lf") ? LineEnding::LF : LineEnding::CRLF;
#else
    m_lineEnding = eol == QLatin1String("crlf") ? LineEnding::CRLF : LineEnding::LF;
#endif
    m_hasBom = Settings::instance().text(key::encoding) == QLatin1String("utf8bom");
    const QString defaultMode = Settings::instance().text(key::defaultMode);
    m_viewMode = defaultMode == QLatin1String("source")  ? ViewMode::Source
                 : defaultMode == QLatin1String("split") ? ViewMode::SourceVisual
                                                         : ViewMode::Visual;
    applySettings();
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &changedKey) {
        if (changedKey.startsWith(QLatin1String("editor/")) || changedKey == QLatin1String(key::highlightActiveBlock))
            applySettings();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_splitter);

    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_doc);
    m_highlighter->setTheme(theme::syntaxTheme());
    updateDefinition();

    connect(m_pill->lockButton(), &QToolButton::toggled, this, [this](bool locked) {
        m_editor->setReadOnly(locked);
        if (m_visual)
            m_visual->setReadOnly(locked);
    });
    connect(m_editor, &TextView::scrolled, this, [this] { syncScroll(m_editor, m_visual); });
    m_editor->installEventFilter(this);
    connect(m_doc, &QTextDocument::modificationChanged, this, &DocumentTab::modificationChanged);
    connect(m_editor, &TextView::cursorPositionChanged, this, [this] {
        if (m_active == m_editor)
            Q_EMIT cursorPositionChanged();
    });
    connect(m_doc, &QTextDocument::contentsChanged, this, &DocumentTab::textChanged);
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
    m_doc->setPlainText(content);
    m_doc->setModified(false);
    m_editor->setTextCursor(QTextCursor(m_doc));
    if (m_visual)
        m_visual->setTextCursor(QTextCursor(m_doc));
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
    m_doc->setModified(false);
    return true;
}

QString DocumentTab::displayName() const
{
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

bool DocumentTab::isModified() const
{
    return m_doc->isModified();
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
    // toPlainText() would turn non-breaking spaces into spaces.
    QString text = m_doc->toRawText();
    text.replace(QChar::ParagraphSeparator, u'\n');
    return text;
}

void DocumentTab::goToLine(int line)
{
    const QTextBlock block = m_doc->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    TextView *view = editor();
    view->setTextCursor(QTextCursor(block));
    view->centerCursor();
    view->setFocus();
}

TextView *DocumentTab::editor() const
{
    return m_active ? m_active : m_editor;
}

bool DocumentTab::isReadOnly() const
{
    return m_pill->lockButton()->isChecked();
}

void DocumentTab::ensureVisualView()
{
    if (m_visual)
        return;
    m_model = new MarkdownModel(m_doc);
    m_visual = new VisualView(m_model, m_splitter);
    m_splitter->addWidget(m_visual);
    m_visual->setReadOnly(isReadOnly());
    m_visual->setTextCursor(QTextCursor(m_doc));
    applyViewSettings(m_visual);
    m_visual->installEventFilter(this);
    connect(m_visual, &VisualView::linkActivated, this, &DocumentTab::linkActivated);
    connect(m_visual, &TextView::scrolled, this, [this] { syncScroll(m_visual, m_editor); });
    connect(m_visual, &TextView::cursorPositionChanged, this, [this] {
        if (m_active == m_visual)
            Q_EMIT cursorPositionChanged();
    });
}

void DocumentTab::syncScroll(TextView *from, TextView *to)
{
    // Keep both halves of Source / Visual on the same line.
    if (m_syncingScroll || !to || !to->isVisible() || !from->isVisible())
        return;
    m_syncingScroll = true;
    to->scrollToLine(from->topLine());
    m_syncingScroll = false;
}

void DocumentTab::setActiveView(TextView *view)
{
    if (m_active == view)
        return;
    m_active = view;
    m_findBar->setView(view);
    Q_EMIT cursorPositionChanged();
}

void DocumentTab::updateViews()
{
    const bool markdown = isMarkdown();
    const ViewMode mode = markdown ? m_viewMode : ViewMode::Source;
    if (mode != ViewMode::Source) {
        ensureVisualView();
        m_visual->setBaseDirectory(m_path.isEmpty() ? QString() : QFileInfo(m_path).absolutePath());
    }
    m_editor->setVisible(mode != ViewMode::Visual);
    if (m_visual)
        m_visual->setVisible(mode != ViewMode::Source);
    if (mode == ViewMode::SourceVisual) {
        const int half = qMax(1, m_splitter->width() / 2);
        m_splitter->setSizes({half, half});
        syncScroll(m_editor, m_visual);
    }
    TextView *active = mode == ViewMode::Visual ? static_cast<TextView *>(m_visual) : m_editor;
    if (m_active && m_active->isVisible() && mode == ViewMode::SourceVisual)
        active = m_active;
    // Carry the caret across when switching between source and visual.
    if (active != m_active && m_active)
        active->setTextCursor(m_active->textCursor());
    setActiveView(active);
    updatePill();
}

void DocumentTab::updateDefinition()
{
    // New documents default to Markdown, like the old app, unless set otherwise.
    auto &repository = theme::syntaxRepository();
    const bool markdownByDefault = Settings::instance().text(key::newFileLanguage) != QLatin1String("plaintext");
    const auto def = m_path.isEmpty() ? (markdownByDefault ? repository.definitionForName(QStringLiteral("Markdown"))
                                                           : KSyntaxHighlighting::Definition())
                                      : repository.definitionForFileName(m_path);
    m_highlighter->setDefinition(def);
    m_editor->setCommentMarker(def.singleLineCommentMarker());
    updateViews();
}

void DocumentTab::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;
    m_viewMode = mode;
    updateViews();
    editor()->setFocus();
    Q_EMIT viewModeChanged();
}

void DocumentTab::updatePill()
{
    m_pill->setVisible(isMarkdown() && m_viewMode != ViewMode::Source);
    m_pill->raise();
    placeOverlays();
}

void DocumentTab::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeOverlays();
}

bool DocumentTab::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_findBar && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        placeOverlays();
    if ((watched == m_editor || watched == m_visual) && event->type() == QEvent::FocusIn)
        setActiveView(static_cast<TextView *>(watched));
    if (watched->objectName() == QLatin1String("GoToLineEdit")) {
        if (event->type() == QEvent::FocusOut
            || (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)) {
            m_goToLine->hide();
            if (event->type() == QEvent::KeyPress) {
                editor()->setFocus();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DocumentTab::placeOverlays()
{
    m_pill->move(width() - m_pill->width() - pillMarginRight, pillMarginTop);
    // The find bar hangs from the top edge, clear of the scroll bar and pill.
    const int right = m_pill->isVisible() ? m_pill->width() + pillMarginRight * 2 : findBarMarginRight;
    m_findBar->adjustSize();
    m_findBar->move(qMax(0, width() - m_findBar->width() - right), 0);
    m_goToLine->adjustSize();
    m_goToLine->move((width() - m_goToLine->width()) / 2, 0);
}

void DocumentTab::applySettings()
{
    applyViewSettings(m_editor);
    if (m_visual)
        applyViewSettings(m_visual);
}

void DocumentTab::applyViewSettings(TextView *view)
{
    Settings &s = Settings::instance();
    QFont font = theme::editorFont();
    font.setPointSizeF(font.pointSizeF() * m_zoom / 100.0);
    view->setFont(font);
    view->setShowWhitespace(s.flag(key::drawWhitespace));
    view->setTabWidth(s.number(key::tabWidth));
    view->setInsertSpaces(s.flag(key::insertSpaces));
    view->setMultiCursorModifier(s.text(key::multiCursorModifier) == QLatin1String("ctrl") ? Qt::ControlModifier
                                                                                            : Qt::AltModifier);
    if (view == m_visual) {
        // The visual editor always wraps and has no gutter.
        view->setWordWrap(true);
        m_visual->setHighlightActiveBlock(s.flag(key::highlightActiveBlock));
    } else {
        view->setLineNumbersVisible(s.flag(key::lineNumbers));
        view->setHighlightCurrentLine(s.flag(key::highlightCurrentLine));
        view->setWordWrap(s.flag(key::wordWrap));
    }
}

void DocumentTab::setZoom(int percent)
{
    if (m_zoom == percent)
        return;
    m_zoom = percent;
    applySettings();
}

QWidget *DocumentTab::createGoToLine()
{
    // VS Code's quick input, reduced to "Go to Line": a field hanging from
    // the top edge. Accepts "line" or "line:column".
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("FindBar"));
    frame->setAttribute(Qt::WA_StyledBackground);
    auto *layout = new QHBoxLayout(frame);
    layout->setContentsMargins(6, 5, 6, 5);
    auto *field = new QWidget(frame);
    field->setObjectName(QStringLiteral("FindInput"));
    field->setAttribute(Qt::WA_StyledBackground);
    field->setProperty("focused", true);
    auto *fieldLayout = new QHBoxLayout(field);
    fieldLayout->setContentsMargins(4, 1, 4, 1);
    auto *edit = new QLineEdit(field);
    edit->setFrame(false);
    edit->setMinimumWidth(320);
    fieldLayout->addWidget(edit);
    layout->addWidget(field);
    auto *shadow = new QGraphicsDropShadowEffect(frame);
    shadow->setBlurRadius(8);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 40));
    frame->setGraphicsEffect(shadow);
    frame->hide();

    connect(edit, &QLineEdit::returnPressed, this, [this, frame, edit] {
        const QStringList parts = edit->text().trimmed().split(u':');
        bool ok = false;
        const int line = parts.value(0).toInt(&ok);
        frame->hide();
        if (!ok)
            return;
        const int column = parts.value(1).toInt();
        const QTextBlock block = m_doc->findBlockByNumber(qBound(1, line, m_doc->blockCount()) - 1);
        QTextCursor cursor(block);
        cursor.setPosition(block.position() + qBound(0, column - 1, block.length() - 1));
        m_editor->setTextCursor(cursor);
        m_editor->centerCursor();
        m_editor->setFocus();
    });
    edit->installEventFilter(this);
    edit->setObjectName(QStringLiteral("GoToLineEdit"));
    return frame;
}

void DocumentTab::showGoToLine()
{
    auto *edit = m_goToLine->findChild<QLineEdit *>();
    const QTextCursor cursor = m_editor->textCursor();
    edit->clear();
    edit->setPlaceholderText(tr("Current line: %1. Type a line number between 1 and %2.")
                                 .arg(cursor.blockNumber() + 1)
                                 .arg(m_doc->blockCount()));
    m_goToLine->show();
    m_goToLine->raise();
    placeOverlays();
    edit->setFocus();
}

void DocumentTab::showFind(bool replace)
{
    m_findBar->open(replace);
    placeOverlays();
}

} // namespace vn
