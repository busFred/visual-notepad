// Preferences window: settings controls and the shortcut editor.
#include "Commands.h"
#include "Encodings.h"
#include "PreferencesDialog.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/MarkdownModel.h"
#include "markdown/VisualView.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QFontDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>
#include <QTextDocument>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QScrollBar>
#include <QScopeGuard>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
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
    void settingsFileIsPrivate();
    void initTestCase();
    void init();
    void cleanup();

    void switchWritesSetting();
    void touchOperatesSwitch();
    void touchFlickScrollsShortcuts();
    void typingSwitchesDefaultOn();
    void externalChangeUpdatesSwitch();
    void spaceTogglesSwitch();
    void comboWritesValue();
    void blocksStartRow();
    void readModeRows();
    void externalChangeUpdatesCombo();
    void tabWidthSpin();
    void pictureCacheSpin();
    void macros();
    void captureShortcut();
    void chordWithCtrlK();
    void backspaceClears();
    void escapeCancels();
    void modifierAloneIsIgnored();
    void conflictWarning();
    void defaultShortcutsDoNotCollide();
    void prefixConflictBothDirections();
    void removeFromOther();
    void resetButtonAndResetAll();
    void codeFontRow();
    void textFontPickerWritesKeys();
    void textFontResetRestoresDefault();
    void lineSpacingWritesKey();
    void lineSpacingChangesRenderedHeight();
    void oldLineSpacingsAreConverted();
    void offListNumericValueIsSorted();
    void preferenceResetButton();
    void syncAnchorRow();
    void encodingRows();
    void showPageSelectsCategory();
    void initialSizeKeepsButtonsVisible();
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
    for (const char *k : {key::lineNumbers, key::tabWidth, key::autoSave, key::mathMacros, key::zoom, key::fontFamily,
                          key::fontSize, key::textFontFamily, key::textFontSize, key::textLineHeight, key::lineSpacing, key::syncAnchor,
                          key::guessEncoding, key::legacyEncoding, key::encoding, key::autoClose, key::autoSurround, key::pictureCacheMiB})
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

void TestPreferences::touchOperatesSwitch()
{
    auto *toggle = find<QAbstractButton>(QString::fromLatin1(key::lineNumbers));
    QVERIFY(toggle && toggle->isVisible());
    const bool before = toggle->isChecked();
    auto touch = QTest::touchEvent(toggle, QTest::createTouchDevice(), false);
    const QPoint start(7, toggle->height() / 2);
    const QPoint end(toggle->width() - 7, start.y());
    touch.press(0, start).commit();
    touch.move(0, end).commit();
    touch.release(0, end).commit();
    QCOMPARE(toggle->isChecked(), !before);
}

void TestPreferences::touchFlickScrollsShortcuts()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("shortcuts"));
    auto *stack = prefsWindow()->findChild<QStackedWidget *>();
    auto *scroll = qobject_cast<QScrollArea *>(stack->currentWidget());
    QVERIFY(scroll && scroll->verticalScrollBar()->maximum() > 300);
    auto *bar = scroll->verticalScrollBar();
    QWidget *view = scroll->viewport();
    auto touch = QTest::touchEvent(view, QTest::createTouchDevice(), false);
    const QPoint start(22, 260);
    const QPoint end = start - QPoint(0, 130);
    touch.press(0, start).commit();
    QTest::qWait(25);
    touch.move(0, start - QPoint(0, 50)).commit();
    QTest::qWait(25);
    touch.move(0, end).commit();
    // Lifted at once: a pause before lifting stops a flick, and a busy test
    // machine can stretch any wait here into one.
    const int dragged = bar->value();
    QVERIFY(dragged > 0);
    touch.release(0, end).commit();
    QTRY_VERIFY(bar->value() > dragged + 20);
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

void TestPreferences::typingSwitchesDefaultOn()
{
    for (const char *k : {key::autoClose, key::autoSurround}) {
        auto *toggle = find<QAbstractButton>(k);
        QVERIFY(toggle);
        QVERIFY(toggle->isChecked());
        toggle->click();
        QVERIFY(!Settings::instance().flag(k));
        Settings::instance().reset(k);
    }
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

void TestPreferences::blocksStartRow()
{
    Settings::instance().reset(key::blocksStartCollapsed);
    const auto restore = qScopeGuard([] { Settings::instance().reset(key::blocksStartCollapsed); });
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    auto *combo = find<QComboBox>(key::blocksStartCollapsed);
    QVERIFY(combo);
    QCOMPARE(combo->count(), 2);
    QCOMPARE(combo->currentText(), QStringLiteral("Expanded"));
    QVERIFY(!Settings::instance().flag(key::blocksStartCollapsed));
    combo->setCurrentIndex(combo->findText(QStringLiteral("Collapsed")));
    QVERIFY(Settings::instance().flag(key::blocksStartCollapsed));
    Settings::instance().setValue(key::blocksStartCollapsed, false);
    QCOMPARE(combo->currentText(), QStringLiteral("Expanded"));
}

// Markdown → Read mode: where a tab in read mode starts.
void TestPreferences::readModeRows()
{
    for (const char *k : {key::readColumns, key::readChars, key::readKeptLines})
        Settings::instance().reset(k);
    const auto restore = qScopeGuard([] {
        for (const char *k : {key::readColumns, key::readChars, key::readKeptLines})
            Settings::instance().reset(k);
    });
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    QWidget *window = prefsWindow();
    QVERIFY(window);
    QVERIFY(window->findChild<QLabel *>(QStringLiteral("PrefHeading"), Qt::FindChildrenRecursively));
    bool heading = false;
    for (const QLabel *label : window->findChildren<QLabel *>(QStringLiteral("PrefHeading")))
        heading = heading || label->text() == QStringLiteral("Read mode");
    QVERIFY(heading);

    // Columns: Auto or any count (no fixed top), with a note under it.
    auto *columns = find<QSpinBox>(key::readColumns);
    QVERIFY(columns);
    QCOMPARE(columns->value(), 0);
    QCOMPARE(columns->text(), QStringLiteral("As many as fit"));
    QVERIFY(columns->maximum() > 4);
    columns->setValue(2);
    QCOMPARE(Settings::instance().number(key::readColumns), 2);
    columns->setValue(7);
    QCOMPARE(Settings::instance().number(key::readColumns), 7);
    auto *info = columns->parentWidget()->findChild<QToolButton *>(QStringLiteral("PrefInfo"));
    QVERIFY(info);
    info->click();
    auto *popup = window->findChild<QFrame *>(QStringLiteral("PrefInfoPopup"));
    QVERIFY(popup && popup->isVisible());
    QVERIFY(popup->findChild<QLabel *>()->text().startsWith(QStringLiteral("A new tab starts")));
    popup->close();

    // Characters per column: 50 to 100 in steps of 5.
    auto *chars = find<QSpinBox>(key::readChars);
    QVERIFY(chars);
    QCOMPARE(chars->value(), 75);
    QCOMPARE(chars->minimum(), 50);
    QCOMPARE(chars->maximum(), 100);
    QCOMPARE(chars->singleStep(), 5);
    chars->setValue(73);
    QCOMPARE(Settings::instance().number(key::readChars), 75);
    QCOMPARE(chars->value(), 75);
    Settings::instance().setValue(key::readChars, 55);
    QCOMPARE(chars->value(), 55);

    // Lines kept from the previous page: 0 to 5, 2 by default.
    auto *kept = find<QComboBox>(key::readKeptLines);
    QVERIFY(kept);
    QCOMPARE(kept->count(), 6);
    QCOMPARE(kept->currentText(), QStringLiteral("2"));
    kept->setCurrentIndex(kept->findText(QStringLiteral("0")));
    QCOMPARE(Settings::instance().number(key::readKeptLines), 0);
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

void TestPreferences::pictureCacheSpin()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    auto *spin = find<QSpinBox>(key::pictureCacheMiB);
    QVERIFY(spin);
    QCOMPARE(spin->minimum(), 16);
    QCOMPARE(spin->maximum(), 2048);
    QCOMPARE(spin->suffix(), QStringLiteral(" MiB"));
    QVERIFY(spin->toolTip().isEmpty());
    auto *info = spin->parentWidget()->findChild<QToolButton *>(QStringLiteral("PrefInfo"));
    QVERIFY(info);
    info->click();
    auto *popup = prefsWindow()->findChild<QFrame *>(QStringLiteral("PrefInfoPopup"));
    QVERIFY(popup && popup->isVisible());
    QVERIFY(popup->findChild<QLabel *>()->text().contains(QStringLiteral("decoded pictures")));
    popup->close();
    QCOMPARE(spin->value(), 128); // The default.

    // Shows what is stored, also when it changes elsewhere.
    Settings::instance().setValue(key::pictureCacheMiB, 300);
    QCOMPARE(spin->value(), 300);

    // Typing and pressing Enter stores it, once.
    QSignalSpy changed(&Settings::instance(), &Settings::changed);
    spin->setFocus();
    spin->selectAll();
    QTest::keyClicks(spin, QStringLiteral("512"));
    QTest::keyClick(spin, Qt::Key_Return);
    QCOMPARE(Settings::instance().number(key::pictureCacheMiB), 512);
    QCOMPARE(changed.size(), 1);

    spin->setValue(256);
    QCOMPARE(Settings::instance().number(key::pictureCacheMiB), 256);
    spin->setValue(100000); // Clamped by the box.
    QCOMPARE(Settings::instance().number(key::pictureCacheMiB), 2048);
    Settings::instance().reset(key::pictureCacheMiB);
    QCOMPARE(spin->value(), 128);
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
    const QString expected = QStringLiteral("Conflicts with: %1 (%2). Neither shortcut will work while the conflict remains.")
                                 .arg(command(QStringLiteral("edit.find"))->text,
                                      QKeySequence(Qt::CTRL | Qt::Key_F).toString(QKeySequence::NativeText));
    QCOMPARE(warning->text(), expected);
    // Drawn in the warning colour of the style sheet, not the plain text colour.
    QVERIFY(warning->property("warning").toBool());
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("QLabel[warning=\"true\"] { color: #")));

    Settings::instance().resetShortcut(QStringLiteral("edit.findNext"));
    QVERIFY(!warning->isVisible());
    QVERIFY(!find<QAbstractButton>(QStringLiteral("shortcut-resolve-edit.findNext"))->isVisible());
}

void TestPreferences::prefixConflictBothDirections()
{
    auto &settings = Settings::instance();
    const QKeySequence chord(QStringLiteral("Ctrl+Alt+Q, V"));
    const QKeySequence single(QStringLiteral("Ctrl+Alt+Q"));
    auto *findWarning = find<QLabel>(QStringLiteral("shortcut-warning-edit.find"));
    auto *nextWarning = find<QLabel>(QStringLiteral("shortcut-warning-edit.findNext"));
    QVERIFY(findWarning && nextWarning);

    settings.setShortcutOverride(QStringLiteral("edit.find"), {chord});
    settings.setShortcutOverride(QStringLiteral("edit.findNext"), {single});
    // The single key is a prefix of the other command's chord, and vice versa.
    // isHidden(): the page may not have been shown yet in this test.
    QVERIFY(!findWarning->isHidden() && !nextWarning->isHidden());
    QVERIFY(findWarning->text().contains(
        QStringLiteral("%1 (%2)").arg(command(QStringLiteral("edit.findNext"))->text, single.toString(QKeySequence::NativeText))));
    QVERIFY(nextWarning->text().contains(
        QStringLiteral("%1 (%2)").arg(command(QStringLiteral("edit.find"))->text, chord.toString(QKeySequence::NativeText))));
    QVERIFY(findWarning->text().contains(QStringLiteral("can't be completed")));
    QVERIFY(!findWarning->text().contains(QStringLiteral("Neither shortcut will work")));

    // A different chord on the same first key does not clash.
    settings.setShortcutOverride(QStringLiteral("edit.findNext"), {QKeySequence(QStringLiteral("Ctrl+Alt+Q, W"))});
    QVERIFY(findWarning->isHidden() && nextWarning->isHidden());

    settings.resetShortcut(QStringLiteral("edit.find"));
    settings.resetShortcut(QStringLiteral("edit.findNext"));
}

void TestPreferences::removeFromOther()
{
    auto &settings = Settings::instance();
    const QKeySequence ctrlF(Qt::CTRL | Qt::Key_F);
    // edit.find keeps Ctrl+F2 besides the Ctrl+F that findNext is about to take.
    const QKeySequence other(Qt::CTRL | Qt::Key_F2);
    settings.setShortcutOverride(QStringLiteral("edit.find"), {ctrlF, other});
    auto *cell = cellFor(QStringLiteral("edit.findNext"));
    auto *warning = find<QLabel>(QStringLiteral("shortcut-warning-edit.findNext"));
    auto *resolve = find<QAbstractButton>(QStringLiteral("shortcut-resolve-edit.findNext"));
    QVERIFY(cell && warning && resolve);
    cell->click();
    QTest::keyClick(cell, Qt::Key_F, Qt::ControlModifier);
    QVERIFY(warning->isVisible() && resolve->isVisible());

    resolve->click();
    QVERIFY(!warning->isVisible());
    QVERIFY(!resolve->isVisible());
    QCOMPARE(shortcutsFor(QStringLiteral("edit.find")), QList<QKeySequence>{other});
    QCOMPARE(shortcutsFor(QStringLiteral("edit.findNext")), QList<QKeySequence>{ctrlF});
    QVERIFY(conflictingCommands(ctrlF, QStringLiteral("edit.findNext")).isEmpty());

    settings.resetShortcut(QStringLiteral("edit.find"));
    settings.resetShortcut(QStringLiteral("edit.findNext"));
}

void TestPreferences::defaultShortcutsDoNotCollide()
{
    QMap<QString, QString> owner;
    for (const Command &c : commands()) {
        for (const QKeySequence &sequence : c.defaults) {
            const QString key = sequence.toString(QKeySequence::PortableText);
            QVERIFY2(!key.isEmpty(), qPrintable(c.id + QStringLiteral(" has an unparsable default")));
            QVERIFY2(!owner.contains(key), qPrintable(key + QStringLiteral(" is used by ") + owner.value(key)
                                                      + QStringLiteral(" and ") + c.id));
            owner.insert(key, c.id);
        }
    }
    // No single-key shortcut may also begin a chord (Ctrl+K, ...).
    for (auto it = owner.cbegin(); it != owner.cend(); ++it) {
        if (it.key().contains(QLatin1String(", ")))
            QVERIFY2(!owner.contains(it.key().section(QLatin1String(", "), 0, 0)), qPrintable(it.key()));
    }
    const auto defaultOf = [](const char *id) { return command(QLatin1String(id))->defaults; };
    const auto k = [](const char *text) { return QKeySequence(QString::fromLatin1(text), QKeySequence::PortableText); };
    QCOMPARE(defaultOf("selection.addCursorsToLineEnds"), QList<QKeySequence>{k("Shift+Alt+I")});
    QCOMPARE(defaultOf("selection.expandLine"), QList<QKeySequence>{k("Ctrl+L")});
    QCOMPARE(defaultOf("selection.moveLastToNextMatch"), QList<QKeySequence>{k("Ctrl+K, Ctrl+D")});
    QCOMPARE(defaultOf("edit.indentLines"), QList<QKeySequence>{k("Ctrl+]")});
    QCOMPARE(defaultOf("edit.outdentLines"), QList<QKeySequence>{k("Ctrl+[")});
    QCOMPARE(defaultOf("edit.trimTrailingWhitespace"), QList<QKeySequence>{k("Ctrl+K, Ctrl+X")});
    QCOMPARE(defaultOf("edit.findNextSelection"), QList<QKeySequence>{k("Ctrl+F3")});
    QCOMPARE(defaultOf("edit.findPreviousSelection"), QList<QKeySequence>{k("Ctrl+Shift+F3")});
    QCOMPARE(defaultOf("view.scrollPageUp"), QList<QKeySequence>{k("Alt+PgUp")});
    QCOMPARE(defaultOf("view.scrollPageDown"), QList<QKeySequence>{k("Alt+PgDown")});
    QVERIFY(defaultOf("selection.cursorUndo").isEmpty());
    QVERIFY(defaultOf("selection.cursorRedo").isEmpty());
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

void TestPreferences::codeFontRow()
{
    bool labelled = false;
    for (const QLabel *label : prefsWindow()->findChildren<QLabel *>()) {
        const QString text = QString(label->text()).remove(QLatin1Char('&')); // Mnemonics
        labelled |= text == QLatin1String("Code font");
        QVERIFY(text != QLatin1String("Font"));
    }
    QVERIFY(labelled);
    QVERIFY(find<QLineEdit>(QStringLiteral("font")));
    auto *reset = find<QAbstractButton>(QStringLiteral("fontReset"));
    QVERIFY(reset);
    Settings::instance().setValue(key::fontFamily, QStringLiteral("DejaVu Sans Mono"));
    QVERIFY(find<QLineEdit>(QStringLiteral("font"))->text().startsWith(QStringLiteral("DejaVu Sans Mono")));
    reset->click();
    QVERIFY(Settings::instance().text(key::fontFamily).isEmpty());
}

void TestPreferences::textFontPickerWritesKeys()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    auto *input = find<QLineEdit>(QStringLiteral("textFont"));
    QVERIFY(input);
    QCOMPARE(Settings::instance().text(key::textFontFamily), QString());
    QCOMPARE(Settings::instance().number(key::textFontSize), 0);

    QTest::mouseClick(input, Qt::LeftButton);
    auto *picker = prefsWindow()->findChild<QFontDialog *>();
    QVERIFY(picker);
    // Proportional fonts must be offered here.
    QVERIFY(!picker->testOption(QFontDialog::MonospacedFonts));
    QFont chosen = QApplication::font();
    chosen.setPointSize(14);
    picker->setCurrentFont(chosen);
    picker->accept();
    QCOMPARE(Settings::instance().text(key::textFontFamily), picker->currentFont().family());
    QCOMPARE(Settings::instance().number(key::textFontSize), 14);
    QVERIFY(input->text().endsWith(QStringLiteral(" 14")));
    QCOMPARE(theme::textFont().pointSize(), 14);
}

void TestPreferences::textFontResetRestoresDefault()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    Settings::instance().setValue(key::textFontFamily, QStringLiteral("Serif"));
    Settings::instance().setValue(key::textFontSize, 18);
    auto *reset = find<QAbstractButton>(QStringLiteral("textFontReset"));
    QVERIFY(reset);
    QVERIFY(!reset->isHidden());
    reset->click();
    QVERIFY(reset->isHidden());
    QCOMPARE(Settings::instance().text(key::textFontFamily), QString());
    QCOMPARE(Settings::instance().number(key::textFontSize), 0);
    // Default size: the UI font, 15% larger.
    QCOMPARE(theme::textFont().pointSizeF(), QApplication::font().pointSizeF() * 1.15);
}

void TestPreferences::lineSpacingWritesKey()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    auto *combo = find<QComboBox>(key::textLineHeight);
    QVERIFY(combo);
    QCOMPARE(combo->count(), 7);
    QCOMPARE(combo->currentText(), QStringLiteral("1.6"));
    QCOMPARE(Settings::instance().value(key::textLineHeight).toDouble(), 1.6);
    combo->setCurrentIndex(combo->findText(QStringLiteral("2.0")));
    QCOMPARE(Settings::instance().value(key::textLineHeight).toDouble(), 2.0);
    // Reading it back (stored as text in the INI file) selects the same item.
    Settings::instance().setValue(key::textLineHeight, 1.2);
    QCOMPARE(combo->currentText(), QStringLiteral("1.2"));
}

void TestPreferences::preferenceResetButton()
{
    auto *button = find<QToolButton>(QString::fromLatin1(key::lineNumbers) + QStringLiteral("Reset"));
    QVERIFY(button);
    QVERIFY(button->isHidden());
    Settings::instance().setValue(key::lineNumbers, false);
    QVERIFY(!button->isHidden());
    QVERIFY(button->toolTip().contains(QStringLiteral("On")));
    button->click();
    QCOMPARE(Settings::instance().flag(key::lineNumbers), true);
    QVERIFY(button->isHidden());
}

void TestPreferences::offListNumericValueIsSorted()
{
    Settings::instance().setValue(key::lineSpacing, 1.4);
    auto *combo = find<QComboBox>(key::lineSpacing);
    QVERIFY(combo);
    const int index = combo->findData(1.4);
    QVERIFY(index > 0 && index < combo->count() - 1);
    QCOMPARE(combo->itemData(index - 1).toDouble(), 1.35);
    QCOMPARE(combo->itemData(index + 1).toDouble(), 1.5);
}

// The old values counted the font's own line height, the new ones count its
// size: a stored old value becomes the listed value nearest the same look.
void TestPreferences::oldLineSpacingsAreConverted()
{
    const QString before = Settings::instance().fileName();
    const auto restore = qScopeGuard([&] { Settings::instance().useFile(before); });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    {
        QSettings old(path, QSettings::IniFormat);
        old.setValue(QStringLiteral("editor/lineSpacing"), 1.0);
        old.setValue(QStringLiteral("markdown/textLineHeight"), 2.0);
    }
    Settings::instance().useFile(path);
    const auto converted = [](double old, qreal base, const QFont &font) {
        const double wanted = old * base / QFontInfo(font).pixelSize();
        double best = 1.0;
        for (const double c : {1.0, 1.2, 1.35, 1.5, 1.6, 1.8, 2.0}) {
            if (qAbs(c - wanted) < qAbs(best - wanted))
                best = c;
        }
        return best;
    };
    const QFont editor = theme::editorFont();
    const QFont text = theme::textFont();
    const QFontMetricsF textMetrics(text);
    const qreal textBase = qCeil(textMetrics.ascent() + textMetrics.descent() + textMetrics.leading());
    QCOMPARE(Settings::instance().value(key::lineSpacing).toDouble(),
             converted(1.0, QFontMetricsF(editor).height(), editor));
    // Values above the list snap to its highest choice.
    const double big = Settings::instance().value(key::textLineHeight).toDouble();
    QCOMPARE(big, converted(2.0, textBase, text));
    QCOMPARE(big, 2.0);
    // The old keys are gone, so the conversion happens once.
    Settings::instance().sync();
    QSettings file(path, QSettings::IniFormat);
    QVERIFY(!file.contains(QStringLiteral("editor/lineSpacing")));
    QVERIFY(!file.contains(QStringLiteral("markdown/textLineHeight")));
    // A broken old value (none could choose 0) gives the default, and goes.
    {
        QSettings broken(dir.filePath(QStringLiteral("broken.ini")), QSettings::IniFormat);
        broken.setValue(QStringLiteral("editor/lineSpacing"), 0);
    }
    Settings::instance().useFile(dir.filePath(QStringLiteral("broken.ini")));
    QCOMPARE(Settings::instance().value(key::lineSpacing).toDouble(), 1.35);
    Settings::instance().sync();
    QVERIFY(!QSettings(dir.filePath(QStringLiteral("broken.ini")), QSettings::IniFormat)
                 .contains(QStringLiteral("editor/lineSpacing")));
    // A file with no old values gets the new defaults.
    Settings::instance().useFile(dir.filePath(QStringLiteral("fresh.ini")));
    QCOMPARE(Settings::instance().value(key::lineSpacing).toDouble(), 1.35);
    QCOMPARE(Settings::instance().value(key::textLineHeight).toDouble(), 1.6);
}

void TestPreferences::syncAnchorRow()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    auto *combo = find<QComboBox>(key::syncAnchor);
    QVERIFY(combo);
    QCOMPARE(combo->count(), 2);
    QCOMPARE(combo->currentText(), QStringLiteral("Top of the view"));
    combo->setCurrentIndex(combo->findText(QStringLiteral("Middle of the view")));
    QCOMPARE(Settings::instance().text(key::syncAnchor), QStringLiteral("middle"));
    Settings::instance().reset(key::syncAnchor);
}

void TestPreferences::lineSpacingChangesRenderedHeight()
{
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    QString paragraph;
    for (int i = 0; i < 60; ++i)
        paragraph += QStringLiteral("word%1 ").arg(i);
    doc.setPlainText(paragraph);
    MarkdownModel model(&doc);
    VisualView view(&model);
    view.resize(420, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    // The bottom of the painted text: a taller paragraph reaches further down.
    const auto textBottom = [&] {
        const QImage image = view.grab().toImage();
        const QRgb background = image.pixel(1, 1);
        for (int y = image.height() - 1; y >= 0; --y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixel(x, y) != background)
                    return y;
            }
        }
        return 0;
    };
    const auto bottomAt = [&](double lineHeight) {
        Settings::instance().setValue(key::textLineHeight, lineHeight);
        return textBottom();
    };
    const int tight = bottomAt(1.2);
    const int normal = bottomAt(1.6);
    const int loose = bottomAt(2.0);
    QVERIFY2(tight < normal && normal < loose, qPrintable(QStringLiteral("%1 %2 %3").arg(tight).arg(normal).arg(loose)));

    // A different text font is a different render, not a cached one.
    Settings::instance().setValue(key::textFontSize, 24);
    QVERIFY(textBottom() > normal);
}

void TestPreferences::encodingRows()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("save"));
    auto *guess = find<QAbstractButton>(key::guessEncoding);
    QVERIFY(guess);
    QVERIFY(guess->isChecked());
    guess->click();
    QVERIFY(!Settings::instance().flag(key::guessEncoding));

    auto *legacy = find<QComboBox>(key::legacyEncoding);
    QVERIFY(legacy);
    QCOMPARE(legacy->currentText(), QStringLiteral("Automatic (system)"));
    QVERIFY(legacy->findData(QStringLiteral("Big5")) >= 0);
    legacy->setCurrentIndex(legacy->findData(QStringLiteral("Big5")));
    QCOMPARE(Settings::instance().text(key::legacyEncoding), QStringLiteral("Big5"));
    QCOMPARE(encoding::legacy(), QStringLiteral("Big5"));
    legacy->setCurrentIndex(0);
    QCOMPARE(Settings::instance().text(key::legacyEncoding), QString());

    // Stored values of the new-file encoding keep working.
    auto *newFiles = find<QComboBox>(key::encoding);
    QVERIFY(newFiles);
    QCOMPARE(newFiles->count(), 3);
    for (const char *value : {"utf8", "utf8bom", "utf16le"})
        QVERIFY(newFiles->findData(QString::fromLatin1(value)) >= 0);
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

void TestPreferences::initialSizeKeepsButtonsVisible()
{
    PreferencesDialog::showPage(nullptr, QStringLiteral("markdown"));
    QWidget *window = prefsWindow();
    QVERIFY(window);
    QVERIFY(window->height() <= 520);
    const QRect available = window->screen()->availableGeometry();
    QVERIFY(window->height() <= available.height() - 120);
    QVERIFY(available.contains(window->frameGeometry()));
    auto *stack = window->findChild<QStackedWidget *>();
    QVERIFY(qobject_cast<QScrollArea *>(stack->currentWidget()));
    for (const QString &name : {QStringLiteral("help"), QStringLiteral("close")}) {
        auto *button = find<QAbstractButton>(name);
        QVERIFY(button);
        QVERIFY(button->isVisible());
        QVERIFY(window->rect().contains(QRect(button->mapTo(window, QPoint()), button->size())));
    }
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

// Recent files are in the settings: readable by the user alone, also after
// QSettings has written the file again.
void TestPreferences::settingsFileIsPrivate()
{
#ifdef Q_OS_UNIX
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.ini"));
    const QString before = Settings::instance().fileName();
    Settings::instance().useFile(path);
    const auto restore = qScopeGuard([&] { Settings::instance().useFile(before); });
    const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    const auto group = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther;
    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(QFile::permissions(path) & group, QFileDevice::Permissions());
    Settings::instance().addRecentFile(QStringLiteral("/tmp/a.md"));
    Settings::instance().setValue(key::zoom, 125);
    Settings::instance().sync();
    QVERIFY(QFile::permissions(path) & owner);
    QCOMPARE(QFile::permissions(path) & group, QFileDevice::Permissions());
#else
    QSKIP("POSIX permissions");
#endif
}

QTEST_MAIN(TestPreferences)
#include "test_preferences.moc"
