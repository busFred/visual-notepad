// Shown in an editor group with no tabs, like VS Code's empty window: the
// app icon, a few commands with their shortcuts, and recent files.
#pragma once

#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace vn {

class WelcomeView : public QWidget
{
    Q_OBJECT

public:
    explicit WelcomeView(QWidget *parent = nullptr);

Q_SIGNALS:
    // A command id from the registry (e.g. "file.new").
    void commandRequested(const QString &id);
    void recentFileRequested(const QString &path);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void rebuild();

    QLabel *m_icon;
    QVBoxLayout *m_commands;
    QVBoxLayout *m_recent;
};

} // namespace vn
