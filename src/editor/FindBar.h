// VS Code style find/replace widget, docked at the top-right of an editor.
#pragma once

#include "Search.h"

#include <QFrame>
#include <QFutureWatcher>
#include <QTextCursor>
#include <QTimer>

#include <functional>
#include <memory>

class QLabel;
class QLineEdit;
class QToolButton;
class QWidget;

namespace vn {

class TextView;

class FindBar : public QFrame
{
    Q_OBJECT

public:
    FindBar(TextView *view, QWidget *parent);
    ~FindBar() override;
    // Searches another view of the same document from now on.
    void setView(TextView *view);

    // Shows the bar, seeding the query from a one-line selection.
    void open(bool replace);
    void close();
    void findNext();
    void findPrevious();
    // Searches for the selected text (or the word at the caret) without
    // showing the bar, and moves to the next or previous match.
    void findSelection(bool forward);
    // Turns every match into a selection and focuses the editor.
    void selectAllMatches();
    void replaceOne();
    void replaceAll();

    QString query() const;
    void setQuery(const QString &text);
    QLineEdit *findField() const { return m_find; }
    QLineEdit *replaceField() const { return m_replace; }
    int matchCount() const { return int(m_matches.size()); }
    int currentMatch() const { return m_current; }
    // True while a search of a big document runs on a worker thread; the
    // matches and the counter still show the previous search.
    bool isSearching() const { return m_job != nullptr; }

Q_SIGNALS:
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QWidget *makeField(QLineEdit *edit, const QList<QToolButton *> &options);
    QToolButton *makeOption(const QString &text, const QString &tip);
    QToolButton *makeIconButton(const QString &icon, const QString &tip);
    search::Options options() const;
    void setReplaceVisible(bool on);
    void research();
    // Shows the matches of the latest search and carries on with whatever
    // waited for it.
    void applyMatches(QList<search::Range> matches);
    void cancelSearch();
    void finishSearch();
    // Runs `action` once the matches are up to date with the query and the
    // text: at once normally, after a search in flight when there is one.
    void runWhenReady(std::function<void()> action);
    void runPending();
    void stepNow(bool forward);
    void replaceOneNow();
    void replaceAllNow();
    void selectAllMatchesNow();
    QTextCursor cursorFor(search::Range range) const;
    void selectMatch(int index);
    void step(bool forward);
    void updateCount();
    void remember(QLineEdit *edit);
    void recall(QLineEdit *edit, int direction);

    TextView *m_view;
    QToolButton *m_toggleReplace;
    QLineEdit *m_find;
    QLineEdit *m_replace;
    QWidget *m_findField;
    QWidget *m_replaceRow;
    QToolButton *m_caseOption;
    QToolButton *m_wordOption;
    QToolButton *m_regexOption;
    QToolButton *m_preserveCaseOption;
    QToolButton *m_inSelection;
    QLabel *m_count;

    QList<search::Range> m_matches;
    bool m_selectAfterResearch = false; // The query was typed: jump to the first match.
    int m_current = -1;
    QTextCursor m_scope; // Find-in-selection range.
    int m_searchStart = 0;
    QTimer m_researchTimer;

    struct Job;
    std::shared_ptr<Job> m_job; // The search in flight on a worker, if any.
    QFutureWatcher<void> m_jobWatcher;
    QTimer m_busyTimer; // Shows "..." when a search takes noticeably long.
    quint64 m_revision = 0; // Counts edits of the document.
    // The document's text for the worker, kept while the document is unedited
    // so that typing a query does not copy it again for every search.
    QString m_snapshot;
    bool m_snapshotValid = false;
    QList<std::function<void()>> m_waiting;
    bool m_runningWaiting = false;
    int m_historyIndex = -1;
};

} // namespace vn
