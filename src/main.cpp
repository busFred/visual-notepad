#include "DesktopSettings.h"
#include "MainWindow.h"
#include "Session.h"
#include "Theme.h"

#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>

#ifdef Q_OS_WIN
#include <cstdio>
#include <cstring>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif


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
    if (qEnvironmentVariableIsEmpty("XCURSOR_SIZE")) {
        // Else libxcb-cursor guesses Xft.dpi * 16 / 72 (21 px at 96 dpi), so
        // the cursor would change size when it enters our windows.
        if (const int size = vn::desktop::cursorSize())
            qputenv("XCURSOR_SIZE", QByteArray::number(size));
    }
#endif
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // QCommandLineParser reports --version in a message box in a GUI program,
    // which blocks scripts and smoke tests. Answer (after the
    // platform plugin loaded, so that a smoke test covers it) on the parent's console.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            if (AttachConsole(ATTACH_PARENT_PROCESS))
                std::freopen("CONOUT$", "w", stdout);
            std::printf("visnotepad %s\n", VN_VERSION);
            std::fflush(stdout);
            return 0;
        }
    }
#endif
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
