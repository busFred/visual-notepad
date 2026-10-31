#include "TextFile.h"

#include "DocumentTab.h"

#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/MarkdownEditing.h"
#include "markdown/MarkdownModel.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace vn {

namespace {

// Like VS Code's recheck before marking a model orphaned (network shares
// report deletes that did not happen), and enough to let a burst of change
// notifications from one write settle.
constexpr int diskCheckDelayMs = 100;
// Scheduled checks of one file are at least this far apart: each one that
// finds a change reads, hashes and reloads the whole file.
constexpr int diskRecheckIntervalMs = 500;

// Above either limit a file is not syntax highlighted. QSyntaxHighlighter
// highlights the whole document synchronously when the text is set, which
// blocks the window for seconds on files this large. The language (and so
// Markdown editing and the visual view) is unaffected; the source view just
// shows plain text. Decided on each load, not while typing.
constexpr qsizetype largeFileChars = 5 * 1024 * 1024;
constexpr qsizetype largeFileLines = 100'000;

QString keyFor(const QString &path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

QString watchPathFor(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// SHA-1 stays: session.json stores it ("diskHash") to notice changes made
// while the app was not running, and it runs at disk-read speed.
QByteArray hashOf(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
}

qint64 now()
{
    static const QElapsedTimer clock = [] {
        QElapsedTimer timer;
        timer.start();
        return timer;
    }();
    return clock.elapsed();
}

// The text with "\r\n" turned into "\n", and the line ending style the
// first line ending decides (used on save).
void normalizeLineEndings(QString &content, TextFile::LineEnding &lineEnding)
{
    const qsizetype lf = content.indexOf(u'\n');
    lineEnding = (lf > 0 && content.at(lf - 1) == u'\r') ? TextFile::LineEnding::CRLF : TextFile::LineEnding::LF;
    // Most files have no CR at all; replace() would scan (and may copy) anyway.
    if (content.contains(u'\r'))
        content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

} // namespace

// Every live TextFile, the path registry and the one file system watcher
// they share (each watcher costs an inotify instance on Linux).
class TextFileRegistry : public QObject
{
public:
    // Never destroyed: files may outlive the application object.
    static TextFileRegistry &instance()
    {
        static auto *registry = new TextFileRegistry;
        return *registry;
    }

    void add(TextFile *file) { m_files.append(file); }

    void remove(TextFile *file)
    {
        m_files.removeAll(file);
        m_pendingChecks.remove(file);
        unregister(file);
        updateWatches();
    }

    // Checks `file` against the disk once it is due: diskCheckDelayMs from
    // now, and no sooner than diskRecheckIntervalMs after its last check.
    void scheduleCheck(TextFile *file)
    {
        if (m_pendingChecks.contains(file))
            return;
        qint64 due = now() + diskCheckDelayMs;
        if (file->m_lastDiskCheck >= 0)
            due = std::max(due, file->m_lastDiskCheck + diskRecheckIntervalMs);
        m_pendingChecks.insert(file, due);
        armCheckTimer();
    }

    void cancelCheck(TextFile *file)
    {
        if (m_pendingChecks.remove(file))
            armCheckTimer();
    }

    TextFile *find(const QString &path) const { return m_byKey.value(keyFor(path)); }

    // Registers the file under its current path if no other file has it.
    void rekey(TextFile *file)
    {
        unregister(file);
        const QString key = keyFor(file->path());
        if (!key.isEmpty() && !m_byKey.contains(key))
            m_byKey.insert(key, file);
    }

    QList<TextFile *> files() const { return m_files; }

    // Watches every file that exists and, for deleted ones, their directory
    // so a recreated file is noticed.
    void updateWatches()
    {
        QSet<QString> files;
        QSet<QString> dirs;
        files.reserve(m_files.size());
        for (const TextFile *file : std::as_const(m_files)) {
            if (file->path().isEmpty())
                continue;
            const QString path = watchPathFor(file->path());
            const QFileInfo info(path);
            if (info.exists())
                files.insert(path);
            else if (QFileInfo::exists(info.absolutePath()))
                dirs.insert(info.absolutePath());
        }
        const QStringList watchedFileList = m_watcher->files();
        const QStringList watchedDirList = m_watcher->directories();
        const QSet<QString> watchedFiles(watchedFileList.cbegin(), watchedFileList.cend());
        const QSet<QString> watchedDirs(watchedDirList.cbegin(), watchedDirList.cend());
        for (const QString &path : watchedFiles) {
            if (!files.contains(path))
                m_watcher->removePath(path);
        }
        for (const QString &path : watchedDirs) {
            if (!dirs.contains(path))
                m_watcher->removePath(path);
        }
        for (const QString &path : std::as_const(files)) {
            if (!watchedFiles.contains(path))
                m_watcher->addPath(path);
        }
        for (const QString &path : std::as_const(dirs)) {
            if (!watchedDirs.contains(path))
                m_watcher->addPath(path);
        }
    }

    // A save replaced the file by a new one (QSaveFile renames over it); a
    // watch on the old inode would never fire again.
    void rearm(const QString &path)
    {
        m_watcher->removePath(watchPathFor(path));
        updateWatches();
    }

private:
    TextFileRegistry()
        : m_watcher(new QFileSystemWatcher(this))
        , m_checkTimer(new QTimer(this))
    {
        m_checkTimer->setSingleShot(true);
        // A coarse timer may fire early, before the earliest check is due.
        m_checkTimer->setTimerType(Qt::PreciseTimer);
        connect(m_checkTimer, &QTimer::timeout, this, &TextFileRegistry::runDueChecks);
        connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
            // Writes by rename leave the watch on the old file; drop it and
            // let the check add the path back.
            m_watcher->removePath(path);
            for (TextFile *file : std::as_const(m_files)) {
                if (!file->path().isEmpty() && watchPathFor(file->path()) == path)
                    file->scheduleDiskCheck();
            }
        });
        connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &dir) {
            for (TextFile *file : std::as_const(m_files)) {
                if (!file->path().isEmpty() && QFileInfo(watchPathFor(file->path())).absolutePath() == dir)
                    file->scheduleDiskCheck();
            }
        });
    }

    void armCheckTimer()
    {
        if (m_pendingChecks.isEmpty()) {
            m_checkTimer->stop();
            return;
        }
        const qint64 next = *std::min_element(m_pendingChecks.cbegin(), m_pendingChecks.cend());
        m_checkTimer->start(int(std::max<qint64>(0, next - now())));
    }

    void runDueChecks()
    {
        const qint64 time = now();
        QList<TextFile *> due;
        for (auto it = m_pendingChecks.begin(); it != m_pendingChecks.end();) {
            if (it.value() <= time) {
                due.append(it.key());
                it = m_pendingChecks.erase(it);
            } else {
                ++it;
            }
        }
        if (!due.isEmpty()) {
            // Watch first, then look, so a change in between is not missed;
            // once for the whole batch (window activation checks every file).
            updateWatches();
            for (TextFile *file : std::as_const(due)) {
                if (m_files.contains(file))
                    file->compareWithDisk();
            }
        }
        armCheckTimer();
    }

    void unregister(TextFile *file)
    {
        for (auto it = m_byKey.begin(); it != m_byKey.end();) {
            if (it.value() == file)
                it = m_byKey.erase(it);
            else
                ++it;
        }
    }

    QFileSystemWatcher *m_watcher;
    QTimer *m_checkTimer;
    // Files waiting for a disk check, with when it is due.
    QHash<TextFile *, qint64> m_pendingChecks;
    QList<TextFile *> m_files;
    QHash<QString, TextFile *> m_byKey;
};

std::shared_ptr<TextFile> TextFile::create()
{
    return std::shared_ptr<TextFile>(new TextFile);
}

std::shared_ptr<TextFile> TextFile::find(const QString &path)
{
    TextFile *file = TextFileRegistry::instance().find(path);
    return file ? file->shared_from_this() : nullptr;
}

QList<TextFile *> TextFile::all()
{
    return TextFileRegistry::instance().files();
}

void TextFile::checkAllOnDisk()
{
    for (TextFile *file : TextFileRegistry::instance().files()) {
        if (!file->path().isEmpty())
            file->scheduleDiskCheck();
    }
}

TextFile::TextFile()
    : m_doc(new QTextDocument(this))
{
    m_doc->setDocumentLayout(new DocumentLayout(m_doc));
    m_doc->setUndoRedoEnabled(true);
    m_linkTimer = new QTimer(this);
    m_linkTimer->setSingleShot(true);
    m_linkTimer->setInterval(500);
    connect(m_linkTimer, &QTimer::timeout, this, &TextFile::checkLinks);

    // New files take their line ending and encoding from the settings.
    const QString eol = Settings::instance().text(key::lineEnding);
#ifdef Q_OS_WIN
    m_lineEnding = eol == QLatin1String("lf") ? LineEnding::LF : LineEnding::CRLF;
#else
    m_lineEnding = eol == QLatin1String("crlf") ? LineEnding::CRLF : LineEnding::LF;
#endif
    const QString newEncoding = encoding::canonical(Settings::instance().text(key::encoding));
    m_encoding = newEncoding.isEmpty() ? encoding::utf8 : newEncoding;
    m_indentation = defaultIndentation();
    m_validateLinks = Settings::instance().flag(key::validateLinks);
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &changedKey) {
        if (changedKey == QLatin1String(key::tabWidth) || changedKey == QLatin1String(key::insertSpaces)
            || changedKey == QLatin1String(key::detectIndentation)) {
            // Guessing reads the whole text; do it now only if someone sees
            // the file, otherwise when a view is shown (applyPendingSettings).
            const bool seen = std::any_of(m_views.cbegin(), m_views.cend(),
                                          [](const DocumentTab *view) { return view->isVisible(); });
            if (seen || m_views.isEmpty())
                refreshIndentation();
            else
                m_indentationStale = true;
        }
        if (changedKey == QLatin1String(key::validateLinks)) {
            m_validateLinks = Settings::instance().flag(key::validateLinks);
            checkLinks();
        }
    });

    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_doc);
    m_highlighter->setTheme(theme::syntaxTheme());
    updateDefinition();

    // Text edits only: highlighter format changes do not emit contentsChange.
    connect(m_doc, &QTextDocument::contentsChange, this, [this] { ++m_changeSerial; });
    connect(m_doc, &QTextDocument::modificationChanged, this, &TextFile::modificationChanged);
    connect(m_doc, &QTextDocument::contentsChanged, this, &TextFile::contentsChanged);
    connect(m_doc, &QTextDocument::contentsChanged, this, &TextFile::scheduleLinkCheck);
    TextFileRegistry::instance().add(this);
}

TextFile::~TextFile()
{
    TextFileRegistry::instance().remove(this);
}

void TextFile::addView(DocumentTab *view)
{
    if (!m_views.contains(view))
        m_views.append(view);
}

void TextFile::removeView(DocumentTab *view)
{
    m_views.removeAll(view);
}

void TextFile::setPath(const QString &path)
{
    if (m_path == path)
        return;
    m_path = path;
    auto &registry = TextFileRegistry::instance();
    registry.rekey(this);
    registry.updateWatches();
    Q_EMIT pathChanged();
}

bool TextFile::load(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = file.errorString();
        return false;
    }
    const QByteArray bytes = file.readAll();
    return applyBytes(path, bytes, encoding::detect(bytes));
}

bool TextFile::reopenWithEncoding(const Encoding &encoding)
{
    if (m_path.isEmpty() || isModified()) {
        m_error = tr("Only a saved file without changes can be reopened.");
        return false;
    }
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = file.errorString();
        return false;
    }
    return applyBytes(m_path, file.readAll(), encoding);
}

// Never fails on bad bytes: undecodable input becomes U+FFFD.
bool TextFile::applyBytes(const QString &path, const QByteArray &bytes, const Encoding &encoding)
{
    QString content = encoding::decode(bytes, encoding);
    normalizeLineEndings(content, m_lineEnding);

    m_encoding = encoding;
    setPath(path);
    rememberDisk(bytes);
    setOrphaned(false);
    setChangedOnDisk(false);
    m_languageOverridden = false;
    updateDefinition();
    updateHighlighting(content);
    m_doc->setPlainText(content);
    m_doc->setModified(false);
    Q_EMIT contentReset();
    m_indentationOverridden = false;
    refreshIndentation();
    Q_EMIT formatChanged();
    return true;
}

void TextFile::loadBackup(const QString &path, const QString &content, const Encoding &encoding, LineEnding lineEnding)
{
    m_encoding = encoding;
    m_lineEnding = lineEnding;
    setPath(path);
    m_disk = {};
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            rememberDisk(file.readAll());
    }
    m_languageOverridden = false;
    updateDefinition();
    updateHighlighting(content);
    m_doc->setPlainText(content);
    Q_EMIT contentReset();
    m_doc->setModified(true);
    m_indentationOverridden = false;
    refreshIndentation();
    Q_EMIT formatChanged();
    // A file deleted since the backup shows as such.
    scheduleDiskCheck();
}

void TextFile::updateHighlighting(const QString &content)
{
    const bool highlight = content.size() <= largeFileChars && content.count(u'\n') < largeFileLines;
    if (highlight == m_highlighted)
        return;
    m_highlighted = highlight;
    // Detached before the text is set, so the highlighter never sees it.
    m_highlighter->setDocument(highlight ? m_doc : nullptr);
}

void TextFile::rememberDisk(const QByteArray &bytes)
{
    const QFileInfo info(m_path);
    m_disk = {true, info.lastModified(), info.size(), hashOf(bytes)};
}

void TextFile::setDiskBaseline(const DiskState &state)
{
    m_disk = state;
}

bool TextFile::save(const QString &path, bool overwrite)
{
    m_saveConflict = false;
    QString content = text();
    if (m_lineEnding == LineEnding::CRLF)
        content.replace(u'\n', QStringLiteral("\r\n"));

    const std::optional<QByteArray> encoded = encoding::encode(content, m_encoding);
    if (!encoded) {
        m_error = tr("The document contains characters that %1 cannot represent. Save it as UTF-8 instead.")
                      .arg(encoding::label(m_encoding));
        return false;
    }

    const bool renamed = m_path.isEmpty() || keyFor(path) != keyFor(m_path);
    if (renamed) {
        // One file per path: saving over another open file would leave two.
        const std::shared_ptr<TextFile> other = find(path);
        if (other && other.get() != this) {
            m_error = tr("The file is open in another tab. Close that tab first.");
            return false;
        }
    } else if (!overwrite && m_disk.known) {
        // VS Code's FILE_MODIFIED_SINCE: the disk is not what we last saw.
        const QFileInfo info(path);
        if (info.exists() && (info.lastModified() != m_disk.modified || info.size() != m_disk.size)) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly) && hashOf(file.readAll()) != m_disk.hash) {
                m_saveConflict = true;
                m_error = tr("The file has been changed on disk since it was opened or last saved.");
                return false;
            }
        }
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_error = file.errorString();
        return false;
    }
    file.write(*encoded);
    if (!file.commit()) {
        m_error = file.errorString();
        return false;
    }

    if (renamed) {
        m_languageOverridden = false;
        setPath(path);
        updateDefinition();
    }
    rememberDisk(*encoded);
    TextFileRegistry::instance().rearm(path);
    m_doc->setModified(false);
    setOrphaned(false);
    setChangedOnDisk(false);
    return true;
}

bool TextFile::revert()
{
    if (m_path.isEmpty()) {
        m_error = tr("An untitled file has nothing to revert to.");
        return false;
    }
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_error = file.errorString();
        return false;
    }
    const QByteArray bytes = file.readAll();
    QString content = encoding::decode(bytes, m_encoding);
    LineEnding lineEnding;
    normalizeLineEndings(content, lineEnding);
    replaceText(content);
    m_doc->setModified(false);
    rememberDisk(bytes);
    m_saveConflict = false;
    setOrphaned(false);
    setChangedOnDisk(false);
    if (lineEnding != m_lineEnding) {
        m_lineEnding = lineEnding;
        Q_EMIT formatChanged();
    }
    return true;
}

void TextFile::replaceText(const QString &content)
{
    // Like VS Code's ModelService.updateModel: replace only what differs, as
    // one undo step, so carets and scroll positions outside it stay put.
    const QString old = text();
    if (old == content)
        return;
    const qsizetype common = std::min(old.size(), content.size());
    qsizetype prefix = 0;
    while (prefix < common && old.at(prefix) == content.at(prefix))
        ++prefix;
    if (prefix > 0 && old.at(prefix - 1).isHighSurrogate())
        --prefix;
    qsizetype suffix = 0;
    while (suffix < common - prefix && old.at(old.size() - 1 - suffix) == content.at(content.size() - 1 - suffix))
        ++suffix;
    if (suffix > 0 && old.at(old.size() - suffix).isLowSurrogate())
        --suffix;
    QTextCursor cursor(m_doc);
    cursor.beginEditBlock();
    cursor.setPosition(int(prefix));
    cursor.setPosition(int(old.size() - suffix), QTextCursor::KeepAnchor);
    cursor.insertText(content.mid(prefix, content.size() - prefix - suffix));
    cursor.endEditBlock();
}

void TextFile::scheduleDiskCheck()
{
    if (!m_path.isEmpty())
        TextFileRegistry::instance().scheduleCheck(this);
}

void TextFile::checkDisk()
{
    auto &registry = TextFileRegistry::instance();
    registry.cancelCheck(this);
    if (m_path.isEmpty())
        return;
    // Watch first, then look, so a change in between is not missed.
    registry.updateWatches();
    compareWithDisk();
}

void TextFile::compareWithDisk()
{
    if (m_path.isEmpty())
        return;
    m_lastDiskCheck = now();
    const QFileInfo info(m_path);
    if (!info.exists()) {
        setOrphaned(true);
        return;
    }
    setOrphaned(false);
    if (!m_disk.known)
        return;
    if (info.lastModified() == m_disk.modified && info.size() == m_disk.size)
        return;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = file.readAll();
    if (hashOf(bytes) == m_disk.hash) {
        // Touched, not changed.
        m_disk.modified = info.lastModified();
        m_disk.size = info.size();
        setChangedOnDisk(false);
        return;
    }
    // Never reload over unsaved changes; saving will report the conflict.
    if (isModified()) {
        setChangedOnDisk(true);
        return;
    }
    revert();
}

void TextFile::setOrphaned(bool orphaned)
{
    if (m_orphaned == orphaned)
        return;
    m_orphaned = orphaned;
    Q_EMIT diskStateChanged();
}

void TextFile::setChangedOnDisk(bool changed)
{
    if (m_changedOnDisk == changed)
        return;
    m_changedOnDisk = changed;
    Q_EMIT diskStateChanged();
}

void TextFile::setLineEnding(LineEnding lineEnding)
{
    if (m_lineEnding == lineEnding)
        return;
    m_lineEnding = lineEnding;
    m_doc->setModified(true);
    Q_EMIT formatChanged();
}

void TextFile::setEncoding(const Encoding &encoding)
{
    if (m_encoding == encoding)
        return;
    m_encoding = encoding;
    m_doc->setModified(true);
    Q_EMIT formatChanged();
}

indentation::Settings TextFile::defaultIndentation() const
{
    const Settings &s = Settings::instance();
    return {std::max(1, s.number(key::tabWidth)), s.flag(key::insertSpaces)};
}

void TextFile::setEffectiveIndentation(const indentation::Settings &indentation)
{
    if (m_indentation == indentation)
        return;
    m_indentation = indentation;
    Q_EMIT indentationChanged();
}

void TextFile::applyPendingSettings()
{
    if (m_indentationStale)
        refreshIndentation();
}

void TextFile::refreshIndentation()
{
    m_indentationStale = false;
    if (m_indentationOverridden)
        return;
    const indentation::Settings defaults = defaultIndentation();
    setEffectiveIndentation(Settings::instance().flag(key::detectIndentation)
                                ? indentation::guess(m_doc, defaults.tabSize, defaults.insertSpaces)
                                : defaults);
}

void TextFile::setIndentation(const indentation::Settings &indentation)
{
    m_indentationOverridden = true;
    setEffectiveIndentation({std::max(1, indentation.tabSize), indentation.insertSpaces});
}

void TextFile::detectIndentation()
{
    m_indentationOverridden = false;
    const indentation::Settings defaults = defaultIndentation();
    setEffectiveIndentation(indentation::guess(m_doc, defaults.tabSize, defaults.insertSpaces));
}

void TextFile::convertIndentation(bool toSpaces)
{
    indentation::convert(m_doc, m_indentation.tabSize, toSpaces);
    setIndentation({m_indentation.tabSize, toSpaces});
}

QString TextFile::definitionName() const
{
    const auto def = m_highlighter->definition();
    return def.isValid() ? def.name() : QString();
}

void TextFile::setLanguage(const QString &definitionName)
{
    m_languageOverridden = true;
    m_languageOverride = definitionName;
    updateDefinition();
}

QString TextFile::displayName() const
{
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

bool TextFile::isModified() const
{
    return m_doc->isModified();
}

bool TextFile::isMarkdown() const
{
    return m_highlighter->definition().name() == QLatin1String("Markdown");
}

QString TextFile::languageName() const
{
    const auto def = m_highlighter->definition();
    return def.isValid() ? def.translatedName() : tr("Plain Text");
}

QString TextFile::text() const
{
    // toPlainText() would turn non-breaking spaces into spaces.
    QString text = m_doc->toRawText();
    text.replace(QChar::ParagraphSeparator, u'\n');
    return text;
}

MarkdownModel *TextFile::markdownModel()
{
    if (!m_model)
        m_model = new MarkdownModel(m_doc);
    return m_model;
}

void TextFile::updateDefinition()
{
    // New documents are plain text unless "Open new files as" says Markdown
    // (the old app's behaviour).
    auto &repository = theme::syntaxRepository();
    const bool markdownByDefault = Settings::instance().text(key::newFileLanguage) == QLatin1String("markdown");
    const auto def = m_languageOverridden ? (m_languageOverride.isEmpty() ? KSyntaxHighlighting::Definition()
                                                                          : repository.definitionForName(m_languageOverride))
                     : m_path.isEmpty() ? (markdownByDefault ? repository.definitionForName(QStringLiteral("Markdown"))
                                                           : KSyntaxHighlighting::Definition())
                                      : repository.definitionForFileName(m_path);
    m_highlighter->setDefinition(def);
    // Views drop their visual halves first; then nothing uses the parser.
    Q_EMIT definitionChanged();
    if (!isMarkdown() && m_model) {
        delete m_model;
        m_model = nullptr;
    }
    scheduleLinkCheck();
    Q_EMIT formatChanged();
}

void TextFile::scheduleLinkCheck()
{
    if (m_validateLinks && isMarkdown())
        m_linkTimer->start();
    else if (!m_diagnostics.isEmpty())
        checkLinks();
}

void TextFile::checkLinks()
{
    m_linkTimer->stop();
    if (m_validateLinks && isMarkdown())
        m_diagnostics = brokenLinks();
    else
        m_diagnostics.clear();
    Q_EMIT diagnosticsChanged();
}

namespace {

// The heading anchors of a linked Markdown file. The check runs half a second
// after each edit, so files are only read again when they change on disk.
QStringList anchorsOfFile(const QFileInfo &info)
{
    struct Entry
    {
        QDateTime modified;
        qint64 size = -1;
        QStringList anchors;
    };
    static QHash<QString, Entry> cache;
    const QString path = info.absoluteFilePath();
    const QDateTime modified = info.lastModified();
    const qint64 size = info.size();
    const auto it = cache.constFind(path);
    if (it != cache.constEnd() && it->modified == modified && it->size == size)
        return it->anchors;
    QFile f(path);
    QString content;
    if (size <= 8 * 1024 * 1024 && f.open(QIODevice::ReadOnly))
        content = QString::fromUtf8(f.readAll());
    // Bounded: links to many files over a long session.
    if (cache.size() >= 256)
        cache.clear();
    Entry entry{modified, size, md::anchorsOf(content)};
    cache.insert(path, entry);
    return entry.anchors;
}

} // namespace

QList<TextView::Diagnostic> TextFile::brokenLinks() const
{
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]+:"));
    static const QRegularExpression lineAnchor(QStringLiteral("^L\\d+"), QRegularExpression::CaseInsensitiveOption);
    const auto decode = [](const QString &s) { return QUrl::fromPercentEncoding(s.toUtf8()); };

    const QDir dir = m_path.isEmpty() ? QDir() : QFileInfo(m_path).absoluteDir();
    const QString content = text();
    QStringList ownAnchors;
    bool ownLoaded = false;
    QList<TextView::Diagnostic> found;

    for (const md::LinkTarget &target : md::linkTargets(content)) {
        const QString &dest = target.dest;
        if (dest.isEmpty() || scheme.match(dest).hasMatch())
            continue;
        QString path = dest.section(u'#', 0, 0);
        const QString fragment = decode(dest.contains(u'#') ? dest.section(u'#', 1) : QString()).toLower();
        path = decode(path.section(u'?', 0, 0));
        const auto report = [&] {
            found.append({target.start, target.end, tr("Link target not found: %1").arg(dest)});
        };

        if (path.isEmpty()) {
            if (fragment.isEmpty() || lineAnchor.match(fragment, 0).hasMatch())
                continue;
            if (!ownLoaded) {
                ownAnchors = md::anchorsOf(content);
                ownLoaded = true;
            }
            if (!ownAnchors.contains(fragment))
                report();
            continue;
        }
        // "/x" is relative to a workspace root, which this app doesn't have;
        // "\\host\share" is on another machine, never touched for a check.
        if (path.startsWith(u'/') || md::isNetworkPath(path))
            continue;
        const QString file = QDir::isAbsolutePath(path) ? path : (m_path.isEmpty() ? QString() : dir.filePath(path));
        if (file.isEmpty())
            continue;
        const QFileInfo info(file);
        if (!info.exists()) {
            report();
            continue;
        }
        const QString suffix = info.suffix().toLower();
        if (fragment.isEmpty() || lineAnchor.match(fragment, 0).hasMatch() || !info.isFile()
            || (suffix != QLatin1String("md") && suffix != QLatin1String("markdown") && suffix != QLatin1String("mdown")
                && suffix != QLatin1String("mkd")))
            continue;
        if (!anchorsOfFile(info).contains(fragment))
            report();
    }
    return found;
}

} // namespace vn
