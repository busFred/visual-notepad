#include "CustomizeColorsDialog.h"
#include "Settings.h"
#include "Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace vn;
using theme::Role;

class TestTheme : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();

    void defaultIsModern();
    void switchingThemeIsLive();
    void overrideSurvivesThemeSwitch();
    void resetRestoresDefault();
    void notifierFiresOnChange();
    void legacyMatchesOriginalColors();
    void dialogCancelWritesNothing();
    void dialogApplyWritesOverride();
    void dialogResetAndDefaults();
    void dialogHitTest();

private:
    QTemporaryDir m_dir;
    int m_count = 0;
};

void TestTheme::init()
{
    QVERIFY(m_dir.isValid());
    Settings::instance().useFile(m_dir.filePath(QStringLiteral("settings%1.ini").arg(m_count++)));
    theme::apply(*qApp);
}

void TestTheme::defaultIsModern()
{
    QCOMPARE(theme::currentThemeId(), QStringLiteral("modern"));
    QCOMPARE(QApplication::palette().color(QPalette::Base), QColor(0xFFFFFF));
    QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(0xF9F9F9));
    QCOMPARE(theme::color(Role::menuHover), QColor(0xE5E5E5));
}

void TestTheme::switchingThemeIsLive()
{
    Settings::instance().setValue(key::theme, QStringLiteral("legacy"));
    QCOMPARE(QApplication::palette().color(QPalette::Base), QColor(0xFFFFFF));
    QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(0xDDDAD4));
    QCOMPARE(theme::color(Role::menuBar), QColor(0xD4D0C8));
    QVERIFY(qApp->styleSheet().contains(QStringLiteral("#d4d0c8")));
    Settings::instance().setValue(key::theme, QStringLiteral("modern"));
    QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(0xF9F9F9));
    QVERIFY(!qApp->styleSheet().contains(QStringLiteral("#d4d0c8")));
}

void TestTheme::overrideSurvivesThemeSwitch()
{
    Settings &settings = Settings::instance();
    settings.setColorOverride(QStringLiteral("modern"), QStringLiteral("editor"), QColor(0x102030));
    QCOMPARE(theme::color(Role::editor), QColor(0x102030));
    QCOMPARE(QApplication::palette().color(QPalette::Base), QColor(0x102030));
    QCOMPARE(settings.overriddenColors(QStringLiteral("modern")), QStringList{QStringLiteral("editor")});

    settings.setValue(key::theme, QStringLiteral("legacy"));
    QCOMPARE(theme::color(Role::editor), QColor(0xFFFFFF));
    QVERIFY(settings.overriddenColors(QStringLiteral("legacy")).isEmpty());

    settings.setValue(key::theme, QStringLiteral("modern"));
    QCOMPARE(theme::color(Role::editor), QColor(0x102030));
}

void TestTheme::resetRestoresDefault()
{
    Settings &settings = Settings::instance();
    settings.setColorOverride(QStringLiteral("modern"), QStringLiteral("accent"), QColor(0xFF0000));
    QCOMPARE(theme::color(Role::accent), QColor(0xFF0000));
    settings.resetColorOverride(QStringLiteral("modern"), QStringLiteral("accent"));
    QCOMPARE(theme::color(Role::accent), QColor(0x005FB8));
    QVERIFY(settings.overriddenColors(QStringLiteral("modern")).isEmpty());
}

void TestTheme::notifierFiresOnChange()
{
    QSignalSpy colors(&theme::notifier(), &theme::Notifier::changed);
    QSignalSpy keys(&Settings::instance(), &Settings::changed);
    Settings::instance().setColorOverride(QStringLiteral("modern"), QStringLiteral("gripDots"), QColor(0x123456));
    QCOMPARE(colors.count(), 1);
    QCOMPARE(keys.last().first().toString(), QStringLiteral("colors/modern/gripDots"));
    Settings::instance().setValue(key::theme, QStringLiteral("legacy"));
    QCOMPARE(colors.count(), 2);
    // Overrides of another theme do not change what is shown, but are cheap to re-apply.
    {
        const theme::UpdateBlocker blocker;
        Settings::instance().setColorOverride(QStringLiteral("legacy"), QStringLiteral("text"), QColor(0x111111));
        Settings::instance().setColorOverride(QStringLiteral("legacy"), QStringLiteral("accent"), QColor(0x222222));
        QCOMPARE(colors.count(), 2);
    }
    QCOMPARE(colors.count(), 3);
}

void TestTheme::legacyMatchesOriginalColors()
{
    Settings::instance().setValue(key::theme, QStringLiteral("legacy"));
    // Roles split from the old shared colours keep their old value.
    for (const Role role : {Role::menuBar, Role::menu, Role::tabStrip, Role::statusBar})
        QCOMPARE(theme::color(role), QColor(0xD4D0C8));
    QCOMPARE(theme::color(Role::activeTab), theme::color(Role::editor));
    QCOMPARE(theme::color(Role::inactiveTab), theme::color(Role::sideBar));
    QCOMPARE(theme::color(Role::selectionText), theme::color(Role::accent));
    QCOMPARE(QApplication::palette().color(QPalette::HighlightedText), QColor(0x0A246A));
}

void TestTheme::dialogCancelWritesNothing()
{
    CustomizeColorsDialog dialog;
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Customize Colors — Modern Windows"));
    QPushButton *apply = dialog.buttonBox()->button(QDialogButtonBox::Apply);
    QVERIFY(!apply->isEnabled());
    dialog.stage(Role::editor, QColor(0x445566));
    QVERIFY(dialog.hasStagedChanges());
    QVERIFY(apply->isEnabled());
    QCOMPARE(dialog.color(Role::editor), QColor(0x445566));
    // Staging does not touch the app.
    QCOMPARE(theme::color(Role::editor), QColor(0xFFFFFF));
    dialog.buttonBox()->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(Settings::instance().overriddenColors(QStringLiteral("modern")).isEmpty());
    QCOMPARE(theme::color(Role::editor), QColor(0xFFFFFF));
}

void TestTheme::dialogApplyWritesOverride()
{
    CustomizeColorsDialog dialog;
    dialog.stage(Role::editor, QColor(0x445566));
    dialog.buttonBox()->button(QDialogButtonBox::Apply)->click();
    QCOMPARE(Settings::instance().colorOverride(QStringLiteral("modern"), QStringLiteral("editor")), QColor(0x445566));
    QCOMPARE(theme::color(Role::editor), QColor(0x445566));
    QVERIFY(!dialog.hasStagedChanges());
    QVERIFY(!dialog.buttonBox()->button(QDialogButtonBox::Apply)->isEnabled());

    // OK applies too.
    dialog.stage(Role::text, QColor(0x010203));
    dialog.buttonBox()->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(theme::color(Role::text), QColor(0x010203));
}

void TestTheme::dialogResetAndDefaults()
{
    Settings::instance().setColorOverride(QStringLiteral("modern"), QStringLiteral("editor"), QColor(0x445566));
    CustomizeColorsDialog dialog;
    QCOMPARE(dialog.color(Role::editor), QColor(0x445566));
    // Staging the default removes the override on Apply.
    dialog.stage(Role::editor, QColor(0xFFFFFF));
    dialog.applyChanges();
    QVERIFY(Settings::instance().overriddenColors(QStringLiteral("modern")).isEmpty());
    QCOMPARE(theme::color(Role::editor), QColor(0xFFFFFF));
    // Staging the colour already in effect is not a change.
    dialog.stage(Role::editor, QColor(0xFFFFFF));
    QVERIFY(!dialog.hasStagedChanges());
}

void TestTheme::dialogHitTest()
{
    CustomizeColorsDialog dialog;
    const auto items = CustomizeColorsDialog::items();
    auto label = [&](const QPoint &p) { return items.value(dialog.itemAt(p)).label; };
    // Empty editor space, in the lower right of the preview.
    QCOMPARE(label(QPoint(520, 150)), QStringLiteral("Editor"));
    QCOMPARE(label(QPoint(300, 8)), QString()); // Outside the mini window.
    QCOMPARE(label(QPoint(300, 180)), QStringLiteral("Editor"));
    QCOMPARE(label(QPoint(300, 12 + 10)), QStringLiteral("Menu bar"));
    QCOMPARE(label(QPoint(590, 222)), QString());
    QCOMPARE(label(QPoint(300, 208)), QStringLiteral("Status bar"));

    // Clicking the preview selects the item in the list.
    QTest::mouseClick(dialog.preview(), Qt::LeftButton, {}, QPoint(520, 150));
    QCOMPARE(items.at(dialog.currentItem()).label, QStringLiteral("Editor"));
    QTest::mouseClick(dialog.preview(), Qt::LeftButton, {}, QPoint(300, 208));
    QCOMPARE(items.at(dialog.currentItem()).label, QStringLiteral("Status bar"));
}

QTEST_MAIN(TestTheme)
#include "test_theme.moc"
