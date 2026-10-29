#include "MainWindow.h"
#include "Session.h"
#include "Theme.h"

#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
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
