// One open file, shared by every tab that shows it, like a VS Code text
// model: the QTextDocument with its highlighter and Markdown model, and the
// per-file state that goes with it (path, encoding, line ending, language,
// indentation, link diagnostics, modified state and what is on disk).
//
// Files with a path are kept in a process-wide registry keyed by canonical
// path, so all windows share one TextFile per file; tabs hold it through a
// shared_ptr and it goes away with the last one. Untitled files are never
// shared through the registry.
//
// Changes on disk are watched: a file without unsaved changes reloads in
// place (as one undoable edit that keeps the carets, like VS Code); a
// modified one is only marked changedOnDisk and a later save reports a
// conflict instead of overwriting. A deleted file is marked orphaned after a
// short recheck and recovers when the file comes back.
#pragma once

#include "Encodings.h"
#include "editor/Indentation.h"
#include "editor/TextView.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

class QTextDocument;
class QTimer;

namespace KSyntaxHighlighting {
class SyntaxHighlighter;
} // namespace KSyntaxHighlighting

namespace vn {

class DocumentTab;
class MarkdownModel;
// Private to TextFile.cpp: a decoded file, and the worker of loadAsync().
namespace load {
struct Decoded;
struct Job;
} // namespace load

class TextFile : public QObject, public std::enable_shared_from_this<TextFile>
{
    Q_OBJECT

public:
    enum class LineEnding { LF, CRLF };
    using Encoding = QString;

    // A new untitled file, not in the registry.
    static std::shared_ptr<TextFile> create();
    // The registered file for `path` (any spelling of it), or null.
    static std::shared_ptr<TextFile> find(const QString &path);
    // Every live file, registered or not.
    static QList<TextFile *> all();
    // Looks at every file on disk again soon; for window activation.
    static void checkAllOnDisk();

    ~TextFile() override;

    // Returns false and sets errorString() on failure. Loading registers the
    // file under its path unless another file already is.
    bool load(const QString &path);
    // load() for a file nobody shows yet (no views, no text), without
    // blocking: a worker thread reads, decodes and builds the document, and
    // loadFinished() follows from the event loop; until then the file is
    // untouchable (it has no text, path or registry entry, and must not be
    // given to a view). Meant for files too big to load synchronously
    // (largeLoadBytes). On failure errorString() says why.
    void loadAsync(const QString &path);
    // Whether loadAsync() is still running.
    bool isLoading() const;
    // Stops loadAsync() and waits for the worker (a few ms); no loadFinished()
    // follows and the file stays empty. Also what destroying the file does.
    void cancelLoad();
    // Whether a loadAsync() of `path` (any spelling of it) is running.
    static bool isLoadingPath(const QString &path);
    // The size from which callers should prefer loadAsync() to load().
    static constexpr qint64 largeLoadBytes = 1024 * 1024;
    bool reopenWithEncoding(const Encoding &encoding);
    // Unsaved content from hot exit; the file is marked modified. The disk
    // baseline for conflict detection is what is on disk now unless
    // setDiskBaseline() is called afterwards.
    void loadBackup(const QString &path, const QString &content, const Encoding &encoding, LineEnding lineEnding);
    // Fails with hasSaveConflict() set when `path` is this file's path and
    // its content on disk changed since it was loaded or last saved, unless
    // `overwrite`. Saving under another path moves the file (and every tab
    // showing it) to that path; that fails if the path is open as another
    // file.
    bool save(const QString &path, bool overwrite = false);
    // Reloads from disk, discarding unsaved changes, as one undoable edit.
    bool revert();

    QString path() const { return m_path; }
    QString displayName() const;
    QString errorString() const { return m_error; }
    QTextDocument *document() const { return m_doc; }
    QString text() const;
    bool isModified() const;
    bool isMarkdown() const;
    QString languageName() const;
    QString definitionName() const;
    void setLanguage(const QString &definitionName);
    bool languageOverridden() const { return m_languageOverridden; }
    LineEnding lineEnding() const { return m_lineEnding; }
    void setLineEnding(LineEnding lineEnding);
    Encoding encoding() const { return m_encoding; }
    void setEncoding(const Encoding &encoding);

    // Brings what was deferred while no view was visible up to date; a view
    // calls it when it is shown.
    void applyPendingSettings();
    indentation::Settings indentation() const { return m_indentation; }
    bool indentationOverridden() const { return m_indentationOverridden; }
    void setIndentation(const indentation::Settings &indentation);
    void detectIndentation();
    void convertIndentation(bool toSpaces);

    const QList<TextView::Diagnostic> &diagnostics() const { return m_diagnostics; }
    // Created on first use, dropped when the file stops being Markdown
    // (after definitionChanged, so views can let go of it first).
    MarkdownModel *markdownModel();

    // The file was deleted (or moved away) on disk.
    bool isOrphaned() const { return m_orphaned; }
    // The file changed on disk while this one had unsaved changes.
    bool isChangedOnDisk() const { return m_changedOnDisk; }
    // The last save failed because the disk content changed.
    bool hasSaveConflict() const { return m_saveConflict; }
    // What conflict detection compares the disk against.
    struct DiskState
    {
        bool known = false;
        QDateTime modified;
        qint64 size = 0;
        QByteArray hash;
    };
    DiskState diskBaseline() const { return m_disk; }
    void setDiskBaseline(const DiskState &state);
    // Compares the disk with the baseline: now, or ~100 ms later (as after a
    // change notification or window activation). Scheduled checks of one file
    // are at least half a second apart, so a file written continuously
    // (a growing log) is not re-read and reloaded on every write; the check
    // after the last write still comes.
    void checkDisk();
    void scheduleDiskCheck();

    // Tabs showing this file that are not being closed.
    QList<DocumentTab *> views() const { return m_views; }
    void addView(DocumentTab *view);
    void removeView(DocumentTab *view);

    // Hot exit backup file name, shared by all tabs of the file.
    QString backupName() const { return m_backupName; }
    void setBackupName(const QString &name) { m_backupName = name; }
    // Counts text changes (edits, undo/redo, reloads; not highlighting), so
    // a backup or an outline can tell whether it is still current.
    quint64 changeSerial() const { return m_changeSerial; }
    // changeSerial() when the backup was last written.
    quint64 backupSerial() const { return m_backupSerial; }
    void setBackupSerial(quint64 serial) { m_backupSerial = serial; }
    // False for files too large to highlight without blocking (see
    // updateHighlighting()), whatever view is shown.
    bool isHighlighted() const { return m_highlighted; }
    // Whether the highlighter is running on the document. A file is only
    // highlighted while a view shows its source (or it has no views), and not
    // before one does: see syncHighlighting().
    bool isHighlighting() const;
    // Starts the highlighter, if the file may be highlighted and a view shows
    // its source now, with a view that just came to. Highlights the whole
    // document at once, without a text change.
    void syncHighlighting();

Q_SIGNALS:
    // loadAsync() ended; `ok` as load() returns it.
    void loadFinished(bool ok);
    void modificationChanged(bool modified);
    void contentsChanged();
    // The text was replaced wholesale (load, backup, encoding reopen): views
    // put their caret at the start.
    void contentReset();
    // Encoding, line ending or language changed.
    void formatChanged();
    // The syntax definition may have changed; sent before formatChanged.
    void definitionChanged();
    void indentationChanged();
    void pathChanged();
    void diagnosticsChanged();
    // Orphaned or changed-on-disk state changed.
    void diskStateChanged();

private:
    TextFile();
    bool applyBytes(const QString &path, const QByteArray &bytes, const Encoding &encoding);
    // The part of loading that follows decoding, with the text already in the
    // document (loadAsync) or still to be set.
    void applyDecoded(const QString &path, load::Decoded &decoded, bool documentBuilt);
    void finishLoad();
    // Replaces the text by the changed middle only, as one undo step.
    void replaceText(const QString &content);
    void setPath(const QString &path);
    void updateDefinition();
    indentation::Settings defaultIndentation() const;
    void setEffectiveIndentation(const indentation::Settings &indentation);
    void refreshIndentation();
    void scheduleLinkCheck();
    void checkLinks();
    QList<TextView::Diagnostic> brokenLinks() const;
    void setOrphaned(bool orphaned);
    void setChangedOnDisk(bool changed);
    void rememberDisk(const QByteArray &bytes);
    // checkDisk() without updating the watches; the registry does that once
    // for a batch of checks.
    void compareWithDisk();
    void updateHighlighting(bool highlightable);
    bool wantsHighlighting() const;
    friend class TextFileRegistry;

    QTextDocument *m_doc;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter;
    MarkdownModel *m_model = nullptr;
    QTimer *m_linkTimer;
    QList<TextView::Diagnostic> m_diagnostics;
    QList<DocumentTab *> m_views;
    std::unique_ptr<load::Job> m_load;
    bool m_indentationStale = false;
    QString m_path;
    QString m_error;
    QString m_backupName;
    LineEnding m_lineEnding = LineEnding::LF;
    Encoding m_encoding = encoding::utf8;
    bool m_languageOverridden = false;
    QString m_languageOverride;
    indentation::Settings m_indentation;
    bool m_indentationOverridden = false;
    DiskState m_disk;
    bool m_orphaned = false;
    bool m_changedOnDisk = false;
    bool m_saveConflict = false;
    bool m_highlighted = true;
    // Cached key::validateLinks, read on every keystroke otherwise.
    bool m_validateLinks = false;
    quint64 m_changeSerial = 0;
    quint64 m_backupSerial = 0;
    // When the disk was last compared (TextFileRegistry's clock), -1 if never.
    qint64 m_lastDiskCheck = -1;
};

} // namespace vn
