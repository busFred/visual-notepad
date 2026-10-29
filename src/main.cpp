#include "MainWindow.h"
#include "Session.h"
#include "Theme.h"

#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    // Our Qt is not the system's, so it has no desktop integration by
    // default. Use the desktop's file dialogs through the XDG portal, and
    // find the OS cursor theme (the bundled libxcb-cursor only searches its
    // build prefix).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    if (qEnvironmentVariableIsEmpty("XCURSOR_PATH"))
        qputenv("XCURSOR_PATH", "~/.local/share/icons:~/.icons:/usr/local/share/icons:/usr/share/icons:/usr/share/pixmaps");
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("visnotepad"));
    QApplication::setApplicationDisplayName(QStringLiteral("Visual Notepad"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/vn/app/visnotepad.svg")));
    QApplication::setApplicationVersion(QStringLiteral(VN_VERSION));
    QApplication::setOrganizationName(QStringLiteral("visnotepad"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Visual Notepad"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Files to open."), QStringLiteral("[files...]"));
    parser.process(app);

    vn::theme::apply(app);

    auto &session = vn::SessionManager::instance();
    session.start();
    const QStringList files = parser.positionalArguments();
    if (files.isEmpty()) {
        if (!session.restore()) {
            auto *window = new vn::MainWindow;
            window->setAttribute(Qt::WA_DeleteOnClose);
            window->newFile();
            window->show();
        }
    } else {
        // Only the given files open; unsaved work from the last session stays
        // in its backups for the next start without arguments.
        session.carryOverUnsaved();
        auto *window = new vn::MainWindow;
        window->setAttribute(Qt::WA_DeleteOnClose);
        for (const QString &file : files)
            window->openFile(file);
        window->show();
    }

    return app.exec();
}
