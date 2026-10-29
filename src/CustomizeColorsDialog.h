// "Customize Colors" window, after Windows 7's Window Color and Appearance:
// a clickable preview of the app, an item list and colour buttons. Edits are
// staged and only written to the settings on Apply or OK.
#pragma once

#include "Theme.h"

#include <QColor>
#include <QDialog>
#include <QHash>

#include <optional>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QPushButton;

namespace vn {

class ColorPreview;

class CustomizeColorsDialog : public QDialog
{
    Q_OBJECT

public:
    // One entry of the item list: the role of its background and/or of its
    // foreground (text, numbers or lines drawn on it).
    struct Item
    {
        QString label;
        std::optional<theme::Role> background;
        std::optional<theme::Role> foreground;
    };
    static QList<Item> items();

    // Edits the colours of the theme selected when the dialog opens.
    explicit CustomizeColorsDialog(QWidget *parent = nullptr);

    QString themeId() const { return m_themeId; }
    int currentItem() const;
    void setCurrentItem(int index);
    // Index of the item drawn at a point of the preview, or -1.
    int itemAt(const QPoint &previewPoint) const;
    QWidget *preview() const;
    QDialogButtonBox *buttonBox() const { return m_buttons; }

    // Staged colour of a role, or the saved one if nothing is staged.
    QColor color(theme::Role role) const;
    void stage(theme::Role role, const QColor &color);
    bool hasStagedChanges() const { return !m_staged.isEmpty(); }
    // Writes the staged colours to the settings.
    void applyChanges();

    void accept() override;

private:
    QColor savedColor(theme::Role role) const;
    QColor defaultColor(theme::Role role) const;
    void chooseColor(bool foreground);
    void resetItem();
    void resetAll();
    void refresh();

    QString m_themeId;
    QList<Item> m_items;
    QHash<theme::Role, QColor> m_staged;
    ColorPreview *m_preview = nullptr;
    QComboBox *m_itemCombo = nullptr;
    QPushButton *m_backgroundButton = nullptr;
    QPushButton *m_foregroundButton = nullptr;
    QLabel *m_backgroundLabel = nullptr;
    QLabel *m_foregroundLabel = nullptr;
    QWidget *m_backgroundDefault = nullptr; // "Changed · theme default ■ #HEX"
    QWidget *m_foregroundDefault = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace vn
