#include "PlainText.h"

#include <QLabel>
#include <QMessageBox>

namespace vn {

namespace {

// Where to cut so a surrogate pair stays whole.
qsizetype cutBefore(const QString &text, qsizetype at)
{
    return at > 0 && at < text.size() && text.at(at).isLowSurrogate() ? at - 1 : at;
}

} // namespace

QString elideMiddle(const QString &text, qsizetype maxLength)
{
    if (text.size() <= maxLength)
        return text;
    const qsizetype kept = qMax<qsizetype>(0, maxLength - 1); // Besides the ellipsis.
    const qsizetype head = cutBefore(text, kept - kept / 2);
    qsizetype tail = text.size() - (kept - head);
    if (tail < text.size() && text.at(tail).isLowSurrogate())
        ++tail;
    return text.left(head) + QChar(0x2026) + text.mid(tail);
}

QString plainToolTip(const QString &text, qsizetype maxLength)
{
    QString shown = text;
    if (shown.size() > maxLength)
        shown = shown.left(cutBefore(shown, maxLength)) + QChar(0x2026);
    return QStringLiteral("<p style='white-space:pre'>") + shown.toHtmlEscaped() + QStringLiteral("</p>");
}

void makePlainText(QMessageBox &box)
{
    box.setTextFormat(Qt::PlainText); // The informative label follows.
    for (QLabel *label : box.findChildren<QLabel *>()) {
        label->setTextFormat(Qt::PlainText);
        label->setOpenExternalLinks(false);
        label->setTextInteractionFlags(label->textInteractionFlags()
                                       & ~(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard));
    }
}

void plainWarning(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
    makePlainText(box);
    box.exec();
}

} // namespace vn
