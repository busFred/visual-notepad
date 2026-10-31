#include "DocumentTab.h"

#include "EvenSplitter.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"
#include "markdown/MarkdownEditing.h"
#include "markdown/MarkdownModel.h"
#include "markdown/VisualView.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <algorithm>

#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QFileInfo>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace vn {

namespace {

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
    QToolButton *penButton() const { return m_pen; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        path.addRoundedRect(r, r.height() / 2, r.height() / 2);
        painter.fillPath(path, theme::color(theme::Role::sideBar));
        painter.setPen(theme::color(theme::Role::divider));
        painter.drawPath(path);
    }

private:
    QToolButton *m_lock;
    QToolButton *m_pen;
};

DocumentTab::DocumentTab(QWidget *parent)
    : DocumentTab(TextFile::create(), parent)
{
}

DocumentTab::DocumentTab(std::shared_ptr<TextFile> file, QWidget *parent)
    : QWidget(parent)
    , m_file(std::move(file))
    , m_doc(m_file->document())
{
    m_file->addView(this);
    m_splitter = new EvenSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(1);
    m_splitter->setChildrenCollapsible(false);
    m_editor = new TextView(m_doc, m_splitter);
    m_splitter->addWidget(m_editor);
    m_active = m_editor;
    m_pill = new EditorPill(this);
    // The find bar and the go-to-line field are built on first use: most
    // tabs never show them, and a restored session creates many tabs.

    const QString defaultMode = Settings::instance().text(key::defaultMode);
    m_viewMode = defaultMode == QLatin1String("source")  ? ViewMode::Source
                 : defaultMode == QLatin1String("split") ? ViewMode::SourceVisual
                                                         : ViewMode::Visual;
    applySettings();
    connect(&Settings::instance(), &Settings::changed, this, &DocumentTab::onSettingChanged);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_splitter);

    connect(m_pill->lockButton(), &QToolButton::toggled, this, [this](bool locked) {
        (m_viewMode == ViewMode::SourceVisual ? m_splitLocked : m_visualLocked) = locked;
        if (m_visual)
            m_visual->setReadOnly(locked);
    });
    connect(m_editor, &TextView::scrolled, this, [this] { syncScroll(m_editor, m_visual); });
    connect(m_editor, &TextView::selectionChanged, this, [this] { mirrorSelection(m_editor, m_visual); });
    m_editor->installEventFilter(this);
    connect(m_editor, &TextView::cursorPositionChanged, this, [this] {
        if (m_active == m_editor)
            Q_EMIT cursorPositionChanged();
    });
    connect(m_editor, &TextView::linkRequested, this, &DocumentTab::linkActivated);
    connect(m_editor, &TextView::filesDropped, this, &DocumentTab::filesDropped);

    // What the file shares with every tab showing it.
    TextFile *f = m_file.get();
    connect(f, &TextFile::modificationChanged, this, &DocumentTab::modificationChanged);
    connect(f, &TextFile::contentsChanged, this, &DocumentTab::textChanged);
    connect(f, &TextFile::contentReset, this, &DocumentTab::resetCarets);
    connect(f, &TextFile::definitionChanged, this, &DocumentTab::onDefinitionChanged);
    connect(f, &TextFile::formatChanged, this, &DocumentTab::formatChanged);
    connect(f, &TextFile::indentationChanged, this, [this] {
        forEachView([this](TextView *view) {
            view->setTabWidth(indentation().tabSize);
            view->setInsertSpaces(indentation().insertSpaces);
        });
        Q_EMIT indentationChanged();
    });
    connect(f, &TextFile::diagnosticsChanged, this, &DocumentTab::applyDiagnostics);
    connect(f, &TextFile::pathChanged, this, [this] {
        updateViews();
        Q_EMIT titleChanged();
    });
    connect(f, &TextFile::diskStateChanged, this, &DocumentTab::titleChanged);
    onDefinitionChanged();
    applyDiagnostics();
}

DocumentTab::~DocumentTab()
{
    // The views use the file's document: they go before the file can. No
    // focus moves between dying views may reach this half-destroyed tab.
    m_file->removeView(this);
    m_editor->removeEventFilter(this);
    m_editor->disconnect(this);
    if (m_visual) {
        m_visual->removeEventFilter(this);
        m_visual->disconnect(this);
    }
    delete m_findBar;
    delete m_goToLine;
    delete m_splitter;
}

DocumentTab *DocumentTab::openAnotherView(QWidget *parent) const
{
    auto *tab = new DocumentTab(m_file, parent);
    tab->m_splitLocked = m_splitLocked;
    tab->m_visualLocked = m_visualLocked;
    tab->setViewMode(m_viewMode);
    tab->setZoom(m_zoom);
    tab->editor()->setTextCursor(editor()->textCursor());
    return tab;
}

void DocumentTab::setClosing()
{
    m_file->removeView(this);
}

QList<DocumentTab *> DocumentTab::otherViews() const
{
    QList<DocumentTab *> views = m_file->views();
    views.removeAll(const_cast<DocumentTab *>(this));
    return views;
}

bool DocumentTab::load(const QString &path)
{
    return m_file->load(path);
}

void DocumentTab::loadBackup(const QString &path, const QString &content, const Encoding &encoding,
                             LineEnding lineEnding)
{
    m_file->loadBackup(path, content, encoding, lineEnding);
}

bool DocumentTab::save(const QString &path)
{
    return m_file->save(path);
}

void DocumentTab::resetCarets()
{
    m_editor->setTextCursor(QTextCursor(m_doc));
    if (m_visual)
        m_visual->setTextCursor(QTextCursor(m_doc));
}

void DocumentTab::convertIndentation(bool toSpaces)
{
    if (isReadOnly())
        return;
    m_file->convertIndentation(toSpaces);
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
    // The source view is always editable, so only Visual mode can be locked.
    return isMarkdown() && m_viewMode == ViewMode::Visual && m_visualLocked;
}

void DocumentTab::ensureVisualView()
{
    if (m_visual)
        return;
    m_visual = new VisualView(m_file->markdownModel(), m_splitter);
    m_splitter->addWidget(m_visual);
    m_visual->setReadOnly(m_viewMode == ViewMode::SourceVisual ? m_splitLocked : m_visualLocked);
    m_visual->setTextCursor(QTextCursor(m_doc));
    applyViewSettings(m_visual);
    m_visual->installEventFilter(this);
    connect(m_visual, &VisualView::linkActivated, this, &DocumentTab::linkActivated);
    connect(m_visual, &TextView::linkRequested, this, &DocumentTab::linkActivated);
    connect(m_visual, &TextView::filesDropped, this, &DocumentTab::filesDropped);
    m_visual->setDiagnostics(m_file->diagnostics());
    connect(m_visual, &TextView::scrolled, this, [this] { syncScroll(m_visual, m_editor); });
    connect(m_visual, &TextView::selectionChanged, this, [this] { mirrorSelection(m_visual, m_editor); });
    connect(m_visual, &TextView::cursorPositionChanged, this, [this] {
        if (m_active == m_visual)
            Q_EMIT cursorPositionChanged();
    });
}

void DocumentTab::syncScroll(TextView *from, TextView *to)
{
    // Keep both halves of Source / Visual at the same source position,
    // fractions of a line included.
    if (m_syncingScroll || !to || !to->isVisible() || !from->isVisible())
        return;
    m_syncingScroll = true;
    to->followScroll(*from);
    m_syncingScroll = false;
}

void DocumentTab::mirrorSelection(TextView *from, TextView *to)
{
    // Only in a split, never onto the view the user is working in, and only
    // when there is a selection to show or an old one to clear.
    if (m_mirroringSelection || !from || !to || !to->isVisible() || !from->isVisible() || to->hasFocus())
        return;
    const bool selecting = from->hasSelection();
    if (!selecting && !to->hasSelection())
        return;
    // mirrorCursorsFrom emits nothing, so the views cannot echo each other;
    // the flag also keeps scrolling the revealed range from dragging the
    // other half back through syncScroll.
    m_mirroringSelection = true;
    const bool wasSyncing = m_syncingScroll;
    m_syncingScroll = true;
    to->mirrorCursorsFrom(from, selecting);
    m_syncingScroll = wasSyncing;
    m_mirroringSelection = false;
}

void DocumentTab::setActiveView(TextView *view)
{
    if (m_active == view)
        return;
    m_active = view;
    if (m_findBar)
        m_findBar->setView(view);
    Q_EMIT cursorPositionChanged();
}

void DocumentTab::updateViews()
{
    const bool markdown = isMarkdown();
    const ViewMode mode = markdown ? m_viewMode : ViewMode::Source;
    if (!markdown && m_visual) {
        // Not Markdown any more: drop the visual view and its parser so
        // edits don't pay for Markdown parsing.
        if (m_active == m_visual) {
            m_editor->setTextCursor(m_visual->textCursor());
            setActiveView(m_editor);
        }
        delete m_visual;
        m_visual = nullptr;
    }
    if (mode != ViewMode::Source) {
        ensureVisualView();
        m_visual->setBaseDirectory(path().isEmpty() ? QString() : QFileInfo(path()).absolutePath());
    }
    applyMarkdownState();
    m_file->syncHighlighting();
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
    if (mode == ViewMode::SourceVisual)
        mirrorSelection(active, active == m_editor ? static_cast<TextView *>(m_visual) : m_editor);
    updatePill();
}

void DocumentTab::applyMarkdownState()
{
    const bool markdown = isMarkdown();
    m_editor->setMarkdownEditing(markdown);
    m_editor->setDocumentPath(path());
    if (m_visual) {
        m_visual->setMarkdownEditing(markdown);
        m_visual->setDocumentPath(path());
    }
}

void DocumentTab::applyDiagnostics()
{
    m_editor->setDiagnostics(m_file->diagnostics());
    if (m_visual)
        m_visual->setDiagnostics(m_file->diagnostics());
}

void DocumentTab::formatDocument()
{
    if (!isMarkdown() || isReadOnly())
        return;
    const QString before = text();
    const QString after = md::formatDocument(before);
    if (after == before)
        return;
    TextView *view = editor();
    const QTextCursor caret = view->textCursor();
    const int line = caret.blockNumber();
    const int column = caret.positionInBlock();
    QTextCursor edit(m_doc);
    edit.beginEditBlock();
    edit.select(QTextCursor::Document);
    edit.insertText(after);
    edit.endEditBlock();
    // Keep the caret on the same line and column where they still exist.
    const QTextBlock block = m_doc->findBlockByNumber(qMin(line, m_doc->blockCount() - 1));
    QTextCursor restored(m_doc);
    restored.setPosition(block.position() + qMin(column, block.length() - 1));
    view->setTextCursor(restored);
}

void DocumentTab::onDefinitionChanged()
{
    const QString name = definitionName();
    const auto def = name.isEmpty() ? KSyntaxHighlighting::Definition() : theme::syntaxRepository().definitionForName(name);
    m_editor->setCommentMarker(def.singleLineCommentMarker());
    m_editor->setCodeEditing(def.isValid() && def.name() != QLatin1String("Markdown"));
    updateViews();
}

bool DocumentTab::showsSource() const
{
    return !isMarkdown() || m_viewMode != ViewMode::Visual;
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
    // The pill locks the visual view; each mode remembers its own state.
    const bool locked = m_viewMode == ViewMode::SourceVisual ? m_splitLocked : m_visualLocked;
    if (m_pill->lockButton()->isChecked() != locked)
        (locked ? m_pill->lockButton() : m_pill->penButton())->setChecked(true);
    m_pill->setVisible(isMarkdown() && m_viewMode != ViewMode::Source);
    m_pill->raise();
    placeOverlays();
}

void DocumentTab::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    placeOverlays();
}

void DocumentTab::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_settingsStale) {
        m_settingsStale = false;
        applySettings();
    }
    m_file->applyPendingSettings();
}

bool DocumentTab::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_findBar && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        placeOverlays();
    if ((watched == m_editor || watched == m_visual) && event->type() == QEvent::FocusIn)
        setActiveView(static_cast<TextView *>(watched));
    if (m_goToLineEdit && watched == m_goToLineEdit) {
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
    if (m_findBar) {
        m_findBar->adjustSize();
        m_findBar->move(qMax(0, width() - m_findBar->width() - right), 0);
    }
    if (m_goToLine) {
        m_goToLine->adjustSize();
        m_goToLine->move((width() - m_goToLine->width()) / 2, 0);
    }
}

void DocumentTab::applySettings()
{
    m_settingsStale = false;
    m_appliedFont = zoomedEditorFont();
    applyViewSettings(m_editor);
    if (m_visual)
        applyViewSettings(m_visual);
}

void DocumentTab::forEachView(const std::function<void(TextView *)> &apply)
{
    apply(m_editor);
    if (m_visual)
        apply(m_visual);
}

void DocumentTab::onSettingChanged(const QString &changedKey)
{
    // Only what the key affects: a font change re-lays out the whole text,
    // so it should not come along with every other editor setting.
    if (!isVisible()) {
        // Hidden tabs catch up when shown (showEvent), in one pass.
        if (changedKey.startsWith(QLatin1String("editor/")) || changedKey == QLatin1String(key::highlightActiveBlock))
            m_settingsStale = true;
        return;
    }
    Settings &s = Settings::instance();
    if (changedKey == QLatin1String(key::fontFamily) || changedKey == QLatin1String(key::fontSize)) {
        // Family and size are written together: the first notice applies the
        // new font, the second finds nothing left to lay out.
        const QFont font = zoomedEditorFont();
        if (font != m_appliedFont) {
            m_appliedFont = font;
            forEachView([&](TextView *view) { view->setFont(font); });
        }
    } else if (changedKey == QLatin1String(key::drawWhitespace)) {
        forEachView([&](TextView *view) { view->setShowWhitespace(s.flag(key::drawWhitespace)); });
    } else if (changedKey == QLatin1String(key::lineSpacing)) {
        forEachView([&](TextView *view) { view->setLineSpacing(s.value(key::lineSpacing).toDouble()); });
    } else if (changedKey == QLatin1String(key::multiCursorModifier)) {
        const Qt::KeyboardModifier modifier =
            s.text(key::multiCursorModifier) == QLatin1String("ctrl") ? Qt::ControlModifier : Qt::AltModifier;
        forEachView([&](TextView *view) { view->setMultiCursorModifier(modifier); });
    } else if (changedKey == QLatin1String(key::lineNumbers)) {
        m_editor->setLineNumbersVisible(s.flag(key::lineNumbers));
    } else if (changedKey == QLatin1String(key::highlightCurrentLine)) {
        m_editor->setHighlightCurrentLine(s.flag(key::highlightCurrentLine));
    } else if (changedKey == QLatin1String(key::wordWrap)) {
        m_editor->setWordWrap(s.flag(key::wordWrap));
    } else if (changedKey == QLatin1String(key::highlightActiveBlock)) {
        if (m_visual)
            m_visual->setHighlightActiveBlock(s.flag(key::highlightActiveBlock));
    } else if (changedKey.startsWith(QLatin1String("editor/"))) {
        // Tab width and the rest arrive through the file (indentationChanged)
        // or are read when used; anything new here gets the full treatment.
        if (changedKey != QLatin1String(key::tabWidth) && changedKey != QLatin1String(key::insertSpaces)
            && changedKey != QLatin1String(key::detectIndentation) && changedKey != QLatin1String(key::autoClose)
            && changedKey != QLatin1String(key::autoSurround))
            applySettings();
    }
}

QFont DocumentTab::zoomedEditorFont() const
{
    QFont font = theme::editorFont();
    font.setPointSizeF(font.pointSizeF() * m_zoom / 100.0);
    return font;
}

void DocumentTab::applyViewSettings(TextView *view)
{
    Settings &s = Settings::instance();
    view->setFont(zoomedEditorFont());
    view->setShowWhitespace(s.flag(key::drawWhitespace));
    view->setTabWidth(indentation().tabSize);
    view->setInsertSpaces(indentation().insertSpaces);
    view->setLineSpacing(s.value(key::lineSpacing).toDouble());
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
    if (!isVisible()) {
        m_settingsStale = true;
        return;
    }
    const QFont font = zoomedEditorFont();
    m_appliedFont = font;
    forEachView([&](TextView *view) { view->setFont(font); });
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
    m_goToLineEdit = edit;
    return frame;
}

FindBar *DocumentTab::findBar() const
{
    if (!m_findBar) {
        auto *self = const_cast<DocumentTab *>(this);
        self->m_findBar = new FindBar(editor(), self);
        auto *shadow = new QGraphicsDropShadowEffect(m_findBar);
        shadow->setBlurRadius(8);
        shadow->setOffset(0, 3);
        shadow->setColor(QColor(0, 0, 0, 40));
        m_findBar->setGraphicsEffect(shadow);
        m_findBar->installEventFilter(self);
        // Under the pill, which updatePill() keeps raised over it.
        m_findBar->stackUnder(m_pill);
    }
    return m_findBar;
}

void DocumentTab::showGoToLine()
{
    if (!m_goToLine)
        m_goToLine = createGoToLine();
    QLineEdit *edit = m_goToLineEdit;
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
    findBar()->open(replace);
    placeOverlays();
}

} // namespace vn
