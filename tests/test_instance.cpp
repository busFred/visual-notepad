#include "DocumentTab.h"
#include "ExternalFiles.h"
#include "InstanceChannel.h"
#include "MainWindow.h"
#include "Session.h"
#include "Settings.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace vn;

class TestInstance : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void protocol();
    void routing();
    void routesPendingFile();
    void waitsForADialog();
#ifdef Q_OS_LINUX
    void simultaneousLaunches();
    void unreachableCopyKeepsItsSession();
#endif

private:
    QString write(const QString &name);
    QTemporaryDir m_dir;
};

void TestInstance::initTestCase()
{
    QVERIFY(m_dir.isValid());
    theme::apply(*qApp);
}

QString TestInstance::write(const QString &name)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("test\n") != 5)
        return {};
    return path;
}

void TestInstance::protocol()
{
    using namespace instanceRequest;
    const QStringList files{m_dir.filePath(QStringLiteral("r\u00e9sum\u00e9.md")),
                            m_dir.filePath(QStringLiteral("some file.txt"))};
    const QByteArray message = encode(files);
    const auto decoded = decode(message);
    QVERIFY(decoded);
    QCOMPARE(*decoded, files);
    const auto untitled = decode(encode({}));
    QVERIFY(untitled);
    QVERIFY(untitled->isEmpty());

    QVERIFY(encode({QStringLiteral("relative.txt")}).isEmpty());
    QVERIFY(encode({QStringLiteral(":/resource.txt")}).isEmpty());
    QVERIFY(encode({m_dir.filePath(QString(256 * 1024, u'x'))}).isEmpty());
    QVERIFY(!decode(message.chopped(1)));
    QVERIFY(!decode(message + 'x'));

    qint64 sent = 0;
    QVERIFY(decode(encode(files, 1234), &sent));
    QCOMPARE(sent, 1234);
    // A launch waits five seconds for its answer: one older is left alone.
    QVERIFY(isFresh(10'000, 10'000 + 3'000));
    QVERIFY(!isFresh(10'000, 10'000 + 4'500));
    QVERIFY(!isFresh(10'000 + 4'500, 10'000)); // (A clock set back.)

    QByteArray malformed = message;
    malformed[4] = 3; // Unknown protocol version.
    QVERIFY(!decode(malformed));
    malformed = message;
    malformed[5] = char(0xff); // Payload size over the limit.
    QVERIFY(!decode(malformed));
    malformed = message;
    malformed.replace(17, 3, "rel"); // A relative path.
    QVERIFY(!decode(malformed));
    malformed = message;
    malformed[18] = char(0xff); // Invalid UTF-8 inside an absolute path.
    QVERIFY(!decode(malformed));
    malformed = message;
    malformed.replace(17, 3, "\xef\xbb\xbf"); // A BOM is part of the supplied path.
    QVERIFY(!decode(malformed));
}

void TestInstance::routing()
{
    ExternalFiles router(*qApp);
    Settings &settings = Settings::instance();
    settings.reset(key::externalFilesWindow);
    const QString firstPath = write(QStringLiteral("first.txt"));
    const QString secondPath = write(QStringLiteral("second.txt"));
    QVERIFY(!firstPath.isEmpty());
    QVERIFY(!secondPath.isEmpty());

    MainWindow first;
    MainWindow second;
    first.showMinimized();
    second.show();
    QCoreApplication::processEvents();
    // Simulate the first being the most recently activated editor window
    // even though it is minimized and the second was created later.
    QEvent activate(QEvent::WindowActivate);
    QCoreApplication::sendEvent(&first, &activate);

    router.request({firstPath});
    QCOMPARE(first.tabCount(), 0);
    router.finishStartup();
    QCOMPARE(first.tabCount(), 1);
    QCOMPARE(first.currentTab()->path(), firstPath);
    QCOMPARE(second.tabCount(), 0);
    QVERIFY(!first.isMinimized());

    settings.setValue(key::externalFilesWindow, QStringLiteral("new"));
    router.request({secondPath, firstPath});
    const QList<MainWindow *> withFiles = SessionManager::instance().windows();
    QCOMPARE(withFiles.size(), 3);
    const std::unique_ptr<MainWindow> newWindow(withFiles.constLast());
    QCOMPARE(newWindow->tabCount(), 2);
    QCOMPARE(newWindow->tabs().at(0)->path(), secondPath);
    QCOMPARE(newWindow->tabs().at(1)->path(), firstPath);

    settings.reset(key::externalFilesWindow);
    router.request({});
    const QList<MainWindow *> withUntitled = SessionManager::instance().windows();
    QCOMPARE(withUntitled.size(), 4);
    const std::unique_ptr<MainWindow> untitled(withUntitled.constLast());
    QCOMPARE(untitled->tabCount(), 1);
    QVERIFY(untitled->currentTab()->path().isEmpty());
}

void TestInstance::routesPendingFile()
{
    const QString path = m_dir.filePath(QStringLiteral("pending.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray contents = QByteArray("A line of text in a large file.\n").repeated(20000);
    QCOMPARE(file.write(contents), contents.size());
    file.close();

    ExternalFiles router(*qApp);
    router.finishStartup();
    MainWindow first;
    first.show();
    QVERIFY(first.openFile(path));
    QVERIFY(first.isOpening());

    auto &settings = Settings::instance();
    settings.setValue(key::externalFilesWindow, QStringLiteral("new"));
    router.request({path});
    settings.reset(key::externalFilesWindow);
    const QList<MainWindow *> windows = SessionManager::instance().windows();
    QCOMPARE(windows.size(), 2);
    const std::unique_ptr<MainWindow> second(windows.constLast());
    QVERIFY(second->isOpening());
    QTRY_COMPARE_WITH_TIMEOUT(second->tabCount(), 1, 20000);
    QCOMPARE(first.tabCount(), 1);
    QCOMPARE(second->currentTab()->file(), first.currentTab()->file());
}

#ifdef Q_OS_LINUX
void TestInstance::simultaneousLaunches()
{
    const QString firstPath = write(QStringLiteral("launch1.txt"));
    const QString secondPath = write(QStringLiteral("launch2.txt"));
    QVERIFY(!firstPath.isEmpty());
    QVERIFY(!secondPath.isEmpty());

    // XDG directories isolate both settings and recovery files of the children.
    const QString data = m_dir.filePath(QStringLiteral("launch-data"));
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), m_dir.filePath(QStringLiteral("launch-config")));
    environment.insert(QStringLiteral("XDG_DATA_HOME"), data);
    environment.insert(QStringLiteral("VN_INSTANCE_NAME"), QStringLiteral("test-instance"));

    QProcess first;
    QProcess second;
    first.setProcessEnvironment(environment);
    second.setProcessEnvironment(environment);
    first.setWorkingDirectory(m_dir.path());
    first.start(QStringLiteral(VN_APP_PATH), {QFileInfo(firstPath).fileName()});
    second.start(QStringLiteral(VN_APP_PATH), {secondPath});
    QVERIFY2(first.waitForStarted(), qPrintable(first.errorString()));
    QVERIFY2(second.waitForStarted(), qPrintable(second.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(first.state() == QProcess::NotRunning || second.state() == QProcess::NotRunning, 10000);
    QProcess &primary = first.state() == QProcess::NotRunning ? second : first;
    QProcess &forwarded = &primary == &first ? second : first;
    QCOMPARE(primary.state(), QProcess::Running);
    QCOMPARE(forwarded.exitStatus(), QProcess::NormalExit);
    QVERIFY2(forwarded.exitCode() == 0, forwarded.readAllStandardError().constData());
    // The two first launches agreed on one key (else both would be primaries,
    // above), kept where only this user can read it.
    QFile key(m_dir.filePath(QStringLiteral("launch-config/visnotepad/instance.key")));
    QVERIFY(key.exists());
    QCOMPARE(key.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions());
    QVERIFY(key.open(QIODevice::ReadOnly));
    QCOMPARE(key.readAll().size(), 64);

    QProcess version;
    version.setProcessEnvironment(environment);
    version.start(QStringLiteral(VN_APP_PATH), {QStringLiteral("--version")});
    QVERIFY(version.waitForFinished());
    QCOMPARE(version.exitStatus(), QProcess::NormalExit);
    QCOMPARE(version.exitCode(), 0);
    QVERIFY(version.readAllStandardOutput().startsWith("visnotepad "));

    const auto sessionTabs = [&data] {
        QFile session(data + QStringLiteral("/visnotepad/visnotepad/session.json"));
        if (!session.open(QIODevice::ReadOnly))
            return QJsonArray();
        const auto windows = QJsonDocument::fromJson(session.readAll()).object().value(QStringLiteral("windows")).toArray();
        if (windows.size() != 1)
            return QJsonArray();
        return windows.at(0).toObject().value(QStringLiteral("tabs")).toArray();
    };
    QTRY_COMPARE_WITH_TIMEOUT(sessionTabs().size(), 2, 10000);
    const auto tabs = sessionTabs();
    QCOMPARE(tabs.at(0).toObject().value(QStringLiteral("path")).toString(), &primary == &first ? firstPath : secondPath);
    QCOMPARE(tabs.at(1).toObject().value(QStringLiteral("path")).toString(), &primary == &first ? secondPath : firstPath);
    primary.terminate();
    QVERIFY(primary.waitForFinished());
    QCOMPARE(primary.exitStatus(), QProcess::NormalExit);
    QCOMPARE(primary.exitCode(), 0);
}
#endif

// Files handed over while a dialog asks (such as whether to save before a
// window closes) wait for it; if the app quits first, they are what a new
// launch is given.
void TestInstance::waitsForADialog()
{
    ExternalFiles router(*qApp);
    Settings::instance().reset(key::externalFilesWindow);
    const QString path = write(QStringLiteral("waiting.txt"));
    MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QEvent activate(QEvent::WindowActivate);
    QCoreApplication::sendEvent(&window, &activate);
    router.finishStartup();

    auto dialog = std::make_unique<QMessageBox>(QMessageBox::Question, QStringLiteral("Save?"), QStringLiteral("Save?"));
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->show();
    QTRY_VERIFY(QApplication::activeModalWidget());
    router.request({path});
    QTest::qWait(250);
    QCOMPARE(window.tabCount(), 0);
    dialog.reset();
    QTRY_COMPARE(window.tabCount(), 1);
    QCOMPARE(window.currentTab()->path(), path);

    dialog = std::make_unique<QMessageBox>(QMessageBox::Question, QStringLiteral("Save?"), QStringLiteral("Save?"));
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->show();
    QTRY_VERIFY(QApplication::activeModalWidget());
    router.request({path, path});
    QCOMPARE(router.takeWaiting(), (QStringList{path, path}));
    QVERIFY(router.takeWaiting().isEmpty());
}

#ifdef Q_OS_LINUX
// Two launches on one session whose channels can't meet: the second's
// runtime folder is too deep for a socket path, so it has no channel name at
// all. It runs on its own, and the first's session stays the first's alone:
// the lock is the session's, not the channel's.
void TestInstance::unreachableCopyKeepsItsSession()
{
    const QString firstPath = write(QStringLiteral("own1.txt"));
    const QString secondPath = write(QStringLiteral("own2.txt"));
    const QString data = m_dir.filePath(QStringLiteral("own-data"));
    const auto environment = [&](const QString &runtime) {
        QDir().mkpath(runtime);
        QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), m_dir.filePath(QStringLiteral("own-config")));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), data);
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        environment.insert(QStringLiteral("VN_INSTANCE_NAME"), QStringLiteral("own-session"));
        return environment;
    };
    QProcess first;
    first.setProcessEnvironment(environment(m_dir.filePath(QStringLiteral("run-a"))));
    first.start(QStringLiteral(VN_APP_PATH), {firstPath});
    QVERIFY2(first.waitForStarted(), qPrintable(first.errorString()));
    const auto sessionPaths = [&data] {
        QStringList paths;
        QFile session(data + QStringLiteral("/visnotepad/visnotepad/session.json"));
        if (!session.open(QIODevice::ReadOnly))
            return paths;
        for (const QJsonValue &window : QJsonDocument::fromJson(session.readAll()).object().value(QStringLiteral("windows")).toArray()) {
            for (const QJsonValue &tab : window.toObject().value(QStringLiteral("tabs")).toArray())
                paths.append(tab.toObject().value(QStringLiteral("path")).toString());
        }
        return paths;
    };
    QTRY_COMPARE_WITH_TIMEOUT(sessionPaths(), QStringList{firstPath}, 10000);

    QProcess second;
    second.setProcessEnvironment(environment(m_dir.filePath(QStringLiteral("run-b/") + QString(110, u'd'))));
    second.start(QStringLiteral(VN_APP_PATH), {secondPath});
    QVERIFY2(second.waitForStarted(), qPrintable(second.errorString()));
    // It can't reach the first and the lock is taken: it opens its file
    // itself, without the session, and stays.
    QTest::qWait(1500);
    QCOMPARE(second.state(), QProcess::Running);
    QCOMPARE(first.state(), QProcess::Running);
    QCOMPARE(sessionPaths(), QStringList{firstPath});

    second.terminate();
    QVERIFY(second.waitForFinished());
    QCOMPARE(sessionPaths(), QStringList{firstPath});
    first.terminate();
    QVERIFY(first.waitForFinished());
}
#endif

QTEST_MAIN(TestInstance)
#include "test_instance.moc"
