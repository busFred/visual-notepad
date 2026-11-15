#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

class QFileDevice;

namespace vn {

// Writes `bytes` to `path` in one step: to a temporary file next to it that
// then replaces the file (QSaveFile), so a failure never leaves it half
// written. `prepare` runs on the open temporary file before the write.
//
// On Windows the replacing rename (MoveFileEx) fails with ERROR_ACCESS_DENIED
// while anything else has the file open, whatever its sharing mode: an
// antivirus scanning a file just written (about one save in a few thousand on
// a machine with real-time scanning, for 10 to 30 ms), the search indexer, a
// backup or sync tool. So the write is tried again for a short while, as other
// editors do; each failed try leaves the file as it was and removes its
// temporary file.
//
// Safe on any thread. On failure returns false with the reason in `error`.
bool writeFileSafely(const QString &path, const QByteArray &bytes, QString *error,
                     const std::function<void(QFileDevice &)> &prepare = {});

} // namespace vn
