// Preferences window: settings controls and the shortcut editor.
#include "Commands.h"
#include "PreferencesDialog.h"
#include "Settings.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

using namespace vn;

namespace {

QWidget *prefsWindow()
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->objectName() == QLatin1String("PreferencesDialog") && w->isVisible())
            return w;
    }
    return nullptr;
}

template<class T>
T *find(const QString &name)
{
    QWidget *window = prefsWindow();
    return window ? window->findChild<T *>(name) : nullptr;
}

} // namespace

class TestPreferences : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void switchWritesSetting();
    void externalChangeUpdatesSwitch();
    void spaceTogglesSwitch();
    void comboWritesValue();
    void externalChangeUpdatesCombo();
    void tabWidthSpin();
    void macros();
    void captureShortcut();
    void chordWithCtrlK();
    void backspaceClears();
    void escapeCancels();
    void modifierAloneIsIgnored();
    void conflictWarning();
    void resetButtonAndResetAll();
    void showPageSelectsCategory();
    void singleInstance();
    void screenshots();
};

void TestPreferences::initTestCase()
{
    theme::apply(*qobject_cast<QApplication *>(qApp));
}

void TestPreferences::init()
{
    Settings::instance().resetAllShortcuts();
    for (const char *k : {key::lineNumbers, key::tabWidth, key::autoSave, key::mathMacros, key::zoom})
        Settings::instance().reset(k);
    PreferencesDialog::showPage(nullptr, QStringLiteral("editor"));
    QVERIFY(prefsWindow());
    QVERIFY(QTest::qWaitForWindowExposed(prefsWindow()));
}

void TestPreferences::cleanup()
{
    if (QWidget *w = prefsWindow())
        w->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void TestPreferences::switchWritesSetting()
{
    auto *toggle = find<QAbstractButton>(key::lineNumbers);
    QVERIFY(toggle);
    const bool before = Settings::instance().flag(key::lineNumbers);
    QCOMPARE(toggle->isChecked(), before);
    toggle->click();
    QCOMPARE(Settings::instance().flag(key::lineNumbers), !before);
    toggle->click();
    QCOMPARE(Settings::instance().flag(key::lineNumbers), before);
}

void TestPreferences::externalChangeUpdatesSwitch()
{
    auto *toggle = find<QAbstractButton>(key::wordWrap);
    QVERIFY(toggle);
    const bool before = toggle->isChecked();
    Settings::instance().setValue(key::wordWrap, !before);
    QCOMPARE(toggle->isChecked(), !before);
    Settings::instance().setValue(key::wordWrap, before);
    QCOMPARE(toggle->isChecked(), before);
}

void TestPreferences::spaceTogglesSwitch()
{
    auto *toggle = find<QAbstractButton>(key::insertSpaces);
    QVERIFY(toggle);
    const bool before = toggle->isChecked();
    toggle->setFocus();
    QTest::keyClick(toggle, Qt::Key_Space);
    QCOMPARE(toggle->isChecked(), !before);
    QCOMPARE(Settings::instance().flag(key::insertSpaces), !before);
    Settings::instance().setValue(key::insertSpaces, before);
}

void TestPreferences::comboWritesValue()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("save"));
    auto *combo = find<QComboBox>(key::autoSave);
    QVERIFY(combo);
    QCOMPARE(combo->currentData().toString(), QStringLiteral("off"));
    combo->setCurrentIndex(combo->findData(QStringLiteral("onFocusChange")));
    QCOMPARE(Settings::instance().text(key::autoSave), QStringLiteral("onFocusChange"));

    PreferencesDialog::showPage(nullptr, QStringLiteral("view"));
    auto *zoom = find<QComboBox>(key::zoom);
    QVERIFY(zoom);
    zoom->setCurrentIndex(zoom->findData(125));
    QCOMPARE(Settings::instance().number(key::zoom), 125);
}

void TestPreferences::externalChangeUpdatesCombo()
{
    auto *combo = find<QComboBox>(key::autoSave);
    QVERIFY(combo);
    Settings::instance().setValue(key::autoSave, QStringLiteral("afterDelay"));
    QCOMPARE(combo->currentData().toString(), QStringLiteral("afterDelay"));
}

void TestPreferences::tabWidthSpin()
{
    auto *spin = find<QWidget>(key::tabWidth);
    QVERIFY(spin);
    auto *value = spin->findChild<QLabel *>(QStringLiteral("value"));
    QCOMPARE(value->text(), QStringLiteral("4"));
    spin->findChild<QToolButton *>(QStringLiteral("plus"))->click();
    QCOMPARE(Settings::instance().number(key::tabWidth), 5);
    QCOMPARE(value->text(), QStringLiteral("5"));
    for (int i = 0; i < 20; ++i)
        spin->findChild<QToolButton *>(QStringLiteral("minus"))->click();
    QCOMPARE(Settings::instance().number(key::tabWidth), 1);
    for (int i = 0; i < 30; ++i)
        spin->findChild<QToolButton *>(QStringLiteral("plus"))->click();
    QCOMPARE(Settings::instance().number(key::tabWidth), 16);
}

void TestPreferences::macros()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    QWidget *window = prefsWindow();
    auto *add = find<QAbstractButton>(QStringLiteral("addMacro"));
    QVERIFY(add);
    add->click();
    auto edits = [&](const char *name) { return window->findChildren<QLineEdit *>(QLatin1String(name)); };
    QCOMPARE(edits("macroCommand").size(), 1);

    // An empty command is ignored.
    edits("macroExpansion").first()->setText(QStringLiteral("ignored"));
    Q_EMIT edits("macroExpansion").first()->editingFinished();
    QVERIFY(Settings::instance().value(key::mathMacros).toMap().isEmpty());

    edits("macroCommand").first()->setText(QStringLiteral("\\R"));
    Q_EMIT edits("macroCommand").first()->editingFinished();
    QVariantMap expected{{QStringLiteral("\\R"), QStringLiteral("ignored")}};
    QCOMPARE(Settings::instance().value(key::mathMacros).toMap(), expected);

    edits("macroExpansion").first()->setText(QStringLiteral("\\mathbb{R}"));
    Q_EMIT edits("macroExpansion").first()->editingFinished();
    expected = {{QStringLiteral("\\R"), QStringLiteral("\\mathbb{R}")}};
    QCOMPARE(Settings::instance().value(key::mathMacros).toMap(), expected);
    QCOMPARE(edits("macroCommand").size(), 1);

    add->click();
    auto commands = edits("macroCommand");
    QCOMPARE(commands.size(), 2);
    commands.last()->setText(QStringLiteral("\\N"));
    edits("macroExpansion").last()->setText(QStringLiteral("\\mathbb{N}"));
    Q_EMIT commands.last()->editingFinished();
    QCOMPARE(Settings::instance().value(key::mathMacros).toMap().size(), 2);

    window->findChildren<QToolButton *>(QStringLiteral("macroRemove")).first()->click();
    expected = {{QStringLiteral("\\N"), QStringLiteral("\\mathbb{N}")}};
    QCOMPARE(Settings::instance().value(key::mathMacros).toMap(), expected);

    // External changes rebuild the table.
    Settings::instance().setValue(key::mathMacros, QVariantMap{{QStringLiteral("a"), QStringLiteral("b")}});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(prefsWindow()->findChildren<QLineEdit *>(QStringLiteral("macroCommand")).size(), 1);
}

namespace {

QAbstractButton *cellFor(const QString &id)
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("shortcuts"));
    return find<QAbstractButton>(QStringLiteral("shortcut-") + id);
}

} // namespace

void TestPreferences::captureShortcut()
{
    auto *cell = cellFor(QStringLiteral("edit.findNext"));
    QVERIFY(cell);
    QCOMPARE(cell->text(), QKeySequence(Qt::Key_F3).toString(QKeySequence::NativeText));
    cell->click();
    QCOMPARE(cell->text(), QStringLiteral("Press keys…"));
    QTest::keyClick(cell, Qt::Key_J, Qt::ControlModifier | Qt::ShiftModifier);
    const QList<QKeySequence> expected{QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J)};
    QCOMPARE(Settings::instance().shortcutOverride(QStringLiteral("edit.findNext")), expected);
    QCOMPARE(shortcutsFor(QStringLiteral("edit.findNext")), expected);
    QCOMPARE(cell->text(), expected.first().toString(QKeySequence::NativeText));
    QVERIFY(find<QWidget>(QStringLiteral("shortcut-reset-edit.findNext"))->isVisible());
}

void TestPreferences::chordWithCtrlK()
{
    auto *cell = cellFor(QStringLiteral("edit.toggleComment"));
    QVERIFY(cell);
    cell->click();
    QTest::keyClick(cell, Qt::Key_K, Qt::ControlModifier);
    QVERIFY(!Settings::instance().hasShortcutOverride(QStringLiteral("edit.toggleComment")));
    QVERIFY(cell->text().endsWith(QStringLiteral("…")));
    QTest::keyClick(cell, Qt::Key_Escape); // Escape is a valid second step
    const QList<QKeySequence> expected{QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_K),
                                                    QKeyCombination(Qt::Key_Escape))};
    QCOMPARE(Settings::instance().shortcutOverride(QStringLiteral("edit.toggleComment")), expected);

    cell->click();
    QTest::keyClick(cell, Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(cell, Qt::Key_X);
    const QList<QKeySequence> chord{QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_K),
                                                 QKeyCombination(Qt::Key_X))};
    QCOMPARE(Settings::instance().shortcutOverride(QStringLiteral("edit.toggleComment")), chord);
}

void TestPreferences::backspaceClears()
{
    auto *cell = cellFor(QStringLiteral("edit.findNext"));
    QVERIFY(cell);
    cell->click();
    QTest::keyClick(cell, Qt::Key_Backspace);
    QVERIFY(Settings::instance().hasShortcutOverride(QStringLiteral("edit.findNext")));
    QVERIFY(Settings::instance().shortcutOverride(QStringLiteral("edit.findNext")).isEmpty());
    QVERIFY(shortcutsFor(QStringLiteral("edit.findNext")).isEmpty());
    QCOMPARE(cell->text(), QStringLiteral("—"));
}

void TestPreferences::escapeCancels()
{
    auto *cell = cellFor(QStringLiteral("edit.findNext"));
    QVERIFY(cell);
    const QString before = cell->text();
    cell->click();
    QTest::keyClick(cell, Qt::Key_Escape);
    QVERIFY(!Settings::instance().hasShortcutOverride(QStringLiteral("edit.findNext")));
    QCOMPARE(cell->text(), before);
}

void TestPreferences::modifierAloneIsIgnored()
{
    auto *cell = cellFor(QStringLiteral("edit.findNext"));
    QVERIFY(cell);
    cell->click();
    QTest::keyClick(cell, Qt::Key_Control);
    QTest::keyClick(cell, Qt::Key_Shift);
    QVERIFY(!Settings::instance().hasShortcutOverride(QStringLiteral("edit.findNext")));
    QCOMPARE(cell->text(), QStringLiteral("Press keys…"));
    QTest::keyClick(cell, Qt::Key_Escape);
}

void TestPreferences::conflictWarning()
{
    auto *cell = cellFor(QStringLiteral("edit.findNext"));
    auto *warning = find<QLabel>(QStringLiteral("shortcut-warning-edit.findNext"));
    QVERIFY(cell && warning);
    QVERIFY(!warning->isVisible());
    cell->click();
    QTest::keyClick(cell, Qt::Key_F, Qt::ControlModifier);
    // Assignment still happens.
    QCOMPARE(Settings::instance().shortcutOverride(QStringLiteral("edit.findNext")).size(), 1);
    QVERIFY(warning->isVisible());
    QCOMPARE(warning->text(), QStringLiteral("Also used by: %1").arg(command(QStringLiteral("edit.find"))->text));

    Settings::instance().resetShortcut(QStringLiteral("edit.findNext"));
    QVERIFY(!warning->isVisible());
}

void TestPreferences::resetButtonAndResetAll()
{
    auto *cell = cellFor(QStringLiteral("edit.findNext"));
    auto *reset = find<QAbstractButton>(QStringLiteral("shortcut-reset-edit.findNext"));
    QVERIFY(cell && reset);
    QVERIFY(!reset->isVisible());
    cell->click();
    QTest::keyClick(cell, Qt::Key_J, Qt::ControlModifier);
    QVERIFY(reset->isVisible());
    reset->click();
    QVERIFY(!Settings::instance().hasShortcutOverride(QStringLiteral("edit.findNext")));
    QVERIFY(!reset->isVisible());

    for (const char *id : {"edit.findNext", "edit.find", "file.save"})
        Settings::instance().setShortcutOverride(QLatin1String(id), {});
    find<QAbstractButton>(QStringLiteral("resetAllShortcuts"))->click();
    for (const char *id : {"edit.findNext", "edit.find", "file.save"})
        QVERIFY(!Settings::instance().hasShortcutOverride(QLatin1String(id)));
    QCOMPARE(cell->text(), QKeySequence(Qt::Key_F3).toString(QKeySequence::NativeText));
}

void TestPreferences::showPageSelectsCategory()
{
    auto *list = find<QListWidget>(QStringLiteral("PrefCategories"));
    QVERIFY(list);
    QCOMPARE(list->count(), 5);
    PreferencesDialog::showPage(nullptr, QStringLiteral("shortcuts"));
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Shortcuts"));
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Markdown"));
    PreferencesDialog::showPage(nullptr, QString());
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Markdown"));
}

void TestPreferences::singleInstance()
{
    QWidget *first = prefsWindow();
    PreferencesDialog::showPage(nullptr, QStringLiteral("view"));
    QCOMPARE(prefsWindow(), first);
    int count = 0;
    for (QWidget *w : QApplication::topLevelWidgets())
        count += w->objectName() == QLatin1String("PreferencesDialog") && w->isVisible();
    QCOMPARE(count, 1);
}

void TestPreferences::screenshots()
{
    const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR");
    if (dir.isEmpty())
        QSKIP("VN_SCREENSHOT_DIR not set");
    QDir().mkpath(dir);
    Settings::instance().setShortcutOverride(QStringLiteral("edit.findNext"), {QKeySequence(Qt::CTRL | Qt::Key_F)});
    Settings::instance().setValue(key::mathMacros, QVariantMap{{QStringLiteral("\\R"), QStringLiteral("\\mathbb{R}")},
                                                               {QStringLiteral("\\N"), QStringLiteral("\\mathbb{N}")}});
    for (const QString &page : {"editor", "markdown", "save", "view", "shortcuts"}) {
        PreferencesDialog::showPage(nullptr, page);
        QTest::qWait(50);
        QVERIFY(prefsWindow()->grab().save(QDir(dir).filePath(QStringLiteral("prefs-%1.png").arg(page))));
    }
    // The dropdown popup, for checking its colours.
    PreferencesDialog::showPage(nullptr, QStringLiteral("save"));
    if (auto *combo = find<QComboBox>(key::autoSave)) {
        combo->showPopup();
        QTest::qWait(50);
        combo->view()->window()->grab().save(QDir(dir).filePath(QStringLiteral("prefs-popup.png")));
        combo->hidePopup();
    }
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestPreferences)
#include "test_preferences.moc"
