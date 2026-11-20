// A tab showing an open file: the editor plus its floating lock/pencil pill.
#pragma once

#include "Encodings.h"
#include "TextFile.h"
#include "editor/Indentation.h"
#include "editor/TextView.h"

#include <QByteArray>
#include <QFont>
#include <QJsonObject>
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
    // A second tab on the same file, with this tab's view mode and zooms; the
    // caller places it (MainWindow::adoptTab / receiveTab).
    DocumentTab *openAnotherView(QWidget *parent = nullptr) const;
    // The tab is going away (its deletion is pending): it no longer counts
    // as a view of its file, e.g. for "is this the last tab" checks.
    void setClosing();
    // Other tabs showing this tab's file.
    QList<DocumentTab *> otherViews() const;

    // A restored tab whose file is not read yet (TextFile::createDeferred):
    // `entry` is what the session stored of it, kept as it was until the tab
    // is loaded, and `afterLoad` puts its view state back then.
    void setDeferred(const QJsonObject &entry, std::function<void(DocumentTab *)> afterLoad);
    // Not loaded since the restore: it was never shown, nor was its text
    // needed.
    bool isDeferred() const { return m_deferred; }
    const QJsonObject &sessionEntry() const { return m_sessionEntry; }
    // Reads the file if that is still to do and restores the view state; a
    // group calls it when the tab becomes its current one. False if the file
    // could not be read.
    bool ensureLoaded();

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
    // Zoom is per tab and per half: the source and the visual view each have
    // their own. In a split the half last clicked or focused (editor()) is
    // the one these act on, and wears an accent line; with one view shown it
    // is that view. A new tab starts at the "default zoom" setting.
    static constexpr int minZoom = 30;
    static constexpr int maxZoom = 500;
    static constexpr int zoomStep = 5;
    int zoom() const;
    int zoom(const TextView *view) const;
    void setZoom(int percent);
    // Moves the half in use by whole steps, or back to the default for 0.
    void zoomBy(int steps);
    void resetZoom();
    // Both the source and the visual view are on screen.
    bool showsBothHalves() const;
    // The visual half is the one in use.
    bool zoomsVisual() const;
    // Rewrites a Markdown document in canonical form as one undoable edit.
    void formatDocument();

    // Columns: the view switch (Scroll | Columns) of the status bar. A tab
    // chooses for itself: it opens in Scroll and nothing is saved. They show
    // in Source (TextView::setColumnsShown) and, locked, in the Visual Editor:
    // read mode (VisualView::setReadMode); choosing them in the Visual Editor
    // while editing locks it. Where they cannot (the Visual Editor unlocked,
    // Source / Visual) the tab shows Scroll, keeping the same
    // line at the top and the caret at its start, and keeps the wish: columns
    // come back, from the caret's line, where they can again. The Columns
    // popup sets the number of columns and their width; those are this tab's,
    // not saved; Preferences give where a new tab starts.
    // The switch shows (not in Source / Visual).
    bool canShowColumns() const;
    // Columns show if wished: Source, or the Visual Editor locked.
    bool columnsShowNow() const;
    bool wantsColumns() const { return m_wantsColumns; }
    void setColumns(bool on);
    // Columns show (in Source, or read mode).
    bool showsColumns() const { return m_columnsView != nullptr; }
    // Read mode: columns in the Visual Editor, locked.
    bool isReadMode() const;
    // The source lines on screen in columns (0-based, both included), or
    // (-1, -1) (also before a large document's first parse).
    std::pair<int, int> readLineRange() const;
    // The count and width to use when there is room. A count of 0 awaits the first layout.
    int readColumns() const { return m_readColumns; }
    int readChars() const { return m_readChars; }
    int columnsShownCount() const;
    int columnsFit() const;
    // Shown and maximum characters per column at the shown count.
    std::pair<int, int> columnsChars() const;
    void setReadColumns(int columns);
    void setReadChars(int chars);

Q_SIGNALS:
    void linkActivated(const QString &url);
    void filesDropped(const QStringList &files);
    void viewModeChanged();
    // The zoom of the half in use changed, or another half came into use.
    void zoomChanged();
    void modificationChanged(bool modified);
    void cursorPositionChanged();
    void textChanged();
    // Encoding, line ending or language changed.
    void formatChanged();
    // Tab size or spaces/tabs changed.
    void indentationChanged();
    // Columns came or went (or could be shown or not), or their screen changed.
    void readStateChanged();
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
    void onStyleChanged(TextView *view);
    void forEachView(const std::function<void(TextView *)> &apply);
    QFont zoomedEditorFont(const TextView *view) const;
    int &zoomOf(const TextView *view);
    void updateAccent();
    QWidget *createGoToLine();
    void ensureVisualView();
    // Starts the formula worker if the document has math, so the first
    // formula shown does not wait for it (see math::prepareWorker).
    void prepareMathWorker();
    void updateViews();
    void syncScroll(TextView *from, TextView *to);
    // Shows from's selection in the other half of a split (see mirrorCursorsFrom).
    void mirrorSelection(TextView *from, TextView *to);
    void setActiveView(TextView *view);
    void applyMarkdownState();
    // "As many as fit" (a count of 0) becomes the count that fits, once, at
    // the first columns layout shown; the count then stays.
    void resolveReadColumns();
    // Columns follow the wish, the mode and the lock; switched: the user
    // asked for them (or for Scroll) just now.
    void updateColumns(bool switched = false);

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
    int m_sourceZoom = 100;
    int m_visualZoom = 100;
    // The 2 px lines along the bottom of each half; the one of the half in
    // use shows in a split.
    QWidget *m_sourceAccent;
    QWidget *m_visualAccent = nullptr;
    // Font, zoom and view settings changed while hidden; applied on show.
    bool m_settingsStale = false;
    bool m_deferred = false;
    QJsonObject m_sessionEntry;
    std::function<void(DocumentTab *)> m_afterLoad;
    // The source half's font, to skip a font change that is already applied.
    QFont m_appliedFont;
    ViewMode m_viewMode = ViewMode::Source;
    // Whether the visual view is read-only, per mode: beside the source it is
    // a preview, on its own an editor.
    bool m_splitLocked = true;
    bool m_visualLocked = false;
    int m_readColumns = 0;
    int m_readChars = 75;
    bool m_wantsColumns = false;
    // The view showing columns, if any.
    TextView *m_columnsView = nullptr;
    // Whether columns could show, as the status bar last heard.
    bool m_couldShowColumns = true;
    bool m_lockingForColumns = false; // The switch is locking the Visual Editor.
};

} // namespace vn
