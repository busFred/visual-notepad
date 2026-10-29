#include "Theme.h"

#include "Settings.h"

#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/Theme>

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QIcon>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QWidget>

#include <algorithm>
#include <array>

namespace vn::theme {

namespace {

QString hex(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

struct RoleInfo
{
    const char *name;
    QRgb standard;
    QRgb classic;
    QRgb modern;
};

// Indexed by Role. Standard and Classic are Windows' two classic schemes:
// button faces #D4D0C8 and #C0C0C0, highlights #0A246A and #000080; the
// panels around them are the same steps lighter or darker from the face (a
// warm grey for Standard, a neutral one for Classic). Modern follows Windows
// 11 Notepad.
constexpr RoleInfo roleTable[] = {
    {"menuBar", 0xD4D0C8, 0xC0C0C0, 0xF3F3F3},
    {"menu", 0xD4D0C8, 0xC0C0C0, 0xF9F9F9},
    {"menuHover", 0x0A246A, 0x000080, 0xE5E5E5},
    {"menuHoverText", 0xFFFFFF, 0xFFFFFF, 0x1A1A1A},
    {"tabStrip", 0xD4D0C8, 0xC0C0C0, 0xF3F3F3},
    {"statusBar", 0xD4D0C8, 0xC0C0C0, 0xF3F3F3},
    {"activeTab", 0xFFFFFF, 0xFFFFFF, 0xFFFFFF},
    {"inactiveTab", 0xDDDAD4, 0xCCCCCC, 0xF3F3F3},
    {"inactiveTabText", 0x555555, 0x404040, 0x5C5C5C},
    {"sideBar", 0xDDDAD4, 0xCCCCCC, 0xF9F9F9},
    {"sideBarHover", 0xD3D0C9, 0xC4C4C4, 0xEAEAEA},
    {"panelTitle", 0xCBC7BD, 0xB4B4B4, 0xE5E5E5},
    {"divider", 0xBAB5AB, 0x999999, 0xE5E5E5},
    {"surface", 0xE9E7E2, 0xDCDCDC, 0xF9F9F9},
    {"button", 0xD4D0C8, 0xC0C0C0, 0xFBFBFB},
    {"buttonHover", 0xE0DDD6, 0xCECECE, 0xF5F5F5},
    {"editor", 0xFFFFFF, 0xFFFFFF, 0xFFFFFF},
    {"text", 0x222222, 0x000000, 0x1A1A1A},
    {"selection", 0x0A246A, 0x000080, 0xCCE4F7},
    {"selectionText", 0xFFFFFF, 0xFFFFFF, 0x1A1A1A},
    {"accent", 0x0A246A, 0x000080, 0x005FB8},
    {"accentBorder", 0x9AA7C4, 0x8080C0, 0x99C3E6},
    {"switchOn", 0x6F82AD, 0x4A5FA8, 0x005FB8},
    {"activeBlock", 0xF6F5F3, 0xF4F4F4, 0xF9F9F9},
    {"currentLine", 0xE9E7E2, 0xEBEBEB, 0xF5F5F5},
    {"codeBlock", 0xE4E2DD, 0xE2E2E2, 0xF3F3F3},
    {"gutterText", 0x9A9488, 0x808080, 0x9A9A9A},
    {"gripDots", 0x8F8A7F, 0x808080, 0x8A8A8A},
    {"mutedText", 0x7C766A, 0x808080, 0x5C5C5C},
};
static_assert(std::size(roleTable) == size_t(Role::Count));

const QList<ThemeInfo> &themeList()
{
    static const QList<ThemeInfo> list = {
        {QStringLiteral("modern"), QCoreApplication::translate("vn::theme", "Windows Modern")},
        {QStringLiteral("standard"), QCoreApplication::translate("vn::theme", "Windows Standard")},
        {QStringLiteral("classic"), QCoreApplication::translate("vn::theme", "Windows Classic")},
    };
    return list;
}

QApplication *s_app = nullptr;
std::array<QColor, size_t(Role::Count)> s_colors;
bool s_colorsBuilt = false;
int s_blockers = 0;
bool s_pending = false;

void rebuildColors()
{
    const QString id = currentThemeId();
    for (int i = 0; i < int(Role::Count); ++i) {
        const QColor override = Settings::instance().colorOverride(id, roleName(Role(i)));
        s_colors[i] = override.isValid() ? override : defaultColor(id, Role(i));
    }
    s_colorsBuilt = true;
}

void onSettingChanged(const QString &changedKey)
{
    if (changedKey != QLatin1String(key::theme) && !changedKey.startsWith(QLatin1String(key::colorsGroup)))
        return;
    if (s_blockers > 0)
        s_pending = true;
    else if (s_app)
        apply(*s_app);
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
        // No icons on dialog buttons either, as on Windows.
        if (hint == SH_UnderlineShortcut || hint == SH_DialogButtonBox_ButtonsHaveIcons)
            return 0;
        // Windows button order everywhere: Save, Don't Save, Cancel; OK, Cancel.
        if (hint == SH_DialogButtonLayout)
            return QDialogButtonBox::WinLayout;
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    // Fusion's message box icons are pale on a grey dialog; ours have outlines.
    QIcon standardIcon(StandardPixmap icon, const QStyleOption *option, const QWidget *widget) const override
    {
        const char *name = icon == SP_MessageBoxWarning       ? "warning"
                           : icon == SP_MessageBoxCritical    ? "error"
                           : icon == SP_MessageBoxInformation ? "information"
                           : icon == SP_MessageBoxQuestion    ? "question"
                                                              : nullptr;
        if (name)
            return QIcon(QStringLiteral(":/vn/icons/dialog-%1.svg").arg(QLatin1String(name)));
        return QProxyStyle::standardIcon(icon, option, widget);
    }
};

// @name placeholders are replaced with theme colours in apply().
const char *styleSheet = R"(
QMainWindow::separator { background: @divider; width: 1px; height: 1px; }

/* Menus share the chrome colour, with a quiet hover instead of a navy highlight. */
QMenuBar { background: @menuBar; border-bottom: 1px solid @divider; padding: 1px 0 1px 4px; spacing: 0px; }
QMenuBar::item { padding: 3px 8px; margin: 0; border-radius: 2px; background: transparent; color: @text; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: @menuHover; color: @menuHoverText; }

QMenu { background: @menu; border: 1px solid @divider; padding: 3px 0; }
QMenu::item { padding: 4px 24px 4px 26px; color: @text; }
QMenu::item:selected { background: @menuHover; color: @menuHoverText; }
QMenu::item:disabled { color: #aaa59b; }
QMenu::item:disabled:selected { background: transparent; }
QMenu::separator { height: 1px; background: @divider; margin: 3px 0; }
QMenu::indicator { width: 12px; height: 12px; left: 8px; }
QMenu::indicator:checked { image: url(:/vn/icons/check.svg); }
QMenu::indicator:unchecked { image: none; }
QMenu::right-arrow { image: url(:/vn/icons/chevron-right.svg); width: 8px; height: 8px; right: 8px; }

#TabStrip { background: @tabStrip; }
QTabBar { background: transparent; }
QTabBar::tab { background: @inactiveTab; color: @inactiveTabText; border: none; border-right: 1px solid @divider;
               border-bottom: 1px solid @divider; padding: 0 6px 0 12px; height: 31px; }
QTabBar::tab:selected { background: @activeTab; color: @text; border-top: 2px solid @accent; border-bottom: none; height: 30px; }
QTabBar::close-button { image: url(:/vn/icons/close.svg); subcontrol-position: right; margin-left: 4px; }
QTabBar::close-button:hover { image: url(:/vn/icons/close-hover.svg); }
QTabBar QToolButton { background: @tabStrip; border: none; }

QToolButton#ModeButton { color: @text; padding: 2px 12px 2px 6px; border: 1px solid transparent; border-radius: 3px; }
QToolButton#ModeButton:hover { background: @panelTitle; }
QToolButton#ModeButton:open { background: @panelTitle; }
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
QTreeWidget::item:selected, QTreeWidget::branch:selected { background: @selection; color: @selectionText; }

#Pill QToolButton { border: none; border-radius: 12px; background: transparent; }
#Pill QToolButton:hover { background: @panelTitle; }
#Pill QToolButton:checked { background: @accent; }

#FindBar { background: @sideBar; border: 1px solid @divider; border-top: none; }
#FindInput { background: @editor; border: 1px solid @divider; border-radius: 2px; }
#FindInput[focused="true"] { border-color: @switchOn; }
#FindInput[error="true"] { border-color: #cd3131; }
#FindInput QLineEdit { background: transparent; border: none; padding: 2px 0; }
QToolButton#FindOption { color: @text; padding: 0 3px; border: 1px solid transparent; border-radius: 2px; }
QToolButton#FindOption:hover { background: @sideBar; }
QToolButton#FindOption:checked { background: @panelTitle; border-color: @accentBorder; color: @accent; }
QToolButton#FindAction, QToolButton#FindToggle { border: none; border-radius: 3px; }
QToolButton#FindAction:hover, QToolButton#FindToggle:hover { background: @panelTitle; }
QToolButton#FindAction:checked { background: @panelTitle; }
QLabel#FindCount { color: #444444; padding-left: 4px; }
QLabel#FindCount[empty="true"] { color: #cd3131; }
QPushButton#FindButton { background: @button; border: 1px solid @divider; border-radius: 2px; padding: 2px 8px; }
QPushButton#FindButton:hover { background: @buttonHover; }
QPushButton#FindButton:pressed { background: @panelTitle; }

#Welcome { background: @editor; }
#Welcome QLabel#WelcomeTitle { color: @mutedText; }
#Welcome QLabel#WelcomeHeading { color: @mutedText; font-weight: 600; padding-top: 4px; }
#Welcome QLabel#WelcomeKey { color: @mutedText; }
#Welcome QPushButton#WelcomeLink { color: @accent; background: transparent; border: none; padding: 2px 0; text-align: left; }
#Welcome QPushButton#WelcomeLink:hover { text-decoration: underline; }

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
#PrefFrame { background: @surface; border: 1px solid @divider; }
#PrefCategories { background: @sideBar; border: none; border-right: 1px solid @divider; outline: 0; }
#PrefCategories::item { padding: 8px 14px; color: @text; }
#PrefCategories::item:hover { background: @sideBarHover; }
#PrefCategories::item:selected { background: @selection; color: @selectionText; font-weight: bold; }
#PrefScroll { background: @surface; border: none; }
#PrefPage { background: @surface; }
#PrefPage QLabel, #PrefRow QLabel { background: transparent; color: @text; }
QLabel#PrefHeading { font-size: 14px; font-weight: bold; }
QLabel#PrefMuted { color: @mutedText; }
QLabel[warning="true"] { color: #a33a1c; }

#PreferencesDialog QLineEdit { background: @editor; border: 1px solid @divider; border-radius: 3px; padding: 0 8px;
                               min-height: 24px; selection-background-color: @selection; selection-color: @selectionText; }
#PreferencesDialog QLineEdit:focus { border-color: @switchOn; }
#PreferencesDialog QComboBox { combobox-popup: 0; background: @editor; border: 1px solid @divider; border-radius: 3px;
                               padding: 0 8px; min-height: 24px; color: @text; }
#PreferencesDialog QComboBox:focus, #PreferencesDialog QComboBox:on { border-color: @switchOn; }
#PreferencesDialog QComboBox::drop-down { border: none; width: 22px; }
#PreferencesDialog QComboBox::down-arrow { image: url(:/vn/icons/chevron-down.svg); width: 8px; height: 8px; }
#PreferencesDialog QComboBox QAbstractItemView { background: @editor; border: 1px solid @divider; outline: 0;
                                                 selection-background-color: @menuHover; selection-color: @menuHoverText; }
#PreferencesDialog QComboBox QAbstractItemView::item { min-height: 24px; padding: 0 8px; color: @text; }
#PreferencesDialog QComboBox QAbstractItemView::item:hover,
#PreferencesDialog QComboBox QAbstractItemView::item:selected { background: @menuHover; color: @menuHoverText; }

#PreferencesDialog QPushButton { background: @button; border: 1px solid @divider; border-radius: 3px; padding: 3px 14px; }
#PreferencesDialog QPushButton:hover { background: @buttonHover; }
#PreferencesDialog QPushButton:pressed { background: @panelTitle; }
#PreferencesDialog QPushButton:focus { border-color: @switchOn; }
#PreferencesDialog QPushButton[link="true"] { background: transparent; border: none; color: @accent; padding: 3px 0; }
#PreferencesDialog QPushButton[link="true"]:hover { text-decoration: underline; }
#PreferencesDialog QToolButton[prefSmall="true"] { background: @button; border: 1px solid @divider; border-radius: 3px; color: @text; }
#PreferencesDialog QToolButton[prefSmall="true"]:hover { background: @buttonHover; }
#PreferencesDialog QToolButton[prefSmall="true"]:pressed { background: @panelTitle; }
#PreferencesDialog QPushButton[shortcutCell="true"] { background: @editor; border: 1px solid @divider; border-radius: 3px;
                                                       padding: 1px 6px; text-align: left; }
#PreferencesDialog QPushButton[shortcutCell="true"][empty="true"] { color: @mutedText; }
#PreferencesDialog QPushButton[shortcutCell="true"][modified="true"] { color: @accent; font-weight: bold; }
#PreferencesDialog QPushButton[shortcutCell="true"][capturing="true"] { border-color: @switchOn; color: @text; font-weight: normal; }

/* Customize Colors */
#CustomizeColorsDialog { background: @statusBar; }
#CustomizeColorsDialog QLabel { color: @text; }
#CustomizeColorsDialog QLabel#MutedHint { color: @mutedText; }
#CustomizeColorsDialog QLabel:disabled { color: @mutedText; }
#CustomizeColorsDialog QComboBox { background: @editor; border: 1px solid @divider; border-radius: 3px; padding: 0 8px;
                                   min-height: 24px; color: @text; }
#CustomizeColorsDialog QComboBox:focus, #CustomizeColorsDialog QComboBox:on { border-color: @switchOn; }
#CustomizeColorsDialog QComboBox::drop-down { border: none; width: 22px; }
#CustomizeColorsDialog QComboBox::down-arrow { image: url(:/vn/icons/chevron-down.svg); width: 8px; height: 8px; }
#CustomizeColorsDialog QComboBox QAbstractItemView { background: @editor; border: 1px solid @divider; outline: 0;
                                                     selection-background-color: @menuHover; selection-color: @menuHoverText; }
#CustomizeColorsDialog QPushButton { background: @button; border: 1px solid @divider; border-radius: 3px; padding: 3px 14px; color: @text; }
#CustomizeColorsDialog QPushButton:hover { background: @buttonHover; }
#CustomizeColorsDialog QPushButton:pressed { background: @panelTitle; }
#CustomizeColorsDialog QPushButton:focus { border-color: @switchOn; }
#CustomizeColorsDialog QPushButton:disabled { color: @mutedText; }
/* The colour pickers read as drop-downs, so their field takes the Editor colour like other drop-down fields. */
#CustomizeColorsDialog QPushButton#BackgroundButton, #CustomizeColorsDialog QPushButton#ForegroundButton { background: @editor; padding: 3px 28px 3px 8px; }
#CustomizeColorsDialog QPushButton#BackgroundButton:hover, #CustomizeColorsDialog QPushButton#ForegroundButton:hover { background: @editor; border-color: @switchOn; }
#CustomizeColorsDialog QPushButton#BackgroundButton:disabled, #CustomizeColorsDialog QPushButton#ForegroundButton:disabled { background: @button; }

QStatusBar { background: @statusBar; border-top: 1px solid @divider; min-height: 22px; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @text; }
)";

} // namespace

QList<ThemeInfo> themes()
{
    return themeList();
}

QString currentThemeId()
{
    const QString id = Settings::instance().text(key::theme);
    for (const ThemeInfo &info : themeList()) {
        if (info.id == id)
            return id;
    }
    return themeList().first().id;
}

QString themeLabel(const QString &themeId)
{
    for (const ThemeInfo &info : themeList()) {
        if (info.id == themeId)
            return info.label;
    }
    return themeId;
}

QString roleName(Role role)
{
    return QLatin1String(roleTable[size_t(role)].name);
}

QColor color(Role role)
{
    if (!s_colorsBuilt)
        rebuildColors();
    return s_colors[size_t(role)];
}

QColor defaultColor(const QString &themeId, Role role)
{
    const RoleInfo &info = roleTable[size_t(role)];
    return QColor(themeId == QLatin1String("standard")  ? info.standard
                  : themeId == QLatin1String("classic") ? info.classic
                                                        : info.modern);
}

Notifier &notifier()
{
    static Notifier instance;
    return instance;
}

UpdateBlocker::UpdateBlocker()
{
    ++s_blockers;
}

UpdateBlocker::~UpdateBlocker()
{
    if (--s_blockers == 0 && s_pending && s_app) {
        s_pending = false;
        apply(*s_app);
    }
}

void apply(QApplication &app)
{
    if (s_app != &app) {
        s_app = &app;
        app.setStyle(new AppStyle);
        QObject::connect(&Settings::instance(), &Settings::changed, &app, &onSettingChanged);
    }
    rebuildColors();

    QPalette p;
    p.setColor(QPalette::Window, color(Role::sideBar));
    p.setColor(QPalette::WindowText, color(Role::text));
    p.setColor(QPalette::Base, color(Role::editor));
    p.setColor(QPalette::AlternateBase, color(Role::activeBlock));
    p.setColor(QPalette::Text, color(Role::text));
    p.setColor(QPalette::Button, color(Role::button));
    p.setColor(QPalette::ButtonText, color(Role::text));
    p.setColor(QPalette::Highlight, color(Role::selection));
    p.setColor(QPalette::HighlightedText, color(Role::selectionText));
    p.setColor(QPalette::Link, color(Role::accent));
    p.setColor(QPalette::PlaceholderText, color(Role::mutedText));
    p.setColor(QPalette::ToolTipBase, color(Role::surface));
    p.setColor(QPalette::ToolTipText, color(Role::text));
    p.setColor(QPalette::Mid, color(Role::divider));
    p.setColor(QPalette::Dark, color(Role::divider));
    p.setColor(QPalette::Disabled, QPalette::Text, color(Role::mutedText));
    p.setColor(QPalette::Disabled, QPalette::WindowText, color(Role::mutedText));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, color(Role::mutedText));
    app.setPalette(p);

    // Longest names first so that @menuHover is not read as @menu + "Hover".
    QList<Role> roles;
    for (int i = 0; i < int(Role::Count); ++i)
        roles.append(Role(i));
    std::sort(roles.begin(), roles.end(),
              [](Role a, Role b) { return roleName(a).size() > roleName(b).size(); });
    QString sheet = QString::fromLatin1(styleSheet);
    for (const Role role : roles)
        sheet.replace(u'@' + roleName(role), hex(color(role)));
    app.setStyleSheet(sheet);

    // Widgets that paint with theme::color() do not see a palette change when
    // only a role outside the palette changed.
    for (QWidget *widget : QApplication::allWidgets())
        widget->update();
    notifier().changed();
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

QFont textFont()
{
    QFont font = QApplication::font();
    const Settings &settings = Settings::instance();
    if (const QString family = settings.text(key::textFontFamily); !family.isEmpty())
        font.setFamily(family);
    if (const int size = settings.number(key::textFontSize); size > 0)
        font.setPointSize(size);
    else
        font.setPointSizeF(font.pointSizeF() * 1.15); // Slightly larger than the UI.
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
