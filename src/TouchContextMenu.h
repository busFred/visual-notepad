#pragma once

class QApplication;
class QWidget;

namespace vn::touch {

void enableContextMenu(QWidget *widget);
void installContextMenus(QApplication &app);

} // namespace vn::touch
