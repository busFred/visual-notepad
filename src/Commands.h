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
    QString text;     // Title, translated, without the mnemonic
    QList<QKeySequence> defaults;
    QString menuText; // Title with its mnemonic (&) for the menu item
};

const QList<Command> &commands();
const Command *command(const QString &id);

// The shortcuts in effect: the user's override if any, else the defaults.
QList<QKeySequence> shortcutsFor(const QString &id);

// Commands (other than id) whose effective shortcuts clash with sequence:
// identical to it, a chord starting with it, or a prefix of it.
QStringList conflictingCommands(const QKeySequence &sequence, const QString &id);

struct ShortcutConflict
{
    QString otherId;       // The other command
    QKeySequence mine;     // The sequence of id that clashes
    QKeySequence theirs;   // The sequence of otherId that it clashes with
};
// Every clash between the effective shortcuts of id and those of other commands.
QList<ShortcutConflict> shortcutConflicts(const QString &id);
// Whether two sequences clash: equal, or one is a prefix of the other.
bool shortcutsClash(const QKeySequence &a, const QKeySequence &b);

// The menu bar's titles with their mnemonics ("&File", "&Edit", …), in order.
QStringList menuTitles();
// The menu (title without mnemonic) whose Alt+letter is `sequence`, or empty.
// Qt alternates an identical shortcut between the menu bar and the action and
// never runs an ambiguous action, so such a command never runs.
QString menuForMnemonic(const QKeySequence &sequence);

} // namespace vn
