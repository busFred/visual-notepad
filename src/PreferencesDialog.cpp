#include "PreferencesDialog.h"

namespace vn {

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent)
{
}

void PreferencesDialog::showPage(QWidget *parent, const QString &page)
{
    Q_UNUSED(parent);
    Q_UNUSED(page);
}

} // namespace vn
