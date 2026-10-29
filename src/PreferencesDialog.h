// xed-style Preferences window: category list on the left, settings on the
// right, Help and Close buttons. Changes apply immediately.
#pragma once

#include <QDialog>

namespace vn {

class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    // Shows the single Preferences window, raising it if already open.
    // page: "", "editor", "markdown", "save", "view" or "shortcuts".
    static void showPage(QWidget *parent, const QString &page);

private:
    explicit PreferencesDialog(QWidget *parent);
};

} // namespace vn
