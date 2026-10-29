#include "TextFile.h"

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

QByteArray hashOf(const QByteArray &bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1);
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
        unregister(file);
        updateWatches();
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
        const QStringList watchedFiles = m_watcher->files();
        const QStringList watchedDirs = m_watcher->directories();
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
    {
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
    m_diskTimer = new QTimer(this);
    m_diskTimer->setSingleShot(true);
    m_diskTimer->setInterval(diskCheckDelayMs);
    connect(m_diskTimer, &QTimer::timeout, this, &TextFile::checkDisk);

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
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &changedKey) {
        if (changedKey == QLatin1String(key::tabWidth) || changedKey == QLatin1String(key::insertSpaces)
            || changedKey == QLatin1String(key::detectIndentation))
            refreshIndentation();
        if (changedKey == QLatin1String(key::validateLinks))
            checkLinks();
    });

    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_doc);
    m_highlighter->setTheme(theme::syntaxTheme());
    updateDefinition();

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

    // The first line ending decides the style used on save.
    const qsizetype lf = content.indexOf(u'\n');
    m_lineEnding = (lf > 0 && content.at(lf - 1) == u'\r') ? LineEnding::CRLF : LineEnding::LF;
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    m_encoding = encoding;
    setPath(path);
    rememberDisk(bytes);
    setOrphaned(false);
    setChangedOnDisk(false);
    m_languageOverridden = false;
    updateDefinition();
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
    m_doc->setPlainText(content);
    Q_EMIT contentReset();
    m_doc->setModified(true);
    m_indentationOverridden = false;
    refreshIndentation();
    Q_EMIT formatChanged();
    // A file deleted since the backup shows as such.
    scheduleDiskCheck();
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
    const qsizetype lf = content.indexOf(u'\n');
    const LineEnding lineEnding = (lf > 0 && content.at(lf - 1) == u'\r') ? LineEnding::CRLF : LineEnding::LF;
    content.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
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
    if (!m_path.isEmpty() && !m_diskTimer->isActive())
        m_diskTimer->start();
}

void TextFile::checkDisk()
{
    m_diskTimer->stop();
    if (m_path.isEmpty())
        return;
    // Watch first, then look, so a change in between is not missed.
    TextFileRegistry::instance().updateWatches();
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

void TextFile::refreshIndentation()
{
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
    // New documents default to Markdown, like the old app, unless set otherwise.
    auto &repository = theme::syntaxRepository();
    const bool markdownByDefault = Settings::instance().text(key::newFileLanguage) != QLatin1String("plaintext");
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
    if (isMarkdown() && Settings::instance().flag(key::validateLinks))
        m_linkTimer->start();
    else if (!m_diagnostics.isEmpty())
        checkLinks();
}

void TextFile::checkLinks()
{
    m_linkTimer->stop();
    if (isMarkdown() && Settings::instance().flag(key::validateLinks))
        m_diagnostics = brokenLinks();
    else
        m_diagnostics.clear();
    Q_EMIT diagnosticsChanged();
}

QList<TextView::Diagnostic> TextFile::brokenLinks() const
{
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]+:"));
    static const QRegularExpression lineAnchor(QStringLiteral("^L\\d+"), QRegularExpression::CaseInsensitiveOption);
    const auto decode = [](const QString &s) { return QUrl::fromPercentEncoding(s.toUtf8()); };

    const QDir dir = m_path.isEmpty() ? QDir() : QFileInfo(m_path).absoluteDir();
    QStringList ownAnchors;
    bool ownLoaded = false;
    QHash<QString, QStringList> fileAnchors;
    QList<TextView::Diagnostic> found;

    for (const md::LinkTarget &target : md::linkTargets(text())) {
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
                ownAnchors = md::anchorsOf(text());
                ownLoaded = true;
            }
            if (!ownAnchors.contains(fragment))
                report();
            continue;
        }
        // "/x" is relative to a workspace root, which this app doesn't have.
        if (path.startsWith(u'/'))
            continue;
        const QString file = QDir::isAbsolutePath(path) ? path : (m_path.isEmpty() ? QString() : dir.filePath(path));
        if (file.isEmpty())
            continue;
        if (!QFileInfo::exists(file)) {
            report();
            continue;
        }
        const QString suffix = QFileInfo(file).suffix().toLower();
        if (fragment.isEmpty() || lineAnchor.match(fragment, 0).hasMatch() || !QFileInfo(file).isFile()
            || (suffix != QLatin1String("md") && suffix != QLatin1String("markdown") && suffix != QLatin1String("mdown")
                && suffix != QLatin1String("mkd")))
            continue;
        auto it = fileAnchors.find(file);
        if (it == fileAnchors.end()) {
            QFile f(file);
            QString content;
            if (f.size() <= 8 * 1024 * 1024 && f.open(QIODevice::ReadOnly))
                content = QString::fromUtf8(f.readAll());
            it = fileAnchors.insert(file, md::anchorsOf(content));
        }
        if (!it->contains(fragment))
            report();
    }
    return found;
}

} // namespace vn
