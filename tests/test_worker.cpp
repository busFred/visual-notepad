// Linked into every test program.
#include "Settings.h"
#include "markdown/MathRenderer.h"

#include <QCoreApplication>
#include <QTemporaryDir>

namespace {

// Formulas are drawn by the app run as a worker (see MathRenderer.h), and a
// test program is not the app.
const bool named = (vn::math::setWorkerProgram(QString::fromUtf8(VN_APP_PATH)), true);

// No test reads or writes the user's settings file. (A test may name a file
// of its own afterwards; one that did not, test_highlighting, reset the
// user's Markdown default mode each time it ran.)
void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

} // namespace
