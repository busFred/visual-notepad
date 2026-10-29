// Every menu command with a stable id and its default shortcuts. Menus and the
// Preferences > Shortcuts page are built from this list; users override
// shortcuts through Settings.
#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

namespace vn {

struct Command
{
    QString id;       // e.g. "edit.find"
    QString category; // Menu name: File, Edit, Selection, View, Help
    QString text;     // Menu text, translated
    QList<QKeySequence> defaults;
};

const QList<Command> &commands();
const Command *command(const QString &id);

// The shortcuts in effect: the user's override if any, else the defaults.
QList<QKeySequence> shortcutsFor(const QString &id);

// Commands (other than id) whose effective shortcuts include sequence.
QStringList conflictingCommands(const QKeySequence &sequence, const QString &id);

} // namespace vn
