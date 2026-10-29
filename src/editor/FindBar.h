// VS Code style find/replace widget, docked at the top-right of an editor.
#pragma once

#include "Search.h"

#include <QFrame>
#include <QTimer>

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

    // Shows the bar, seeding the query from a one-line selection.
    void open(bool replace);
    void close();
    void findNext();
    void findPrevious();
    void replaceOne();
    void replaceAll();

    QString query() const;
    void setQuery(const QString &text);
    QLineEdit *findField() const { return m_find; }
    QLineEdit *replaceField() const { return m_replace; }
    int matchCount() const { return int(m_matches.size()); }
    int currentMatch() const { return m_current; }

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
    void selectMatch(int index);
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

    QList<QTextCursor> m_matches;
    int m_current = -1;
    QTextCursor m_scope; // Find-in-selection range.
    int m_searchStart = 0;
    QTimer m_researchTimer;
    int m_historyIndex = -1;
};

} // namespace vn
