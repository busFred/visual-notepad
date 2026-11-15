#include "SafeWrite.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QSaveFile>
#include <QThread>

#include <algorithm>
#include <optional>

namespace vn {

namespace {

#ifdef Q_OS_WIN
// Many times what a scan of a file just written takes on a busy machine
// (tens of milliseconds); short enough that a file held for good (open in
// a program that locks it) keeps the window waiting only briefly per save.
constexpr int retryForMs = 1000;
#else
// A failing rename does not clear up by itself here.
constexpr int retryForMs = 0;
#endif

} // namespace

bool writeFileSafely(const QString &path, const QByteArray &bytes, QString *error,
                     const std::function<void(QFileDevice &)> &prepare)
{
    // From the first failed rename: writing a large file may take longer.
    std::optional<QDeadlineTimer> deadline;
    int pauseMs = 10;
    for (;;) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
        if (prepare)
            prepare(file);
        if (file.write(bytes) == bytes.size() && file.commit())
            return true;
        // The temporary file is gone and `path` is as it was.
        if (file.error() == QFileDevice::RenameError && !deadline)
            deadline = QDeadlineTimer(retryForMs);
        if (file.error() != QFileDevice::RenameError || deadline->hasExpired()) {
            if (error) {
                *error = file.errorString();
                if (file.error() == QFileDevice::RenameError && retryForMs > 0) {
                    *error = QCoreApplication::translate(
                                 "vn::SafeWrite",
                                 "%1\nAnother program (an antivirus, backup or sync tool, for example) "
                                 "may be using the file. Try again in a moment.")
                                 .arg(file.errorString());
                }
            }
            return false;
        }
        QThread::msleep(std::min<qint64>(pauseMs, std::max<qint64>(1, deadline->remainingTime())));
        pauseMs = std::min(pauseMs * 2, 200);
    }
}

} // namespace vn
