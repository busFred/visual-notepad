#pragma once

#include <KSyntaxHighlighting/Repository>

#include <QMainWindow>

class QAction;
class QDockWidget;
class QLabel;
class QTabWidget;
class QTimer;
class QTreeWidget;

namespace vn {

class DocumentTab;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void newFile();
    bool openFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createMenus();
    void createOutline();
    void createStatusBar();

    DocumentTab *currentTab() const;
    DocumentTab *tabAt(int index) const;
    void addTab(DocumentTab *tab);
    void openWithDialog();
    bool save(DocumentTab *tab);
    bool saveAs(DocumentTab *tab);
    bool closeTab(int index);
    bool confirmDiscard(DocumentTab *tab);
    void zoom(int steps);

    void onCurrentTabChanged();
    void updateTabTitle(DocumentTab *tab);
    void updateStatus();
    void updateOutline();
    void updateOutlineVisibility();

    KSyntaxHighlighting::Repository m_repository;
    QTabWidget *m_tabs;
    QDockWidget *m_outlineDock;
    QTreeWidget *m_outline;
    QTimer *m_outlineTimer;
    QAction *m_outlineAction;
    QLabel *m_position;
    QLabel *m_zoom;
    QLabel *m_lineEnding;
    QLabel *m_encoding;
    QLabel *m_language;
    int m_zoomPercent = 100;
};

} // namespace vn
