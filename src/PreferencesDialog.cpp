#include "PreferencesDialog.h"

#include "Commands.h"
#include "CustomizeColorsDialog.h"
#include "Encodings.h"
#include "Settings.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QCloseEvent>
#include <QComboBox>
#include <QFontDialog>
#include <QFontInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

namespace vn {

struct PreferencesDialog::ComboItem
{
    QString text;
    QVariant value;
};

namespace {

QPointer<PreferencesDialog> s_instance;

constexpr int rowIndent = 28;

void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

// Toggle switch. Space (or a click) flips it.
class Switch : public QAbstractButton
{
public:
    explicit Switch(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(40, 22);
    }

    QSize sizeHint() const override { return {40, 22}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? theme::color(theme::Role::switchOn) : theme::color(theme::Role::divider));
        p.drawRoundedRect(QRectF(0, 0, 40, 22), 11, 11);

        const qreal x = isChecked() ? 20 : 2;
        p.setBrush(QColor(0, 0, 0, 50));
        p.drawEllipse(QRectF(x, 3, 18, 18));
        p.setBrush(Qt::white);
        p.drawEllipse(QRectF(x, 2, 18, 18));

        if (hasFocus()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(theme::color(theme::Role::accent), 1));
            p.drawRoundedRect(QRectF(0.5, 0.5, 39, 21), 10.5, 10.5);
        }
    }
};

// A read-only input that acts like a button.
class ClickableLineEdit : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<void()> onClicked;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        QLineEdit::mousePressEvent(event);
        if (onClicked)
            onClicked();
    }
};

} // namespace

// Shows a shortcut and records a new one when clicked: the next key
// combination wins, and Ctrl+K waits for a second step.
class ShortcutCell : public QPushButton
{
public:
    explicit ShortcutCell(QWidget *parent)
        : QPushButton(parent)
    {
        setProperty("shortcutCell", true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        connect(this, &QAbstractButton::clicked, this, [this] { startCapture(); });
    }

    // An empty list is sent when the user clears the shortcut.
    std::function<void(const QList<QKeySequence> &)> onAssign;

    void setShortcut(const QString &text, bool modified)
    {
        m_idleText = text.isEmpty() ? QStringLiteral("—") : text;
        setProperty("empty", text.isEmpty());
        setProperty("modified", modified);
        if (!m_capturing)
            setText(m_idleText);
        repolish(this);
    }

    bool capturing() const { return m_capturing; }

protected:
    bool event(QEvent *e) override
    {
        if (m_capturing) {
            if (e->type() == QEvent::ShortcutOverride) {
                e->accept();
                return true;
            }
            if (e->type() == QEvent::KeyPress) {
                handleKey(static_cast<QKeyEvent *>(e));
                return true;
            }
        }
        return QPushButton::event(e);
    }

    void focusOutEvent(QFocusEvent *e) override
    {
        stopCapture();
        QPushButton::focusOutEvent(e);
    }

private:
    static bool isModifierKey(int key)
    {
        switch (key) {
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_Super_L:
        case Qt::Key_Super_R:
        case Qt::Key_Hyper_L:
        case Qt::Key_Hyper_R:
        case Qt::Key_CapsLock:
        case Qt::Key_NumLock:
        case Qt::Key_ScrollLock:
        case Qt::Key_unknown:
            return true;
        default:
            return false;
        }
    }

    void startCapture()
    {
        m_capturing = true;
        m_first.reset();
        setText(tr("Press keys…"));
        setProperty("capturing", true);
        repolish(this);
        setFocus();
    }

    void stopCapture()
    {
        if (!m_capturing)
            return;
        m_capturing = false;
        m_first.reset();
        setText(m_idleText);
        setProperty("capturing", false);
        repolish(this);
    }

    void commit(const QList<QKeySequence> &keys)
    {
        stopCapture();
        if (onAssign)
            onAssign(keys);
    }

    void handleKey(QKeyEvent *e)
    {
        const int key = e->key();
        if (isModifierKey(key))
            return;
        const Qt::KeyboardModifiers mods =
            e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        const QKeyCombination combo(mods, Qt::Key(key));

        if (m_first) {
            commit({QKeySequence(*m_first, combo)});
            return;
        }
        if (mods == Qt::NoModifier) {
            if (key == Qt::Key_Escape) {
                stopCapture();
                return;
            }
            if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
                commit({});
                return;
            }
        }
        if (combo == QKeyCombination(Qt::ControlModifier, Qt::Key_K)) {
            m_first = combo;
            setText(QKeySequence(combo).toString(QKeySequence::NativeText) + QStringLiteral(", …"));
            return;
        }
        commit({QKeySequence(combo)});
    }

    bool m_capturing = false;
    std::optional<QKeyCombination> m_first;
    QString m_idleText;
};

namespace {

QToolButton *smallButton(const QString &text, const QString &name, QWidget *parent = nullptr)
{
    auto *button = new QToolButton(parent);
    button->setObjectName(name);
    button->setText(text);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(26, 26);
    button->setProperty("prefSmall", true);
    return button;
}

QPushButton *textButton(const QString &text, const QString &name, QWidget *parent = nullptr)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::StrongFocus);
    return button;
}

QStringList registryCategories()
{
    QStringList categories;
    for (const Command &c : commands()) {
        if (!categories.contains(c.category))
            categories.append(c.category);
    }
    return categories;
}

} // namespace

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent, Qt::Window)
{
    setObjectName(QStringLiteral("PreferencesDialog"));
    setWindowTitle(tr("Visual Notepad Preferences"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(640, 470);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(10);

    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("PrefFrame"));
    auto *frameLayout = new QHBoxLayout(frame);
    frameLayout->setContentsMargins(1, 1, 1, 1);
    frameLayout->setSpacing(0);

    m_categories = new QListWidget(frame);
    m_categories->setObjectName(QStringLiteral("PrefCategories"));
    m_categories->setFixedWidth(110);
    m_categories->setFrameShape(QFrame::NoFrame);
    m_categories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stack = new QStackedWidget(frame);
    frameLayout->addWidget(m_categories);
    frameLayout->addWidget(m_stack, 1);
    outer->addWidget(frame, 1);

    buildEditorPage();
    buildMarkdownPage();
    buildSavePage();
    buildViewPage();
    buildShortcutsPage();

    auto *buttons = new QHBoxLayout;
    auto *help = textButton(tr("Help"), QStringLiteral("help"), this);
    auto *close = textButton(tr("Close"), QStringLiteral("close"), this);
    close->setDefault(true);
    buttons->addWidget(help);
    buttons->addStretch(1);
    buttons->addWidget(close);
    outer->addLayout(buttons);

    connect(help, &QAbstractButton::clicked, this, [this] {
        QMessageBox::information(this, windowTitle(),
                                 tr("Changes apply as soon as you make them.\n\n"
                                    "To change a keyboard shortcut, open Shortcuts, click the shortcut and "
                                    "press the new keys. Press Backspace to remove it, Escape to cancel."));
    });
    connect(close, &QAbstractButton::clicked, this, &QWidget::close);
    connect(m_categories, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    // Style sheets cannot bold a single item, so do it on the item itself.
    connect(m_categories, &QListWidget::currentRowChanged, this, [this](int row) {
        for (int i = 0; i < m_categories->count(); ++i) {
            QFont font = m_categories->item(i)->font();
            font.setBold(i == row);
            m_categories->item(i)->setFont(font);
        }
    });
    connect(&Settings::instance(), &Settings::changed, this, &PreferencesDialog::onSettingChanged);

    m_categories->setCurrentRow(0);

    // Open tall enough that the Markdown page, the longest one people visit
    // often, needs no scrolling (within the screen).
    if (auto *markdown = qobject_cast<QScrollArea *>(m_stack->widget(int(m_pageIds.indexOf(QStringLiteral("markdown")))))) {
        markdown->setMinimumHeight(markdown->widget()->sizeHint().height());
        int height = qMax(this->height(), layout()->minimumSize().height());
        markdown->setMinimumHeight(0);
        if (const QScreen *screen = this->screen())
            height = qMin(height, screen->availableGeometry().height() - 80);
        resize(width(), height);
    }
}

void PreferencesDialog::closeEvent(QCloseEvent *event)
{
    if (s_instance == this)
        s_instance.clear();
    QDialog::closeEvent(event);
}

void PreferencesDialog::showPage(QWidget *parent, const QString &page)
{
    if (!s_instance)
        s_instance = new PreferencesDialog(parent);
    s_instance->selectCategory(page);
    s_instance->show();
    s_instance->raise();
    s_instance->activateWindow();
}

void PreferencesDialog::selectCategory(const QString &page)
{
    const int index = m_pageIds.indexOf(page);
    if (index >= 0)
        m_categories->setCurrentRow(index);
}

void PreferencesDialog::onSettingChanged(const QString &changedKey)
{
    const auto refreshers = m_refreshers.values(changedKey);
    for (const auto &refresh : refreshers)
        refresh();
    if (changedKey.startsWith(QLatin1String(key::shortcutsGroup)))
        refreshShortcuts();
}

void PreferencesDialog::track(const char *key, std::function<void()> refresh)
{
    refresh();
    m_refreshers.insert(QString::fromLatin1(key), std::move(refresh));
}

// Page building ------------------------------------------------------------

void PreferencesDialog::beginPage()
{
    m_pageWidget = new QWidget;
    m_pageWidget->setObjectName(QStringLiteral("PrefPage"));
    m_page = new QVBoxLayout(m_pageWidget);
    m_page->setContentsMargins(14, 4, 14, 12);
    m_page->setSpacing(2);
    m_firstHeading = true;
}

void PreferencesDialog::endPage(const QString &category, const QString &pageId)
{
    m_page->addStretch(1);
    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("PrefScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(m_pageWidget);
    m_stack->addWidget(scroll);
    m_categories->addItem(category);
    m_pageIds.append(pageId);
    m_page = nullptr;
    m_pageWidget = nullptr;
}

void PreferencesDialog::addHeading(const QString &text)
{
    if (!m_firstHeading)
        m_page->addSpacing(10);
    m_firstHeading = false;
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("PrefHeading"));
    m_page->addWidget(label);
}

QWidget *PreferencesDialog::addRow(const QString &label, QWidget *control)
{
    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("PrefRow"));
    row->setMinimumHeight(30);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(rowIndent, 0, 0, 0);
    layout->setSpacing(12);
    auto *text = new QLabel(label);
    text->setWordWrap(true);
    layout->addWidget(text, 1);
    if (control) {
        layout->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
        text->setBuddy(control);
    }
    m_page->addWidget(row);
    return row;
}

void PreferencesDialog::addSwitch(const QString &label, const char *settingKey)
{
    auto *toggle = new Switch;
    toggle->setObjectName(QString::fromLatin1(settingKey));
    addRow(label, toggle);
    connect(toggle, &QAbstractButton::toggled, this,
            [settingKey](bool on) { Settings::instance().setValue(settingKey, on); });
    track(settingKey, [toggle, settingKey] {
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(Settings::instance().flag(settingKey));
    });
}

void PreferencesDialog::addCombo(const QString &label, const char *settingKey, const QList<ComboItem> &items)
{
    auto *combo = new QComboBox;
    combo->setObjectName(QString::fromLatin1(settingKey));
    combo->setMinimumWidth(140);
    combo->setFixedHeight(26);
    for (const ComboItem &item : items)
        combo->addItem(item.text, item.value);
    addRow(label, combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [combo, settingKey](int index) {
        if (index >= 0)
            Settings::instance().setValue(settingKey, combo->itemData(index));
    });
    track(settingKey, [combo, settingKey] {
        const QSignalBlocker blocker(combo);
        const QVariant value = Settings::instance().value(settingKey);
        int index = combo->findData(value);
        if (index < 0) {
            // A value written by hand or by a newer version: show it rather than hide it.
            combo->addItem(value.toString(), value);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(index);
    });
}

void PreferencesDialog::addTextInput(const QString &label, const char *settingKey, const QString &placeholder)
{
    auto *edit = new QLineEdit;
    edit->setObjectName(QString::fromLatin1(settingKey));
    edit->setPlaceholderText(placeholder);
    edit->setFixedHeight(26);
    edit->setMinimumWidth(200);
    addRow(label, edit);
    connect(edit, &QLineEdit::editingFinished, this,
            [edit, settingKey] { Settings::instance().setValue(settingKey, edit->text()); });
    track(settingKey, [edit, settingKey] {
        const QString text = Settings::instance().text(settingKey);
        if (edit->text() != text)
            edit->setText(text);
    });
}

void PreferencesDialog::buildEditorPage()
{
    beginPage();
    addHeading(tr("Code font"));
    addFontRow(tr("Code font"), QStringLiteral("font"), key::fontFamily, key::fontSize, &theme::editorFont, true);
    addHeading(tr("Display"));
    addSwitch(tr("Display line numbers"), key::lineNumbers);
    addSwitch(tr("Highlight the current line"), key::highlightCurrentLine);
    addSwitch(tr("Draw whitespace"), key::drawWhitespace);
    addCombo(tr("Line spacing"), key::lineSpacing,
             {{QStringLiteral("1.0"), 1.0},
              {QStringLiteral("1.2"), 1.2},
              {QStringLiteral("1.4"), 1.4},
              {QStringLiteral("1.6"), 1.6},
              {QStringLiteral("1.8"), 1.8},
              {QStringLiteral("2.0"), 2.0}});
    addHeading(tr("Indentation"));
    addTabWidthRow();
    addSwitch(tr("Use spaces instead of tabs"), key::insertSpaces);
    addSwitch(tr("Detect indentation from file content"), key::detectIndentation);
    addHeading(tr("Word wrap"));
    addSwitch(tr("Word wrap"), key::wordWrap);
    addHeading(tr("Cursors"));
    addCombo(tr("Add a cursor with"), key::multiCursorModifier,
             {{tr("Alt+Click"), QStringLiteral("alt")}, {tr("Ctrl+Click"), QStringLiteral("ctrl")}});
    endPage(tr("Editor"), QStringLiteral("editor"));
}

void PreferencesDialog::addFontRow(const QString &label, const QString &name, const char *familyKey,
                                   const char *sizeKey, QFont (*currentFont)(), bool monospaced)
{
    auto *box = new QWidget;
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto *input = new ClickableLineEdit;
    input->setObjectName(name);
    input->setReadOnly(true);
    input->setFixedHeight(26);
    input->setMinimumWidth(180);
    input->setCursor(Qt::PointingHandCursor);
    input->onClicked = [this, familyKey, sizeKey, currentFont, monospaced] {
        chooseFont(familyKey, sizeKey, currentFont, monospaced);
    };
    auto *reset = textButton(tr("Reset"), name + QStringLiteral("Reset"));
    layout->addWidget(input);
    layout->addWidget(reset);
    addRow(label, box);

    connect(reset, &QAbstractButton::clicked, this, [familyKey, sizeKey] {
        Settings::instance().reset(familyKey);
        Settings::instance().reset(sizeKey);
    });
    auto refresh = [input, familyKey, currentFont] {
        const QFont font = currentFont();
        const QString family = Settings::instance().text(familyKey);
        const QString shown = family.isEmpty() ? QFontInfo(font).family() : family;
        input->setText(QStringLiteral("%1 %2").arg(shown).arg(qRound(font.pointSizeF())));
    };
    track(familyKey, refresh);
    m_refreshers.insert(QString::fromLatin1(sizeKey), refresh);
}

// Proportional fonts are allowed unless `monospaced`.
void PreferencesDialog::chooseFont(const char *familyKey, const char *sizeKey, QFont (*currentFont)(),
                                   bool monospaced)
{
    auto *picker = new QFontDialog(currentFont(), this);
    picker->setObjectName(QStringLiteral("fontDialog"));
    picker->setOption(QFontDialog::MonospacedFonts, monospaced);
    picker->setAttribute(Qt::WA_DeleteOnClose);
    connect(picker, &QFontDialog::fontSelected, this, [familyKey, sizeKey](const QFont &font) {
        Settings::instance().setValue(familyKey, font.family());
        Settings::instance().setValue(sizeKey, font.pointSize());
    });
    picker->open();
}

void PreferencesDialog::addTabWidthRow()
{
    auto *box = new QWidget;
    box->setObjectName(QString::fromLatin1(key::tabWidth));
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *minus = smallButton(QStringLiteral("−"), QStringLiteral("minus"));
    auto *plus = smallButton(QStringLiteral("+"), QStringLiteral("plus"));
    auto *value = new QLabel;
    value->setObjectName(QStringLiteral("value"));
    value->setAlignment(Qt::AlignCenter);
    value->setFixedWidth(32);
    layout->addWidget(minus);
    layout->addWidget(value);
    layout->addWidget(plus);
    addRow(tr("Tab width"), box);

    auto step = [](int delta) {
        const int width = std::clamp(Settings::instance().number(key::tabWidth) + delta, 1, 16);
        Settings::instance().setValue(key::tabWidth, width);
    };
    connect(minus, &QAbstractButton::clicked, this, [step] { step(-1); });
    connect(plus, &QAbstractButton::clicked, this, [step] { step(1); });
    track(key::tabWidth, [value] { value->setText(QString::number(Settings::instance().number(key::tabWidth))); });
}

void PreferencesDialog::buildMarkdownPage()
{
    beginPage();
    addHeading(tr("Editing"));
    addCombo(tr("Default mode"), key::defaultMode,
             {{tr("Source"), QStringLiteral("source")},
              {tr("Source / Visual"), QStringLiteral("split")},
              {tr("Visual Editor"), QStringLiteral("visual")}});
    addSwitch(tr("Highlight the block being edited"), key::highlightActiveBlock);
    addHeading(tr("Rendering"));
    addSwitch(tr("Treat single line breaks as line breaks"), key::breaks);
    addSwitch(tr("Turn URL-like text into links"), key::linkify);
    addSwitch(tr("Smart quotes and dashes"), key::typographer);
    addFontRow(tr("Text font"), QStringLiteral("textFont"), key::textFontFamily, key::textFontSize, &theme::textFont,
               false);
    addCombo(tr("Line spacing"), key::textLineHeight,
             {{QStringLiteral("1.2"), 1.2},
              {QStringLiteral("1.4"), 1.4},
              {QStringLiteral("1.6"), 1.6},
              {QStringLiteral("1.8"), 1.8},
              {QStringLiteral("2.0"), 2.0}});
    addCombo(tr("Front matter"), key::frontMatter,
             {{tr("Hide"), QStringLiteral("hide")},
              {tr("Code block"), QStringLiteral("code")},
              {tr("Table"), QStringLiteral("table")}});
    addHeading(tr("Math"));
    addSwitch(tr("Render math"), key::renderMath);
    addMacroTable();
    addHeading(tr("Diagrams"));
    addCombo(tr("Mermaid theme"), key::mermaidTheme,
             {{tr("Match app"), QStringLiteral("auto")},
              {tr("Base"), QStringLiteral("base")},
              {tr("Default"), QStringLiteral("default")},
              {tr("Forest"), QStringLiteral("forest")},
              {tr("Neutral"), QStringLiteral("neutral")},
              {tr("Dark"), QStringLiteral("dark")}});
    addHeading(tr("Links and paste"));
    addTextInput(tr("Folder for pasted images"), key::pasteImageFolder, tr("e.g. images/"));
    addSwitch(tr("Update links when pasting Markdown"), key::updateLinksOnPaste);
    addSwitch(tr("Check links for errors"), key::validateLinks);
    endPage(tr("Markdown"), QStringLiteral("markdown"));
}

void PreferencesDialog::addMacroTable()
{
    addRow(tr("Macros"), nullptr);

    auto *table = new QWidget;
    table->setObjectName(QStringLiteral("macroTable"));
    auto *tableLayout = new QVBoxLayout(table);
    tableLayout->setContentsMargins(rowIndent, 0, 0, 0);
    tableLayout->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    auto *command = new QLabel(tr("Command"));
    auto *expands = new QLabel(tr("Expands to"));
    for (QLabel *l : {command, expands})
        l->setObjectName(QStringLiteral("PrefMuted"));
    command->setFixedWidth(150);
    header->addWidget(command);
    header->addWidget(expands, 1);
    header->addSpacing(26);
    tableLayout->addLayout(header);

    m_macroRows = new QVBoxLayout;
    m_macroRows->setSpacing(4);
    tableLayout->addLayout(m_macroRows);

    auto *add = textButton(tr("+ Add macro"), QStringLiteral("addMacro"));
    add->setProperty("link", true);
    add->setFlat(true);
    tableLayout->addWidget(add, 0, Qt::AlignLeft);
    m_page->addWidget(table);

    connect(add, &QAbstractButton::clicked, this, [this] { addMacroRow({}, {}); });
    track(key::mathMacros, [this] {
        const QVariantMap macros = Settings::instance().value(key::mathMacros).toMap();
        // Our own save comes back here; rebuilding would steal focus mid-edit.
        if (macros == collectMacros())
            return;
        while (QLayoutItem *item = m_macroRows->takeAt(0)) {
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }
        for (auto it = macros.cbegin(); it != macros.cend(); ++it)
            addMacroRow(it.key(), it.value().toString());
    });
}

void PreferencesDialog::addMacroRow(const QString &command, const QString &expansion)
{
    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("macroRow"));
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *commandEdit = new QLineEdit(command);
    commandEdit->setObjectName(QStringLiteral("macroCommand"));
    commandEdit->setFixedSize(150, 26);
    auto *expansionEdit = new QLineEdit(expansion);
    expansionEdit->setObjectName(QStringLiteral("macroExpansion"));
    expansionEdit->setFixedHeight(26);
    for (QLineEdit *edit : {commandEdit, expansionEdit}) {
        edit->setFont(theme::editorFont());
        connect(edit, &QLineEdit::editingFinished, this, [this] { saveMacros(); });
    }
    auto *remove = smallButton(QStringLiteral("✕"), QStringLiteral("macroRemove"));
    layout->addWidget(commandEdit);
    layout->addWidget(expansionEdit, 1);
    layout->addWidget(remove);
    m_macroRows->addWidget(row);

    connect(remove, &QAbstractButton::clicked, this, [this, row] {
        m_macroRows->removeWidget(row);
        row->hide();
        row->deleteLater();
        saveMacros();
    });
    if (command.isEmpty() && expansion.isEmpty())
        commandEdit->setFocus();
}

QVariantMap PreferencesDialog::collectMacros() const
{
    QVariantMap macros;
    for (int i = 0; i < m_macroRows->count(); ++i) {
        const QWidget *row = m_macroRows->itemAt(i)->widget();
        if (!row)
            continue;
        const QString command = row->findChild<QLineEdit *>(QStringLiteral("macroCommand"))->text().trimmed();
        if (!command.isEmpty())
            macros.insert(command, row->findChild<QLineEdit *>(QStringLiteral("macroExpansion"))->text());
    }
    return macros;
}

void PreferencesDialog::saveMacros()
{
    Settings::instance().setValue(key::mathMacros, collectMacros());
}

void PreferencesDialog::buildSavePage()
{
    beginPage();
    addHeading(tr("Restore"));
    addSwitch(tr("Recover unsaved changes after a crash or forced shutdown"), key::restoreUnsaved);
    addSwitch(tr("Reopen last session's files"), key::restoreSession);
    addSwitch(tr("Remember recently opened files"), key::rememberRecent);
    addHeading(tr("Auto save"));
    addCombo(tr("Auto save"), key::autoSave,
             {{tr("Off"), QStringLiteral("off")},
              {tr("After a delay"), QStringLiteral("afterDelay")},
              {tr("When focus changes"), QStringLiteral("onFocusChange")}});
    addHeading(tr("Opening files"));
    addSwitch(tr("Guess the encoding of files that aren't UTF-8"), key::guessEncoding);
    addLegacyEncodingRow();
    addHeading(tr("New files"));
    addCombo(tr("Open new files as"), key::newFileLanguage,
             {{tr("Markdown"), QStringLiteral("markdown")}, {tr("Plain text"), QStringLiteral("plaintext")}});
    addCombo(tr("Encoding"), key::encoding,
             {{tr("UTF-8"), QStringLiteral("utf8")},
              {tr("UTF-8 with BOM"), QStringLiteral("utf8bom")},
              {tr("UTF-16 LE"), QStringLiteral("utf16le")}});
    addCombo(tr("Line endings"), key::lineEnding,
             {{tr("System default"), QStringLiteral("auto")},
              {tr("Unix (LF)"), QStringLiteral("lf")},
              {tr("Windows (CRLF)"), QStringLiteral("crlf")}});
    endPage(tr("Save"), QStringLiteral("save"));
}

void PreferencesDialog::addLegacyEncodingRow()
{
    QList<ComboItem> items{{tr("Automatic (system)"), QString()}};
    // Unicode encodings (the first group) are not "legacy".
    const auto groups = encoding::curatedGroups();
    for (qsizetype i = 1; i < groups.size(); ++i) {
        for (const encoding::Entry &entry : groups[i])
            items.append({entry.label, entry.name});
    }
    addCombo(tr("Encoding for non-UTF-8 files"), key::legacyEncoding, items);
}

// "Colors" row: how many colours differ from the theme, and the dialog button.
void PreferencesDialog::addColorsRow()
{
    auto *box = new QWidget;
    auto *boxLayout = new QHBoxLayout(box);
    boxLayout->setContentsMargins(0, 0, 0, 0);
    boxLayout->setSpacing(12);
    auto *summary = new QLabel;
    summary->setObjectName(QStringLiteral("PrefMuted"));
    auto *customize = textButton(tr("Customize colors…"), QStringLiteral("customizeColors"), box);
    boxLayout->addWidget(summary);
    boxLayout->addWidget(customize);
    addRow(tr("Colors"), box);

    const auto refresh = [summary] {
        const QString id = theme::currentThemeId();
        const int changed = int(Settings::instance().overriddenColors(id).size());
        summary->setText(changed > 0 ? tr("%1 changed from %2").arg(changed).arg(theme::themeLabel(id)) : QString());
    };
    refresh();
    connect(&Settings::instance(), &Settings::changed, summary, [refresh](const QString &changedKey) {
        if (changedKey == QLatin1String(key::theme) || changedKey.startsWith(QLatin1String(key::colorsGroup)))
            refresh();
    });
    connect(customize, &QAbstractButton::clicked, this, [this] {
        CustomizeColorsDialog dialog(this);
        dialog.exec();
    });
}

void PreferencesDialog::buildViewPage()
{
    beginPage();
    addHeading(tr("Appearance"));
    QList<ComboItem> themes;
    for (const theme::ThemeInfo &info : theme::themes())
        themes.append({info.label, info.id});
    addCombo(tr("Theme"), key::theme, themes);
    addColorsRow();
    addHeading(tr("Layout"));
    addSwitch(tr("Show status bar"), key::statusBar);
    addSwitch(tr("Show Outline panel for Markdown files"), key::outline);
    addCombo(tr("Zoom level"), key::zoom, {{QStringLiteral("90%"), 90}, {QStringLiteral("100%"), 100},
                                           {QStringLiteral("110%"), 110}, {QStringLiteral("125%"), 125}});
    endPage(tr("View"), QStringLiteral("view"));
}

void PreferencesDialog::buildShortcutsPage()
{
    beginPage();

    auto *titleRow = new QWidget;
    auto *titleLayout = new QHBoxLayout(titleRow);
    titleLayout->setContentsMargins(0, 6, 0, 4);
    auto *title = new QLabel(tr("Keyboard shortcuts"));
    title->setObjectName(QStringLiteral("PrefHeading"));
    auto *resetAll = textButton(tr("Reset All"), QStringLiteral("resetAllShortcuts"));
    titleLayout->addWidget(title, 1);
    titleLayout->addWidget(resetAll);
    m_page->addWidget(titleRow);
    connect(resetAll, &QAbstractButton::clicked, this, [] { Settings::instance().resetAllShortcuts(); });

    for (const QString &category : registryCategories()) {
        auto *heading = new QLabel(category);
        heading->setObjectName(QStringLiteral("PrefHeading"));
        m_page->addSpacing(8);
        m_page->addWidget(heading);

        for (const Command &command : commands()) {
            if (command.category != category)
                continue;
            auto *row = new QWidget;
            row->setObjectName(QStringLiteral("shortcut-row-") + command.id);
            auto *rowLayout = new QVBoxLayout(row);
            rowLayout->setContentsMargins(rowIndent, 2, 0, 2);
            rowLayout->setSpacing(2);

            auto *line = new QHBoxLayout;
            line->setSpacing(8);
            auto *label = new QLabel(command.text);
            auto *cell = new ShortcutCell(row);
            cell->setObjectName(QStringLiteral("shortcut-") + command.id);
            cell->setFixedSize(150, 24);
            auto *reset = smallButton(QStringLiteral("↺"), QStringLiteral("shortcut-reset-") + command.id, row);
            reset->setToolTip(tr("Restore the default shortcut"));
            auto policy = reset->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            reset->setSizePolicy(policy);
            reset->setFixedSize(24, 24);
            line->addWidget(label, 1);
            line->addWidget(cell);
            line->addWidget(reset);
            rowLayout->addLayout(line);

            auto *warning = new QLabel;
            warning->setObjectName(QStringLiteral("shortcut-warning-") + command.id);
            warning->setProperty("warning", true);
            warning->setWordWrap(true);
            warning->hide();
            rowLayout->addWidget(warning);
            m_page->addWidget(row);

            const QString id = command.id;
            cell->onAssign = [id](const QList<QKeySequence> &keys) {
                Settings::instance().setShortcutOverride(id, keys);
            };
            connect(reset, &QAbstractButton::clicked, this, [id] { Settings::instance().resetShortcut(id); });
            m_shortcutRows.append({id, cell, reset, warning});
        }
    }
    endPage(tr("Shortcuts"), QStringLiteral("shortcuts"));
    refreshShortcuts();
}

void PreferencesDialog::refreshShortcuts()
{
    const Settings &settings = Settings::instance();
    for (const ShortcutRow &row : std::as_const(m_shortcutRows)) {
        const QList<QKeySequence> keys = shortcutsFor(row.id);
        QStringList names;
        QStringList others;
        for (const QKeySequence &sequence : keys) {
            names.append(sequence.toString(QKeySequence::NativeText));
            for (const QString &otherId : conflictingCommands(sequence, row.id)) {
                if (const Command *other = command(otherId); other && !others.contains(other->text))
                    others.append(other->text);
            }
        }
        const bool modified = settings.hasShortcutOverride(row.id);
        row.cell->setShortcut(names.join(QStringLiteral(", ")), modified);
        row.reset->setVisible(modified);
        row.warning->setText(others.isEmpty() ? QString() : tr("Also used by: ") + others.join(QStringLiteral(", ")));
        row.warning->setVisible(!others.isEmpty());
    }
}

} // namespace vn
