#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("visnotepad"));
    QApplication::setApplicationDisplayName(QStringLiteral("Visual Notepad"));
    QApplication::setApplicationVersion(QStringLiteral(VN_VERSION));
    QApplication::setOrganizationName(QStringLiteral("visnotepad"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Visual Notepad"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Files to open."), QStringLiteral("[files...]"));
    parser.process(app);

    vn::theme::apply(app);

    vn::MainWindow window;
    const QStringList files = parser.positionalArguments();
    for (const QString &file : files)
        window.openFile(file);
    if (files.isEmpty())
        window.newFile();
    window.show();

    return app.exec();
}
