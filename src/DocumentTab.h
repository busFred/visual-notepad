// A tab showing an open file: the editor plus its floating lock/pencil pill.
#pragma once

#include "Encodings.h"
#include "TextFile.h"
#include "editor/Indentation.h"
#include "editor/TextView.h"

#include <QByteArray>
#include <QFont>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <utility>

class QLineEdit;
class QSplitter;
class QTimer;
class QTextDocument;

namespace vn {

class EditorPill;
class FindBar;
class TextView;
class VisualView;

// A view onto a TextFile: the source view and, for Markdown, the visual
// view, with the per-tab state (view mode, read-only lock, zoom, split,
// carets and scroll). Several tabs, in any windows, can show one file; edits,
// modified state, encoding, language and indentation are the file's and so
// shared by all of them.
class DocumentTab : public QWidget
{
    Q_OBJECT

public:
    using LineEnding = TextFile::LineEnding;
    // A canonical name from encoding:: (Encodings.h). The UTF-16 variants are
    // always written with a BOM.
    using Encoding = TextFile::Encoding;
    // How a Markdown document is shown.
    enum class ViewMode { Source, SourceVisual, Visual };

    // A tab on a new untitled file.
    explicit DocumentTab(QWidget *parent = nullptr);
    // A tab on `file`, which may already be shown by other tabs.
    explicit DocumentTab(std::shared_ptr<TextFile> file, QWidget *parent = nullptr);
    ~DocumentTab() override;

    // The shared file this tab shows.
    TextFile *file() const { return m_file.get(); }
    std::shared_ptr<TextFile> sharedFile() const { return m_file; }
    // A second tab on the same file, with this tab's view mode and zoom; the
    // caller places it (MainWindow::adoptTab / receiveTab).
    DocumentTab *openAnotherView(QWidget *parent = nullptr) const;
    // The tab is going away (its deletion is pending): it no longer counts
    // as a view of its file, e.g. for "is this the last tab" checks.
    void setClosing();
    // Other tabs showing this tab's file.
    QList<DocumentTab *> otherViews() const;

    // Returns false and sets errorString() on failure.
    bool load(const QString &path);
    bool save(const QString &path);
    // Restores unsaved content saved by hot exit: the tab keeps `path` (empty
    // for untitled), shows `content` and is marked modified.
    void loadBackup(const QString &path, const QString &content, const Encoding &encoding, LineEnding lineEnding);

    QString path() const { return m_file->path(); }
    QString displayName() const { return m_file->displayName(); }
    QString errorString() const { return m_file->errorString(); }
    bool isModified() const { return m_file->isModified(); }
    bool isMarkdown() const { return m_file->isMarkdown(); }
    QString languageName() const { return m_file->languageName(); }
    LineEnding lineEnding() const { return m_file->lineEnding(); }
    // Changing the line ending or encoding marks the document modified so the
    // next save rewrites the file.
    void setLineEnding(LineEnding lineEnding) { m_file->setLineEnding(lineEnding); }
    Encoding encoding() const { return m_file->encoding(); }
    QString encodingName() const { return encoding::label(m_file->encoding()); }
    void setEncoding(const Encoding &encoding) { m_file->setEncoding(encoding); }
    // Re-reads the file from disk with the given encoding. Fails (and sets
    // errorString()) for untitled or modified documents.
    bool reopenWithEncoding(const Encoding &encoding) { return m_file->reopenWithEncoding(encoding); }
    // Syntax definition name as KSyntaxHighlighting knows it; empty for plain
    // text. An override lasts until the file is saved under a new name.
    QString definitionName() const { return m_file->definitionName(); }
    void setLanguage(const QString &definitionName) { m_file->setLanguage(definitionName); }
    bool languageOverridden() const { return m_file->languageOverridden(); }
    QString text() const { return m_file->text(); }

    // Indentation of this file: detected from the content when it arrives
    // (if the detectIndentation setting is on), else the tab width / spaces
    // settings. setIndentation overrides both until the file is closed.
    indentation::Settings indentation() const { return m_file->indentation(); }
    bool indentationOverridden() const { return m_file->indentationOverridden(); }
    void setIndentation(const indentation::Settings &indentation) { m_file->setIndentation(indentation); }
    // Guesses from the content once, whatever the setting, and drops the
    // override.
    void detectIndentation() { m_file->detectIndentation(); }
    // Rewrites the indentation as one undoable edit and uses the target
    // style from now on. Does nothing when read-only.
    void convertIndentation(bool toSpaces);

    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    // Whether the source view is shown (always for a file that is not Markdown).
    bool showsSource() const;
    // The lines (first, last) the source view has on screen, or (-1, -1).
    std::pair<int, int> visibleSourceLines() const;

    QTextDocument *document() const { return m_doc; }
    // The view that has (or last had) focus; for plain text the source view.
    TextView *editor() const;
    TextView *sourceView() const { return m_editor; }
    VisualView *visualView() const { return m_visual; }
    bool isReadOnly() const;
    void goToLine(int line);
    // Created on first use.
    FindBar *findBar() const;
    void showFind(bool replace);
    void showGoToLine();
    int zoom() const { return m_zoom; }
    void setZoom(int percent);
    // Rewrites a Markdown document in canonical form as one undoable edit.
    void formatDocument();

Q_SIGNALS:
    void linkActivated(const QString &url);
    void filesDropped(const QStringList &files);
    void viewModeChanged();
    void modificationChanged(bool modified);
    void cursorPositionChanged();
    void textChanged();
    // Encoding, line ending or language changed.
    void formatChanged();
    // Tab size or spaces/tabs changed.
    void indentationChanged();
    // The file's name or its deleted / changed-on-disk state changed.
    void titleChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void onDefinitionChanged();
    void resetCarets();
    void updatePill();
    void placeOverlays();
    void applySettings();
    void applyDiagnostics();
    void applyViewSettings(TextView *view);
    void onSettingChanged(const QString &changedKey);
    void forEachView(const std::function<void(TextView *)> &apply);
    QFont zoomedEditorFont() const;
    QWidget *createGoToLine();
    void ensureVisualView();
    void updateViews();
    void syncScroll(TextView *from, TextView *to);
    // Shows from's selection in the other half of a split (see mirrorCursorsFrom).
    void mirrorSelection(TextView *from, TextView *to);
    void setActiveView(TextView *view);
    void applyMarkdownState();

    std::shared_ptr<TextFile> m_file;
    QTextDocument *m_doc;
    TextView *m_editor; // Source view.
    VisualView *m_visual = nullptr;
    QSplitter *m_splitter;
    TextView *m_active = nullptr;
    bool m_syncingScroll = false;
    bool m_mirroringSelection = false;
    EditorPill *m_pill;
    FindBar *m_findBar = nullptr;
    QWidget *m_goToLine = nullptr;
    QLineEdit *m_goToLineEdit = nullptr;
    int m_zoom = 100;
    // Font, zoom and view settings changed while hidden; applied on show.
    bool m_settingsStale = false;
    QFont m_appliedFont;
    ViewMode m_viewMode = ViewMode::Source;
    // Whether the visual view is read-only, per mode: beside the source it is
    // a preview, on its own an editor.
    bool m_splitLocked = true;
    bool m_visualLocked = false;
};

} // namespace vn
