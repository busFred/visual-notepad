// The small toolbar floating above the table being edited in the visual
// editor: rows, columns, alignment and a menu with the rest. It never takes
// the keyboard focus, so the editor keeps its caret (and its open block).
#pragma once

#include "MarkdownEditing.h"

#include <QFrame>
#include <QHash>

class QMenu;
class QToolButton;

namespace vn {

class TableToolbar : public QFrame
{
    Q_OBJECT

public:
    explicit TableToolbar(QWidget *parent = nullptr);

    // Enables the buttons that apply to `info`'s cell, and shows the
    // alignment of its column.
    void setState(const md::TableInfo &info);
    // The menu of the "more" button; fill it when it is about to show.
    QMenu *moreMenu() const { return m_menu; }

Q_SIGNALS:
    void operation(vn::md::TableOp op);

protected:
    void changeEvent(QEvent *event) override;

private:
    QToolButton *addButton(const QString &text, const QString &toolTip, md::TableOp op);
    void updateIcons();

    QHash<QToolButton *, md::TableOp> m_ops;
    QToolButton *m_aligns[4] = {};
    QMenu *m_menu = nullptr;
};

} // namespace vn
