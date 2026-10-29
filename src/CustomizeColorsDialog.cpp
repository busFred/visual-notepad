#include "CustomizeColorsDialog.h"

#include "Settings.h"

#include <QColorDialog>
#include <QComboBox>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QGridLayout>
#include <QIcon>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <array>

namespace vn {

using theme::Role;

namespace {

constexpr int previewWidth = 600;
constexpr int previewHeight = 230;
constexpr int windowMargin = 12;

// Indexes into CustomizeColorsDialog::items().
enum ItemIndex {
    MenuBarItem,
    MenuItem,
    MenuHighlightItem,
    TabStripItem,
    ActiveTabItem,
    InactiveTabItem,
    OutlineItem,
    OutlineTitleItem,
    OutlineHoverItem,
    StatusBarItem,
    DividersItem,
    EditorItem,
    SelectionItem,
    ActiveBlockItem,
    CodeItem,
    AccentItem,
    MutedTextItem,
    LineNumbersItem,
    ButtonsItem, // Not in the preview; chosen from the list.
    ButtonHoverItem,
    ItemCount
};

// Drawn for the sharpest screen so that it stays crisp when scaled.
QPixmap swatch(const QColor &color)
{
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pixmap((QSizeF(22, 14) * dpr).toSize());
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(color);
    QPainter painter(&pixmap);
    painter.setPen(QPen(QColor(0x80, 0x80, 0x80), 1));
    painter.drawRect(QRectF(0.5, 0.5, 21, 13));
    return pixmap;
}

// Beside a picker whose colour differs from the theme's: the default, with
// the same bordered swatch as the picker so it shows on any background.
class DefaultNote : public QWidget
{
public:
    DefaultNote()
        : m_swatch(new QLabel)
        , m_hex(new QLabel)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        layout->addWidget(new QLabel(CustomizeColorsDialog::tr("Changed · theme default")));
        layout->addWidget(m_swatch);
        layout->addWidget(m_hex);
        layout->addStretch(1);
        // Fixed sizes: the note is hidden and shown again as items change.
        m_swatch->setFixedSize(swatch(Qt::white).deviceIndependentSize().toSize());
        m_hex->setMinimumWidth(m_hex->fontMetrics().horizontalAdvance(QStringLiteral("#WWWWWW")));
    }

    // Shows the default, or nothing when the colour is the default.
    void setDefault(const std::optional<QColor> &color)
    {
        setVisible(color.has_value());
        if (color) {
            m_swatch->setPixmap(swatch(*color));
            m_hex->setText(color->name(QColor::HexRgb).toUpper());
        }
    }

private:
    QLabel *m_swatch;
    QLabel *m_hex;
};

// A colour picker button like the classic Windows one: swatch, hex code and
// a drop arrow on the right (the style sheet leaves room for it).
class ColorButton : public QPushButton
{
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event);
        if (!isEnabled())
            return;
        QPainter painter(this);
        const QRect arrow(width() - 20, (height() - 8) / 2, 8, 8);
        QIcon(QStringLiteral(":/vn/icons/chevron-down.svg")).paint(&painter, arrow);
    }
};

void showColor(QPushButton *button, const QColor &color)
{
    button->setIcon(QIcon(swatch(color)));
    button->setText(color.name(QColor::HexRgb).toUpper());
}

} // namespace

// Mini app window painted with the dialog's staged colours. Everything is
// laid out in a fixed 600x230 box, so hit testing needs no child widgets.
class ColorPreview : public QWidget
{
    Q_OBJECT

public:
    explicit ColorPreview(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(previewWidth, previewHeight);
        m_font = font();
        m_font.setPixelSize(12);
        m_codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        m_codeFont.setPixelSize(12);
        m_headingFont = m_font;
        m_headingFont.setPixelSize(18);
        m_headingFont.setBold(true);
        for (QColor &c : m_colors)
            c = Qt::gray;
    }

    void setColors(const std::array<QColor, size_t(Role::Count)> &colors)
    {
        m_colors = colors;
        update();
    }

    void setSelected(int item)
    {
        m_selected = item;
        update();
    }

    // Item under a point, or -1.
    int hitTest(const QPoint &point) const
    {
        const Layout l = layout();
        if (l.popup.contains(point))
            return l.popupRows[1].contains(point) ? MenuHighlightItem : MenuItem;
        if (l.menuBar.contains(point))
            return l.fileItem.contains(point) ? MenuHighlightItem : MenuBarItem;
        for (const QRect &divider : l.dividers) {
            if (divider.contains(point))
                return DividersItem;
        }
        if (l.outlineTitle.contains(point))
            return OutlineTitleItem;
        if (l.hoveredRow.contains(point))
            return OutlineHoverItem;
        if (l.outline.contains(point))
            return OutlineItem;
        if (l.activeTabLine.contains(point))
            return AccentItem;
        if (l.activeTab.contains(point))
            return ActiveTabItem;
        if (l.inactiveTab.contains(point))
            return InactiveTabItem;
        if (l.tabStrip.contains(point))
            return TabStripItem;
        if (l.statusBar.contains(point))
            return StatusBarItem;
        if (l.link.contains(point))
            return AccentItem;
        if (l.code.contains(point))
            return CodeItem;
        if (l.selected.contains(point))
            return SelectionItem;
        if (l.muted.contains(point))
            return MutedTextItem;
        if (l.blockRow.contains(point))
            return ActiveBlockItem;
        if (l.gutter.contains(point))
            return LineNumbersItem;
        if (l.editor.contains(point))
            return EditorItem;
        return -1;
    }

Q_SIGNALS:
    void itemClicked(int item);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        const int item = hitTest(event->position().toPoint());
        if (item >= 0)
            Q_EMIT itemClicked(item);
    }

    void paintEvent(QPaintEvent *) override
    {
        const Layout l = layout();
        QPainter p(this);
        p.fillRect(rect(), QColor(0x2B, 0x2B, 0x2B));

        auto fill = [&](const QRect &r, Role role) { p.fillRect(r, color(role)); };
        auto label = [&](const QRect &r, const QString &text, Role role, int flags = Qt::AlignLeft) {
            p.setPen(color(role));
            p.drawText(r, flags | Qt::AlignVCenter, text);
        };
        p.setFont(m_font);

        // Menu bar
        fill(l.menuBar, Role::menuBar);
        fill(l.fileItem, Role::menuHover);
        for (int i = 0; i < 4; ++i)
            label(l.menuItems[i], menuTitles[i], i == 0 ? Role::menuHoverText : Role::text, Qt::AlignHCenter);

        // Outline
        fill(l.outline, Role::sideBar);
        fill(l.outlineTitle, Role::panelTitle);
        label(l.outlineTitle.adjusted(8, 0, 0, 0), tr("OUTLINE"), Role::text);
        fill(l.hoveredRow, Role::sideBarHover);
        for (int i = 0; i < 5; ++i)
            label(l.outlineRows[i].adjusted(i == 0 ? 8 : 20, 0, 0, 0), outlineRows[i], Role::text);

        // Tab strip
        fill(l.tabStrip, Role::tabStrip);
        fill(QRect(l.tabStrip.left(), l.tabStrip.bottom(), l.tabStrip.width(), 1), Role::divider);
        fill(l.inactiveTab, Role::inactiveTab);
        label(l.inactiveTab.adjusted(12, 0, 0, 0), tr("todo.txt"), Role::inactiveTabText);
        fill(l.activeTab, Role::activeTab);
        fill(l.activeTabLine, Role::accent);
        label(l.activeTab.adjusted(12, 0, 0, 0), tr("notes.md"), Role::text);

        // Editor
        fill(l.editor, Role::editor);
        fill(l.blockRow, Role::activeBlock);
        fill(QRect(l.blockRow.left(), l.blockRow.top(), 2, l.blockRow.height()), Role::accent);
        p.setFont(m_headingFont);
        label(l.heading, tr("Notes"), Role::text);
        p.setFont(m_font);
        p.setPen(color(Role::gutterText));
        for (int i = 0; i < 5; ++i)
            p.drawText(l.gutterRows[i], Qt::AlignRight | Qt::AlignVCenter, QString::number(i + 1));
        label(l.lineStart, tr("Visit "), Role::text);
        label(l.link, tr("example.com"), Role::accent);
        label(l.afterLink, tr(" and run "), Role::text);
        fill(l.code, Role::codeBlock);
        p.setFont(m_codeFont);
        label(l.code, tr("make"), Role::text, Qt::AlignHCenter);
        p.setFont(m_font);
        label(l.blockRow.adjusted(10, 0, 0, 0), tr("## Block being edited"), Role::text);
        fill(l.selected, Role::selection);
        label(l.selected, tr("Selected text"), Role::selectionText);
        label(l.muted, tr("muted text"), Role::mutedText);

        // Status bar
        fill(l.statusBar, Role::statusBar);
        for (int i = 0; i < 4; ++i) {
            label(l.statusCells[i], statusTexts[i], Role::text, Qt::AlignHCenter);
            if (i > 0)
                fill(QRect(l.statusCells[i].left(), l.statusBar.top() + 3, 1, l.statusBar.height() - 6), Role::divider);
        }

        // Dividers
        for (const QRect &divider : l.dividers)
            fill(dividerLine(divider), Role::divider);

        // Menu popup
        fill(l.popup, Role::menu);
        p.setPen(color(Role::divider));
        p.drawRect(l.popup.adjusted(0, 0, -1, -1));
        fill(l.popupRows[1], Role::menuHover);
        for (int i = 0; i < 3; ++i) {
            const bool highlighted = i == 1;
            label(l.popupRows[i].adjusted(14, 0, 0, 0), popupTexts[i], highlighted ? Role::menuHoverText : Role::text);
            label(l.popupRows[i].adjusted(0, 0, -10, 0), popupKeys[i], highlighted ? Role::menuHoverText : Role::mutedText,
                  Qt::AlignRight);
        }

        // Selection marker
        if (m_selected >= 0) {
            p.setBrush(Qt::NoBrush);
            QPen pen(QColor(0xE3, 0x00, 0x8C), 1, Qt::DashLine);
            p.setPen(pen);
            for (const QRect &r : areasOf(m_selected, l))
                p.drawRect(r.adjusted(0, 0, -1, -1));
        }
    }

private:
    // Everything in widget coordinates.
    struct Layout
    {
        QRect menuBar, fileItem, popup, outline, outlineTitle, hoveredRow, tabStrip, activeTab, inactiveTab,
            activeTabLine, editor, gutter, blockRow, heading, lineStart, link, afterLink, code, selected, muted,
            statusBar;
        QRect menuItems[4], popupRows[3], outlineRows[5], gutterRows[5], statusCells[4];
        QList<QRect> dividers;
    };

    QColor color(Role role) const { return m_colors[size_t(role)]; }

    // The 1px line inside a wider divider hit area.
    static QRect dividerLine(const QRect &hitArea)
    {
        return hitArea.width() < hitArea.height()
                   ? QRect(hitArea.center().x(), hitArea.top(), 1, hitArea.height())
                   : QRect(hitArea.left(), hitArea.center().y(), hitArea.width(), 1);
    }

    Layout layout() const
    {
        Layout l;
        const QFontMetrics fm(m_font);
        const QFontMetrics codeFm(m_codeFont);
        const QRect w(windowMargin, windowMargin, previewWidth - 2 * windowMargin, previewHeight - 2 * windowMargin);
        constexpr int menuHeight = 22, statusHeight = 20, titleHeight = 20, rowHeight = 20, tabHeight = 28;
        constexpr int outlineWidth = 170, lineHeight = 24, gutterWidth = 28;

        l.menuBar = QRect(w.left(), w.top(), w.width(), menuHeight);
        int x = w.left() + 4;
        for (int i = 0; i < 4; ++i) {
            l.menuItems[i] = QRect(x, w.top(), fm.horizontalAdvance(menuTitles[i]) + 16, menuHeight);
            x = l.menuItems[i].right() + 1;
        }
        l.fileItem = l.menuItems[0];

        const int bodyTop = w.top() + menuHeight;
        const int bodyBottom = w.bottom() - statusHeight; // Last row of the body.
        l.statusBar = QRect(w.left(), bodyBottom + 1, w.width(), statusHeight);

        l.outline = QRect(w.left(), bodyTop, outlineWidth, bodyBottom + 1 - bodyTop);
        l.outlineTitle = QRect(l.outline.left(), l.outline.top(), outlineWidth, titleHeight);
        for (int i = 0; i < 5; ++i)
            l.outlineRows[i] = QRect(l.outline.left(), l.outlineTitle.bottom() + 5 + i * rowHeight, outlineWidth, rowHeight);
        l.hoveredRow = l.outlineRows[3];

        const int rightLeft = w.left() + outlineWidth;
        l.tabStrip = QRect(rightLeft, bodyTop, w.width() - outlineWidth, tabHeight);
        l.activeTab = QRect(rightLeft + 1, bodyTop, 92, tabHeight);
        l.activeTabLine = QRect(l.activeTab.left(), l.activeTab.top(), l.activeTab.width(), 2);
        l.inactiveTab = QRect(l.activeTab.right() + 1, bodyTop, 92, tabHeight - 1);

        l.editor = QRect(rightLeft, l.tabStrip.bottom() + 1, w.width() - outlineWidth, bodyBottom - l.tabStrip.bottom());
        l.gutter = QRect(l.editor.left(), l.editor.top(), gutterWidth, l.editor.height());
        const int textLeft = l.gutter.right() + 10;
        const int top = l.editor.top() + 6;
        l.heading = QRect(textLeft, top, 200, 28);
        // Line rows 1..4 follow the heading row.
        for (int i = 0; i < 5; ++i)
            l.gutterRows[i] = QRect(l.gutter.left(), top + i * lineHeight + (i > 0 ? 4 : 0), gutterWidth - 6, lineHeight);
        const int visitWidth = fm.horizontalAdvance(tr("Visit "));
        const int linkWidth = fm.horizontalAdvance(tr("example.com"));
        const int andWidth = fm.horizontalAdvance(tr(" and run "));
        const QRect line1 = l.gutterRows[1];
        l.lineStart = QRect(textLeft, line1.top(), visitWidth, lineHeight);
        l.link = QRect(l.lineStart.right() + 1, line1.top(), linkWidth, lineHeight);
        l.afterLink = QRect(l.link.right() + 1, line1.top(), andWidth, lineHeight);
        l.code = QRect(l.afterLink.right() + 1, line1.top() + 2, codeFm.horizontalAdvance(tr("make")) + 10, lineHeight - 4);
        l.blockRow = QRect(l.gutter.right() + 1, l.gutterRows[2].top(), l.editor.right() - l.gutter.right(), lineHeight);
        l.selected = QRect(textLeft, l.gutterRows[3].top() + 2, fm.horizontalAdvance(tr("Selected text")) + 4, lineHeight - 4);
        l.muted = QRect(textLeft, l.gutterRows[4].top(), fm.horizontalAdvance(tr("muted text")) + 4, lineHeight);

        // Status cells share the width; the first is wider.
        const int cellWidths[4] = {110, 80, 90, 100};
        int cx = l.statusBar.right() + 1;
        for (int i = 3; i >= 0; --i) {
            cx -= cellWidths[i];
            l.statusCells[i] = QRect(cx, l.statusBar.top(), cellWidths[i], statusHeight);
        }

        // Popup under the File item.
        l.popup = QRect(l.fileItem.left(), l.menuBar.bottom() + 1, 160, 3 + 3 * rowHeight + 3);
        for (int i = 0; i < 3; ++i)
            l.popupRows[i] = QRect(l.popup.left() + 2, l.popup.top() + 3 + i * rowHeight, l.popup.width() - 4, rowHeight);

        // Hit areas of the 1px dividers are wider.
        l.dividers = {QRect(l.menuBar.left(), l.menuBar.bottom() - 1, l.menuBar.width(), 3),
                      QRect(rightLeft - 2, l.tabStrip.top(), 5, l.outline.height()),
                      QRect(l.statusBar.left(), l.statusBar.top() - 1, l.statusBar.width(), 3)};
        return l;
    }

    static QList<QRect> areasOf(int item, const Layout &l)
    {
        switch (item) {
        case MenuBarItem: return {l.menuBar};
        case MenuItem: return {l.popup};
        case MenuHighlightItem: return {l.popupRows[1]};
        case TabStripItem: return {l.tabStrip};
        case ActiveTabItem: return {l.activeTab};
        case InactiveTabItem: return {l.inactiveTab};
        case OutlineItem: return {l.outline};
        case OutlineTitleItem: return {l.outlineTitle};
        case OutlineHoverItem: return {l.hoveredRow};
        case StatusBarItem: return {l.statusBar};
        case DividersItem: {
            QList<QRect> lines;
            for (const QRect &divider : l.dividers)
                lines.append(divider);
            return lines;
        }
        case EditorItem: return {l.editor};
        case SelectionItem: return {l.selected};
        case ActiveBlockItem: return {l.blockRow};
        case CodeItem: return {l.code};
        case AccentItem: return {l.link, l.activeTabLine};
        case MutedTextItem: return {l.muted};
        case LineNumbersItem: return {l.gutter};
        }
        return {};
    }

    const QString menuTitles[4] = {tr("File"), tr("Edit"), tr("View"), tr("Help")};
    const QString outlineRows[5] = {tr("Notes"), tr("Overview"), tr("Install"), tr("Usage"), tr("Troubleshooting")};
    const QString popupTexts[3] = {tr("New File"), tr("Open…"), tr("Save")};
    const QString popupKeys[3] = {tr("Ctrl+N"), tr("Ctrl+O"), tr("Ctrl+S")};
    const QString statusTexts[4] = {tr("Ln 3, Col 1"), tr("100%"), tr("UTF-8"), tr("Markdown")};

    std::array<QColor, size_t(Role::Count)> m_colors;
    QFont m_font, m_codeFont, m_headingFont;
    int m_selected = -1;
};

QList<CustomizeColorsDialog::Item> CustomizeColorsDialog::items()
{
    constexpr std::nullopt_t none = std::nullopt;
    return {
        {tr("Menu bar"), Role::menuBar, none},
        {tr("Menu"), Role::menu, none},
        {tr("Menu highlight"), Role::menuHover, Role::menuHoverText},
        {tr("Tab strip"), Role::tabStrip, none},
        {tr("Active tab"), Role::activeTab, none},
        {tr("Inactive tab"), Role::inactiveTab, Role::inactiveTabText},
        {tr("Outline panel"), Role::sideBar, none},
        {tr("Outline title"), Role::panelTitle, none},
        {tr("Outline hover"), Role::sideBarHover, none},
        {tr("Status bar"), Role::statusBar, none},
        {tr("Dividers"), none, Role::divider},
        {tr("Editor"), Role::editor, Role::text},
        {tr("Selection"), Role::selection, Role::selectionText},
        {tr("Block being edited"), Role::activeBlock, none},
        {tr("Code and tables"), Role::codeBlock, none},
        {tr("Links and accent"), none, Role::accent},
        {tr("Muted text"), none, Role::mutedText},
        {tr("Line numbers"), none, Role::gutterText},
        {tr("Buttons"), Role::button, none},
        {tr("Button hover"), Role::buttonHover, none},
    };
}

CustomizeColorsDialog::CustomizeColorsDialog(QWidget *parent)
    : QDialog(parent)
    , m_themeId(theme::currentThemeId())
    , m_items(items())
{
    setObjectName(QStringLiteral("CustomizeColorsDialog"));
    setWindowTitle(tr("Customize Colors — %1").arg(theme::themeLabel(m_themeId)));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_preview = new ColorPreview(this);
    layout->addWidget(m_preview, 0, Qt::AlignHCenter);


    // Item, then its two colours underneath, each with its theme default
    // beside it when changed.
    m_itemCombo = new QComboBox;
    m_itemCombo->setObjectName(QStringLiteral("ItemCombo"));
    m_itemCombo->setMinimumWidth(200);
    for (const Item &item : std::as_const(m_items))
        m_itemCombo->addItem(item.label);
    m_backgroundButton = new ColorButton;
    m_backgroundButton->setObjectName(QStringLiteral("BackgroundButton"));
    m_foregroundButton = new ColorButton;
    m_foregroundButton->setObjectName(QStringLiteral("ForegroundButton"));
    for (QPushButton *button : {m_backgroundButton, m_foregroundButton})
        button->setMinimumWidth(130);
    auto *itemLabel = new QLabel(tr("Item:"));
    itemLabel->setBuddy(m_itemCombo);
    m_backgroundLabel = new QLabel(tr("Background:"));
    m_backgroundLabel->setBuddy(m_backgroundButton);
    m_foregroundLabel = new QLabel(tr("Foreground:"));
    m_foregroundLabel->setBuddy(m_foregroundButton);
    m_backgroundDefault = new DefaultNote;
    m_foregroundDefault = new DefaultNote;

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    grid->addWidget(itemLabel, 0, 0);
    grid->addWidget(m_itemCombo, 0, 1, 1, 2, Qt::AlignLeft);
    grid->addWidget(m_backgroundLabel, 1, 0);
    grid->addWidget(m_backgroundButton, 1, 1);
    grid->addWidget(m_backgroundDefault, 1, 2);
    grid->addWidget(m_foregroundLabel, 2, 0);
    grid->addWidget(m_foregroundButton, 2, 1);
    grid->addWidget(m_foregroundDefault, 2, 2);
    grid->setColumnStretch(2, 1);
    layout->addLayout(grid);
    layout->addStretch(1);

    auto *footer = new QHBoxLayout;
    auto *resetItem = new QPushButton(tr("Reset this item"));
    auto *resetAll = new QPushButton(tr("Reset all"));
    resetItem->setAutoDefault(false);
    resetAll->setAutoDefault(false);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    footer->addWidget(resetItem);
    footer->addWidget(resetAll);
    footer->addStretch(1);
    footer->addWidget(m_buttons);
    layout->addLayout(footer);

    connect(m_preview, &ColorPreview::itemClicked, this, &CustomizeColorsDialog::setCurrentItem);
    connect(m_itemCombo, &QComboBox::currentIndexChanged, this, [this] {
        m_preview->setSelected(m_itemCombo->currentIndex());
        refresh();
    });
    connect(m_backgroundButton, &QPushButton::clicked, this, [this] { chooseColor(false); });
    connect(m_foregroundButton, &QPushButton::clicked, this, [this] { chooseColor(true); });
    connect(resetItem, &QPushButton::clicked, this, &CustomizeColorsDialog::resetItem);
    connect(resetAll, &QPushButton::clicked, this, &CustomizeColorsDialog::resetAll);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &CustomizeColorsDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            &CustomizeColorsDialog::applyChanges);

    m_preview->setSelected(0);
    refresh();
}

int CustomizeColorsDialog::currentItem() const
{
    return m_itemCombo->currentIndex();
}

void CustomizeColorsDialog::setCurrentItem(int index)
{
    m_itemCombo->setCurrentIndex(index);
}

int CustomizeColorsDialog::itemAt(const QPoint &previewPoint) const
{
    return m_preview->hitTest(previewPoint);
}

QWidget *CustomizeColorsDialog::preview() const
{
    return m_preview;
}

QColor CustomizeColorsDialog::defaultColor(Role role) const
{
    return theme::defaultColor(m_themeId, role);
}

QColor CustomizeColorsDialog::savedColor(Role role) const
{
    const QColor override = Settings::instance().colorOverride(m_themeId, theme::roleName(role));
    return override.isValid() ? override : defaultColor(role);
}

QColor CustomizeColorsDialog::color(Role role) const
{
    return m_staged.value(role, savedColor(role));
}

void CustomizeColorsDialog::stage(Role role, const QColor &color)
{
    if (color == savedColor(role))
        m_staged.remove(role);
    else
        m_staged.insert(role, color);
    refresh();
}

void CustomizeColorsDialog::applyChanges()
{
    if (m_staged.isEmpty())
        return;
    {
        // One re-theme for all roles.
        const theme::UpdateBlocker blocker;
        for (auto it = m_staged.cbegin(); it != m_staged.cend(); ++it) {
            const QString name = theme::roleName(it.key());
            if (it.value() == defaultColor(it.key()))
                Settings::instance().resetColorOverride(m_themeId, name);
            else
                Settings::instance().setColorOverride(m_themeId, name, it.value());
        }
    }
    m_staged.clear();
    refresh();
}

void CustomizeColorsDialog::accept()
{
    applyChanges();
    QDialog::accept();
}

void CustomizeColorsDialog::chooseColor(bool foreground)
{
    const Item &item = m_items.at(currentItem());
    const std::optional<Role> role = foreground ? item.foreground : item.background;
    if (!role)
        return;
    const QColor chosen = QColorDialog::getColor(color(*role), this,
                                                 foreground ? tr("Foreground color") : tr("Background color"));
    if (chosen.isValid())
        stage(*role, chosen);
}

void CustomizeColorsDialog::resetItem()
{
    const Item &item = m_items.at(currentItem());
    for (const std::optional<Role> &role : {item.background, item.foreground}) {
        if (role)
            stage(*role, defaultColor(*role));
    }
}

void CustomizeColorsDialog::resetAll()
{
    for (int i = 0; i < int(Role::Count); ++i)
        stage(Role(i), defaultColor(Role(i)));
}

void CustomizeColorsDialog::refresh()
{
    std::array<QColor, size_t(Role::Count)> colors;
    for (int i = 0; i < int(Role::Count); ++i)
        colors[i] = color(Role(i));
    m_preview->setColors(colors);

    // A colour the item does not have is greyed out, as in Windows.
    const Item &item = m_items.at(currentItem());
    const auto show = [this](const std::optional<Role> &role, QLabel *label, QPushButton *button, QWidget *note) {
        label->setEnabled(role.has_value());
        button->setEnabled(role.has_value());
        auto *defaultNote = static_cast<DefaultNote *>(note);
        if (!role) {
            button->setIcon({});
            button->setText(QString());
            defaultNote->setDefault(std::nullopt);
            return;
        }
        showColor(button, color(*role));
        const QColor original = defaultColor(*role);
        defaultNote->setDefault(color(*role) == original ? std::nullopt : std::optional<QColor>(original));
    };
    show(item.background, m_backgroundLabel, m_backgroundButton, m_backgroundDefault);
    show(item.foreground, m_foregroundLabel, m_foregroundButton, m_foregroundDefault);

    m_buttons->button(QDialogButtonBox::Apply)->setEnabled(hasStagedChanges());
}

} // namespace vn

#include "CustomizeColorsDialog.moc"
