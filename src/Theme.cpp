#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace vn::theme {

namespace {

QString hex(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

} // namespace

void apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

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

    // Flat surfaces: menus and dropdowns share the menu bar grey with a pale
    // navy hover; no bevels anywhere.
    app.setStyleSheet(QStringLiteral(R"(
QMenuBar { background: %1; border: none; }
QMenuBar::item { padding: 3px 8px; background: transparent; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: %2; color: %3; }
QMenu { background: %1; border: 1px solid %4; padding: 3px 0; }
QMenu::item { padding: 4px 28px 4px 24px; }
QMenu::item:selected { background: %2; color: %3; }
QMenu::item:disabled { color: %5; }
QMenu::separator { height: 1px; background: %4; margin: 3px 6px; }
QStatusBar { background: %6; border-top: 1px solid %4; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 0 10px; border-left: 1px solid %4; }
QTabWidget::pane { border: none; }
QTabBar { background: %6; }
QTabBar::tab { background: %7; border: none; border-right: 1px solid %4; padding: 5px 12px; color: %5; }
QTabBar::tab:selected { background: %8; color: %9; }
QDockWidget::title { background: %10; padding: 4px 8px; }
QTreeWidget { background: %7; border: none; }
QPlainTextEdit { border: none; }
)")
                         .arg(hex(menuBar), hex(selection), hex(accent), hex(divider), hex(mutedText),
                              hex(statusBar), hex(sideBar), hex(editor), hex(text))
                         .arg(hex(panelTitle)));
}

} // namespace vn::theme
