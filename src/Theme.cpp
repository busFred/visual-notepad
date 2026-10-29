#include "Theme.h"

#include "Settings.h"

#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

namespace vn::theme {

namespace {

QString hex(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

// Fusion, minus the always-on mnemonic underlines (Alt+letter still works).
class AppStyle : public QProxyStyle
{
public:
    AppStyle()
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion")))
    {
    }

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *returnData) const override
    {
        if (hint == SH_UnderlineShortcut)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

// @name placeholders are replaced with theme colours in apply().
const char *styleSheet = R"(
QMainWindow::separator { background: @divider; width: 1px; height: 1px; }

QMenuBar { background: @menuBar; border-bottom: 1px solid @divider; padding: 1px 0 1px 4px; spacing: 0px; }
QMenuBar::item { padding: 3px 8px; margin: 0; border-radius: 2px; background: transparent; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: @selection; color: @accent; }

QMenu { background: @menuBar; border: 1px solid @divider; padding: 3px 0; }
QMenu::item { padding: 4px 24px 4px 26px; color: @text; }
QMenu::item:selected { background: @selection; color: @accent; }
QMenu::item:disabled { color: #aaa59b; }
QMenu::item:disabled:selected { background: transparent; }
QMenu::separator { height: 1px; background: @divider; margin: 3px 0; }
QMenu::indicator { width: 12px; height: 12px; left: 8px; }
QMenu::indicator:checked { image: url(:/vn/icons/check.svg); }
QMenu::indicator:unchecked { image: none; }
QMenu::right-arrow { image: url(:/vn/icons/chevron-right.svg); width: 8px; height: 8px; right: 8px; }

#TabStrip { background: @statusBar; }
QTabBar { background: transparent; }
QTabBar::tab { background: @sideBar; color: @inactiveTabText; border: none; border-right: 1px solid @divider;
               border-bottom: 1px solid @divider; padding: 0 6px 0 12px; height: 31px; }
QTabBar::tab:selected { background: @editor; color: @text; border-top: 2px solid @accent; border-bottom: none; height: 30px; }
QTabBar::close-button { image: url(:/vn/icons/close.svg); subcontrol-position: right; margin-left: 4px; }
QTabBar::close-button:hover { image: url(:/vn/icons/close-hover.svg); }
QTabBar QToolButton { background: @statusBar; border: none; }

QToolButton#ModeButton { color: @text; padding: 2px 12px 2px 6px; border: 1px solid transparent; border-radius: 3px; }
QToolButton#ModeButton:hover { background: @panelTitle; }
QToolButton#ModeButton:open { background: @selection; color: @accent; border-color: @accentBorder; }
QToolButton#ModeButton::menu-indicator { image: url(:/vn/icons/chevron-down.svg); subcontrol-origin: padding;
                                         subcontrol-position: right center; right: 3px; }
QToolButton#IconButton { border: none; border-radius: 3px; }
QToolButton#IconButton:hover { background: @panelTitle; }
QToolButton#IconButton:checked { background: @panelTitle; }

#OutlineTitle { background: @panelTitle; }
#OutlineTitle QLabel { color: #000000; }
#OutlineTitle QToolButton { border: none; border-radius: 2px; color: @text; }
#OutlineTitle QToolButton:hover { background: @statusBar; }
QTreeWidget { background: @sideBar; border: none; outline: 0; }
QTreeWidget::item { padding: 2px 0; color: @text; }
QTreeWidget::item:hover, QTreeWidget::branch:hover { background: @sideBarHover; }
QTreeWidget::item:selected, QTreeWidget::branch:selected { background: @selection; color: @text; }

#FindBar { background: @sideBar; border: 1px solid @divider; border-top: none; }
#FindInput { background: @editor; border: 1px solid @divider; border-radius: 2px; }
#FindInput[focused="true"] { border-color: @switchOn; }
#FindInput[error="true"] { border-color: #cd3131; }
#FindInput QLineEdit { background: transparent; border: none; padding: 2px 0; }
QToolButton#FindOption { color: @text; padding: 0 3px; border: 1px solid transparent; border-radius: 2px; }
QToolButton#FindOption:hover { background: @sideBar; }
QToolButton#FindOption:checked { background: @selection; border-color: @accentBorder; color: @accent; }
QToolButton#FindAction, QToolButton#FindToggle { border: none; border-radius: 3px; }
QToolButton#FindAction:hover, QToolButton#FindToggle:hover { background: @panelTitle; }
QToolButton#FindAction:checked { background: @selection; }
QLabel#FindCount { color: #444444; padding-left: 4px; }
QLabel#FindCount[empty="true"] { color: #cd3131; }
QPushButton#FindButton { background: @editor; border: 1px solid @divider; border-radius: 2px; padding: 2px 8px; }
QPushButton#FindButton:hover { background: @activeBlock; }
QPushButton#FindButton:pressed { background: @selection; }

QLabel#EmptyGroup { background: @editor; color: @mutedText; }

QScrollBar:vertical { background: @editor; width: 12px; margin: 0; border-left: 1px solid @sideBar; }
QScrollBar:horizontal { background: @editor; height: 12px; margin: 0; border-top: 1px solid @sideBar; }
QScrollBar::handle { background: @panelTitle; border-radius: 0; }
QScrollBar::handle:hover { background: @divider; }
QScrollBar::handle:vertical { min-height: 24px; margin: 0 2px; }
QScrollBar::handle:horizontal { min-width: 24px; margin: 2px 0; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

/* Preferences */
#PreferencesDialog { background: @statusBar; }
#PrefFrame { background: @menuBar; border: 1px solid @divider; }
#PrefCategories { background: @sideBar; border: none; border-right: 1px solid @divider; outline: 0; }
#PrefCategories::item { padding: 8px 14px; color: @text; }
#PrefCategories::item:hover { background: @sideBarHover; }
#PrefCategories::item:selected { background: @selection; color: @accent; font-weight: bold; }
#PrefScroll { background: @menuBar; border: none; }
#PrefPage { background: @menuBar; }
#PrefPage QLabel, #PrefRow QLabel { background: transparent; color: @text; }
QLabel#PrefHeading { font-size: 14px; font-weight: bold; }
QLabel#PrefMuted { color: @mutedText; }
QLabel[warning="true"] { color: #a33a1c; }

#PreferencesDialog QLineEdit { background: @editor; border: 1px solid @divider; border-radius: 3px; padding: 0 8px;
                               min-height: 24px; selection-background-color: @selection; selection-color: @accent; }
#PreferencesDialog QLineEdit:focus { border-color: @switchOn; }
#PreferencesDialog QComboBox { combobox-popup: 0; background: @editor; border: 1px solid @divider; border-radius: 3px;
                               padding: 0 8px; min-height: 24px; color: @text; }
#PreferencesDialog QComboBox:focus, #PreferencesDialog QComboBox:on { border-color: @switchOn; }
#PreferencesDialog QComboBox::drop-down { border: none; width: 22px; }
#PreferencesDialog QComboBox::down-arrow { image: url(:/vn/icons/chevron-down.svg); width: 8px; height: 8px; }
#PreferencesDialog QComboBox QAbstractItemView { background: @menuBar; border: 1px solid @divider; outline: 0;
                                                 selection-background-color: @selection; selection-color: @accent; }
#PreferencesDialog QComboBox QAbstractItemView::item { min-height: 24px; padding: 0 8px; color: @text; }
#PreferencesDialog QComboBox QAbstractItemView::item:hover,
#PreferencesDialog QComboBox QAbstractItemView::item:selected { background: @selection; color: @accent; }

#PreferencesDialog QPushButton { background: @editor; border: 1px solid @divider; border-radius: 3px; padding: 3px 14px; }
#PreferencesDialog QPushButton:hover { background: @activeBlock; }
#PreferencesDialog QPushButton:pressed { background: @selection; }
#PreferencesDialog QPushButton:focus { border-color: @switchOn; }
#PreferencesDialog QPushButton[link="true"] { background: transparent; border: none; color: @accent; padding: 3px 0; }
#PreferencesDialog QPushButton[link="true"]:hover { text-decoration: underline; }
#PreferencesDialog QToolButton[prefSmall="true"] { background: @editor; border: 1px solid @divider; border-radius: 3px; color: @text; }
#PreferencesDialog QToolButton[prefSmall="true"]:hover { background: @activeBlock; }
#PreferencesDialog QToolButton[prefSmall="true"]:pressed { background: @selection; }
#PreferencesDialog QPushButton[shortcutCell="true"] { background: @editor; border: 1px solid @divider; border-radius: 3px;
                                                       padding: 1px 6px; text-align: left; }
#PreferencesDialog QPushButton[shortcutCell="true"][empty="true"] { color: @mutedText; }
#PreferencesDialog QPushButton[shortcutCell="true"][modified="true"] { color: @accent; font-weight: bold; }
#PreferencesDialog QPushButton[shortcutCell="true"][capturing="true"] { border-color: @switchOn; color: @text; font-weight: normal; }

QStatusBar { background: @statusBar; border-top: 1px solid @divider; min-height: 22px; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @text; }
)";

} // namespace

void apply(QApplication &app)
{
    app.setStyle(new AppStyle);

    QPalette p;
    p.setColor(QPalette::Window, sideBar);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, editor);
    p.setColor(QPalette::AlternateBase, activeBlock);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, menuBar);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, selection);
    p.setColor(QPalette::HighlightedText, accent);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::PlaceholderText, mutedText);
    p.setColor(QPalette::ToolTipBase, menuBar);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Mid, divider);
    p.setColor(QPalette::Dark, divider);
    p.setColor(QPalette::Disabled, QPalette::Text, mutedText);
    p.setColor(QPalette::Disabled, QPalette::WindowText, mutedText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, mutedText);
    app.setPalette(p);

    const std::pair<const char *, QColor> colors[] = {
        {"@menuBar", menuBar},       {"@selection", selection},   {"@accentBorder", accentBorder},
        {"@accent", accent},         {"@divider", divider},       {"@statusBar", statusBar},
        {"@sideBarHover", sideBarHover}, {"@sideBar", sideBar},   {"@editor", editor},
        {"@text", text},             {"@panelTitle", panelTitle}, {"@mutedText", mutedText},
        {"@inactiveTabText", inactiveTabText}, {"@switchOn", switchOn}, {"@activeBlock", activeBlock},
    };
    QString sheet = QString::fromLatin1(styleSheet);
    for (const auto &[name, color] : colors)
        sheet.replace(QLatin1String(name), hex(color));
    app.setStyleSheet(sheet);
}

QFont editorFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QString family = Settings::instance().text(key::fontFamily);
    if (family.isEmpty()) {
        font.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("DejaVu Sans Mono"),
                          QStringLiteral("Consolas"), font.family()});
    } else {
        font.setFamilies({family, font.family()});
    }
    font.setStyleHint(QFont::Monospace);
    if (const int size = Settings::instance().number(key::fontSize); size > 0)
        font.setPointSize(size);
    return font;
}

KSyntaxHighlighting::Repository &syntaxRepository()
{
    static KSyntaxHighlighting::Repository repository;
    static const bool themesAdded = [] {
        repository.addCustomSearchPath(QStringLiteral(":/vn"));
        return true;
    }();
    Q_UNUSED(themesAdded);
    return repository;
}

KSyntaxHighlighting::Theme syntaxTheme()
{
    const auto theme = syntaxRepository().theme(QStringLiteral("Light Grey Warm"));
    return theme.isValid() ? theme : syntaxRepository().defaultTheme(KSyntaxHighlighting::Repository::LightTheme);
}

} // namespace vn::theme
