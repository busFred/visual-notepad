#include "DocumentTab.h"

#include "EvenSplitter.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"
#include "markdown/MarkdownEditing.h"
#include "markdown/MarkdownModel.h"
#include "markdown/MathRenderer.h"
#include "markdown/VisualView.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <algorithm>

#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QFileInfo>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
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

namespace {

// The 2 px line along the bottom of the half in use. It is a child of the
// half, so it follows it however the splitter moves.
class AccentLine : public QWidget
{
public:
    explicit AccentLine(QWidget *half)
        : QWidget(half)
    {
        setObjectName(QStringLiteral("ZoomAccent"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedHeight(2);
        hide();
        half->installEventFilter(this);
        place();
    }

    void place()
    {
        setGeometry(0, parentWidget()->height() - height(), parentWidget()->width(), height());
        raise();
    }

protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), theme::color(theme::Role::accent)); }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Resize)
            place();
        return QWidget::eventFilter(watched, event);
    }
};

} // namespace

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
    m_sourceAccent = new AccentLine(m_editor);
    m_pill = new EditorPill(this);
    // The find bar and the go-to-line field are built on first use: most
    // tabs never show them, and a restored session creates many tabs.

    const QString defaultMode = Settings::instance().text(key::defaultMode);
    m_viewMode = defaultMode == QLatin1String("source")  ? ViewMode::Source
                 : defaultMode == QLatin1String("split") ? ViewMode::SourceVisual
                                                         : ViewMode::Visual;
    m_sourceZoom = m_visualZoom = Settings::instance().number(key::zoom);
    m_readColumns = qBound(0, Settings::instance().number(key::readColumns), 99);
    setReadChars(Settings::instance().number(key::readChars));
    applySettings();
    connect(&Settings::instance(), &Settings::changed, this, &DocumentTab::onSettingChanged);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_splitter);

    connect(m_pill->lockButton(), &QToolButton::toggled, this, [this](bool locked) {
        (m_viewMode == ViewMode::SourceVisual ? m_splitLocked : m_visualLocked) = locked;
        if (m_visual)
            m_visual->setReadOnly(locked);
        // (Locked by the switch: the line at the top stays there, as when
        // columns are switched on.)
        updateColumns(m_lockingForColumns);
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
    connect(m_editor, &TextView::zoomRequested, this, &DocumentTab::zoomBy);
    connect(m_editor, &TextView::columnsChanged, this, [this] {
        if (m_columnsView != m_editor)
            return;
        resolveReadColumns();
        Q_EMIT readStateChanged();
    });
    m_editor->setColumnCount(m_readColumns);

    // What the file shares with every tab showing it.
    TextFile *f = m_file.get();
    connect(f, &TextFile::modificationChanged, this, &DocumentTab::modificationChanged);
    connect(f, &TextFile::contentsChanged, this, &DocumentTab::textChanged);
    connect(f, &TextFile::contentReset, this, &DocumentTab::resetCarets);
    connect(f, &TextFile::contentReset, this, &DocumentTab::prepareMathWorker);
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
    connect(f, &TextFile::highlighted, this, [this](int first, int last) {
        forEachView([&](TextView *view) { view->formatsChanged(first, last); });
    });
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
    // The copy takes this tab's caret, view state and text.
    const_cast<DocumentTab *>(this)->ensureLoaded();
    auto *tab = new DocumentTab(m_file, parent);
    tab->m_splitLocked = m_splitLocked;
    tab->m_visualLocked = m_visualLocked;
    tab->setViewMode(m_viewMode);
    tab->m_sourceZoom = m_sourceZoom;
    tab->m_visualZoom = m_visualZoom;
    tab->setReadColumns(m_readColumns);
    tab->setReadChars(m_readChars);
    // A copy shows what this tab does (columns too, as read mode always was).
    tab->m_wantsColumns = m_wantsColumns;
    tab->updateColumns();
    // The views take their fonts when the tab is shown, as after setZoom.
    tab->m_settingsStale = true;
    tab->editor()->setTextCursor(editor()->textCursor());
    // What is collapsed in the copy's rendered view is as here (and goes no
    // further: the state is not saved).
    if (m_visual) {
        tab->ensureVisualView();
        tab->m_visual->setFoldState(m_visual->foldState());
    }
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

void DocumentTab::setDeferred(const QJsonObject &entry, std::function<void(DocumentTab *)> afterLoad)
{
    m_deferred = true;
    m_sessionEntry = entry;
    m_afterLoad = std::move(afterLoad);
}

bool DocumentTab::ensureLoaded()
{
    const bool ok = m_file->ensureLoaded();
    if (m_deferred) {
        m_deferred = false;
        auto afterLoad = std::move(m_afterLoad);
        m_afterLoad = nullptr;
        m_sessionEntry = {};
        if (afterLoad)
            afterLoad(this);
    }
    return ok;
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
    view->revealCaret();
    view->centerCursor();
    view->setFocus();
}

TextView *DocumentTab::editor() const
{
    return m_active ? m_active : m_editor;
}

bool DocumentTab::isReadOnly() const
{
    // Only Visual mode can be locked; the source view is editable (in
    // columns too).
    if (isMarkdown() && m_viewMode == ViewMode::Visual)
        return m_visualLocked;
    if (isMarkdown() && m_viewMode == ViewMode::SourceVisual)
        return false;
    return m_editor->isReadOnly();
}

void DocumentTab::prepareMathWorker()
{
    // Math is between dollar signs (and nothing else is read as math), so a
    // document with none has none; a very large one is not looked through.
    constexpr int mostChars = 8 * 1024 * 1024;
    if (!m_visual || !isMarkdown() || m_doc->characterCount() > mostChars)
        return;
    if (!m_doc->find(QStringLiteral("$")).isNull())
        math::prepareWorker();
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
    m_visualAccent = new AccentLine(m_visual);
    connect(m_visual, &TextView::zoomRequested, this, &DocumentTab::zoomBy);
    connect(m_visual, &VisualView::linkActivated, this, &DocumentTab::linkActivated);
    connect(m_visual, &TextView::linkRequested, this, &DocumentTab::linkActivated);
    connect(m_visual, &TextView::filesDropped, this, &DocumentTab::filesDropped);
    connect(m_file.get(), &TextFile::contentReset, m_visual, &VisualView::documentReset);
    m_visual->setDiagnostics(m_file->diagnostics());
    prepareMathWorker();
    connect(m_visual, &TextView::scrolled, this, [this] { syncScroll(m_visual, m_editor); });
    connect(m_visual, &TextView::selectionChanged, this, [this] { mirrorSelection(m_visual, m_editor); });
    connect(m_visual, &TextView::cursorPositionChanged, this, [this] {
        if (m_active == m_visual)
            Q_EMIT cursorPositionChanged();
    });
    m_visual->setReadColumns(m_readColumns);
    m_visual->setReadChars(m_readChars);
    connect(m_visual, &VisualView::readChanged, this, [this] {
        if (m_columnsView != m_visual)
            return;
        resolveReadColumns();
        // The hidden source view keeps the place too: the session saves it,
        // and Source mode opens there.
        if (const auto [first, last] = m_visual->readLineRange(); first >= 0)
            m_editor->scrollToLine(first);
        Q_EMIT readStateChanged();
    });
}

void DocumentTab::syncScroll(TextView *from, TextView *to)
{
    // Keep both halves of Source / Visual at the same source position,
    // fractions of a line included: at their tops, or their middles.
    if (m_syncingScroll || !to || !to->isVisible() || !from->isVisible())
        return;
    m_syncingScroll = true;
    to->followScroll(*from, Settings::instance().text(key::syncAnchor) == QLatin1String("middle"));
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
    updateAccent();
    Q_EMIT cursorPositionChanged();
    Q_EMIT zoomChanged();
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
        m_visualAccent = nullptr;
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
    updateAccent();
    updateColumns();
    Q_EMIT zoomChanged();
}

bool DocumentTab::canShowColumns() const
{
    // Not beside the other half. In the Visual Editor, editing, choosing
    // them locks it (see setColumns).
    return !isMarkdown() || m_viewMode != ViewMode::SourceVisual;
}

bool DocumentTab::columnsShowNow() const
{
    if (!isMarkdown() || m_viewMode == ViewMode::Source)
        return true;
    // Not while editing in the Visual Editor (pictures and tables would have
    // to move with every keystroke), and not beside the other half.
    return m_viewMode == ViewMode::Visual && m_visualLocked;
}

void DocumentTab::setColumns(bool on)
{
    if (on && isMarkdown() && m_viewMode == ViewMode::Visual && !m_visualLocked) {
        // Columns from the Visual Editor, editing: read mode, locked.
        m_wantsColumns = true;
        m_lockingForColumns = true;
        m_pill->lockButton()->setChecked(true); // (Its handler shows them.)
        m_lockingForColumns = false;
        return;
    }
    if (m_wantsColumns == on)
        return;
    m_wantsColumns = on;
    updateColumns(true);
}

bool DocumentTab::isReadMode() const
{
    return m_columnsView && m_columnsView == m_visual;
}

std::pair<int, int> DocumentTab::readLineRange() const
{
    if (m_columnsView == m_editor)
        return m_editor->columnLineRange();
    return isReadMode() ? m_visual->readLineRange() : std::pair<int, int>(-1, -1);
}

int DocumentTab::columnsShownCount() const
{
    if (m_columnsView == m_editor)
        return m_editor->shownColumnCount();
    return isReadMode() ? m_visual->readColumnCount() : 1;
}

int DocumentTab::columnsFit() const
{
    if (m_columnsView == m_editor)
        return m_editor->maxColumnCount();
    return isReadMode() ? m_visual->readMaxColumns() : 1;
}

std::pair<int, int> DocumentTab::columnsChars() const
{
    if (m_columnsView == m_editor)
        return m_editor->columnChars();
    return isReadMode() ? m_visual->readColumnChars() : std::pair{50, 50};
}

void DocumentTab::resolveReadColumns()
{
    // Only the first visible columns layout uses the preference's fit at the
    // requested width. A hidden tab or a Visual parse still waiting has no layout yet.
    if (m_readColumns == 0 && m_columnsView && m_columnsView->isVisible()
        && (!isReadMode() || m_visual->showsReadColumns()))
        setReadColumns(columnsShownCount());
}

void DocumentTab::setReadColumns(int columns)
{
    m_readColumns = qMax(0, columns);
    m_editor->setColumnCount(m_readColumns);
    if (m_visual)
        m_visual->setReadColumns(m_readColumns);
}

void DocumentTab::setReadChars(int chars)
{
    m_readChars = qRound(qBound(50, chars, 100) / 5.0) * 5;
    m_editor->setColumnChars(m_readChars);
    if (m_visual)
        m_visual->setReadChars(m_readChars);
}

void DocumentTab::updateColumns(bool switched)
{
    const bool visual = isMarkdown() && m_viewMode == ViewMode::Visual;
    TextView *target = nullptr;
    if (m_wantsColumns && columnsShowNow() && (!visual || m_visual))
        target = visual ? static_cast<TextView *>(m_visual) : m_editor;
    TextView *old = m_columnsView;
    if (target != old) {
        // The text at the top: the first line of the first column.
        const int top = old == m_editor ? m_editor->topLine() : old ? m_visual->readTopLine() : -1;
        // Off, the view scrolls to have that line at its top.
        if (old == m_editor)
            m_editor->setColumnsShown(false);
        else if (old && m_visual)
            m_visual->setReadMode(false);
        m_columnsView = target;
        if (target) {
            int start = -1; // Switched on: the line at the top stays there.
            if (old) {
                start = top; // From Source to the Visual Editor or back: the same place.
            } else if (!switched) {
                // Back where columns can be: from the caret's line, if it shows.
                const int caret = target->textCursor().blockNumber();
                start = target->isLineShown(caret) ? caret : target->topLine();
            }
            if (target == m_editor)
                m_editor->setColumnsShown(true, start);
            else
                m_visual->setReadMode(true, start);
            resolveReadColumns();
        } else if (old && m_wantsColumns && top >= 0) {
            // Columns cannot show here: Scroll, with the same text at the top
            // and the caret at its start, ready to edit.
            TextView *view = editor();
            view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(top)));
            view->scrollToLine(top);
            if (showsBothHalves())
                syncScroll(view, view == m_editor ? static_cast<TextView *>(m_visual) : m_editor);
        }
    }
    placeOverlays();
    if (target != old || columnsShowNow() != m_couldShowColumns) {
        m_couldShowColumns = columnsShowNow();
        Q_EMIT readStateChanged();
    }
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
    // Markdown has no line comment, and asking would load its definition (72 ms) before the first paint.
    const bool markdown = def.name() == QLatin1String("Markdown");
    m_editor->setCommentMarker(markdown ? QString() : def.singleLineCommentMarker());
    m_editor->setCodeEditing(def.isValid() && !markdown);
    updateViews();
}

bool DocumentTab::showsSource() const
{
    return !isMarkdown() || m_viewMode != ViewMode::Visual;
}

std::pair<int, int> DocumentTab::visibleSourceLines() const
{
    if (!m_editor->isVisible())
        return {-1, -1};
    return {m_editor->topLine(), m_editor->bottomLine()};
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
    resolveReadColumns();
}

bool DocumentTab::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_findBar && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        placeOverlays();
    if ((watched == m_editor || watched == m_visual) && event->type() == QEvent::FocusIn)
        setActiveView(static_cast<TextView *>(watched));
    if ((watched == m_editor || watched == m_visual) && event->type() == QEvent::StyleChange)
        onStyleChanged(static_cast<TextView *>(watched));
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
    int right = m_pill->isVisible() ? m_pill->width() + pillMarginRight * 2 : findBarMarginRight;
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
    m_appliedFont = zoomedEditorFont(m_editor);
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
        const QFont font = zoomedEditorFont(m_editor);
        if (font != m_appliedFont) {
            m_appliedFont = font;
            forEachView([&](TextView *view) { view->setFont(zoomedEditorFont(view)); });
        }
    } else if (changedKey == QLatin1String(key::drawWhitespace)) {
        forEachView([&](TextView *view) { view->setShowWhitespace(s.flag(key::drawWhitespace)); });
    } else if (changedKey == QLatin1String(key::lineSpacing)) {
        // (The visual view's spacing is the Markdown setting's.)
        m_editor->setLineSpacing(s.value(key::lineSpacing).toDouble());
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

void DocumentTab::onStyleChanged(TextView *view)
{
    // The style sheet style puts back the font a widget had when it was first
    // polished, whenever it polishes the widget again: on a theme or colour
    // change, and when the tab moves to another group or window. That undoes
    // the font and zoom set since (Preferences still shows them). Hidden
    // tabs set them when shown.
    if (!isVisible()) {
        m_settingsStale = true;
        return;
    }
    const QFont font = zoomedEditorFont(view);
    if (view->font() != font)
        view->setFont(font);
}

QFont DocumentTab::zoomedEditorFont(const TextView *view) const
{
    QFont font = theme::editorFont();
    font.setPointSizeF(font.pointSizeF() * zoom(view) / 100.0);
    return font;
}

int &DocumentTab::zoomOf(const TextView *view)
{
    return view == m_visual ? m_visualZoom : m_sourceZoom;
}

void DocumentTab::updateAccent()
{
    // Only a split has two halves to tell apart.
    const bool split = showsBothHalves();
    m_sourceAccent->setVisible(split && m_active == m_editor);
    if (m_visualAccent)
        m_visualAccent->setVisible(split && m_active == m_visual);
}

void DocumentTab::applyViewSettings(TextView *view)
{
    Settings &s = Settings::instance();
    view->beginSettingsUpdate();
    view->setFont(zoomedEditorFont(view));
    view->setShowWhitespace(s.flag(key::drawWhitespace));
    view->setTabWidth(indentation().tabSize);
    view->setInsertSpaces(indentation().insertSpaces);
    if (view != m_visual)
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
    view->endSettingsUpdate();
}

int DocumentTab::zoom() const
{
    return zoom(editor());
}

int DocumentTab::zoom(const TextView *view) const
{
    return view == m_visual ? m_visualZoom : m_sourceZoom;
}

void DocumentTab::setZoom(int percent)
{
    TextView *view = editor();
    percent = qBound(minZoom, percent, maxZoom);
    if (zoomOf(view) == percent)
        return;
    zoomOf(view) = percent;
    if (!isVisible()) {
        m_settingsStale = true;
    } else {
        if (view == m_editor)
            m_appliedFont = zoomedEditorFont(view);
        view->setFont(zoomedEditorFont(view));
    }
    Q_EMIT zoomChanged();
}

void DocumentTab::zoomBy(int steps)
{
    if (steps == 0)
        return;
    // Whole steps from the nearest one, so a value that is off the grid (a
    // preference of 110%) lands on it.
    const int stepped = qRound(zoom() / double(zoomStep)) * zoomStep;
    setZoom(stepped + steps * zoomStep);
}

void DocumentTab::resetZoom()
{
    setZoom(Settings::instance().number(key::zoom));
}

bool DocumentTab::zoomsVisual() const
{
    return m_visual && editor() == m_visual;
}

bool DocumentTab::showsBothHalves() const
{
    return isMarkdown() && m_viewMode == ViewMode::SourceVisual && m_visual;
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
        // In the view in use, as goToLine() (in read mode, its screen).
        TextView *view = editor();
        view->setTextCursor(cursor);
        view->revealCaret();
        view->centerCursor();
        view->setFocus();
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
    const QTextCursor cursor = editor()->textCursor();
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
