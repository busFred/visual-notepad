// The single built-in theme, "Light Grey Warm". Not user-selectable.
#pragma once

#include <QColor>
#include <QFont>

class QApplication;

namespace KSyntaxHighlighting {
class Repository;
class Theme;
} // namespace KSyntaxHighlighting

namespace vn::theme {

// Warm grey family derived from #D4D0C8 (hue 40, saturation 12%).
inline const QColor editor{0xEF, 0xEE, 0xEB};
inline const QColor activeBlock{0xF6, 0xF5, 0xF3};
inline const QColor menuBar{0xE9, 0xE7, 0xE2};
inline const QColor currentLine{0xE9, 0xE7, 0xE2};
inline const QColor codeBlock{0xE4, 0xE2, 0xDD};
inline const QColor sideBar{0xDD, 0xDA, 0xD4};
inline const QColor sideBarHover{0xD3, 0xD0, 0xC9};
inline const QColor statusBar{0xD4, 0xD0, 0xC8};
inline const QColor panelTitle{0xCB, 0xC7, 0xBD};
inline const QColor divider{0xBA, 0xB5, 0xAB};
inline const QColor gripDots{0x8F, 0x8A, 0x7F};
inline const QColor gutterText{0x9A, 0x94, 0x88};
inline const QColor mutedText{0x7C, 0x76, 0x6A};
inline const QColor inactiveTabText{0x55, 0x55, 0x55};
inline const QColor text{0x22, 0x22, 0x22};
inline const QColor accent{0x0A, 0x24, 0x6A};
inline const QColor accentBorder{0x9A, 0xA7, 0xC4};
inline const QColor selection{0xCF, 0xD5, 0xE3};
inline const QColor switchOn{0x6F, 0x82, 0xAD};

// Sets the Fusion style, palette and style sheet on the application.
void apply(QApplication &app);

// Monospace font for editors.
QFont editorFont();

// Shared syntax definitions, including the app's own highlighting theme.
KSyntaxHighlighting::Repository &syntaxRepository();
KSyntaxHighlighting::Theme syntaxTheme();

} // namespace vn::theme
