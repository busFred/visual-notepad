// xed-style Preferences window: category list on the left, settings on the
// right, Help and Close buttons. Changes apply immediately.
#pragma once

#include <QDialog>
#include <QFont>
#include <QMultiHash>

#include <functional>

class QBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;

namespace vn {

class ShortcutCell;



class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    // Shows the single Preferences window, raising it if already open.
    // page: "", "editor", "markdown", "save", "view" or "shortcuts".
    static void showPage(QWidget *parent, const QString &page);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct ShortcutRow
    {
        QString id;
        ShortcutCell *cell = nullptr;
        QToolButton *reset = nullptr;
        QLabel *warning = nullptr;
        QPushButton *resolve = nullptr;
    };
    struct ComboItem;

    explicit PreferencesDialog(QWidget *parent);

    void selectCategory(const QString &page);
    void onSettingChanged(const QString &changedKey);
    // Runs refresh now and again whenever settings key changes.
    void track(const char *key, std::function<void()> refresh);

    void beginPage();
    void endPage(const QString &category, const QString &pageId);
    void addHeading(const QString &text);
    QWidget *addRow(const QString &label, QWidget *control);
    void addSwitch(const QString &label, const char *key);
    void addCombo(const QString &label, const char *key, const QList<ComboItem> &items);
    void addTextInput(const QString &label, const char *key, const QString &placeholder);

    void buildEditorPage();
    void buildMarkdownPage();
    void buildSavePage();
    void buildViewPage();
    void buildShortcutsPage();
    // currentFont gives the font in effect; the dialog offers only monospaced
    // fonts if `monospaced`.
    void addFontRow(const QString &label, const QString &name, const char *familyKey, const char *sizeKey,
                    QFont (*currentFont)(), bool monospaced);
    void addLegacyEncodingRow();
    void addTabWidthRow();
    void addColorsRow();
    void addPictureCacheRow();
    void addMacroTable();
    void addMacroRow(const QString &command, const QString &expansion);
    QVariantMap collectMacros() const;
    void saveMacros();
    void refreshShortcuts();
    void chooseFont(const char *familyKey, const char *sizeKey, QFont (*currentFont)(), bool monospaced);

    QListWidget *m_categories = nullptr;
    QStackedWidget *m_stack = nullptr;
    QStringList m_pageIds;
    QVBoxLayout *m_page = nullptr;
    QWidget *m_pageWidget = nullptr;
    bool m_firstHeading = true;
    QMultiHash<QString, std::function<void()>> m_refreshers;
    QVBoxLayout *m_macroRows = nullptr;
    QList<ShortcutRow> m_shortcutRows;
    bool m_batchingShortcuts = false; // Reset All: refresh the table once at the end
};

} // namespace vn
