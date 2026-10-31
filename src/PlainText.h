#pragma once

#include <QString>

class QMessageBox;
class QWidget;

namespace vn {

// Text from documents and file names shown in widgets that guess rich text
// (tooltips, labels, message boxes). Guessed as HTML, an <img> in it would
// make Qt open the picture it names, on Windows even one on another machine
// (an SMB login that hands it the user's password hash), and links in it
// would be clickable. These keep such text plain.

// `text` cut to `maxLength` characters with an ellipsis in the middle.
QString elideMiddle(const QString &text, qsizetype maxLength);
// Tooltip markup that shows `text` as written (spaces and line breaks kept),
// cut to `maxLength` characters with a trailing ellipsis.
QString plainToolTip(const QString &text, qsizetype maxLength = 512);
// Shows every label of `box` (text and informative text) as plain text
// without links. Call after setting the texts.
void makePlainText(QMessageBox &box);
// QMessageBox::warning() with plain text.
void plainWarning(QWidget *parent, const QString &title, const QString &text);

} // namespace vn
