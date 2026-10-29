#include "FindBar.h"

#include "TextView.h"
#include "../Theme.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace vn {

namespace {

// Shared by every find bar, newest first, like VS Code's find history.
QStringList &history(bool replace)
{
    static QStringList find;
    static QStringList replacement;
    return replace ? replacement : find;
}

constexpr int historyLimit = 50;

} // namespace

FindBar::FindBar(TextView *view, QWidget *parent)
    : QFrame(parent)
    , m_view(view)
    , m_find(new QLineEdit(this))
    , m_replace(new QLineEdit(this))
{
    setObjectName(QStringLiteral("FindBar"));
    setAttribute(Qt::WA_StyledBackground);

    m_toggleReplace = new QToolButton(this);
    m_toggleReplace->setObjectName(QStringLiteral("FindToggle"));
    m_toggleReplace->setCheckable(true);
    m_toggleReplace->setToolTip(tr("Toggle Replace"));
    m_toggleReplace->setFixedWidth(16);
    m_toggleReplace->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    connect(m_toggleReplace, &QToolButton::toggled, this, &FindBar::setReplaceVisible);

    m_caseOption = makeOption(QStringLiteral("Aa"), tr("Match Case (Alt+C)"));
    m_wordOption = makeOption(QStringLiteral("ab"), tr("Match Whole Word (Alt+W)"));
    m_regexOption = makeOption(QStringLiteral(".*"), tr("Use Regular Expression (Alt+R)"));
    m_preserveCaseOption = makeOption(QStringLiteral("AB"), tr("Preserve Case (Alt+P)"));
    QFont underlined = m_wordOption->font();
    underlined.setUnderline(true);
    m_wordOption->setFont(underlined);
    for (QToolButton *option : {m_caseOption, m_wordOption, m_regexOption})
        connect(option, &QToolButton::toggled, this, &FindBar::research);

    m_find->setPlaceholderText(tr("Find"));
    m_replace->setPlaceholderText(tr("Replace"));
    m_findField = makeField(m_find, {m_caseOption, m_wordOption, m_regexOption});
    QWidget *replaceField = makeField(m_replace, {m_preserveCaseOption});

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("FindCount"));
    m_count->setMinimumWidth(68);

    QToolButton *previous = makeIconButton(QStringLiteral("arrow-up"), tr("Previous Match (Shift+Enter)"));
    QToolButton *next = makeIconButton(QStringLiteral("arrow-down"), tr("Next Match (Enter)"));
    m_inSelection = makeIconButton(QStringLiteral("selection"), tr("Find in Selection"));
    m_inSelection->setCheckable(true);
    QToolButton *closeButton = makeIconButton(QStringLiteral("close-hover"), tr("Close (Escape)"));
    connect(previous, &QToolButton::clicked, this, &FindBar::findPrevious);
    connect(next, &QToolButton::clicked, this, &FindBar::findNext);
    connect(closeButton, &QToolButton::clicked, this, &FindBar::close);
    connect(m_inSelection, &QToolButton::toggled, this, [this](bool on) {
        m_scope = QTextCursor();
        if (on) {
            m_scope = m_view->textCursor();
            if (!m_scope.hasSelection()) {
                // Nothing selected: search the caret's line.
                m_scope.movePosition(QTextCursor::StartOfBlock);
                m_scope.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            }
        }
        research();
    });

    auto *replaceOneButton = new QPushButton(tr("Replace"), this);
    auto *replaceAllButton = new QPushButton(tr("Replace All"), this);
    for (QPushButton *button : {replaceOneButton, replaceAllButton}) {
        button->setObjectName(QStringLiteral("FindButton"));
        button->setFocusPolicy(Qt::TabFocus);
    }
    replaceOneButton->setToolTip(tr("Replace (Enter)"));
    replaceAllButton->setToolTip(tr("Replace All (Ctrl+Alt+Enter)"));
    connect(replaceOneButton, &QPushButton::clicked, this, &FindBar::replaceOne);
    connect(replaceAllButton, &QPushButton::clicked, this, &FindBar::replaceAll);

    auto *findRow = new QHBoxLayout;
    findRow->setSpacing(4);
    findRow->addWidget(m_findField);
    findRow->addWidget(m_count);
    findRow->addWidget(previous);
    findRow->addWidget(next);
    findRow->addWidget(m_inSelection);
    findRow->addWidget(closeButton);

    m_replaceRow = new QWidget(this);
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(4);
    replaceRow->addWidget(replaceField);
    replaceRow->addWidget(replaceOneButton);
    replaceRow->addWidget(replaceAllButton);
    replaceRow->addStretch(1);

    auto *rows = new QVBoxLayout;
    rows->setSpacing(4);
    rows->addLayout(findRow);
    rows->addWidget(m_replaceRow);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 5, 6, 5);
    layout->setSpacing(4);
    layout->addWidget(m_toggleReplace);
    layout->addLayout(rows);

    for (QLineEdit *edit : {m_find, m_replace})
        edit->installEventFilter(this);
    connect(m_find, &QLineEdit::textEdited, this, [this] {
        m_historyIndex = -1;
        research();
        if (!m_matches.isEmpty())
            selectMatch(m_current);
    });

    m_researchTimer.setSingleShot(true);
    m_researchTimer.setInterval(100);
    connect(&m_researchTimer, &QTimer::timeout, this, &FindBar::research);
    connect(m_view->document(), &QTextDocument::contentsChanged, this, [this] {
        if (isVisible())
            m_researchTimer.start();
    });

    setReplaceVisible(false);
    hide();
}

void FindBar::setView(TextView *view)
{
    if (m_view == view)
        return;
    m_view->setSearchHighlights({}, -1);
    m_view = view;
    if (isVisible())
        research();
}

QWidget *FindBar::makeField(QLineEdit *edit, const QList<QToolButton *> &options)
{
    auto *field = new QWidget(this);
    field->setObjectName(QStringLiteral("FindInput"));
    field->setAttribute(Qt::WA_StyledBackground);
    field->setFixedWidth(230);
    auto *layout = new QHBoxLayout(field);
    layout->setContentsMargins(4, 1, 3, 1);
    layout->setSpacing(1);
    edit->setFrame(false);
    layout->addWidget(edit, 1);
    for (QToolButton *option : options)
        layout->addWidget(option);
    return field;
}

QToolButton *FindBar::makeOption(const QString &text, const QString &tip)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("FindOption"));
    button->setText(text);
    QFont font = theme::editorFont();
    font.setPixelSize(11);
    button->setFont(font);
    button->setFixedSize(QFontMetrics(font).horizontalAdvance(text) + 8, 18);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

QToolButton *FindBar::makeIconButton(const QString &icon, const QString &tip)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("FindAction"));
    button->setIcon(QIcon(QStringLiteral(":/vn/icons/%1.svg").arg(icon)));
    button->setIconSize({16, 16});
    button->setFixedSize(22, 22);
    button->setToolTip(tip);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

void FindBar::setReplaceVisible(bool on)
{
    m_replaceRow->setVisible(on);
    m_toggleReplace->setIcon(QIcon(on ? QStringLiteral(":/vn/icons/chevron-down-dark.svg")
                                      : QStringLiteral(":/vn/icons/chevron-right-dark.svg")));
    adjustSize();
}

search::Options FindBar::options() const
{
    return {m_find->text(), m_caseOption->isChecked(), m_wordOption->isChecked(), m_regexOption->isChecked()};
}

QString FindBar::query() const
{
    return m_find->text();
}

void FindBar::setQuery(const QString &text)
{
    m_find->setText(text);
    research();
}

void FindBar::open(bool replace)
{
    const QTextCursor cursor = m_view->textCursor();
    const QString selected = cursor.selectedText();
    if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator))
        m_find->setText(selected);
    m_searchStart = cursor.selectionStart();
    m_toggleReplace->setChecked(replace);
    show();
    raise();
    research();
    QLineEdit *field = replace && !m_find->text().isEmpty() ? m_replace : m_find;
    field->setFocus();
    field->selectAll();
}

void FindBar::close()
{
    remember(m_find);
    hide();
    m_matches.clear();
    m_view->setSearchHighlights({}, -1);
    m_view->setFocus();
    Q_EMIT closed();
}

void FindBar::research()
{
    const search::Options opts = options();
    const bool valid = search::isValid(opts);
    m_findField->setProperty("error", !valid);
    m_findField->style()->unpolish(m_findField);
    m_findField->style()->polish(m_findField);

    const int scopeStart = m_scope.isNull() ? -1 : m_scope.selectionStart();
    const int scopeEnd = m_scope.isNull() ? -1 : m_scope.selectionEnd();
    m_matches = search::findAll(m_view->document(), opts, scopeStart, scopeEnd);

    // The current match is the first one at or after the search start.
    m_current = m_matches.isEmpty() ? -1 : 0;
    const QTextCursor caret = m_view->textCursor();
    const int start = caret.hasSelection() ? caret.selectionStart() : m_searchStart;
    for (int i = 0; i < m_matches.size(); ++i) {
        if (m_matches[i].selectionStart() >= start) {
            m_current = i;
            break;
        }
    }
    m_view->setSearchHighlights(m_matches, m_current);
    updateCount();
}

void FindBar::updateCount()
{
    QString text;
    if (m_find->text().isEmpty())
        text.clear();
    else if (m_matches.isEmpty())
        text = tr("No results");
    else if (m_matches.size() >= search::maxMatches)
        text = tr("%1 of %2+").arg(m_current + 1).arg(search::maxMatches);
    else
        text = tr("%1 of %2").arg(m_current + 1).arg(m_matches.size());
    m_count->setText(text);
    m_count->setProperty("empty", !m_find->text().isEmpty() && m_matches.isEmpty());
    m_count->style()->unpolish(m_count);
    m_count->style()->polish(m_count);
}

void FindBar::selectMatch(int index)
{
    if (index < 0 || index >= m_matches.size())
        return;
    m_current = index;
    m_view->setTextCursor(m_matches[index]);
    m_view->setSearchHighlights(m_matches, m_current);
    m_searchStart = m_matches[index].selectionStart();
    updateCount();
}

void FindBar::findNext()
{
    if (!isVisible())
        open(false);
    remember(m_find);
    if (m_matches.isEmpty())
        return;
    const QTextCursor caret = m_view->textCursor();
    // Next match after the caret; wraps to the first.
    int next = 0;
    for (int i = 0; i < m_matches.size(); ++i) {
        if (m_matches[i].selectionStart() > caret.selectionStart()
            || (m_matches[i].selectionStart() == caret.selectionStart() && !caret.hasSelection())) {
            next = i;
            break;
        }
        next = 0;
    }
    selectMatch(next);
}

void FindBar::findPrevious()
{
    if (!isVisible())
        open(false);
    remember(m_find);
    if (m_matches.isEmpty())
        return;
    const QTextCursor caret = m_view->textCursor();
    int previous = int(m_matches.size()) - 1;
    for (int i = int(m_matches.size()) - 1; i >= 0; --i) {
        if (m_matches[i].selectionStart() < caret.selectionStart()) {
            previous = i;
            break;
        }
    }
    selectMatch(previous);
}

void FindBar::replaceOne()
{
    if (m_view->isReadOnly() || m_matches.isEmpty())
        return;
    remember(m_replace);
    const QTextCursor caret = m_view->textCursor();
    // Replace the selected match if the selection is one, then move on.
    for (const QTextCursor &match : std::as_const(m_matches)) {
        if (match.selectionStart() == caret.selectionStart() && match.selectionEnd() == caret.selectionEnd()) {
            QTextCursor edit = match;
            edit.insertText(
                search::replacementFor(options(), match, m_replace->text(), m_preserveCaseOption->isChecked()));
            m_view->setTextCursor(edit);
            research();
            break;
        }
    }
    findNext();
}

void FindBar::replaceAll()
{
    if (m_view->isReadOnly() || m_matches.isEmpty())
        return;
    remember(m_replace);
    const search::Options opts = options();
    // Compute every replacement first: regex groups need the original text.
    QStringList replacements;
    for (const QTextCursor &match : std::as_const(m_matches))
        replacements.append(search::replacementFor(opts, match, m_replace->text(), m_preserveCaseOption->isChecked()));
    QTextCursor block(m_view->document());
    block.beginEditBlock();
    for (int i = int(m_matches.size()) - 1; i >= 0; --i) {
        QTextCursor edit = m_matches[i];
        edit.insertText(replacements[i]);
    }
    block.endEditBlock();
    research();
}

void FindBar::remember(QLineEdit *edit)
{
    const QString text = edit->text();
    if (text.isEmpty())
        return;
    QStringList &list = history(edit == m_replace);
    list.removeAll(text);
    list.prepend(text);
    while (list.size() > historyLimit)
        list.removeLast();
    m_historyIndex = -1;
}

void FindBar::recall(QLineEdit *edit, int direction)
{
    const QStringList &list = history(edit == m_replace);
    if (list.isEmpty())
        return;
    m_historyIndex = qBound(-1, m_historyIndex + direction, int(list.size()) - 1);
    edit->setText(m_historyIndex < 0 ? QString() : list.at(m_historyIndex));
    edit->selectAll();
    if (edit == m_find)
        research();
}

bool FindBar::eventFilter(QObject *watched, QEvent *event)
{
    auto *edit = qobject_cast<QLineEdit *>(watched);
    if (!edit)
        return QFrame::eventFilter(watched, event);

    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
        QWidget *field = edit->parentWidget();
        field->setProperty("focused", event->type() == QEvent::FocusIn);
        field->style()->unpolish(field);
        field->style()->polish(field);
        return false;
    }
    if (event->type() != QEvent::KeyPress)
        return false;

    auto *key = static_cast<QKeyEvent *>(event);
    const auto mods = key->modifiers() & ~Qt::KeypadModifier;
    if (mods == Qt::AltModifier) {
        switch (key->key()) {
        case Qt::Key_C:
            m_caseOption->toggle();
            return true;
        case Qt::Key_W:
            m_wordOption->toggle();
            return true;
        case Qt::Key_R:
            m_regexOption->toggle();
            return true;
        case Qt::Key_P:
            m_preserveCaseOption->toggle();
            return true;
        default:
            break;
        }
    }
    switch (key->key()) {
    case Qt::Key_Escape:
        close();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (edit == m_replace) {
            if (mods == (Qt::ControlModifier | Qt::AltModifier))
                replaceAll();
            else
                replaceOne();
        } else if (mods & Qt::ShiftModifier) {
            findPrevious();
        } else {
            findNext();
        }
        return true;
    case Qt::Key_Up:
        recall(edit, 1);
        return true;
    case Qt::Key_Down:
        recall(edit, -1);
        return true;
    default:
        return false;
    }
}

} // namespace vn
