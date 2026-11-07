#include "VisualView.h"

#include "../PlainText.h"
#include "../Settings.h"
#include "../Theme.h"
#include "ImageViewer.h"
#include "MarkdownModel.h"
#include "RemoteImages.h"
#include "TableToolbar.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <optional>
#include <utility>

namespace vn {

namespace {

constexpr qreal sideMargin = 26;
constexpr qreal topMargin = 12;
constexpr qreal gapRowFactor = 0.6;
constexpr qreal activeBarInset = 10;
// Render cache budget per view, in KiB (see RenderedBlock::cost).
constexpr int renderCacheKiB = 48 * 1024;
// Renders the view keeps in hand besides the cache (see rendered()): a screen
// shows a few dozen blocks, at most a few hundred. What the last paint showed
// is kept even past the budget (one block alone may be over it), up to a
// hard limit on their number.
constexpr int renderMax = 512;
constexpr int renderKiB = 16 * 1024;
constexpr int renderHardMax = 2 * renderMax;

// Pointing at a table's edge: how far the band above (and left of) the table
// reaches out of it and into it, how near a border the quick-insert mark
// shows, and the mark's size and distance from the table.
constexpr qreal tableBandOut = 14;
constexpr qreal tableBandIn = 2;
constexpr qreal insertReach = 6;
constexpr qreal markRadius = 6.5;
constexpr qreal markOffset = 7.5;

bool sameChar(QChar a, QChar b)
{
    return a == b || (a.isSpace() && b.isSpace());
}

// The pointer over a column's top edge (an arrow down) or a row's left edge
// (an arrow right); Qt has no such cursors. Made once for each pixel ratio.
QCursor edgeArrowCursor(bool down, qreal ratio)
{
    static QHash<int, QCursor> cursors;
    const int key = int(ratio * 100) * 2 + (down ? 1 : 0);
    if (const auto found = cursors.constFind(key); found != cursors.cend())
        return *found;
    constexpr int size = 24;
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainterPath arrow;
    arrow.moveTo(9, 2);
    arrow.lineTo(15, 2);
    arrow.lineTo(15, 12);
    arrow.lineTo(20, 12);
    arrow.lineTo(12, 22);
    arrow.lineTo(4, 12);
    arrow.lineTo(9, 12);
    arrow.closeSubpath();
    if (!down)
        arrow = QTransform(0, 1, 1, 0, 0, 0).map(arrow); // The same, turned to the right.
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::white, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(arrow);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    p.drawPath(arrow);
    p.end();
    return QCursor(pixmap, down ? 12 : 22, down ? 22 : 12);
}

} // namespace

int sourceOffsetFor(const QString &renderedPrefix, const QString &source)
{
    int r = 0;
    for (int s = 0; s < source.size(); ++s) {
        if (r >= renderedPrefix.size())
            return s;
        if (sameChar(renderedPrefix.at(r), source.at(s)))
            ++r;
    }
    return int(source.size());
}

int renderedOffsetFor(const QString &rendered, const QString &source, int sourceOffset)
{
    int r = 0;
    const int end = qMin(sourceOffset, int(source.size()));
    for (int s = 0; s < end && r < rendered.size(); ++s) {
        if (sameChar(rendered.at(r), source.at(s)))
            ++r;
    }
    return r;
}

VisualView::VisualView(MarkdownModel *model, QWidget *parent)
    : TextView(model->document(), parent)
    , m_model(model)
    , m_cache(renderCacheKiB)
{
    setLineNumbersVisible(false);
    // The edited block has its own shading instead.
    setHighlightCurrentLine(false);
    // A table in source form is realigned when the caret leaves it.
    setRealignTablesOnLeave(true);
    setTextMargins(sideMargin, sideMargin, topMargin);
    viewport()->setMouseTracking(true);
    connect(this, &TextView::scrolled, this, &VisualView::scheduleToolbarUpdate);
    // (The mark of the quick insert is kept in viewport coordinates.)
    connect(this, &TextView::scrolled, this, &VisualView::clearTableHover);
    connect(model, &MarkdownModel::optionsChanged, this, [this] {
        clearRenderCache();
        relayout();
    });
    // Rendered blocks have the theme's colours baked in.
    connect(&theme::notifier(), &theme::Notifier::changed, this, [this] {
        clearRenderCache();
        relayout();
    });
    // Typing settled: one visible view asks for the reparse (off the GUI
    // thread for a large document), every view then rebuilds its rows
    // (hidden ones when shown).
    connect(model, &MarkdownModel::reparseDue, this, [this] {
        if (isVisible())
            m_model->flushInBackground();
    });
    connect(model, &MarkdownModel::reparsed, this, &VisualView::relayout);
    connect(model, &MarkdownModel::reparsed, this, &VisualView::updateRemoteBanner);
    connect(&DiagramNotifier::instance(), &DiagramNotifier::ready, this, &VisualView::asyncRenderReady);
    connect(&RemoteImages::instance(), &RemoteImages::ready, this, &VisualView::asyncRenderReady);
    // Pictures are only requested once the user agreed (the bar asks).
    connect(model, &MarkdownModel::remoteConsentChanged, this, [this] {
        clearRenderCache();
        relayout();
        updateRemoteBanner();
    });
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &changedKey) {
        if (changedKey != QLatin1String(key::remoteImages))
            return;
        clearRenderCache();
        relayout();
        updateRemoteBanner();
    });
    // Rows are built when the view is first shown (see showEvent).
}

void VisualView::setBaseDirectory(const QString &directory)
{
    if (m_baseDirectory == directory)
        return;
    m_baseDirectory = directory;
    clearRenderCache();
    relayout();
}

void VisualView::clearRenderCache()
{
    m_cache.clear();
    m_pendingKeys.clear();
    resetRenders();
}

void VisualView::resetRenders()
{
    m_renders.clear();
    m_renderOrder.clear();
    m_renderKiB = 0;
}

void VisualView::asyncRenderReady()
{
    // Drop the source stand-ins of diagrams and the blocks with pictures on
    // their way so they render again, now with the results.
    if (m_pendingKeys.isEmpty())
        return;
    for (const QString &key : std::as_const(m_pendingKeys))
        m_cache.remove(key);
    m_pendingKeys.clear();
    std::erase_if(m_renderOrder, [this](int line) {
        const auto it = m_renders.find(line);
        if (!it->second.block->isPending())
            return false;
        m_renderKiB -= it->second.cost;
        m_renders.erase(it);
        return true;
    });
    relayout();
}

void VisualView::setHighlightActiveBlock(bool on)
{
    m_highlightActiveBlock = on;
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Rows

void VisualView::rowsMayHaveChanged()
{
    scheduleToolbarUpdate();
    // An edit ends the selection of cells, and moves what the mark pointed at.
    clearTableHover();
    if (m_tableSel.active && m_tableSel.revision != document()->revision())
        clearTableSelection();
    // A hidden view (Source mode) waits until it is shown, so edits don't
    // re-parse the document for nothing.
    if (!isVisible()) {
        m_rowsStale = true;
        return;
    }
    rebuildRows();
}

void VisualView::showEvent(QShowEvent *event)
{
    TextView::showEvent(event);
    m_hasFocus = hasFocus();
    scheduleToolbarUpdate();
    // Edits made while hidden are parsed once now rather than shown stale,
    // but a large document with nothing usable parsed is parsed on a worker
    // (its source shows meanwhile).
    if (m_model->hasUsableSpans() || !m_model->parseInBackground())
        m_model->flush();
    if (m_rowsStale)
        relayout();
    updateRemoteBanner();
}

void VisualView::hideEvent(QHideEvent *event)
{
    TextView::hideEvent(event);
    scheduleToolbarUpdate();
    m_showsParse = false;
    m_unpainted = true;
}

// ---------------------------------------------------------------------------
// Remote pictures

bool VisualView::remoteImagesAllowed() const
{
    return Settings::instance().text(key::remoteImages) != QLatin1String("never")
           && m_model->remoteConsent() == MarkdownModel::RemoteConsent::Allow;
}

void VisualView::updateRemoteBanner()
{
    QStringList urls;
    if (isVisible() && Settings::instance().text(key::remoteImages) != QLatin1String("never")
        && m_model->remoteConsent() == MarkdownModel::RemoteConsent::Undecided) {
        if (m_remoteUrlsGeneration != m_model->parseGeneration()) {
            // The last parse as it is (blocks() would parse a large document
            // again here, on the GUI thread). Blocks are scanned by their
            // HTML, so after an edit only the changed ones are laid out again.
            m_remoteUrls.clear();
            QSet<QString> seen;
            QHash<size_t, QStringList> scans;
            const RenderContext ctx = context();
            for (int i = 0, n = m_model->parsedBlockCount(); i < n; ++i) {
                const core::Block &block = m_model->parsedBlock(i);
                // (Most blocks can't ask for any: no need to keep their scans.)
                if (!RenderedBlock::mayAskForPictures(block))
                    continue;
                const size_t hash = qHash(QByteArrayView(block.html.data(), qsizetype(block.html.size())));
                const auto known = m_remoteScans.constFind(hash);
                const QStringList urls = known != m_remoteScans.constEnd() ? *known : RenderedBlock::remoteImages(block, ctx);
                scans.insert(hash, urls);
                for (const QString &url : urls) {
                    if (!seen.contains(url)) {
                        seen.insert(url);
                        m_remoteUrls.append(url);
                    }
                }
            }
            m_remoteScans = std::move(scans);
            m_remoteUrlsGeneration = m_model->parseGeneration();
        }
        urls = m_remoteUrls;
    }
    if (urls.isEmpty()) {
        if (m_banner && !m_banner->isHidden()) {
            m_banner->hide();
            layoutBanner();
        }
        return;
    }

    if (!m_banner) {
        m_banner = new QFrame(this);
        m_banner->setObjectName(QStringLiteral("RemoteImageBanner"));
        m_banner->setAttribute(Qt::WA_StyledBackground);
        auto *layout = new QHBoxLayout(m_banner);
        // The right edge is left to the lock/pencil pill floating over it.
        layout->setContentsMargins(int(sideMargin), 6, 84, 6);
        layout->setSpacing(8);
        m_bannerText = new QLabel(m_banner);
        m_bannerText->setWordWrap(true);
        m_bannerText->setTextFormat(Qt::PlainText); // Host names come from the document.
        layout->addWidget(m_bannerText, 1);
        const auto addButton = [&](const QString &text, MarkdownModel::RemoteConsent consent) {
            auto *button = new QPushButton(text, m_banner);
            button->setObjectName(QStringLiteral("BannerButton"));
            layout->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, consent] {
                // Choosing to load is also the way to try failed pictures again.
                if (consent == MarkdownModel::RemoteConsent::Allow)
                    RemoteImages::instance().clearFailures();
                m_model->setRemoteConsent(consent);
                setFocus();
            });
        };
        addButton(tr("Load images"), MarkdownModel::RemoteConsent::Allow);
        addButton(tr("Don't load"), MarkdownModel::RemoteConsent::Deny);
    }
    // Hosts in order of appearance, with how many pictures each serves.
    QStringList hosts;
    QHash<QString, int> counts;
    for (const QString &url : urls) {
        const QUrl parsed(url);
        const QString host = parsed.port() > 0 ? QStringLiteral("%1:%2").arg(parsed.host()).arg(parsed.port()) : parsed.host();
        if (counts[host]++ == 0)
            hosts.append(host);
    }
    QStringList shown;
    for (const QString &host : hosts)
        shown.append(QStringLiteral("%1 (%2)").arg(host).arg(counts[host]));
    constexpr int maxShown = 4;
    QString text = shown.mid(0, maxShown).join(QLatin1String(", "));
    if (shown.size() > maxShown)
        text += tr(" and %n more host(s)", "", shown.size() - maxShown);
    m_bannerText->setText(tr("%n remote image(s) from %1. Load them only if you trust the links in this file.", "",
                             int(urls.size())).arg(text));
    m_bannerText->setToolTip(plainToolTip(shown.join(u'\n') + QLatin1String("\n\n")
                                          + tr("Pictures on this computer or the local network are never loaded."),
                                          4096));
    m_banner->show();
    layoutBanner();
}

void VisualView::layoutBanner()
{
    int height = 0;
    if (m_banner && !m_banner->isHidden()) {
        const int width = viewport()->width();
        height = m_banner->layout()->totalHeightForWidth(width);
        m_banner->setGeometry(0, 0, width, height);
    }
    if (height != m_bannerHeight) {
        m_bannerHeight = height;
        setViewportMargins(0, height, 0, 0);
    }
}

void VisualView::resizeEvent(QResizeEvent *event)
{
    TextView::resizeEvent(event);
    layoutBanner();
    scheduleToolbarUpdate();
}

// ---------------------------------------------------------------------------
// Table toolbar

void VisualView::scheduleToolbarUpdate()
{
    if (m_toolbarUpdatePending)
        return;
    m_toolbarUpdatePending = true;
    QTimer::singleShot(0, this, [this] {
        m_toolbarUpdatePending = false;
        updateTableToolbar();
    });
}

void VisualView::updateTableToolbar()
{
    const auto hideToolbar = [this] {
        if (m_tableToolbar)
            m_tableToolbar->hide();
    };
    // The box over a cell follows its cell (the view scrolled, was resized or
    // re-rendered); one whose cell is gone ends there.
    if (m_cellEditor && !placeCellEditor())
        closeCellEditor(TableCellEditor::Exit::FocusLost);
    if (!isVisible() || isReadOnly() || !(m_hasFocus || m_cellEditor) || cursorCount() != 1 || !markdownEditing())
        return hideToolbar();
    const md::TableInfo info = tableAtCaret();
    if (!info.found)
        return hideToolbar();
    qreal firstTop = 0;
    qreal firstBottom = 0;
    qreal lastTop = 0;
    qreal lastBottom = 0;
    const bool firstShown = lineBounds(info.firstLine, &firstTop, &firstBottom);
    const bool lastShown = lineBounds(info.lastLine, &lastTop, &lastBottom);
    if (!firstShown && !lastShown && textCursor().blockNumber() > info.lastLine)
        return hideToolbar();

    if (!m_tableToolbar) {
        m_tableToolbar = new TableToolbar(viewport());
        connect(m_tableToolbar, &TableToolbar::operation, this, &VisualView::toolbarOperation);
        connect(m_tableToolbar->moreMenu(), &QMenu::aboutToShow, this, [this] {
            m_tableToolbar->moreMenu()->clear();
            populateTableMenu(m_tableToolbar->moreMenu());
        });
    }
    // A press on the toolbar gives the focus to the first widget up from the
    // button that takes it on a click (QApplication), the view: that would end
    // the cell box, and the toolbar with it, before the button is released.
    // The search stops at a widget whose focus proxy has the focus already.
    m_tableToolbar->setFocusProxy(m_cellEditor);
    m_tableToolbar->setState(m_tableSel.active ? selectionInfo() : info);
    m_tableToolbar->adjustSize();
    const QSize size = m_tableToolbar->size();
    constexpr int gap = 4;
    // Above the table; else below it; else (it covers the viewport) at the top.
    int y = 2;
    if (firstShown && firstTop - size.height() - gap >= 0)
        y = int(firstTop) - size.height() - gap;
    else if (lastShown && lastBottom + size.height() + gap <= viewport()->height())
        y = int(lastBottom) + gap;
    const int x = qBound(0, int(textLeft() - activeBarInset), qMax(0, viewport()->width() - size.width()));
    m_tableToolbar->move(x, y);
    m_tableToolbar->show();
    m_tableToolbar->raise();
}

void VisualView::toolbarOperation(md::TableOp op)
{
    // Choosing the alignment a column has goes back to the default.
    const md::TableInfo info = tableAtCaret();
    const struct
    {
        md::TableOp op;
        md::TableAlignment alignment;
    } same[] = {{md::TableOp::AlignLeft, md::TableAlignment::Left},
                {md::TableOp::AlignCenter, md::TableAlignment::Center},
                {md::TableOp::AlignRight, md::TableAlignment::Right}};
    for (const auto &s : same) {
        if (s.op == op && info.found && info.alignment == s.alignment)
            op = md::TableOp::AlignNone;
    }
    if (m_tableSel.active)
        selectionOperation(op);
    else
        editTable(op);
    setFocus();
}

// ---------------------------------------------------------------------------
// Cell editing

bool VisualView::tableCellPressed(const Row &r, const QPointF &point, QMouseEvent *event)
{
    if (isReadOnly() || !markdownEditing() || (event->modifiers() & ~Qt::KeypadModifier))
        return false;
    const Render *render = rendered(r);
    if (!render)
        return false;
    const auto hit = render->block->tableCellAt(point);
    if (!hit || !checkTable(r, render).ok)
        return false;
    // Consumed: the caret stays where it is. A click (no drag) opens the box
    // when the button comes up; a drag selects the cells from this one to
    // where the button comes up (see mouseMoveEvent).
    m_pendingCell = {true, r.firstLine, hit->row, hit->col, event->position().toPoint(), point};
    return true;
}

const VisualView::TableCheck &VisualView::checkTable(const Row &r, const Render *render)
{
    const int revision = document()->revision();
    TableCheck &check = m_tableCheck;
    if (check.line == r.firstLine && check.revision == revision && check.block == render->block.get())
        return check;
    check = {};
    check.line = r.firstLine;
    check.revision = revision;
    check.block = render->block.get();
    // The block must be one table (and not, say, front matter, shown as a
    // table), the rendered one with the cells the source has: a pipe table,
    // or an HTML block that renders as nothing but its <table>.
    int at = 0;
    md::TableInfo info = md::tableInfo(render->source, 0, 0);
    if (!info.found) {
        static const QRegularExpression tag(QStringLiteral("^ {0,3}<table[\\s>/]"),
                                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        if (const QRegularExpressionMatch m = tag.match(render->source); m.hasMatch()) {
            at = int(QStringView(render->source).first(m.capturedStart()).count(u'\n'));
            info = md::tableInfo(render->source, at, 0);
        }
    }
    const int lines = r.lastLine - r.firstLine;
    const QSize cells = render->block->tableCells();
    check.ok = info.found && info.firstLine == at && info.lastLine <= lines && (info.html || info.lastLine == lines)
               && cells.height() == info.rows && cells.width() == info.cols;
    check.at = at;
    check.info = info;
    // Not in a table with merged cells, where a row or column is not a row
    // or column of the source.
    check.canInsert = check.ok && md::tableOpAvailable(info, md::TableOp::RowBelow)
                      && md::tableOpAvailable(info, md::TableOp::ColumnRight);
    return check;
}

bool VisualView::openCellEditor(int line, int row, int col)
{
    clearTableSelection();
    if (m_cellEditor)
        closeCellEditor(TableCellEditor::Exit::Commit);
    const int index = rowOfLine(line);
    if (index < 0 || index >= rowCount())
        return false;
    const Row r = this->row(index);
    if (!r.rendered || r.firstLine != line)
        return false;
    const Render *render = rendered(r);
    if (!render)
        return false;
    const TableCheck &check = checkTable(r, render);
    if (!check.ok || row >= check.info.rows || col >= check.info.cols)
        return false;
    const int at = check.at;
    const md::TableCell cell = md::tableCell(render->source, at, row, col);
    if (!cell.found)
        return false;

    m_cell = {line, at, row, col, cell.text};
    if (!m_cellEditor) {
        m_cellEditor = new TableCellEditor(viewport());
        connect(m_cellEditor, &TableCellEditor::finished, this, [this](TableCellEditor::Exit how) { closeCellEditor(how); });
    }
    m_cellEditor->setFont(context().font);
    m_cellEditor->open(cell.text);
    if (!placeCellEditor()) {
        TableCellEditor *editor = m_cellEditor;
        m_cellEditor = nullptr;
        m_cell = {};
        delete editor;
        return false;
    }
    setTableTarget(line + at, row, col);
    m_cellEditor->show();
    m_cellEditor->raise();
    m_cellEditor->setFocus(Qt::MouseFocusReason);
    scheduleToolbarUpdate();
    return true;
}

bool VisualView::placeCellEditor()
{
    if (!m_cellEditor || m_cell.line < 0)
        return false;
    const int index = rowOfLine(m_cell.line);
    if (index < 0 || index >= rowCount())
        return false;
    const Row r = row(index);
    if (!r.rendered || r.firstLine != m_cell.line)
        return false;
    const Render *render = rendered(r);
    if (!render)
        return false;
    const md::TableCell cell = md::tableCell(render->source, m_cell.at, m_cell.row, m_cell.col);
    const auto rect = render->block->tableCellRect(m_cell.row, m_cell.col);
    if (!cell.found || !rect || cell.text != m_cell.original)
        return false;
    // The cell's line is where the view scrolls to when the cell is out of view.
    const int sourceLine = m_cell.line + cell.line;
    for (int attempt = 0; attempt < 2; ++attempt) {
        qreal top = 0;
        qreal bottom = 0;
        if (lineBounds(m_cell.line, &top, &bottom)) {
            const qreal y = top + rect->top();
            if (y + rect->height() > 0 && y < viewport()->height()) {
                const int width = qBound(60, int(rect->width()), qMax(60, viewport()->width() - int(textLeft() + rect->left())));
                m_cellEditor->place(QPoint(int(textLeft() + rect->left()), int(y)), width, int(rect->height()));
                return true;
            }
        }
        if (attempt == 0)
            scrollToLine(sourceLine);
    }
    return false;
}

void VisualView::insertTable(int rows, int cols)
{
    const int first = insertTableLines(rows, cols, true);
    if (first < 0)
        return;
    const md::TableInfo info = tableInfoAt(first, 0);
    if (info.found && !isReadOnly()) {
        // The caret below the table (past the blank line after it), so the
        // table is shown as a table, not as its source.
        const int below = qMin(info.lastLine + 2, document()->blockCount() - 1);
        if (below > info.lastLine) {
            setTextCursor(QTextCursor(document()->findBlockByNumber(below)));
            m_model->flush();
            if (openCellEditor(first, 0, 0)) {
                m_cellEditor->selectAll();
                return;
            }
        }
    }
    moveToTableCell(first, 0, 0); // Its source, then: the header selected.
}

void VisualView::tableTargetEditing()
{
    // A command of the table (the toolbar, a menu) is about to change it: the
    // text typed so far goes in first.
    closeCellEditor(TableCellEditor::Exit::Commit);
}

void VisualView::closeCellEditor(TableCellEditor::Exit how)
{
    using Exit = TableCellEditor::Exit;
    if (!m_cellEditor)
        return;
    // Forget the box first: closing it moves the focus, which ends it again.
    TableCellEditor *editor = m_cellEditor;
    m_cellEditor = nullptr;
    const CellTarget cell = m_cell;
    m_cell = {};
    setTableTarget(-1);
    const QString text = editor->toPlainText();
    editor->hide();
    editor->deleteLater();
    if (how != Exit::FocusLost)
        setFocus(Qt::OtherFocusReason);

    bool changed = false;
    if (how != Exit::Cancel && text != cell.original)
        changed = editTableAt(cell.line + cell.at, md::TableOp::SetCell, cell.row, cell.col, text, false);
    if (changed)
        m_model->flush(); // Show the new text (and widths) now.
    scheduleToolbarUpdate();

    // Tab, Shift+Tab and the arrows go on to the next cell.
    if (how == Exit::Commit || how == Exit::Cancel || how == Exit::FocusLost)
        return;
    const md::TableCell now = tableCellOf(cell.line + cell.at, cell.row, cell.col);
    if (!now.found)
        return;
    const md::TableInfo info = tableInfoAt(now.line, now.start);
    int row = cell.row;
    int col = cell.col;
    switch (how) {
    case Exit::Next:
        if (col + 1 < info.cols) {
            ++col;
        } else if (row + 1 < info.rows) {
            ++row;
            col = 0;
        } else {
            // Past the last cell: a new row.
            if (!editTableAt(cell.line + cell.at, md::TableOp::RowBelow, row, 0, {}, false))
                return;
            m_model->flush();
            ++row;
            col = 0;
        }
        break;
    case Exit::Previous:
        if (col > 0) {
            --col;
        } else if (row > 0) {
            --row;
            col = info.cols - 1;
        } else {
            return;
        }
        break;
    case Exit::Up:
        if (row == 0)
            return;
        --row;
        break;
    case Exit::Down:
        if (row + 1 >= info.rows)
            return;
        ++row;
        break;
    default:
        return;
    }
    openCellEditor(cell.line, row, col);
}

void VisualView::rebuildRows()
{
    const int lines = document()->blockCount();
    // Shown before a large document is parsed: its source stands in, line
    // by line, until the worker is done (see MarkdownModel::parseInBackground).
    if (m_model->isAwaitingParse()
        || (!m_model->hasUsableSpans() && !m_showsParse && m_model->parseInBackground())) {
        if (m_standIn && int(m_lineToRow.size()) == lines)
            return;
        m_rows.clear();
        m_rows.reserve(lines);
        m_lineToRow.resize(lines);
        for (int line = 0; line < lines; ++line) {
            m_lineToRow[line] = line;
            m_rows.push_back({{line, line, false}, -1});
        }
        m_expandedBlocks.clear();
        m_partSpans.clear();
        m_rowsExpanded.clear();
        m_rowsStale = false;
        m_rowsRevision = -1;
        m_standIn = true;
        return;
    }
    // The rendered rows that replace the stand-in keep its place.
    std::optional<Place> keptPlace;
    if (m_standIn) {
        keptPlace = place();
        m_standIn = false;
    }
    m_showsParse = true;
    const auto &spans = m_model->spans();

    // Blocks open for editing only while this view has focus, so a document
    // opens (and stays, when you look elsewhere) fully rendered.
    // While the mouse is selecting, rows stay as they are: blocks must not
    // swap to source and shift under the pointer mid-drag. mouseReleaseEvent
    // applies the expansion once the selection is made.
    if (isMouseSelecting() && !m_rows.empty() && !m_rowsStale && int(m_lineToRow.size()) == lines
        && m_rowsRevision == m_model->revision())
        return;

    // Line ranges carets touch, widened to whole spans, sorted and merged.
    std::vector<std::pair<int, int>> expanded;
    if (!isReadOnly() && hasFocus()) {
        for (const QTextCursor &c : cursors()) {
            int from = document()->findBlock(c.selectionStart()).blockNumber();
            int to = document()->findBlock(c.selectionEnd()).blockNumber();
            if (const int s = m_model->spanAt(from); s >= 0)
                from = spans[s].first;
            if (const int s = m_model->spanAt(to); s >= 0)
                to = spans[s].last;
            expanded.push_back({from, to});
        }
        std::sort(expanded.begin(), expanded.end());
        std::vector<std::pair<int, int>> merged;
        for (const auto &range : expanded) {
            if (!merged.empty() && range.first <= merged.back().second)
                merged.back().second = qMax(merged.back().second, range.second);
            else
                merged.push_back(range);
        }
        expanded = std::move(merged);
    }
    if (!m_rowsStale && m_rowsRevision == m_model->revision() && int(m_lineToRow.size()) == lines
        && expanded == m_rowsExpanded)
        return;

    // Rows that matched the document and now open or close for editing
    // (a caret moved into a table, a selection ended in it) change height and
    // count: the top of the view stays at the same source line, or a click
    // among the last rows of a long table would scroll to its first row.
    if (!keptPlace && !m_rows.empty() && !m_rowsStale && m_rowsRevision == m_model->revision()
        && int(m_lineToRow.size()) == lines)
        keptPlace = place();

    m_rows.clear();
    m_rows.reserve(spans.size() * 2 + 16);
    m_expandedBlocks.clear();
    m_partSpans.clear();
    m_lineToRow.assign(lines, 0);
    // Queries come in ascending line order, so one pass over the ranges.
    size_t next = 0;
    const auto touched = [&](int first, int last) {
        while (next < expanded.size() && expanded[next].second < first)
            ++next;
        return next < expanded.size() && expanded[next].first <= last;
    };
    size_t s = 0;
    int line = 0;
    while (line < lines) {
        while (s < spans.size() && spans[s].first < line)
            ++s;
        if (s < spans.size() && spans[s].first == line) {
            const MarkdownModel::Span &span = spans[s];
            const int last = qMin(span.last, lines - 1);
            const bool open = touched(line, last);
            if (open)
                m_expandedBlocks.insert(int(s));
            // Edited lines with nothing parsed to stand in show as source
            // until the reparse.
            if (open || (span.block < 0 && span.previous < 0)) {
                for (int l = line; l <= last; ++l) {
                    m_lineToRow[l] = int(m_rows.size());
                    m_rows.push_back({{l, l, false}, int(s)});
                }
            } else if (madeInParts(int(s))) {
                // A row a part, so that only the parts on screen are laid out.
                m_partSpans.insert(int(s));
                for (int from = line; from <= last; from += RenderedBlock::partLines) {
                    const int to = qMin(last, from + RenderedBlock::partLines - 1);
                    for (int l = from; l <= to; ++l)
                        m_lineToRow[l] = int(m_rows.size());
                    m_rows.push_back({{from, to, true}, int(s)});
                }
            } else {
                for (int l = line; l <= last; ++l)
                    m_lineToRow[l] = int(m_rows.size());
                m_rows.push_back({{line, last, true}, int(s)});
            }
            line = last + 1;
            ++s;
            continue;
        }
        // A line outside every block (blank): a small gap unless edited.
        m_lineToRow[line] = int(m_rows.size());
        m_rows.push_back({{line, line, !touched(line, line)}, -1});
        ++line;
    }
    m_rowsStale = false;
    m_rowsRevision = m_model->revision();
    m_rowsExpanded = std::move(expanded);
    if (keptPlace)
        restorePlace(*keptPlace);
}

VisualView::Place VisualView::place()
{
    qreal top = 0;
    const int bottom = rowAt(viewport()->height() - 1, &top);
    const int caret = textCursor().blockNumber();
    return {topLinePosition(), caret >= topLine() && (bottom < 0 || caret <= row(bottom).lastLine)};
}

void VisualView::restorePlace(const Place &place)
{
    scrollToLinePosition(place.top);
    if (place.caretShown)
        ensureCursorVisible();
}

int VisualView::rowCount() const
{
    return qMax(1, int(m_rows.size()));
}

TextView::Row VisualView::row(int index) const
{
    if (index < 0 || index >= int(m_rows.size()))
        return {0, 0, false};
    return m_rows[index].row;
}

int VisualView::rowOfLine(int line) const
{
    return line >= 0 && line < int(m_lineToRow.size()) ? m_lineToRow[line] : 0;
}

int VisualView::spanForRow(const Row &r) const
{
    const int index = rowOfLine(r.firstLine);
    return index < int(m_rows.size()) ? m_rows[index].span : -1;
}

// ---------------------------------------------------------------------------
// Rendering

RenderContext VisualView::context() const
{
    RenderContext ctx;
    ctx.baseDirectory = m_baseDirectory;
    ctx.codeFont = font();
    // Body text follows the "text font" setting, scaled with the editor's zoom.
    const qreal zoom = font().pointSizeF() / theme::editorFont().pointSizeF();
    ctx.font = theme::textFont();
    ctx.font.setPointSizeF(ctx.font.pointSizeF() * zoom);
    ctx.lineHeight = Settings::instance().value(key::textLineHeight).toDouble();
    ctx.width = textWidth();
    ctx.devicePixelRatio = devicePixelRatioF();
    ctx.mermaidTheme = Settings::instance().text(key::mermaidTheme);
    ctx.allowRemoteImages = remoteImagesAllowed();
    return ctx;
}

VisualView::LongBox &VisualView::longBox(int blockIndex)
{
    if (m_longBoxesGeneration != m_model->parseGeneration()) {
        m_longBoxes.clear();
        m_longBoxesGeneration = m_model->parseGeneration();
    }
    return m_longBoxes[blockIndex];
}

bool VisualView::madeInParts(int s)
{
    const MarkdownModel::Span &span = m_model->spans()[s];
    const int blockIndex = span.block >= 0 ? span.block : span.previous;
    if (blockIndex < 0 || span.last - span.first < RenderedBlock::partLines)
        return false;
    QTextDocument *doc = document();
    const QTextBlock last = doc->findBlockByNumber(qMin(span.last, doc->blockCount() - 1));
    // As long as sourceText(span.first, span.last).
    const qsizetype chars = last.position() + last.length() - 1 - doc->findBlockByNumber(span.first).position();
    const core::Block &block = m_model->parsedBlock(blockIndex);
    // A parsed block's lines are its own until the next parse; a stand-in's
    // were edited since.
    if (span.block < 0)
        return RenderedBlock::isLongCodeBox(block, chars);
    LongBox &box = longBox(blockIndex);
    if (box.parts == -1)
        box.parts = RenderedBlock::isLongCodeBox(block, chars);
    return box.parts == 1;
}

const VisualView::Render *VisualView::rendered(const Row &r)
{
    const int index = spanForRow(r);
    const auto &spans = m_model->spans();
    if (index < 0 || index >= int(spans.size()))
        return nullptr;
    const MarkdownModel::Span &span = spans[index];
    // An edited block shows its last parse until the reparse.
    const int blockIndex = span.block >= 0 ? span.block : span.previous;
    if (blockIndex < 0)
        return nullptr;

    const RenderContext ctx = context();
    if (m_rendersRevision != m_model->revision() || m_rendersWidth != ctx.width
        || m_rendersDpr != ctx.devicePixelRatio) {
        resetRenders();
        m_rendersRevision = m_model->revision();
        m_rendersWidth = ctx.width;
        m_rendersDpr = ctx.devicePixelRatio;
    }
    if (const auto it = m_renders.find(r.firstLine); it != m_renders.end()) {
        it->second.used = m_frame;
        return &it->second;
    }

    const core::Block &block = m_model->parsedBlock(blockIndex);
    // A part of a long code box: the lines of the row, and where it sits in the box.
    const bool inParts = m_partSpans.contains(index);
    RenderedBlock::Part part;
    QString source;
    if (inParts) {
        source = m_model->sourceText(r.firstLine, r.lastLine);
        part.first = r.firstLine == span.first;
        part.last = r.lastLine >= qMin(span.last, document()->blockCount() - 1);
        int wrap = -1;
        LongBox *box = span.block >= 0 ? &longBox(blockIndex) : nullptr;
        if (box)
            wrap = box->wrap;
        if (wrap == -1) {
            wrap = RenderedBlock::wrapsAnywhere(m_model->sourceText(span.first, span.last));
            if (box)
                box->wrap = wrap;
        }
        part.wrapAnywhere = wrap == 1;
    } else {
        source = m_model->sourceText(span.first, span.last);
    }
    // Identical blocks render identically, so the source (by hash, checked
    // on a hit) is the key. A stand-in pairs an old parse with new source:
    // never mistake it for the real render of that source.
    QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                      .arg(int(ctx.width))
                      .arg(ctx.font.pointSizeF())
                      .arg(ctx.font.family())
                      .arg(ctx.lineHeight)
                      .arg(int(block.kind))
                      .arg(ctx.codeFont.family())
                      .arg(ctx.devicePixelRatio)
                      .arg(source.size())
                      .arg(qHash(source));
    if (inParts)
        key += QStringLiteral("|part:%1%2%3").arg(int(part.first)).arg(int(part.last)).arg(int(part.wrapAnywhere));
    else if (span.block < 0)
        key += QStringLiteral("|stale:%1:%2").arg(m_model->parseGeneration()).arg(blockIndex);
    Render memo;
    const Render *hit = m_cache.object(key);
    if (hit && hit->source == source) {
        memo = *hit;
    } else {
        const std::shared_ptr<RenderedBlock> created(inParts ? RenderedBlock::createPart(source, part, ctx)
                                                             : RenderedBlock::create(block, source, ctx));
        const int cost = created->cost() + int(source.size() / 512);
        ++m_renderCount;
        memo = {source, key, created, cost};
        if (created->isPending())
            m_pendingKeys.insert(key);
        m_cache.insert(key, new Render(memo), cost);
    }
    memo.used = m_frame;
    Render &kept = m_renders[r.firstLine] = std::move(memo);
    // Keep the memos of the latest renders only: the cache holds the rest
    // within its budget, while memos on every block seen would keep them all
    // alive, however far the view has scrolled on. Those the paint in
    // progress asked for stay, though, or one block over the budget would be
    // made again for every other block shown with it.
    m_renderOrder.push_back(r.firstLine);
    m_renderKiB += kept.cost;
    for (size_t rotated = 0; rotated < m_renderOrder.size()
                             && (int(m_renders.size()) > renderMax || m_renderKiB > renderKiB);) {
        const int oldest = m_renderOrder.front();
        m_renderOrder.pop_front();
        const auto it = m_renders.find(oldest);
        if (oldest == r.firstLine || (it->second.used == m_frame && int(m_renders.size()) <= renderHardMax)) {
            m_renderOrder.push_back(oldest);
            ++rotated;
            continue;
        }
        m_renderKiB -= it->second.cost;
        m_renders.erase(it);
    }
    return &kept;
}

const RenderedBlock *VisualView::renderedForRow(const Row &r)
{
    const Render *render = rendered(r);
    return render ? render->block.get() : nullptr;
}

const VisualView::Render *VisualView::memoized(const Row &r) const
{
    if (m_rendersRevision != m_model->revision() || m_rendersWidth != textWidth()
        || m_rendersDpr != devicePixelRatioF())
        return nullptr;
    const auto it = m_renders.find(r.firstLine);
    return it != m_renders.end() ? &it->second : nullptr;
}

qreal VisualView::renderedRowHeight(const Row &r, qreal)
{
    if (spanForRow(r) < 0)
        return lineHeight() * gapRowFactor;
    // Before the first paint the width may change yet (a splitter lays out
    // a view it shows after showing it, and a mode switch shows it in a
    // half first): blocks not rendered at this width are measured roughly,
    // and rendered at the width the paint finds (see paintEvent).
    if (m_unpainted) {
        if (const Render *render = memoized(r))
            return render->block->height();
        m_estimated = true;
        return lineHeight() * (r.lastLine - r.firstLine + 1);
    }
    const RenderedBlock *block = renderedForRow(r);
    return block ? block->height() : lineHeight();
}

void VisualView::paintRenderedRow(QPainter &painter, const Row &r, const QRectF &rect)
{
    if (const Render *render = rendered(r)) {
        const RenderedBlock *block = render->block.get();
        block->paint(painter, rect.topLeft(), rect.width(), selectionForRow(r, block, render->source));
    }
}

void VisualView::paintEvent(QPaintEvent *event)
{
    if (m_unpainted) {
        // The rough heights give way to the real ones, at the same place:
        // the same source line at the top, and the caret in view if it was.
        std::optional<Place> estimatedPlace;
        if (m_estimated)
            estimatedPlace = place();
        m_unpainted = false;
        m_estimated = false;
        if (estimatedPlace) {
            relayout();
            restorePlace(*estimatedPlace);
        }
    }
    ++m_frame;
    TextView::paintEvent(event);
    paintTableOverlay(event);
}

RenderedSelection VisualView::selectionForRow(const Row &r, const RenderedBlock *block, const QString &source) const
{
    RenderedSelection result;
    if (!block || block->kind() == RenderedBlock::Kind::Empty || spanForRow(r) < 0 || !hasSelection())
        return result;
    QTextDocument *doc = document();
    const QTextBlock first = doc->findBlockByNumber(r.firstLine);
    const QTextBlock last = doc->findBlockByNumber(r.lastLine);
    const int rowStart = first.position();
    const int rowEnd = last.position() + last.length() - 1; // Without the line break.

    QString text;
    for (const QTextCursor &c : cursors()) {
        if (!c.hasSelection())
            continue;
        const int a = c.selectionStart();
        const int b = c.selectionEnd();
        if (b <= rowStart || a >= rowEnd)
            continue;
        switch (block->kind()) {
        case RenderedBlock::Kind::Image: {
            // Highlight the picture when the selection spans its content.
            const int contentStart = r.lastLine > r.firstLine ? first.next().position() : rowStart;
            const int contentEnd = r.lastLine > r.firstLine ? last.position() : rowEnd;
            if (a <= contentStart && b >= contentEnd)
                result.whole = true;
            break;
        }
        case RenderedBlock::Kind::Code:
            // The code box is a plain-text copy of the source (a part of one
            // has the line break after it too).
            result.ranges.append({qMax(a, rowStart) - rowStart, qMin(b, rowEnd + 1) - rowStart});
            break;
        case RenderedBlock::Kind::Rich: {
            // Only a selection end inside the block needs the (linear) mapping.
            if (text.isNull())
                text = block->renderedText();
            const int from = a <= rowStart ? 0 : renderedOffsetFor(text, source, a - rowStart);
            const int to = b >= rowEnd ? int(text.size()) : renderedOffsetFor(text, source, b - rowStart);
            if (to > from)
                result.ranges.append({from, to});
            break;
        }
        case RenderedBlock::Kind::Empty:
            break;
        }
    }
    return result;
}

QTextCursor VisualView::lineUnitAt(int position) const
{
    // A rendered block is one paragraph however many source lines it has
    // (and rows it is made in).
    const QTextBlock b = document()->findBlock(position);
    const int index = rowOfLine(b.blockNumber());
    Row r = row(index);
    if (r.rendered && index < int(m_rows.size()) && m_partSpans.contains(m_rows[index].span)) {
        int first = index;
        int last = index;
        while (first > 0 && m_rows[first - 1].span == m_rows[index].span)
            --first;
        while (last + 1 < int(m_rows.size()) && m_rows[last + 1].span == m_rows[index].span)
            ++last;
        r = {m_rows[first].row.firstLine, m_rows[last].row.lastLine, true};
    }
    if (!r.rendered || r.lastLine <= r.firstLine)
        return TextView::lineUnitAt(position);
    const QTextBlock last = document()->findBlockByNumber(r.lastLine);
    QTextCursor cursor(document());
    cursor.setPosition(document()->findBlockByNumber(r.firstLine).position());
    cursor.setPosition(last.next().isValid() ? last.next().position() : last.position() + last.length() - 1,
                       QTextCursor::KeepAnchor);
    return cursor;
}

void VisualView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_tableDrag.active && event->button() == Qt::LeftButton) {
        // The selection of cells, rows or columns is made as it was dragged.
        m_tableDrag = {};
        return;
    }
    const bool wasSelecting = isMouseSelecting();
    const PendingCell pending = std::exchange(m_pendingCell, PendingCell());
    TextView::mouseReleaseEvent(event);
    if (pending.active && event->button() == Qt::LeftButton && !isReadOnly()
        && (event->position().toPoint() - pending.pos).manhattanLength() <= QApplication::startDragDistance()) {
        // A click on a cell of a rendered table: its box, not the table's
        // source.
        if (openCellEditor(pending.line, pending.row, pending.col))
            return;
        // No box for this table: the click is a click on its text.
        const int index = rowOfLine(pending.line);
        if (index >= 0 && index < rowCount() && row(index).rendered) {
            QTextCursor cursor(document());
            cursor.setPosition(hitTestRenderedRow(row(index), pending.point));
            setTextCursor(cursor);
        }
    }
    realignLeftTable();
    // Editable: the blocks the selection touches open for editing now.
    if (wasSelecting && !isReadOnly())
        relayout();
}

void VisualView::paintLineBackground(QPainter &painter, int line, const QRectF &rect)
{
    // Source lines of the block being edited get the active-block shade and
    // a navy bar, as in the mockup.
    if (!m_highlightActiveBlock)
        return;
    const int index = rowOfLine(line);
    const int s = index < int(m_rows.size()) ? m_rows[index].span : -1;
    if (s < 0 || !m_expandedBlocks.contains(s))
        return;
    const qreal left = textLeft() - activeBarInset;
    const QRectF area(left, rect.top(), viewport()->width() - left - sideMargin + activeBarInset, rect.height());
    painter.fillRect(area, theme::color(theme::Role::activeBlock));
    painter.fillRect(QRectF(left, rect.top(), 2, rect.height()), theme::color(theme::Role::accent));
}

int VisualView::hitTestRenderedRow(const Row &r, const QPointF &point)
{
    QTextDocument *doc = document();
    const QTextBlock first = doc->findBlockByNumber(r.firstLine);
    const int s = spanForRow(r);
    const Render *render = rendered(r);
    if (s < 0 || !render)
        return first.position();
    const RenderedBlock *block = render->block.get();

    const MarkdownModel::Span &span = m_model->spans()[s];
    const auto &info = m_model->parsedBlock(span.block >= 0 ? span.block : span.previous);
    switch (info.kind) {
    case core::BlockKind::Html: {
        const int pos = block->hitTest(point);
        if (pos < 0)
            return first.position();
        return first.position() + sourceOffsetFor(block->renderedText().left(pos), render->source);
    }
    case core::BlockKind::Code:
    case core::BlockKind::FrontMatter: {
        const int pos = block->hitTest(point);
        if (block->kind() == RenderedBlock::Kind::Code && pos >= 0)
            return qMin(first.position() + pos, doc->characterCount() - 1);
        [[fallthrough]];
    }
    default: {
        // Images (math, diagrams): the lower half is the end of the block, so
        // a drag past the picture selects all of it; else the first content line.
        if (block->kind() == RenderedBlock::Kind::Image && point.y() > block->height() / 2) {
            const QTextBlock last = doc->findBlockByNumber(r.lastLine);
            return last.position() + last.length() - 1;
        }
        const QTextBlock content = r.lastLine > r.firstLine ? first.next() : first;
        return content.position();
    }
    }
}

bool VisualView::renderedRowPressed(const Row &r, const QPointF &point, QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return false;
    const RenderedBlock *block = renderedForRow(r);
    if (!block)
        return false;
    const QString anchor = block->anchorAt(point);
    if (anchor.isEmpty())
        return openPicture(block, point, event) || tableCellPressed(r, point, event);
    if (anchor.startsWith(QLatin1String("vn-task:"))) {
        toggleTask(anchor.section(u':', 1).toInt());
        return true;
    }
    Q_EMIT linkActivated(anchor);
    return true;
}

bool VisualView::zoomClick(const QMouseEvent *event) const
{
    // Read-only, a click is free to enlarge; editable, a click opens the
    // block's source, so it takes the link modifier (as for links in source).
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    return isReadOnly() ? mods == Qt::NoModifier : mods == linkModifier();
}

bool VisualView::openPicture(const RenderedBlock *block, const QPointF &point, const QMouseEvent *event)
{
    if (!zoomClick(event))
        return false;
    const auto picture = block->pictureAt(point);
    if (!picture)
        return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage image = block->pictureImage(*picture);
    QApplication::restoreOverrideCursor();
    if (image.isNull())
        return false;
    // Closes itself; the window is the parent so it stays on top of this tab.
    (new ImageViewer(image, picture->title, window()))->show();
    return true;
}

std::vector<TextView::LineAnchor> VisualView::rowLineAnchors(const Row &r, qreal)
{
    // Gap and source rows are one line each; rendered blocks know where
    // their table rows, list items and code lines are.
    std::vector<LineAnchor> anchors;
    if (!r.rendered || r.lastLine == r.firstLine)
        return anchors;
    // (Rough heights spread the lines evenly; see renderedRowHeight.)
    if (m_unpainted && !memoized(r))
        return anchors;
    if (const RenderedBlock *block = renderedForRow(r)) {
        for (const auto &[offset, y] : block->lineAnchors())
            anchors.push_back({qreal(r.firstLine + offset), y});
    }
    return anchors;
}

void VisualView::toggleTask(int line)
{
    if (isReadOnly())
        return;
    const QTextBlock block = document()->findBlockByNumber(line);
    const QString text = block.text();
    static const QRegularExpression box(QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s+\\[([ xX])\\]"));
    const QRegularExpressionMatch m = box.match(text);
    if (!m.hasMatch())
        return;
    QTextCursor c(block);
    c.setPosition(block.position() + m.capturedStart(1));
    c.setPosition(block.position() + m.capturedEnd(1), QTextCursor::KeepAnchor);
    c.insertText(m.captured(1) == QLatin1String(" ") ? QStringLiteral("x") : QStringLiteral(" "));
    // A click is one edit, not typing: show the new state now.
    m_model->flush();
}

void VisualView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_tableDrag.active) {
        if (event->buttons() & Qt::LeftButton) {
            updateTableDrag(event->position());
            return;
        }
        m_tableDrag = {}; // (The release went elsewhere.)
    }
    if (m_pendingCell.active && (event->buttons() & Qt::LeftButton)
        && (event->position().toPoint() - m_pendingCell.pos).manhattanLength() > QApplication::startDragDistance()) {
        // A drag from a cell: the cells from it to the pointer are selected.
        const PendingCell pending = std::exchange(m_pendingCell, PendingCell());
        const int index = rowOfLine(pending.line);
        if (index >= 0 && index < rowCount() && row(index).rendered) {
            const Row r = row(index);
            const Render *render = memoized(r);
            if (render && render->block && render->block->tableGrid()) {
                const TableCheck &check = checkTable(r, render);
                TableSpot spot;
                spot.line = r.firstLine;
                spot.at = check.at;
                spot.rows = check.info.rows;
                spot.cols = check.info.cols;
                if (check.ok && !m_cellEditor) {
                    selectTableCells(SelectionMode::Cells, spot, pending.row, pending.col, false);
                    m_tableDrag.active = true;
                    updateTableDrag(event->position());
                    return;
                }
            }
        }
    }
    if (event->buttons() == Qt::NoButton) {
        // The edges of a table: arrows to select a column or a row, and the
        // marks to insert one.
        const TableSpot spot = tableSpotAt(event->position());
        setTableHover(spot);
        if (spot.zone != TableZone::None && spot.zone != TableZone::Cell) {
            if (spot.zone == TableZone::Column || spot.zone == TableZone::Row) {
                const bool down = spot.zone == TableZone::Column;
                const qreal ratio = viewport()->devicePixelRatioF();
                const int key = int(ratio * 100) * 2 + (down ? 1 : 0);
                auto cursor = m_edgeCursors.find(key);
                if (cursor == m_edgeCursors.end())
                    cursor = m_edgeCursors.insert(key, edgeArrowCursor(down, ratio));
                viewport()->setCursor(*cursor);
            } else {
                viewport()->setCursor(Qt::PointingHandCursor);
            }
            QToolTip::hideText();
            return;
        }
        // Pointing hand and URL tooltip over links in rendered text.
        qreal top = 0;
        const int index = rowAt(event->position().y(), &top);
        QString anchor;
        bool zoomable = false;
        if (index >= 0 && row(index).rendered) {
            if (const RenderedBlock *block = renderedForRow(row(index))) {
                const QPointF point(event->position().x() - textLeft(), event->position().y() - top);
                anchor = block->anchorAt(point);
                zoomable = anchor.isEmpty() && zoomClick(event) && block->pictureAt(point);
            }
        }
        viewport()->setCursor(anchor.isEmpty() && !zoomable ? Qt::IBeamCursor : Qt::PointingHandCursor);
        if (!anchor.isEmpty() && !anchor.startsWith(QLatin1String("vn-task:")))
            QToolTip::showText(event->globalPosition().toPoint(), plainToolTip(anchor), viewport());
        else
            QToolTip::hideText();
    }
    TextView::mouseMoveEvent(event);
}

bool VisualView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave)
        clearTableHover();
    return TextView::viewportEvent(event);
}

// ---------------------------------------------------------------------------
// Selecting cells, rows and columns; quick insert

VisualView::TableSpot VisualView::tableSpotAt(const QPointF &p)
{
    if (isReadOnly() || !markdownEditing())
        return {};
    // The row under the pointer, then the one that starts just below it (the
    // band above a table is in the space before it).
    const auto inRow = [&](int index, qreal top) -> TableSpot {
        if (index < 0)
            return {};
        const Row r = row(index);
        if (!r.rendered)
            return {};
        const Render *render = memoized(r);
        if (!render || !render->block)
            return {};
        const RenderedBlock::TableGrid *grid = render->block->tableGrid();
        if (!grid)
            return {};
        const TableCheck &check = checkTable(r, render);
        if (!check.ok)
            return {};
        TableSpot spot;
        spot.line = r.firstLine;
        spot.at = check.at;
        spot.rows = grid->rows();
        spot.cols = grid->cols();
        spot.top = top;
        spot.grid = grid;
        const qreal lx = p.x() - textLeft();
        const qreal ly = p.y() - top;
        const qreal x0 = grid->xs.front();
        const qreal x1 = grid->xs.back();
        const qreal y0 = grid->ys.front();
        const qreal y1 = grid->ys.back();
        // The border nearest to a coordinate, no nearer the start than
        // `first`, and whether it is near enough (and its cells wide enough)
        // for the mark.
        const auto border = [](const std::vector<qreal> &edges, qreal v, int first, int *index) {
            const int count = int(edges.size());
            int k = int(std::lower_bound(edges.begin() + first, edges.end(), v) - edges.begin());
            if (k >= count || (k > first && v - edges[k - 1] < edges[k] - v))
                --k;
            *index = k;
            qreal room = insertReach;
            if (k > 0)
                room = qMin(room, (edges[k] - edges[k - 1]) / 3);
            if (k + 1 < count)
                room = qMin(room, (edges[k + 1] - edges[k]) / 3);
            return qAbs(v - edges[k]) <= room;
        };
        if (ly >= y0 - tableBandOut && ly < y0 + tableBandIn && lx >= x0 - insertReach && lx <= x1 + insertReach) {
            int k = 0;
            if (check.canInsert && border(grid->xs, lx, 0, &k)) {
                spot.zone = TableZone::InsertColumn;
                spot.index = k;
                return spot;
            }
            if (lx >= x0 && lx < x1) {
                spot.zone = TableZone::Column;
                spot.index = grid->colAt(lx);
                return spot;
            }
        }
        if (lx >= x0 - tableBandOut && lx < x0 + tableBandIn && ly >= y0 - insertReach && ly <= y1 + insertReach) {
            int k = 0;
            if (check.canInsert && border(grid->ys, ly, 1, &k)) {
                spot.zone = TableZone::InsertRow;
                spot.index = k;
                return spot;
            }
            if (ly >= y0 && ly < y1) {
                spot.zone = TableZone::Row;
                spot.index = grid->rowAt(ly);
                return spot;
            }
        }
        if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1) {
            spot.zone = TableZone::Cell;
            spot.row = grid->rowAt(ly);
            spot.col = grid->colAt(lx);
        }
        return spot;
    };
    qreal top = 0;
    const int index = rowAt(p.y(), &top);
    TableSpot spot = inRow(index, top);
    if (spot.zone != TableZone::None)
        return spot;
    qreal below = 0;
    const int next = rowAt(p.y() + tableBandOut, &below);
    if (next < 0 || next == index)
        return {};
    return inRow(next, below);
}

void VisualView::setTableHover(const TableSpot &spot)
{
    const bool insert = spot.zone == TableZone::InsertColumn || spot.zone == TableZone::InsertRow;
    if (!insert) {
        clearTableHover();
        return;
    }
    if (m_tableHover.zone == spot.zone && m_tableHover.line == spot.line && m_tableHover.index == spot.index)
        return;
    const QRect old = m_tableHover.area;
    const bool column = spot.zone == TableZone::InsertColumn;
    const RenderedBlock::TableGrid &g = *spot.grid;
    const qreal left = textLeft();
    TableHover hover;
    hover.zone = spot.zone;
    hover.line = spot.line;
    hover.index = spot.index;
    if (column) {
        const qreal x = left + g.xs[spot.index];
        hover.center = QPointF(x, spot.top + g.ys.front() - markOffset);
        hover.from = QPointF(x, spot.top + g.ys.front() - markOffset);
        hover.to = QPointF(x, spot.top + g.ys.back());
    } else {
        const qreal y = spot.top + g.ys[spot.index];
        hover.center = QPointF(left + g.xs.front() - markOffset, y);
        hover.from = QPointF(left + g.xs.front() - markOffset, y);
        hover.to = QPointF(left + g.xs.back(), y);
    }
    hover.area = QRectF(hover.from, hover.to).normalized().adjusted(-markRadius - 2, -markRadius - 2, markRadius + 2, markRadius + 2).toAlignedRect();
    m_tableHover = hover;
    if (!old.isNull())
        viewport()->update(old);
    viewport()->update(hover.area);
}

void VisualView::clearTableHover()
{
    if (m_tableHover.zone == TableZone::None)
        return;
    const QRect area = m_tableHover.area;
    m_tableHover = {};
    viewport()->update(area);
}

void VisualView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !isReadOnly() && markdownEditing()) {
        TableSpot spot = tableSpotAt(event->position());
        const bool shift = (event->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier;
        const bool onBand = spot.zone != TableZone::None && spot.zone != TableZone::Cell;
        if (onBand && m_cellEditor) {
            // What was typed goes in first, and may change the table's widths.
            closeCellEditor(TableCellEditor::Exit::Commit);
            spot = tableSpotAt(event->position());
        }
        if (spot.zone != TableZone::None && spot.zone != TableZone::Cell) {
            setFocus(Qt::MouseFocusReason);
            event->accept();
            tablePressed(spot, event);
            return;
        }
        if (m_tableSel.active) {
            if (shift && spot.zone == TableZone::Cell && spot.line == m_tableSel.line && tableSelectionValid()) {
                // Shift+click on a cell grows the selection to it.
                setFocus(Qt::MouseFocusReason);
                event->accept();
                selectTableCells(m_tableSel.mode, spot, spot.row, spot.col, true);
                return;
            }
            clearTableSelection();
        }
    }
    TextView::mousePressEvent(event);
}

void VisualView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // A quick second click on an edge is another click on it, not a word.
    if (event->button() == Qt::LeftButton && !isReadOnly() && markdownEditing()) {
        const TableSpot spot = tableSpotAt(event->position());
        if (spot.zone != TableZone::None && spot.zone != TableZone::Cell) {
            mousePressEvent(event);
            return;
        }
    }
    TextView::mouseDoubleClickEvent(event);
}

void VisualView::tablePressed(const TableSpot &spot, QMouseEvent *event)
{
    const bool extend = (event->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier;
    switch (spot.zone) {
    case TableZone::InsertColumn:
    case TableZone::InsertRow:
        clearTableSelection();
        insertAtBorder(spot);
        break;
    case TableZone::Column:
        selectTableCells(SelectionMode::Columns, spot, 0, spot.index, extend);
        m_tableDrag.active = true;
        break;
    case TableZone::Row:
        selectTableCells(SelectionMode::Rows, spot, spot.index, 0, extend);
        m_tableDrag.active = true;
        break;
    default:
        break;
    }
}

void VisualView::insertAtBorder(const TableSpot &spot)
{
    clearTableHover();
    const int line = spot.line + spot.at;
    using Op = md::TableOp;
    bool done = false;
    if (spot.zone == TableZone::InsertRow)
        done = editTableAt(line, Op::RowBelow, spot.index - 1, 0, {}, false);
    else if (spot.index < spot.cols)
        done = editTableAt(line, Op::ColumnLeft, 0, spot.index, {}, false);
    else
        done = editTableAt(line, Op::ColumnRight, 0, spot.cols - 1, {}, false);
    if (done)
        m_model->flush(); // Show it now.
    scheduleToolbarUpdate();
}

void VisualView::selectTableCells(SelectionMode mode, const TableSpot &spot, int row, int col, bool extend)
{
    TableSelection s;
    if (extend && m_tableSel.active && m_tableSel.line == spot.line && m_tableSel.mode == mode) {
        s = m_tableSel;
    } else {
        s.active = true;
        s.mode = mode;
        s.line = spot.line;
        s.at = spot.at;
        s.rows = spot.rows;
        s.cols = spot.cols;
        s.anchorRow = row;
        s.anchorCol = col;
    }
    s.row = row;
    s.col = col;
    s.revision = document()->revision();
    setTableSelection(s);
}

void VisualView::updateTableDrag(const QPointF &p)
{
    if (!m_tableSel.active)
        return;
    const int index = rowOfLine(m_tableSel.line);
    if (index < 0 || index >= rowCount())
        return;
    const Row r = row(index);
    qreal top = 0;
    qreal bottom = 0;
    const Render *render = r.rendered ? memoized(r) : nullptr;
    const RenderedBlock::TableGrid *grid = render && render->block ? render->block->tableGrid() : nullptr;
    if (!grid || !lineBounds(m_tableSel.line, &top, &bottom))
        return;
    // (The table's edges clamp: a pointer past the table selects up to its end.)
    TableSelection s = m_tableSel;
    const int col = grid->colAt(p.x() - textLeft());
    const int row = grid->rowAt(p.y() - top);
    if (s.mode != SelectionMode::Rows)
        s.col = col;
    if (s.mode != SelectionMode::Columns)
        s.row = row;
    if (s == m_tableSel)
        return;
    setTableSelection(s);
}

bool VisualView::tableSelectionValid()
{
    if (!m_tableSel.active)
        return false;
    if (m_tableSel.revision != document()->revision()) {
        clearTableSelection();
        return false;
    }
    return true;
}

bool VisualView::selectionViewRect(QRectF *rect)
{
    const TableSelection &s = m_tableSel;
    if (!s.active)
        return false;
    const int index = rowOfLine(s.line);
    if (index < 0 || index >= rowCount())
        return false;
    const Row r = row(index);
    if (!r.rendered || r.firstLine != s.line)
        return false;
    const Render *render = memoized(r);
    const RenderedBlock::TableGrid *grid = render && render->block ? render->block->tableGrid() : nullptr;
    qreal top = 0;
    qreal bottom = 0;
    if (!grid || grid->rows() != s.rows || grid->cols() != s.cols || !lineBounds(s.line, &top, &bottom))
        return false;
    const qreal left = textLeft();
    *rect = QRectF(QPointF(left + grid->xs[s.left()], top + grid->ys[s.top()]),
                   QPointF(left + grid->xs[s.right() + 1], top + grid->ys[s.bottom() + 1]));
    return true;
}

void VisualView::setTableSelection(const TableSelection &selection)
{
    QRectF before;
    QRectF after;
    const bool hadBefore = selectionViewRect(&before);
    m_tableSel = selection;
    const bool hasAfter = selectionViewRect(&after);
    // Only what changes is painted again.
    if (hadBefore && hasAfter)
        viewport()->update(before.united(after).adjusted(-3, -3, 3, 3).toAlignedRect());
    else
        viewport()->update();
    // The table commands (the toolbar, the Table menu) act on the first cell.
    setTableTarget(selection.line + selection.at, selection.top(), selection.left());
    scheduleToolbarUpdate();
}

void VisualView::clearTableSelection()
{
    if (!m_tableSel.active)
        return;
    QRectF rect;
    const bool shown = selectionViewRect(&rect);
    m_tableSel = {};
    m_tableDrag = {};
    if (!m_cellEditor)
        setTableTarget(-1);
    if (shown)
        viewport()->update(rect.adjusted(-3, -3, 3, 3).toAlignedRect());
    else
        viewport()->update();
    scheduleToolbarUpdate();
}

md::TableInfo VisualView::selectionInfo()
{
    md::TableInfo info = tableAtCaret();
    if (!m_tableSel.active || !info.found)
        return info;
    const TableSelection &s = m_tableSel;
    const int line = s.line + s.at;
    using Op = md::TableOp;
    // Whether a command is accepted at a cell of the table.
    const auto allowed = [&](Op op, int row, int col) {
        const md::TableCell cell = tableCellOf(line, row, col);
        return cell.found && md::tableOpAvailable(tableInfoAt(cell.line, cell.start), op);
    };
    const bool allColumns = s.right() - s.left() + 1 >= s.cols;
    md::setTableOpAvailable(info, Op::RowAbove, allowed(Op::RowAbove, s.top(), s.left()));
    md::setTableOpAvailable(info, Op::RowBelow, allowed(Op::RowBelow, s.bottom(), s.left()));
    md::setTableOpAvailable(info, Op::ColumnLeft, allowed(Op::ColumnLeft, s.top(), s.left()));
    md::setTableOpAvailable(info, Op::ColumnRight, allowed(Op::ColumnRight, s.top(), s.right()));
    // (Deleting rows is about the last one: the header cannot be.)
    md::setTableOpAvailable(info, Op::DeleteRow, allowed(Op::DeleteRow, s.bottom(), s.left()));
    md::setTableOpAvailable(info, Op::DeleteColumn, !allColumns && allowed(Op::DeleteColumn, s.top(), s.left()));
    md::setTableOpAvailable(info, Op::SetCell, allowed(Op::SetCell, s.top(), s.left()));
    return info;
}

bool VisualView::selectionOperation(md::TableOp op)
{
    if (!tableSelectionValid())
        return false;
    const TableSelection s = m_tableSel;
    const int line = s.line + s.at;
    const int rows = s.bottom() - s.top() + 1;
    const int cols = s.right() - s.left() + 1;
    using Op = md::TableOp;
    bool done = false;
    switch (op) {
    case Op::RowAbove:
    case Op::ColumnLeft:
    case Op::DeleteRow:
    case Op::DeleteColumn:
    case Op::AlignNone:
    case Op::AlignLeft:
    case Op::AlignCenter:
    case Op::AlignRight:
    case Op::SetCell:
        done = editTableAt(line, op, s.top(), s.left(), {}, false, rows, cols);
        break;
    case Op::RowBelow:
        done = editTableAt(line, op, s.bottom(), s.left(), {}, false, rows, cols);
        break;
    case Op::ColumnRight:
        done = editTableAt(line, op, s.top(), s.right(), {}, false, rows, cols);
        break;
    default:
        done = editTable(op); // On the first cell.
        break;
    }
    if (done)
        m_model->flush(); // Show the new table now.
    clearTableSelection();
    scheduleToolbarUpdate();
    return done;
}

void VisualView::populateSelectionMenu(QMenu *menu)
{
    if (!m_tableSel.active)
        return;
    using Op = md::TableOp;
    const md::TableInfo info = selectionInfo();
    const int rows = m_tableSel.bottom() - m_tableSel.top() + 1;
    const int cols = m_tableSel.right() - m_tableSel.left() + 1;
    const auto add = [&](QMenu *to, const QString &title, Op op) {
        QAction *action = to->addAction(title, this, [this, op] { selectionOperation(op); });
        action->setEnabled(md::tableOpAvailable(info, op));
        return action;
    };
    add(menu, rows == 1 ? tr("Insert Row Above") : tr("Insert %1 Rows Above").arg(rows), Op::RowAbove);
    add(menu, rows == 1 ? tr("Insert Row Below") : tr("Insert %1 Rows Below").arg(rows), Op::RowBelow);
    add(menu, rows == 1 ? tr("Delete Row") : tr("Delete %1 Rows").arg(rows), Op::DeleteRow);
    menu->addSeparator();
    add(menu, cols == 1 ? tr("Insert Column Left") : tr("Insert %1 Columns Left").arg(cols), Op::ColumnLeft);
    add(menu, cols == 1 ? tr("Insert Column Right") : tr("Insert %1 Columns Right").arg(cols), Op::ColumnRight);
    add(menu, cols == 1 ? tr("Delete Column") : tr("Delete %1 Columns").arg(cols), Op::DeleteColumn);
    menu->addSeparator();
    QMenu *align = menu->addMenu(cols == 1 ? tr("Align Column") : tr("Align Columns"));
    align->setEnabled(md::tableOpAvailable(info, Op::AlignLeft));
    const struct
    {
        const char *title;
        Op op;
        md::TableAlignment alignment;
    } alignments[] = {{QT_TR_NOOP("Left"), Op::AlignLeft, md::TableAlignment::Left},
                      {QT_TR_NOOP("Center"), Op::AlignCenter, md::TableAlignment::Center},
                      {QT_TR_NOOP("Right"), Op::AlignRight, md::TableAlignment::Right},
                      {QT_TR_NOOP("None"), Op::AlignNone, md::TableAlignment::None}};
    for (const auto &a : alignments) {
        QAction *action = add(align, tr(a.title), a.op);
        action->setCheckable(true);
        action->setChecked(info.alignment == a.alignment);
    }
    menu->addSeparator();
    add(menu, tr("Clear Cells"), Op::SetCell);
    menu->addSeparator();
    populateTableMenu(menu->addMenu(tr("Table")));
}

void VisualView::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_tableSel.active && tableSelectionValid()) {
        QRectF rect;
        if (selectionViewRect(&rect) && rect.contains(event->pos())) {
            QMenu menu(this);
            populateSelectionMenu(&menu);
            menu.exec(event->globalPos());
            return;
        }
        clearTableSelection();
    }
    TextView::contextMenuEvent(event);
}

void VisualView::keyPressEvent(QKeyEvent *event)
{
    if (m_tableSel.active) {
        const auto mods = event->modifiers() & ~Qt::KeypadModifier;
        switch (event->key()) {
        case Qt::Key_Escape:
            clearTableSelection();
            event->accept();
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (mods == Qt::NoModifier) {
                selectionOperation(md::TableOp::SetCell);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_CapsLock:
            break;
        default:
            // Typing or moving the caret leaves the selection (a shortcut,
            // Ctrl+S, does not).
            if (!(mods & Qt::ControlModifier))
                clearTableSelection();
            break;
        }
    }
    TextView::keyPressEvent(event);
}

QRect VisualView::tableCellViewRect(int line, int row, int col)
{
    const int index = rowOfLine(line);
    if (index < 0 || index >= rowCount())
        return {};
    const Row r = this->row(index);
    const Render *render = r.rendered && r.firstLine == line ? memoized(r) : nullptr;
    qreal top = 0;
    qreal bottom = 0;
    if (!render || !render->block || !lineBounds(line, &top, &bottom))
        return {};
    const auto rect = render->block->tableCellRect(row, col);
    if (!rect)
        return {};
    return rect->translated(textLeft(), top).toAlignedRect();
}

void VisualView::paintTableOverlay(QPaintEvent *event)
{
    QRectF selected;
    const bool shade = m_tableSel.active && m_tableSel.revision == document()->revision() && selectionViewRect(&selected)
                       && selected.intersects(event->rect());
    const bool mark = m_tableHover.zone != TableZone::None && m_tableHover.area.intersects(event->rect());
    if (!shade && !mark)
        return;
    QPainter painter(viewport());
    painter.setClipRegion(event->region());
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor accent = theme::color(theme::Role::accent);
    if (shade) {
        QColor tint = theme::color(theme::Role::selection);
        tint.setAlpha(110);
        painter.fillRect(selected, tint);
        painter.setPen(QPen(accent, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(selected.adjusted(0.75, 0.75, -0.75, -0.75));
    }
    if (mark) {
        painter.setPen(QPen(accent, 2));
        painter.drawLine(m_tableHover.from, m_tableHover.to);
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        painter.drawEllipse(m_tableHover.center, markRadius, markRadius);
        painter.setPen(QPen(theme::color(theme::Role::editor), 1.8, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = m_tableHover.center;
        painter.drawLine(c + QPointF(-3, 0), c + QPointF(3, 0));
        painter.drawLine(c + QPointF(0, -3), c + QPointF(0, 3));
    }
}

void VisualView::focusInEvent(QFocusEvent *event)
{
    TextView::focusInEvent(event);
    m_hasFocus = true;
    scheduleToolbarUpdate();
    rebuildRows();
    viewport()->update();
}

void VisualView::focusOutEvent(QFocusEvent *event)
{
    TextView::focusOutEvent(event);
    // Context menus and the find bar take focus briefly; keep the block open
    // for those, close it when focus really moves elsewhere.
    if (event->reason() == Qt::PopupFocusReason)
        return;
    m_hasFocus = false;
    scheduleToolbarUpdate();
    rebuildRows();
    viewport()->update();
}

void VisualView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange)
        clearRenderCache();
    TextView::changeEvent(event);
}

} // namespace vn
